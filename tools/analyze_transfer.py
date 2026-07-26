#!/usr/bin/env python3
"""Two-log transfer forensics: correlate SENDER and RECEIVER logs onto one timeline.

Answers, for one file transfer:
  * where every retransmission came from, and WHY (SACK hole / crater / rate-change abort / RTO)
  * whether the receiver ever ACKed ON TOP of a sender burst (half-duplex collision)
  * the airtime budget: key-down vs turnaround vs dead air, and the PA duty cycle
  * per-burst cycle timing, and where the wall clock actually went
  * delivered goodput vs the rung's raw capability (efficiency)

Usage:
    tools/analyze_transfer.py <sender.log> <receiver.log> [--json out.json]

Clock alignment: each log stamps [SSS.SSS] seconds since ITS OWN process start, so the two
are offset. We align on a CAUSAL anchor — the sender's first burst TX must precede the
receiver's first decode of it — and report the offset used so it can be sanity-checked.
"""

import argparse
import json
import re
import sys
from collections import Counter, defaultdict

T = r'^\[\s*([0-9]+\.[0-9]+)\]'


def parse(path, patterns):
    """Return [(t, kind, groupdict)] for every line matching any pattern."""
    out = []
    try:
        fh = open(path, errors='ignore')
    except OSError as e:
        sys.exit(f"cannot read {path}: {e}")
    with fh:
        for ln in fh:
            m = re.match(T, ln)
            if not m:
                continue
            t = float(m.group(1))
            for kind, rx in patterns.items():
                mm = rx.search(ln)
                if mm:
                    out.append((t, kind, mm.groupdict()))
                    break
    return out


# postProcessTx prepends a fixed lead-in and appends a tail to every keyed transmission
# (ULTRA_TX_LEADIN_MS / ULTRA_TX_TAIL_MS, defaults 150/50 ms). The emitted sample count
# logged by 'TX Burst' is the modulated burst only, so the lead-in is added back here.
LEAD_IN_S = 0.150

SENDER_PATTERNS = {
    'tx_burst':    re.compile(r'TX Burst descriptor: group=(?P<n>\d+) cw/frame=(?P<cw>\d+) (?P<mod>\S+) (?P<rate>\S+)'),
    # GROUND TRUTH for key-down: the sender's own emitted sample count for the whole
    # burst (chirp + LTS + descriptor + N data frames). Modelling it as
    # anchor + N*frame_ms understates it by ~1.2 s because the descriptor block is
    # omitted and the light-anchor branch assumes a handoff that does not happen.
    'tx_samples':  re.compile(r'TX Burst: (?P<n>\d+) frames -> (?P<samples>\d+) samples'),
    'flush':       re.compile(r'Flushing burst of (?P<n>\d+) frames'),
    'monitor_arm': re.compile(r'ToneBurstAckMonitor armed'),
    'ack_seen':    re.compile(r'ToneBurstAck monitor: detected'),
    'arq_ack':     re.compile(r'SR-ARQ: ACK seq=(?P<seq>\d+) bitmap=(?P<bm>0x[0-9A-Fa-f]+) \(base=(?P<base>\d+), in_flight=(?P<inf>\d+)\)'),
    'sack_conf':   re.compile(r'SR-ARQ: SACK seq=(?P<seq>\d+) confirmed received'),
    # requeued_bytes is the exact span put back on the air; requeued CHUNKS over-counts
    # the true waste ~3x because most re-queued chunks are genuine holes. Optional so
    # logs from before the byte-pricing patch still parse.
    'requeue':     re.compile(r'FileTransfer: Re-queued (?P<n>\d+) pending chunks after ARQ abort \(acked=(?P<acked>\d+), resume_offset=(?P<off>\d+)(?:, requeued_bytes=(?P<bytes>\d+))?\)'),
    'epoch':       re.compile(r'MOVE-EPOCH bumped to (?P<e>\d+)'),
    'obey':        re.compile(r'RX-AUTHORITY obey .*?-> (?P<rung>\S+ \S+)'),
    'commit':      re.compile(r'DESC-SWITCH commit (?P<rung>\S+ \S+)'),
    'full_chirp':  re.compile(r'Full chirp\+LTS preamble emitted'),
    'light_lts':   re.compile(r's16-warm-handoff: light LTS'),
    'retx':        re.compile(r'(?i)retransmit|resend'),
    'timeout':     re.compile(r'(?i)SR-ARQ.*timeout|RTO'),
    'drive':       re.compile(r'ALC: tx_drive (?P<a>[\d.]+) -> (?P<b>[\d.]+)'),
    'arqcfg':      re.compile(r'ARQ window=\d+.*?data=(?P<data_ms>\d+)ms'),
}

RECEIVER_PATTERNS = {
    'regrade':     re.compile(r'RX-AUTHORITY crater REGRADED to hold \(idx (?P<idx>\d+) delivered=(?P<deliv>[0-9.]+) >= break-even=(?P<be>[0-9.]+)'),

    'group':       re.compile(r'Burst (?:#(?P<ord>\d+) )?\(?group_seq=(?P<g>\d+)\)? delivered as unit: (?P<ok>\d+)/(?P<tot>\d+) logical OK \(all_ok=(?P<all>\d)\) max_iters=(?P<it>\d+) quality=(?P<q>[\d.]+)'),
    'frame':       re.compile(r'Burst logical frame (?P<i>\d+)/(?P<n>\d+): (?P<res>OK|FAIL)'),
    'ack_tx':      re.compile(r'TX ToneBurstAck: group_seq=(?P<g>\d+) type=(?P<ty>\S+) frame_mask=(?P<fm>0x[0-9A-Fa-f]+) samples=(?P<s>\d+)'),
    'ack_built':   re.compile(r'SR-ARQ: Sent TONE-BURST ack base=(?P<base>\d+) \(next=(?P<next>\d+)\) bitmap=(?P<bm>0x[0-9A-Fa-f]+)'),
    'mode':        re.compile(r'\[MODE\] OFDM (?P<rung>\S+ \S+).*?usable RX SNR=(?P<snr>[\d.]+)'),
    'crater':      re.compile(r'OFDM decode failed with 0/(?P<n>\d+) CWs'),
    'reanchor':    re.compile(r'forcing full chirp\+LTS re-anchor'),
    'progress':    re.compile(r'\[FILE\] RX (?P<got>\d+)/(?P<tot>\d+) bytes'),
    'done':        re.compile(r'\[FILE\] Received .*?\((?P<bytes>\d+) bytes, CRC ok, (?P<secs>[\d.]+)s, (?P<kbps>[\d.]+) kbps\)'),
    'alc':         re.compile(r'\[ALC-RX\] data_rms=(?P<d>[\d.]+) noise_rms=(?P<n>[\d.]+) headroom_db=(?P<h>[-\d.]+)'),
    'selectivity': re.compile(r'freq-selectivity: S_gm=(?P<sgm>[+-][\d.]+).*?class=(?P<cls>\S+)'),
    'verdict':     re.compile(r'RX-AUTHORITY verdict .*?\(idx (?P<a>\d+) -> (?P<b>\d+)\)'),
    'evm_demote':  re.compile(r'EVM-DEMOTE idx (?P<a>\d+) -> (?P<b>\d+)'),
    'backstop':    re.compile(r'ANCHORED-BURST BACKSTOP'),
    'ping':        re.compile(r'RX PING'),
    'connect':     re.compile(r'\[CONNECT\]|Now CONNECTED'),
    'recv_start':  re.compile(r'\[FILE\] Receiving (?P<name>\S+) \((?P<tot>\d+) bytes'),
    'modechange':  re.compile(r'MODE_CHANGE: OFDM (?P<rung>\S+ \S+).*?peer_snr=(?P<snr>[-\d.]+)'),
    'disconnect':  re.compile(r'DISCONNECT|Remote disconnected'),
}


def align(sender, receiver):
    """Offset to ADD to sender times to put them on the receiver clock.

    Causal anchor: the sender's first burst TX must precede the receiver's first group
    decode of it. We align first-burst -> first-group and subtract a nominal burst airtime
    so the sender's key-down STARTS before the receiver finishes decoding it.
    """
    tx = [t for t, k, _ in sender if k == 'tx_burst']
    gp = [t for t, k, _ in receiver if k == 'group']
    if not tx or not gp:
        return 0.0, 'none (missing anchors)'
    # First decode happens ~one burst airtime after the first TX starts. Use the
    # sender's OWN emitted sample count for that first burst -- a hardcoded nominal
    # (this was 9.0 s against a real 7.74 s) biases the offset by the difference and
    # dumps it straight into the rx_latency bucket, manufacturing a phantom
    # "standing decode latency" identical across runs.
    smp = [d for _, k, d in sender if k == 'tx_samples']
    if smp:
        burst_airtime = int(smp[0]['samples']) / 48000.0 + LEAD_IN_S
        how = f'measured first-burst airtime {burst_airtime:.2f}s'
    else:
        burst_airtime = 9.0
        how = f'ASSUMED {burst_airtime:.1f}s airtime (no TX-samples line; offset unreliable)'
    off = (gp[0] - burst_airtime) - tx[0]
    return off, f'first TX {tx[0]:.2f}s -> first group {gp[0]:.2f}s ({how})'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('sender')
    ap.add_argument('receiver')
    ap.add_argument('--json')
    a = ap.parse_args()

    S = parse(a.sender, SENDER_PATTERNS)
    R = parse(a.receiver, RECEIVER_PATTERNS)
    off, how = align(S, R)
    Sa = [(t + off, k, d) for t, k, d in S]   # sender on receiver clock

    print("=" * 78)
    print(f"TRANSFER FORENSICS   sender={a.sender}   receiver={a.receiver}")
    print(f"clock offset applied to sender: {off:+.2f}s   [{how}]")
    print("=" * 78)

    # ---------- outcome ----------
    done = [d for _, k, d in R if k == 'done']
    groups = [(t, d) for t, k, d in R if k == 'group']
    txs = [(t, d) for t, k, d in Sa if k == 'tx_burst']
    print("\n## OUTCOME")
    if done:
        d = done[-1]
        print(f"  delivered {d['bytes']} bytes  CRC ok  in {d['secs']}s  =>  {d['kbps']} kbps")
    else:
        prog = [d for _, k, d in R if k == 'progress']
        got = max((int(p['got']) for p in prog), default=0)
        tot = int(prog[-1]['tot']) if prog else 0
        print(f"  INCOMPLETE — best progress {got}/{tot} bytes"
              f"{f' ({100.0*got/tot:.0f}%)' if tot else ''}")
    print(f"  burst groups: sent {len(txs)}   decoded {len(groups)}")

    # ---------- per-burst cycle ----------
    print("\n## BURST CYCLES (sender key-down -> receiver decode -> ACK -> next key-down)")
    arms = [t for t, k, _ in Sa if k == 'monitor_arm']
    acktx = [(t, d) for t, k, d in R if k == 'ack_tx']
    seen = [t for t, k, _ in Sa if k == 'ack_seen']
    print(f"  {'#':>3} {'txStart':>8} {'keyDown':>8} {'decode':>8} {'ackTX':>8} {'ackSeen':>8} {'cycle':>7}  grp res")
    cycles = []
    for i, (t0, d0) in enumerate(txs):
        t1 = txs[i + 1][0] if i + 1 < len(txs) else None
        arm = next((x for x in arms if x > t0), None)
        g = next(((t, g) for t, g in groups if t > t0), None)
        atx = next(((t, x) for t, x in acktx if g and t >= g[0] - 0.5), None)
        sn = next((x for x in seen if atx and x > atx[0]), None)
        kd = (arm - t0) if arm else float('nan')
        cyc = (t1 - t0) if t1 else float('nan')
        if t1:
            cycles.append(cyc)
        res = f"{g[1]['ok']}/{g[1]['tot']}" if g else "  -"
        print(f"  {i+1:>3} {t0:8.2f} {kd:8.2f} "
              f"{(g[0]-t0) if g else float('nan'):8.2f} "
              f"{(atx[0]-t0) if atx else float('nan'):8.2f} "
              f"{(sn-t0) if sn else float('nan'):8.2f} {cyc:7.2f}  {res} "
              f"{'CRATER' if g and g[1]['ok']=='0' else ''}")

    # ---------- TRUE KEY-DOWN AIRTIME (needed by both the collision check and the budget) ----
    # Computed from LOGGED config, not from the receiver's decode timestamp: that timestamp is
    # burst-end *as seen by the receiver* and also contains propagation, host buffering and
    # decode latency. Worse, if a group is never decoded the "next decode" can be tens of
    # seconds later, which would fabricate an enormous key-down window and false collisions.
    #   airtime = anchor + N * per-frame data_ms
    # per-frame comes from the sender's own "ARQ window=... data=NNNms" line; the anchor is the
    # full chirp+LTS (~1200 ms) or the warm light-LTS (~200 ms), whichever that burst emitted.
    # Key-down comes from the sender's OWN emitted sample count, which already
    # includes chirp + LTS + the 1.410 s descriptor block + N data frames. The old
    # model (anchor + N*frame_ms with FULL/LIGHT_ANCHOR_MS = 1200/200) understated
    # every burst by ~1.2 s: it omits the descriptor entirely and believes in a
    # light-LTS handoff the descriptor path never takes (measured: 64/64 bursts
    # emitted a full chirp). Both errors are one-sided and landed in rx_latency.
    cfgs = [(t, float(d['data_ms'])) for t, k, d in Sa if k == 'arqcfg']
    samples = [(t, int(d['samples'])) for t, k, d in Sa if k == 'tx_samples']
    # Kept for the airtime-budget report AND as the #69 anchor-skip metric: the skip
    # is default-ON (ULTRA_ANCHOR_SKIP_K=2) but measured 0/64 on this rig, so the
    # full/light split is how we tell whether it ever actually fires.
    fulls = [t for t, k, _ in Sa if k == 'full_chirp']
    lights = [t for t, k, _ in Sa if k == 'light_lts']

    def frame_ms_at(t):
        prior = [v for tc, v in cfgs if tc <= t]
        return prior[-1] if prior else 1272.0

    keydowns = []
    kd_measured = 0
    for t0, d0 in txs:
        n = int(d0['n'])
        near = [(abs(ts - t0), sv) for ts, sv in samples if t0 - 0.5 <= ts <= t0 + 2.0]
        if near:
            keydowns.append(min(near)[1] / 48000.0 + LEAD_IN_S)
            kd_measured += 1
        else:
            # Fallback only: flagged in the report so a modelled number is never
            # mistaken for a measured one.
            keydowns.append((1200.0 + n * frame_ms_at(t0)) / 1000.0)

    # ---------- COLLISION CHECK ----------
    # The sender's key-down ENDS when its last sample lands at the receiver. The receiver
    # decodes a burst group as those final samples arrive, so the group-decode timestamp is a
    # good, log-derived estimate of burst END. (Do NOT use "ToneBurstAckMonitor armed" — that
    # fires at the end of the whole CYCLE, i.e. after the listening gap, so it would mark every
    # normal ACK as a collision.)
    #
    # A legitimate ACK fires immediately AFTER its own burst ends. A COLLISION is the receiver
    # transmitting while the sender is genuinely mid-burst — the BUG-DECODE-BACKLOG signature,
    # where the receiver has fallen behind and answers a burst the sender has already moved on
    # from. We require the ACK to land at least GUARD seconds before a burst's end to call it.
    GUARD = 0.5
    print("\n## HALF-DUPLEX COLLISION CHECK  (receiver ACK while sender is mid-burst)")
    windows = []          # (burst#, start, end) with end = start + COMPUTED airtime
    for i, (t0, _) in enumerate(txs):
        windows.append((i + 1, t0, t0 + keydowns[i]))
    collisions = []
    for ta, da in acktx:
        for n, t0, t_end in windows:
            if t0 < ta < t_end - GUARD:
                collisions.append((n, t0, ta, t_end, da))
                break
    if collisions:
        print(f"  !! {len(collisions)} COLLISION(S) — receiver transmitted over an in-progress burst:")
        for n, t0, ta, t_end, da in collisions:
            print(f"     burst #{n}: on air [{t0:.2f} .. {t_end:.2f}]  ACK at {ta:.2f} "
                  f"(+{ta-t0:.2f}s in, {t_end-ta:.2f}s before the burst ended, mask={da['fm']})")
    else:
        print("  clean — every ACK landed at/after its burst's end, in the listening gap")
    # How tight is the margin? If ACKs routinely fire within ~0 of burst end that is correct
    # behaviour; a NEGATIVE margin trend would mean the receiver is drifting late.
    # Report the ACK's delay AFTER its burst ended. POSITIVE = the receiver waited for the
    # sender to stop (correct half-duplex behaviour). NEGATIVE = the ACK began while the sender
    # was still transmitting, i.e. a collision. Trending toward zero/negative across a run is
    # the early-warning sign of the receiver falling behind (BUG-DECODE-BACKLOG class).
    delays = []
    for ta, _ in acktx:
        cand = [(ta - t_end) for _, t0, t_end in windows if t0 <= ta <= t_end + 6.0]
        if cand:
            delays.append(min(cand, key=abs))
    if delays:
        delays.sort()
        print(f"  ACK delay after burst end (s): min {delays[0]:+.2f}  med {delays[len(delays)//2]:+.2f}"
              f"  max {delays[-1]:+.2f}   (POSITIVE = receiver waited for TX to stop = correct)")
    print(f"  (bursts checked: {len(windows)}; receiver ACKs: {len(acktx)}; guard {GUARD}s)")

    # ---------- TURNAROUND DECOMPOSITION ----------
    # The gap between the sender's last data sample and its next key-down is the single
    # largest addressable loss (measured 31-36% of cycle). Break it into its parts so the
    # attack targets the right one instead of the whole blob.
    print("\n## TURNAROUND DECOMPOSITION  (end-of-TX -> next key-down)")
    print(f"  {'#':>3} {'txEnd':>8} {'rxDecode':>9} {'ackTX':>7} {'ackSeen':>8} {'nextTX':>7} {'gap':>6}")
    seg = {'rx_latency': [], 'ack_air': [], 'detect': [], 'rekey': [], 'gap': []}
    for i, (t0, _) in enumerate(txs):
        t_end = t0 + keydowns[i]
        t1 = txs[i + 1][0] if i + 1 < len(txs) else None
        g = next((t for t, _ in groups if t >= t_end - 2.0), None)
        atx = next((t for t, _ in acktx if g is not None and t >= g - 0.5), None)
        sn = next((x for x in seen if atx is not None and x > atx), None)
        if t1 is None:
            continue
        seg['gap'].append(t1 - t_end)
        if g is not None:
            seg['rx_latency'].append(g - t_end)          # burst end -> receiver finishes decode
        if g is not None and atx is not None:
            seg['ack_air'].append(atx - g)               # decode -> ACK on air
        if atx is not None and sn is not None:
            seg['detect'].append(sn - atx)               # ACK on air -> sender detects it
        if sn is not None:
            seg['rekey'].append(t1 - sn)                 # sender detects -> next key-down
        if i < 6:
            print(f"  {i+1:>3} {t_end:8.2f} "
                  f"{(g-t_end) if g is not None else float('nan'):9.2f} "
                  f"{(atx-g) if (g is not None and atx is not None) else float('nan'):7.2f} "
                  f"{(sn-atx) if (atx is not None and sn is not None) else float('nan'):8.2f} "
                  f"{(t1-sn) if sn is not None else float('nan'):7.2f} {t1-t_end:6.2f}")
    def med(v):
        return sorted(v)[len(v)//2] if v else float('nan')
    tot = med(seg['gap'])
    # SANITY GUARD (2026-07-25): burst-end -> RX-decode is bounded by ONE frame
    # airtime -- the receiver is arrival-gated and cannot decode a group before its
    # last sample lands, but it also cannot lag a whole frame without the load-shed
    # firing. A larger value means the key-down model is wrong (it previously
    # hardcoded a 9.0 s nominal airtime and a 1200/200 ms anchor, understating
    # key-down by ~1.2 s and manufacturing a phantom 2.5 s "standing decode
    # latency"). Fail loudly rather than report a fabricated lever.
    if seg['rx_latency'] and med(seg['rx_latency']) > 1.5:
        print(f"\n  !! WARNING: burst-end -> RX-decode median {med(seg['rx_latency']):.2f}s exceeds one frame "
              f"airtime. The key-down model is probably wrong -- do NOT treat this as a\n"
              f"     real lever until the TX-samples line is being parsed "
              f"(kd_measured={kd_measured}/{len(txs)}).")

    print(f"\n  MEDIAN turnaround budget (total {tot:.2f}s):")
    for name, label in (('rx_latency', 'burst-end -> RX decode done '),
                        ('ack_air',    'RX decode -> ACK on air     '),
                        ('detect',     'ACK on air -> sender detects'),
                        ('rekey',      'sender detects -> next TX   ')):
        v = med(seg[name])
        pct = (100.0 * v / tot) if (tot and tot == tot and v == v) else float('nan')
        print(f"     {label} {v:6.2f}s  ({pct:5.1f}% of the gap)  n={len(seg[name])}")


    print("\n## RETRANSMISSIONS — what and why")
    rq = [(t, d) for t, k, d in Sa if k == 'requeue']
    craters = [t for t, k, _ in R if k == 'crater']
    partials = [(t, d) for t, d in groups if d['all'] == '0' and d['ok'] != '0']
    zeros = [(t, d) for t, d in groups if d['ok'] == '0']
    print(f"  rate-change requeues : {len(rq)}  (chunks re-queued: {sum(int(d['n']) for _, d in rq)})")
    for t, d in rq:
        print(f"     t={t:7.2f}  re-queued {d['n']} chunks, acked={d['acked']}, resume_offset={d['off']}")
    print(f"  partial groups (SACK hole -> selective resend): {len(partials)}")
    for t, d in partials[:12]:
        print(f"     t={t:7.2f}  {d['ok']}/{d['tot']} decoded  q={d['q']}")
    print(f"  FULL craters (0/N, whole group lost)          : {len(zeros)}")
    for t, d in zeros[:12]:
        print(f"     t={t:7.2f}  0/{d['tot']}")
    print(f"  decoder-forced re-anchors                     : {len([1 for _,k,_ in R if k=='reanchor'])}")
    print(f"  anchored-burst backstops                      : {len([1 for _,k,_ in R if k=='backstop'])}")

    # per-frame-position failure profile (is it positional, i.e. stale CSI?)
    pos = defaultdict(lambda: [0, 0])
    for _, k, d in R:
        if k == 'frame':
            pos[int(d['i'])][1] += 1
            if d['res'] == 'OK':
                pos[int(d['i'])][0] += 1
    if pos:
        print("  per-frame-position decode rate (flat => NOT stale-CSI-within-burst):")
        for i in sorted(pos):
            ok, tot = pos[i]
            print(f"     frame {i}: {ok:>4}/{tot:<4} ({100.0*ok/tot:5.1f}%)")

    # ---------- AIRTIME BUDGET ----------
    # TRUE key-down is computed from LOGGED config, not from the receiver's decode timestamp:
    # that timestamp lands at burst END *as seen by the receiver*, so it also contains
    # propagation, host buffering and decode latency and would overstate duty (it read 99%).
    # Airtime = anchor + N * per-frame data_ms, where per-frame comes from the sender's own
    # "ARQ window=... data=NNNms" line and the anchor is the full chirp+LTS (~1200 ms) or the
    # warm light-LTS (~200 ms), whichever that burst emitted.
    print("\n## AIRTIME BUDGET")
    if cfgs:
        print(f"  per-frame airtime (from ARQ config): {frame_ms_at(txs[-1][0]) if txs else 0:.0f} ms"
              f"   full-chirp bursts {len(fulls)} / light-LTS {len(lights)}")
    span = (txs[-1][0] - txs[0][0]) if len(txs) > 1 else 0.0
    kd_tot = sum(keydowns)
    if cycles:
        cyc_mean = sum(cycles) / len(cycles)
        kd_mean = (kd_tot / len(keydowns)) if keydowns else 0.0
        print(f"  mean cycle        : {cyc_mean:6.2f}s")
        print(f"  mean key-down     : {kd_mean:6.2f}s  ({100.0*kd_mean/cyc_mean:.1f}% of cycle)")
        print(f"  mean turnaround   : {cyc_mean-kd_mean:6.2f}s  ({100.0*(cyc_mean-kd_mean)/cyc_mean:.1f}% of cycle)")
    if span > 0:
        duty = 100.0 * kd_tot / span
        print(f"  PA DUTY CYCLE     : {duty:5.1f}%  (key-down {kd_tot:.1f}s of {span:.1f}s span)")
        if duty > 60:
            print(f"     ^^ a real 100W final derates near ~50% duty for digital modes —"
                  f" this would need a duty governor on hardware")

    # ---------- RUNG / CHANNEL ----------
    print("\n## RUNG + CHANNEL")
    rungs = [(t, d['rung'], d['snr']) for t, k, d in R if k == 'mode']
    print(f"  rung changes: {len(rungs)}")
    for t, r, s in rungs[:16]:
        print(f"     t={t:7.2f}  {r:<14} usable SNR {s}")
    print(f"  RX-authority verdicts: {len([1 for _,k,_ in R if k=='verdict'])}"
          f"   EVM demotes: {len([1 for _,k,_ in R if k=='evm_demote'])}")
    sel = Counter(d['cls'] for _, k, d in R if k == 'selectivity')
    if sel:
        print(f"  channel class (freq-selectivity): {dict(sel)}")
    alc = [float(d['h']) for _, k, d in R if k == 'alc']
    if alc:
        alc.sort()
        print(f"  ALC headroom dB: min {alc[0]:.1f}  med {alc[len(alc)//2]:.1f}  max {alc[-1]:.1f}")

    # ---------- PHASES + PROGRESS ----------
    print("\n## PHASES + PROGRESS")
    def first(kind, src=R):
        return next((t for t, k, _ in src if k == kind), None)
    t_ping, t_conn = first('ping'), first('connect')
    t_recv = first('recv_start')
    t_first_tx = txs[0][0] if txs else None
    for label, t in (("first PING", t_ping), ("CONNECTED", t_conn),
                     ("file RX begins", t_recv), ("first burst TX", t_first_tx)):
        if t is not None:
            print(f"  {label:<18} t={t:7.2f}s")
    if t_conn is not None and t_recv is not None:
        print(f"  handshake->data gap: {t_recv-t_conn:.2f}s")
    prog = [(t, int(d['got']), int(d['tot'])) for t, k, d in R if k == 'progress']
    # Rate baseline: prefer the logged RX-start; else the sender's first burst. NEVER use the
    # first progress mark itself — that makes elapsed 0 and the rate meaningless/infinite.
    base = t_recv if t_recv is not None else t_first_tx
    if prog:
        print("  progress milestones:")
        for t, got, tot in prog:
            pct = 100.0 * got / tot
            el = (t - base) if base is not None else None
            rate = f"{got*8/el/1000.0:5.2f} kbps" if el and el > 1.0 else "    n/a"
            print(f"     t={t:7.2f}s (+{el:6.2f}s)  {got:>6}/{tot} = {pct:5.1f}%   avg so far {rate}"
                  if el is not None else
                  f"     t={t:7.2f}s  {got:>6}/{tot} = {pct:5.1f}%")
    if done:
        d = done[-1]
        base = t_recv if t_recv is not None else 0.0
        print(f"     COMPLETE  {d['bytes']} bytes in {d['secs']}s  =>  {d['kbps']} kbps")

    # ---------- RUNG DWELL: where the transfer actually spent its time ----------
    print("\n## RUNG DWELL  (time at each waveform/rate, and what drove each change)")
    # cause attribution: an EVM demote or an RX-authority verdict just before a change owns it
    evm = [t for t, k, _ in R if k == 'evm_demote']
    verd = [t for t, k, _ in R if k == 'verdict']
    changes = [(t, d['rung'], float(d['snr'])) for t, k, d in R if k == 'mode']
    # Bound the dwell window to the actual TRANSFER: from the first burst to file completion
    # (the link often stays up afterwards, and post-transfer rung changes are not transfer time).
    t_done = next((t for t, k, _ in R if k == 'done'), None)
    t_end = t_done if t_done is not None else (groups[-1][0] if groups else None)
    t_begin = t_first_tx if t_first_tx is not None else (changes[0][0] if changes else None)
    dwell = defaultdict(float)
    if changes:
        for i, (t, rung, snr) in enumerate(changes):
            if t_end is not None and t > t_end:
                continue                      # post-transfer churn: not transfer time
            nxt = changes[i + 1][0] if i + 1 < len(changes) else t_end
            if t_end is not None and nxt is not None:
                nxt = min(nxt, t_end)
            if nxt:
                dwell[rung] += max(0.0, nxt - t)
            cause = ""
            if any(abs(t - x) < 4.0 for x in evm):
                cause = "EVM-DEMOTE"
            elif any(abs(t - x) < 4.0 for x in verd):
                cause = "RX-authority verdict"
            dur = (nxt - t) if nxt else float('nan')
            tag = "" if (t_end is None or t <= t_end) else "  [after transfer]"
            print(f"  t={t:7.2f}s  -> {rung:<14} usable SNR {snr:5.1f}   held {dur:6.2f}s   {cause}{tag}")
        total = sum(dwell.values()) or 1.0
        print("\n  dwell summary:")
        for rung, secs in sorted(dwell.items(), key=lambda kv: -kv[1]):
            print(f"     {rung:<14} {secs:7.2f}s  ({100.0*secs/total:5.1f}% of the transfer)")
        in_xfer = [c for c in changes if t_end is None or c[0] <= t_end]
        if t_end is not None and t_begin is not None and in_xfer:
            print(f"  rung changes DURING the transfer: {len(in_xfer)} in {t_end-t_begin:.0f}s "
                  f"(~1 per {(t_end-t_begin)/max(len(in_xfer),1):.1f}s)")

    # ---------- CHANNEL CLASS + DRIVE ----------
    ep = [(t, d['e']) for t, k, d in Sa if k == 'epoch']
    dr = [(t, d['a'], d['b']) for t, k, d in Sa if k == 'drive']
    if ep or dr:
        print("\n## ARQ EPOCHS + TX DRIVE")
        for t, e in ep:
            print(f"  t={t:7.2f}s  MOVE-EPOCH -> {e}   (seq space regridded; in-flight frames abandoned)")
        if dr:
            import math as _m
            a0, b1 = float(dr[0][1]), float(dr[-1][2])
            db = 20.0 * _m.log10(b1 / a0) if a0 > 0 and b1 > 0 else float('nan')
            print(f"  tx_drive: {a0:.3f} -> {b1:.3f} over {len(dr)} steps ({db:+.2f} dB)")

    # ---------- EFFICIENCY ----------
    if done and cycles:
        d = done[-1]
        kbps = float(d['kbps'])
        payload_bits = int(d['bytes']) * 8
        onair = kd_tot if kd_tot else 1.0
        onair_kbps = payload_bits / onair / 1000.0
        print("\n## EFFICIENCY")
        print(f"  delivered              : {kbps:.2f} kbps over {d['secs']}s wall clock")
        print(f"  on-air payload rate    : {onair_kbps:.2f} kbps  (payload / key-down time)")
        print(f"  scheduling efficiency  : {100.0*kbps/onair_kbps:.1f}%  "
              f"(what fraction of the on-air rate survives turnaround+retx)")
        print(f"  => headroom from scheduling alone: {onair_kbps-kbps:.2f} kbps "
              f"({100.0*(onair_kbps-kbps)/kbps:.0f}% over today) if turnaround/retx went to zero")

    if a.json:
        with open(a.json, 'w') as fh:
            json.dump({
                'sender': a.sender, 'receiver': a.receiver, 'offset': off,
                'groups_sent': len(txs), 'groups_decoded': len(groups),
                'collisions': len(collisions),
                'requeues': len(rq), 'requeued_chunks': sum(int(x['n']) for _, x in rq),
                'requeued_bytes': sum(int(x['bytes']) for _, x in rq if x.get('bytes')),
                'crater_regrades': len([1 for _, k, _ in R if k == 'regrade']),
                'partial_groups': len(partials), 'full_craters': len(zeros),
                'mean_cycle_s': (sum(cycles)/len(cycles)) if cycles else None,
                'duty_pct': (100.0*kd_tot/span) if span > 0 else None,
                'kbps': float(done[-1]['kbps']) if done else 0.0,
            }, fh, indent=1)
        print(f"\n(json -> {a.json})")


if __name__ == '__main__':
    main()
