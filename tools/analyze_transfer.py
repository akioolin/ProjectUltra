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
are offset. We select a multi-burst consensus of measured sender burst ends and receiver
pre-decode group-air-end markers, then report its support and residual so the offset can be
sanity-checked. Legacy logs without that marker fall back to post-decode group callbacks.
"""

import argparse
import json
import re
import statistics
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
# logged by 'TX Burst' is the modulated burst only.  The receiver finishes decoding at
# the end of lead-in+signal; the PA remains keyed for the additional silent tail.  Keep
# those two intervals distinct: using the tail in clock alignment biases every receive
# timestamp, while omitting it from PA duty under-counts key-down time.
LEAD_IN_S = 0.150
TAIL_S = 0.050
TX_GUARD_SAMPLES = round((LEAD_IN_S + TAIL_S) * 48000.0)


def ack_render_matches_commit(render_samples, commit_samples):
    """Match the modem's raw ACK render to the App's keyed waveform.

    ``TX ToneBurstAck`` is logged before ``postProcessTx()``.  The App-level
    ``TX-AUDIO-COMMIT`` count is the vector accepted by the audio queue after
    the default lead-in and tail have been added.  Comparing those counts for
    equality makes every real commit look orphaned (19,584 -> 29,184 samples on
    the 12 ms ACK).  This analyzer models the same wrapper geometry it already
    uses for DATA key-down accounting.
    """
    try:
        return int(commit_samples) == int(render_samples) + TX_GUARD_SAMPLES
    except (TypeError, ValueError):
        return False


def ack_keydown_seconds(data):
    """Physical ACK key-down duration, without charging TX guards twice."""
    if '_keyed_samples' in data:
        return int(data['_keyed_samples']) / 48000.0
    # Legacy render-only logs contain the raw modem waveform, not the wrapper.
    return int(data.get('s') or 0) / 48000.0 + LEAD_IN_S + TAIL_S


def drive_transition_summary(events):
    """Summarize in-session ALC movement without treating resets as steering.

    ``events`` contains ``(time, groupdict)`` entries parsed from ``ALC:
    tx_drive`` lines.  Current logs name lifecycle transitions as ``reset: ...``;
    historical logs have no reason and remain classified as operational moves.
    """
    if not events:
        return None

    def is_reset(data):
        return (data.get('reason') or '').strip().lower().startswith('reset:')

    operational = [(t, d) for t, d in events if not is_reset(d)]
    resets = [(t, d) for t, d in events if is_reset(d)]
    first_data = operational[0][1] if operational else events[0][1]
    start = float(first_data['a'])
    values = [start]
    values.extend(float(d['b']) for _, d in operational)
    final = values[-1]
    minimum = min(values)
    peak = max(values)

    import math
    def db_ratio(value):
        if start <= 0.0 or value <= 0.0:
            return None
        return 20.0 * math.log10(value / start)

    return {
        'start': start,
        'minimum': minimum,
        'peak': peak,
        'final_before_reset': final,
        'advisory_moves': len(operational),
        'lifecycle_resets': len(resets),
        'peak_db_from_start': db_ratio(peak),
        'final_db_from_start': db_ratio(final),
    }


def transfer_time_bases(t_done, app_duration_s, first_keydown_t,
                        file_start_t=None):
    """Return the two deliberately distinct transfer clocks.

    The application's receive timer starts when FILE_START is delivered.  A
    delivery-as-unit burst can already have occupied the channel before that
    callback, so this timer is the right basis for the application's reported
    goodput but the wrong denominator for PA duty or keyed-to-wall accounting.
    Physical accounting starts at the sender's first DATA key-down instead.
    """
    app_origin = file_start_t
    if app_origin is not None:
        app_origin_evidence = 'FILE_START log marker'
    elif t_done is not None and app_duration_s is not None:
        app_origin = t_done - app_duration_s
        app_origin_evidence = 'inferred from completion duration'
    else:
        app_origin_evidence = 'unavailable'

    physical_span_s = None
    if t_done is not None and first_keydown_t is not None:
        physical_span_s = max(0.0, t_done - first_keydown_t)

    return {
        'app_origin_t': app_origin,
        'app_origin_evidence': app_origin_evidence,
        'physical_span_s': physical_span_s,
        'pre_app_keydown_s': (
            app_origin - first_keydown_t
            if app_origin is not None and first_keydown_t is not None else None),
    }


def keydown_overlap_seconds(txs, keydowns, start_t, end_t):
    """Sum key-down occupancy inside a reporting interval."""
    if start_t is None or end_t is None or end_t <= start_t:
        return 0.0
    return sum(
        max(0.0, min(t0 + duration, end_t) - max(t0, start_t))
        for (t0, _), duration in zip(txs, keydowns))


def rung_transfer_timeline(changes, start_t, end_t):
    """Build state dwell segments and true transitions in a bounded interval.

    A ``[MODE]`` line is a state announcement.  The first announcement supplies
    the state already active at ``start_t``; it is not itself a transition during
    the transfer.  Duplicate announcements of the same rung are not transitions.

    Returns ``(segments, transitions)``.  Segment tuples are
    ``(start, end, rung, snr, announcement_time)`` and transition tuples retain
    the original ``(time, rung, snr)`` shape.
    """
    if start_t is None or end_t is None or end_t <= start_t:
        return [], []

    # The reporting interval is half-open [start_t, end_t): an announcement at
    # completion owns no transfer dwell and is not an in-transfer transition.
    bounded = sorted(c for c in changes if c[0] < end_t)
    prior = [c for c in bounded if c[0] <= start_t]
    segments = []
    transitions = []
    if prior:
        current = prior[-1]
        cursor = start_t
        future = [c for c in bounded if c[0] > start_t]
    else:
        future = [c for c in bounded if c[0] >= start_t]
        if not future:
            return [(start_t, end_t, 'UNKNOWN', float('nan'), None)], []
        current = future.pop(0)
        if current[0] > start_t:
            segments.append(
                (start_t, current[0], 'UNKNOWN', float('nan'), None))
        cursor = current[0]

    current_announcement_t, current_rung, current_snr = current
    for event_t, rung, snr in future:
        if event_t < cursor:
            continue
        if rung == current_rung:
            # This is another observation of the same state, not a transition.
            # Keep one continuous dwell segment and the SNR attached to the state
            # that began it.
            continue
        if event_t > cursor:
            segments.append((cursor, event_t, current_rung, current_snr,
                             current_announcement_t))
        transitions.append((event_t, rung, snr))
        cursor = event_t
        current_announcement_t, current_rung, current_snr = event_t, rung, snr

    if end_t > cursor:
        segments.append((cursor, end_t, current_rung, current_snr,
                         current_announcement_t))
    return segments, transitions

SENDER_PATTERNS = {
    'tx_burst':    re.compile(r'TX Burst descriptor: group=(?P<n>\d+) cw/frame=(?P<cw>\d+) (?P<mod>\S+) (?P<rate>\S+)'),
    # GROUND TRUTH for key-down: the sender's own emitted sample count for the whole
    # burst (chirp + LTS + descriptor + N data frames). Modelling it as
    # anchor + N*frame_ms understates it by ~1.2 s because the descriptor block is
    # omitted and the light-anchor branch assumes a handoff that does not happen.
    'tx_samples':  re.compile(r'TX Burst: (?P<n>\d+) frames -> (?P<samples>\d+) samples'),
    'flush':       re.compile(r'Flushing burst of (?P<n>\d+) frames'),
    'monitor_arm': re.compile(r'ToneBurstAckMonitor armed'),
    # Detailed acceptance telemetry (current logs). Keep the broad fallback
    # immediately after it so older logs still contribute turnaround timing.
    'ack_seen_detail': re.compile(
        r'ToneBurstAck monitor: detected group_seq=(?P<g>\d+).*?'
        r'normalized_peak=(?P<peak>[\d.]+).*?min_conf=(?P<conf>[\d.]+).*?'
        r'hamming_corrected=(?P<hamming>\d+).*?symbol_ms=(?P<symbol_ms>\d+)'),
    'ack_seen':    re.compile(r'ToneBurstAck monitor: detected'),
    'ack_semantic_reject': re.compile(
        r'ToneBurstAckMonitor: CRC-valid candidate rejected before commit '
        r'group_seq=(?P<g>\d+).*?normalized_peak=(?P<peak>[\d.]+).*?'
        r'min_conf=(?P<conf>[\d.]+).*?hamming_corrected=(?P<hamming>\d+).*?'
        r'symbol_ms=(?P<symbol_ms>\d+).*?rejected_in_search=(?P<count>\d+)'),
    'audio_arrived': re.compile(
        r'AudioActivity #\d+: ARRIVED rms=(?P<rms>[\d.]+)'),
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
    # Keep the structured form before the broad historical fallback: parse()
    # records only the first matching kind for a line.  The cause is the exact
    # distinction the hardware campaign needs (prompt SACK repair versus RTO).
    'retx_detail': re.compile(
        r'SR-ARQ: Retransmitting seq=(?P<seq>\d+).*?cause=(?P<cause>[A-Za-z0-9_-]+)'),
    'retx':        re.compile(r'(?i)retransmit|resend'),
    'timeout':     re.compile(r'(?i)SR-ARQ.*timeout|RTO'),
    'drive':       re.compile(
        r'ALC: tx_drive (?P<a>[\d.]+) -> (?P<b>[\d.]+)'
        r'(?: \((?P<reason>[^)]*)\))?'),
    'arqcfg':      re.compile(r'ARQ window=\d+.*?data=(?P<data_ms>\d+)ms'),
}

RECEIVER_PATTERNS = {
    'regrade':     re.compile(r'RX-AUTHORITY crater REGRADED to hold \(idx (?P<idx>\d+) delivered=(?P<deliv>[0-9.]+) >= break-even=(?P<be>[0-9.]+)'),

    # This marker is emitted after the complete physical group has been buffered
    # but before deinterleaving and LDPC work. It is therefore the receiver-side
    # air-end anchor. The later `group` callback can move by hundreds of
    # milliseconds when BI1 defers a hard decode to group end; using that callback
    # for cross-host clock alignment mistakes decoder CPU time for radio timing.
    'group_physical_end': re.compile(
        r'Burst group complete \((?P<tot>\d+) frames\), deinterleaving'),
    'group':       re.compile(r'Burst (?:#(?P<ord>\d+) )?\(?group_seq=(?P<g>\d+)\)? delivered as unit: (?P<ok>\d+)/(?P<tot>\d+) logical OK \(all_ok=(?P<all>\d)\) max_iters=(?P<it>\d+) quality=(?P<q>[\d.]+)'),
    'frame':       re.compile(r'Burst logical frame (?P<i>\d+)/(?P<n>\d+): (?P<res>OK|FAIL)'),
    'ack_tx':      re.compile(r'TX ToneBurstAck: group_seq=(?P<g>\d+) type=(?P<ty>\S+) frame_mask=(?P<fm>0x[0-9A-Fa-f]+) samples=(?P<s>\d+)'),
    # `TX ToneBurstAck` is a RENDER record: listen-before-ACK may defer, replace,
    # or drop that audio.  This App-level marker is emitted only after the audio
    # queue accepts the samples, so it is authoritative committed-transmission
    # evidence in new logs. It precedes the actual DAC/on-air boundary.
    # `source` separates a normal ACK from a cached
    # silent-gated repeat (which has no second render record).
    'ack_commit':  re.compile(r'TX-AUDIO-COMMIT: tone-ack source=(?P<src>[A-Za-z0-9_-]+) samples=(?P<s>\d+)'),
    # The transition is the App/software TX queue becoming empty. It is NOT a
    # DAC/on-air completion marker because SDL/device buffering remains below it.
    'tx_queue_idle': re.compile(r'SEND_BTN .*?tx_inprog=0'),
    # App-level cached copies bypass transmitToneBurstAck(), so they do not emit
    # another `TX ToneBurstAck` line.  The firing line is immediately followed by
    # queueRealTxSamples() and represents a second committed transmission. Omitting it
    # hid the worst collision mechanism in real IONOS logs.
    'ack_repeat_tx': re.compile(r'ACK-REPEAT-SILENT: firing'),
    'ack_built':   re.compile(r'SR-ARQ: Sent TONE-BURST ack base=(?P<base>\d+) \(next=(?P<next>\d+)\) bitmap=(?P<bm>0x[0-9A-Fa-f]+)'),
    'sack_timer':  re.compile(r'SR-ARQ: SACK timer expired, sending SACK'),
    'data_rx':     re.compile(r'RX << DATA seq=(?P<seq>\d+)'),
    'mode':        re.compile(r'\[MODE\] OFDM (?P<rung>\S+ \S+).*?usable RX SNR=(?P<snr>[\d.]+)'),
    # Keep this before `crater`: the same line contains both phrases and parse()
    # records the first match.  Group callbacks already provide the authoritative
    # 0/N count; this event exists to count the forced re-anchor action.
    'reanchor':    re.compile(r'forcing full chirp\+LTS re-anchor'),
    'crater':      re.compile(r'OFDM decode failed with 0/(?P<n>\d+) CWs'),
    'progress':    re.compile(r'\[FILE\] RX (?P<got>\d+)/(?P<tot>\d+) bytes'),
    'done':        re.compile(r'\[FILE\] Received .*?\((?P<bytes>\d+) bytes, CRC ok, (?P<secs>[\d.]+)s, (?P<kbps>[\d.]+) kbps\)'),
    'alc':         re.compile(r'\[ALC-RX\] data_rms=(?P<d>[\d.]+) noise_rms=(?P<n>[\d.]+)(?: \(latest [\d.]+\))? headroom_db=(?P<h>[-\d.]+)'),
    'selectivity': re.compile(r'freq-selectivity: S_gm=(?P<sgm>[+-][\d.]+).*?class=(?P<cls>\S+)'),
    'verdict':     re.compile(r'RX-AUTHORITY verdict .*?\(idx (?P<a>\d+) -> (?P<b>\d+)\)'),
    'latent':      re.compile(r'LATENT-RATE idx (?P<a>\d+) -> (?P<b>\d+)'),
    'evm_demote':  re.compile(r'EVM-DEMOTE idx (?P<a>\d+) -> (?P<b>\d+)'),
    'backstop':    re.compile(r'ANCHORED-BURST BACKSTOP'),
    'ping':        re.compile(r'RX PING'),
    'connect':     re.compile(r'\[CONNECT\]|Now CONNECTED'),
    'recv_start':  re.compile(r'\[FILE\] Receiving (?P<name>\S+) \((?P<tot>\d+) bytes'),
    'modechange':  re.compile(r'MODE_CHANGE: OFDM (?P<rung>\S+ \S+).*?peer_snr=(?P<snr>[-\d.]+)'),
    'disconnect':  re.compile(r'DISCONNECT|Remote disconnected'),
}


def associate_groups_to_bursts(txs, signal_airtimes, groups,
                               early_tolerance_s=0.5, late_tolerance_s=1.5):
    """One-to-one physical association of RX group callbacks to sender bursts.

    A group callback normally lands within a few tens of milliseconds of the final
    signal sample.  It may be delayed by up to one frame while the marker/backstop
    completes, but it cannot belong to every earlier burst that happened to lack a
    callback.  The old `next(group after txStart)` lookup did exactly that and reused
    one callback two or three times after a dropped burst.

    Return ({tx_index: group_tuple}, unmatched_group_tuples).  Matching is deliberately
    conservative: anything outside the one-frame late bound remains visible as
    unmatched evidence rather than being silently attributed to the wrong burst.
    """
    candidates = []
    for gi, (tg, _) in enumerate(groups):
        for ti, ((t0, _), air_s) in enumerate(zip(txs, signal_airtimes)):
            delta = tg - (t0 + air_s)
            if -early_tolerance_s <= delta <= late_tolerance_s:
                candidates.append((abs(delta), gi, ti))

    by_tx = {}
    used_groups = set()
    used_txs = set()
    for _, gi, ti in sorted(candidates):
        if gi in used_groups or ti in used_txs:
            continue
        by_tx[ti] = groups[gi]
        used_groups.add(gi)
        used_txs.add(ti)
    return by_tx, [g for gi, g in enumerate(groups) if gi not in used_groups]


def classify_ack_origins(receiver_events, acktx, group_window_s=0.050,
                         cause_window_s=0.050):
    """Classify each physical ACK from the event which caused its render.

    A deferred ACK can be committed seconds after its group/DATA/timer event.  New
    commit-backed records therefore carry `_render_t`; using the queue-commit
    timestamp for provenance would incorrectly turn every deferred group ACK into
    `other`.  Collision timing still uses the tuple timestamp (`ta`).
    """
    group_times = [t for t, k, _ in receiver_events if k == 'group']
    timer_times = [t for t, k, _ in receiver_events if k == 'sack_timer']
    data_times = [t for t, k, _ in receiver_events if k == 'data_rx']
    origins = []
    for ta, data in acktx:
        cause_t = data.get('_render_t', ta)
        if data.get('_origin'):
            origin = data['_origin']
        elif any(abs(cause_t - t) <= group_window_s for t in group_times):
            origin = 'group'
        elif any(0.0 <= cause_t - t <= cause_window_s for t in timer_times):
            origin = 'timer_sack'
        elif any(0.0 <= cause_t - t <= cause_window_s for t in data_times):
            origin = 'standalone_data'
        else:
            origin = 'other'
        origins.append(origin)
    return origins


def reconstruct_ack_transmissions(receiver_events):
    """Return committed receiver ACK transmissions, preferring queue evidence.

    `TX ToneBurstAck` is emitted by ModemEngine when samples are rendered.  The
    App can subsequently defer, supersede, or drop those samples, so it is not
    proof of transmission.  If this log contains any `ack_commit` event, count
    *only* commit events and attach metadata from the newest matching prior
    render.  This makes collision and reverse-airtime counts physically exact.

    Old logs have no commit marker.  For them, preserve the historical render +
    `ACK-REPEAT-SILENT: firing` reconstruction and tag the evidence as legacy so
    reports cannot mistake the estimate for queue-level truth.
    """
    rendered = [(t, dict(d)) for t, k, d in receiver_events if k == 'ack_tx']
    commits_present = any(k == 'ack_commit' for _, k, _ in receiver_events)

    if commits_present:
        # Iterate in log order, not timestamp order. Multiple lines commonly share
        # one millisecond timestamp; line order still proves that rendering preceded
        # queue acceptance.
        render_history = []
        used_primary_renders = set()
        out = []
        for t, kind, event_data in receiver_events:
            if kind == 'ack_tx':
                render_history.append((t, dict(event_data)))
                continue
            if kind != 'ack_commit':
                continue

            source = event_data.get('src') or 'primary'
            sample_count = event_data.get('s') or '0'
            is_repeat = source in ('silent-repeat', 'silent_repeat')

            # Primary commits consume one render. A cached repeat deliberately
            # reuses an already-consumed render, so it only looks backward.
            candidates = [
                (i, tr, d) for i, (tr, d) in enumerate(render_history)
                if ack_render_matches_commit(d.get('s'), sample_count) and
                (is_repeat or i not in used_primary_renders)
            ]
            if not candidates:
                # A truncated log may have lost the matching render. Keep the
                # queue commit (timestamp and sample count remain authoritative)
                # with explicit unknown payload metadata.
                data = {'g': '?', 'ty': 'ACK', 'fm': 'unknown', 's': sample_count}
                data['_render_matched'] = False
            else:
                render_idx, render_t, template = candidates[-1]
                data = dict(template)
                data['_render_t'] = render_t
                data['_render_idx'] = render_idx
                data['_render_matched'] = True
                if not is_repeat:
                    used_primary_renders.add(render_idx)

            data['_evidence'] = 'audio_commit'
            data['_commit_source'] = source
            # Commit samples are already postProcessTx output: lead-in + ACK +
            # tail.  Preserve that exact physical count independently of the
            # matched render's raw `s` metadata.
            data['_keyed_samples'] = int(sample_count)
            if is_repeat:
                data['_origin'] = 'silent_repeat'
            out.append((t, data))
        return out

    # Backward-compatible fallback for pre-marker logs.
    repeats = [t for t, k, _ in receiver_events if k == 'ack_repeat_tx']
    out = []
    for i, (tr, template) in enumerate(rendered):
        data = dict(template)
        data['_render_t'] = tr
        data['_render_idx'] = i
        data['_render_matched'] = True
        data['_evidence'] = 'legacy_render_proxy'
        out.append((tr, data))
    for tr in repeats:
        prior = [(t, d) for t, d in rendered if t <= tr]
        if prior:
            _, template = prior[-1]
            data = dict(template)
        else:
            # Defensive fallback for a truncated log whose arm/render line was
            # lost.  The timestamp still matters for collision detection.
            data = {'g': '?', 'ty': 'ACK', 'fm': 'unknown', 's': '0'}
        data['_origin'] = 'silent_repeat'
        data['_evidence'] = 'legacy_repeat_proxy'
        out.append((tr, data))
    return sorted(out, key=lambda item: item[0])


def align(sender, receiver):
    """Offset to ADD to sender times to put them on the receiver clock.

    Build every plausible burst-end -> pre-decode group-air-end offset, then choose the offset
    supported by the largest one-to-one consensus across the complete transfer.  A
    first-TX -> first-group anchor is unsafe: the first physical burst can be lost before
    producing any group callback, shifting every later association by one burst.

    New logs provide a `Burst group complete` marker before deinterleaving and LDPC.
    Prefer it because post-decode callbacks include data-dependent decoder latency. Fall
    back to group callbacks only for legacy logs and name that provenance explicitly.

    The descriptor's group size and the callback's logical total are independent evidence
    for the same physical geometry, so exact geometry matches lead the candidate score.
    Timing-match count, residual scatter, and finally the small process-start-skew prior
    break remaining ties.  The latter is only a tie-breaker; it cannot beat a candidate
    supported by more physical observations.
    """
    txs = [(t, d) for t, k, d in sender if k == 'tx_burst']
    groups = [(t, d) for t, k, d in receiver if k == 'group']
    physical_ends = [
        (t, d) for t, k, d in receiver if k == 'group_physical_end'
    ]
    anchors = physical_ends if physical_ends else groups
    anchor_kind = 'physical-end' if physical_ends else 'group-callback'
    if not txs or not anchors:
        return 0.0, 'none (missing anchors)'

    # Pair each descriptor with its nearby emitted-sample record.  Keep the legacy
    # nominal fallback for old logs, but expose its use in the alignment provenance.
    samples = [(t, int(d['samples'])) for t, k, d in sender if k == 'tx_samples']
    signal_airtimes = []
    measured_airtimes = 0
    for t0, _ in txs:
        near = [(abs(ts - t0), sample_count) for ts, sample_count in samples
                if t0 - 0.5 <= ts <= t0 + 2.0]
        if near:
            signal_airtimes.append(min(near)[1] / 48000.0 + LEAD_IN_S)
            measured_airtimes += 1
        else:
            signal_airtimes.append(9.0)

    # Every physical pair proposes an offset.  For each proposal, perform the same
    # conservative one-to-one association used by the report, refine the proposal to
    # the median support offset, and score the resulting consensus.
    proposals = {
        round(tg - (tt + air_s), 6)
        for tt, air_s in zip((t for t, _ in txs), signal_airtimes)
        for tg, _ in anchors
    }
    best = None
    for proposed_off in proposals:
        refined_off = proposed_off
        by_tx = {}
        unmatched = anchors
        for _ in range(2):
            shifted = [(t + refined_off, d) for t, d in txs]
            by_tx, unmatched = associate_groups_to_bursts(
                shifted, signal_airtimes, anchors)
            if not by_tx:
                break
            support_offsets = [
                group[0] - (txs[ti][0] + signal_airtimes[ti])
                for ti, group in by_tx.items()
            ]
            refined_off = statistics.median(support_offsets)
        if not by_tx:
            continue

        shifted = [(t + refined_off, d) for t, d in txs]
        by_tx, unmatched = associate_groups_to_bursts(
            shifted, signal_airtimes, anchors)
        residuals = [
            group[0] - (shifted[ti][0] + signal_airtimes[ti])
            for ti, group in by_tx.items()
        ]
        geometry_matches = 0
        geometry_mismatches = 0
        for ti, (_, group_data) in by_tx.items():
            tx_total = txs[ti][1].get('n')
            rx_total = group_data.get('tot')
            if tx_total is None or rx_total is None:
                continue
            if int(tx_total) == int(rx_total):
                geometry_matches += 1
            else:
                geometry_mismatches += 1

        # Geometry is a hard physical signature when present.  The final absolute-
        # offset term only resolves genuinely indistinguishable periodic traces.
        score = (
            geometry_matches,
            len(by_tx),
            -geometry_mismatches,
            -sum(abs(r) for r in residuals),
            -abs(refined_off),
        )
        candidate = (score, refined_off, by_tx, unmatched, residuals)
        if best is None or candidate[0] > best[0]:
            best = candidate

    if best is None:
        return 0.0, 'none (no causal burst/group consensus)'

    _, off, by_tx, unmatched, residuals = best
    first_matched_tx = min(by_tx) + 1
    max_residual = max((abs(r) for r in residuals), default=0.0)
    airtime_note = (
        f'measured airtime {measured_airtimes}/{len(txs)} bursts'
        if measured_airtimes
        else 'ASSUMED 9.0s airtime (no TX-samples lines; offset unreliable)'
    )
    return off, (
        f'multi-burst consensus {len(by_tx)}/{len(txs)} TX, '
        f'{len(anchors) - len(unmatched)}/{len(anchors)} groups; '
        f'first matched TX #{first_matched_tx}; max residual {max_residual:.3f}s; '
        f'{airtime_note}; anchor={anchor_kind}'
    )


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('sender')
    ap.add_argument('receiver')
    ap.add_argument('--json')
    a = ap.parse_args()

    S = parse(a.sender, SENDER_PATTERNS)
    R = parse(a.receiver, RECEIVER_PATTERNS)
    off, how = align(S, R)
    # align() deliberately retains its stable two-value public API for the unit
    # tests and downstream imports.  Export the human-readable consensus as
    # structured JSON as well, so an automated hardware runner can reject a
    # finite-but-poor cross-host alignment instead of silently scoring it.
    alignment_match = re.fullmatch(
        r'multi-burst consensus (?P<tx_support>\d+)/(?P<tx_total>\d+) TX, '
        r'(?P<group_support>\d+)/(?P<group_total>\d+) groups; '
        r'first matched TX #(?P<first_tx>\d+); max residual '
        r'(?P<max_residual>[0-9.]+)s; '
        r'(?:(?:measured airtime (?P<airtime_measured>\d+)/'
        r'(?P<airtime_total>\d+) bursts)|(?:ASSUMED 9\.0s airtime .*?))'
        r'(?:; anchor=(?P<anchor_kind>physical-end|group-callback))?',
        how)
    alignment_meta = {
        'clock_alignment_provenance': how,
        'clock_alignment_tx_support': None,
        'clock_alignment_tx_total': None,
        'clock_alignment_group_support': None,
        'clock_alignment_group_total': None,
        'clock_alignment_first_matched_tx': None,
        'clock_alignment_max_residual_s': None,
        'clock_alignment_measured_airtimes': 0,
        'clock_alignment_airtime_total': None,
        'clock_alignment_anchor_kind': None,
    }
    if alignment_match:
        values = alignment_match.groupdict()
        alignment_meta.update({
            'clock_alignment_tx_support': int(values['tx_support']),
            'clock_alignment_tx_total': int(values['tx_total']),
            'clock_alignment_group_support': int(values['group_support']),
            'clock_alignment_group_total': int(values['group_total']),
            'clock_alignment_first_matched_tx': int(values['first_tx']),
            'clock_alignment_max_residual_s': float(values['max_residual']),
            'clock_alignment_measured_airtimes': int(
                values['airtime_measured'] or 0),
            'clock_alignment_airtime_total': int(
                values['airtime_total'] or values['tx_total']),
            'clock_alignment_anchor_kind': (
                values['anchor_kind'] or 'group-callback'),
        })
    Sa = [(t + off, k, d) for t, k, d in S]   # sender on receiver clock

    print("=" * 78)
    print(f"TRANSFER FORENSICS   sender={a.sender}   receiver={a.receiver}")
    print(f"clock offset applied to sender: {off:+.2f}s   [{how}]")
    print("=" * 78)

    # ---------- outcome / transfer boundary ----------
    # A connected modem continues logging after the file callback.  Treat the final
    # CRC-ok callback as the transfer boundary, while reporting (not deleting) any
    # decoder/ACK activity after it.  The old all-log counts turned a post-completion
    # phantom 0/3 group into a transfer crater in baseline_02.
    done_events = [(t, d) for t, k, d in R if k == 'done']
    done = [d for _, d in done_events]
    t_done = done_events[-1][0] if done_events else None
    groups_all = [(t, d) for t, k, d in R if k == 'group']
    groups = [(t, d) for t, d in groups_all if t_done is None or t <= t_done]
    post_groups = [(t, d) for t, d in groups_all if t_done is not None and t > t_done]
    txs_all = [(t, d) for t, k, d in Sa if k == 'tx_burst']
    txs = [(t, d) for t, d in txs_all if t_done is None or t <= t_done]
    post_txs = [(t, d) for t, d in txs_all if t_done is not None and t > t_done]
    t_first_tx = txs[0][0] if txs else None

    # ---------- measured sender signal/key-down airtime ----------
    # Compute this before cycle correlation: physical burst ends are the only safe
    # association key when one burst has no group callback.
    cfgs = [(t, float(d['data_ms'])) for t, k, d in Sa
            if k == 'arqcfg' and (t_done is None or t <= t_done)]
    samples = [(t, int(d['samples'])) for t, k, d in Sa
               if k == 'tx_samples' and (t_done is None or t <= t_done)]
    fulls = [t for t, k, _ in Sa
             if k == 'full_chirp' and (t_done is None or t <= t_done)]
    lights = [t for t, k, _ in Sa
              if k == 'light_lts' and (t_done is None or t <= t_done)]

    def frame_ms_at(t):
        prior = [v for tc, v in cfgs if tc <= t]
        return prior[-1] if prior else 1272.0

    signal_airtimes = []       # lead-in + emitted signal: predicts RX group completion
    keydowns = []              # signal airtime + silent tail: PA duty
    kd_measured = 0
    for t0, d0 in txs:
        n = int(d0['n'])
        near = [(abs(ts - t0), sv) for ts, sv in samples if t0 - 0.5 <= ts <= t0 + 2.0]
        if near:
            signal_s = min(near)[1] / 48000.0 + LEAD_IN_S
            kd_measured += 1
        else:
            # Fallback only: flagged in the report so a modelled number is never
            # mistaken for a measured one.
            signal_s = (1200.0 + n * frame_ms_at(t0)) / 1000.0
        signal_airtimes.append(signal_s)
        keydowns.append(signal_s + TAIL_S)

    groups_by_tx, unmatched_groups = associate_groups_to_bursts(
        txs, signal_airtimes, groups)

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
    print(f"  burst groups: sent {len(txs)}   decoded {len(groups)}"
          f"   physically matched {len(groups_by_tx)}")
    if unmatched_groups:
        print(f"  !! {len(unmatched_groups)} in-transfer group callback(s) could not be "
              "matched to a physical burst within [-0.5,+1.5]s of signal end")
    if post_groups or post_txs:
        print(f"  post-completion artifacts excluded: sender bursts {len(post_txs)}, "
              f"receiver groups {len(post_groups)}")

    # ---------- per-burst cycle ----------
    print("\n## BURST CYCLES (sender key-down -> receiver decode -> ACK -> next key-down)")
    acktx_all = reconstruct_ack_transmissions(R)
    ack_commits_present = any(k == 'ack_commit' for _, k, _ in R)
    ack_render_count = sum(1 for _, k, _ in R if k == 'ack_tx')
    committed_render_ids = {
        d['_render_idx'] for _, d in acktx_all
        if d.get('_commit_source', 'primary') == 'primary' and
        d.get('_render_matched') and '_render_idx' in d
    }
    uncommitted_ack_renders = (
        ack_render_count - len(committed_render_ids)
        if ack_commits_present else None
    )
    acktx = [(t, d) for t, d in acktx_all if t_done is None or t <= t_done]
    post_acktx = [(t, d) for t, d in acktx_all if t_done is not None and t > t_done]
    ack_origins = classify_ack_origins(R, acktx)
    # Do not cut sender-side ACK observations at the receiver's FILE-complete
    # timestamp: the final ACK is queued by that callback and necessarily
    # reaches the sender later. Bound the observation tail from the final
    # committed vector instead, so unrelated post-session detections are not
    # charged to the transfer.
    ack_observation_start = txs[0][0] if txs else float('-inf')
    ack_observation_end = max(
        (t + ack_keydown_seconds(d) + 2.0 for t, d in acktx),
        default=(t_done if t_done is not None else float('inf')))
    seen_events = [(t, d) for t, k, d in Sa
                   if k in ('ack_seen', 'ack_seen_detail') and
                   ack_observation_start <= t <= ack_observation_end]
    seen = [t for t, _ in seen_events]
    ack_quality = [(t, d) for t, k, d in Sa
                   if k == 'ack_seen_detail' and
                   ack_observation_start <= t <= ack_observation_end]
    semantic_rejects = [(t, d) for t, k, d in Sa
                        if k == 'ack_semantic_reject' and
                        ack_observation_start <= t <= ack_observation_end]
    print(f"  {'#':>3} {'txStart':>8} {'keyDown':>8} {'decode':>8} "
          f"{'ackCommit':>9} {'ackSeen':>8} {'cycle':>7}  grp res")
    cycles = []
    for i, (t0, d0) in enumerate(txs):
        t1 = txs[i + 1][0] if i + 1 < len(txs) else None
        g = groups_by_tx.get(i)
        # A group-generated ACK is logged in the same millisecond as its callback.
        # For a deferred ACK, match by render/cause time but report the later
        # audio-queue commit time. Do not steal an unrelated timer/standalone ACK
        # from a later failed burst.
        atx = next(((t, x) for (t, x), origin in zip(acktx, ack_origins)
                    if g and origin == 'group' and
                    abs(x.get('_render_t', t) - g[0]) <= 0.050), None)
        # Bound ACK detection to this cycle.  A detection after the next key-down
        # belongs to a repeat/repair and used to create negative "rekey" time.
        sn = next((x for x in seen if atx and x > atx[0]
                   and (t1 is None or x <= t1)), None)
        kd = keydowns[i]
        cyc = (t1 - t0) if t1 else float('nan')
        if t1:
            cycles.append(cyc)
        res = f"{g[1]['ok']}/{g[1]['tot']}" if g else "  -"
        print(f"  {i+1:>3} {t0:8.2f} {kd:8.2f} "
              f"{(g[0]-t0) if g else float('nan'):8.2f} "
              f"{(atx[0]-t0) if atx else float('nan'):9.2f} "
              f"{(sn-t0) if sn else float('nan'):8.2f} {cyc:7.2f}  {res} "
              f"{'CRATER' if g and g[1]['ok']=='0' else ''}")

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
    origin_counts = Counter(ack_origins)
    if ack_commits_present:
        print("  ACK TX evidence: exact App audio-queue commits "
              f"({uncommitted_ack_renders} rendered-but-uncommitted ACK(s) excluded)")
    else:
        print("  ACK TX evidence: LEGACY render/firing proxy (this log predates "
              "audio-commit markers; deferred/dropped renders may be over-counted)")
    print("  ACK provenance (transmissions, in-transfer): "
          f"group={origin_counts['group']}  timer-SACK={origin_counts['timer_sack']}  "
          f"standalone-DATA={origin_counts['standalone_data']}  "
          f"silent-repeat={origin_counts['silent_repeat']}  other={origin_counts['other']}")
    if post_acktx:
        print(f"  post-completion ACK transmissions excluded: {len(post_acktx)}")
    windows = []          # (burst#, start, end) with end = start + COMPUTED airtime
    for i, (t0, _) in enumerate(txs):
        windows.append((i + 1, t0, t0 + signal_airtimes[i]))
    collisions = []
    strict_overlaps = []
    for (ta, da), origin in zip(acktx, ack_origins):
        for n, t0, t_end in windows:
            if t0 < ta < t_end:
                event = (n, t0, ta, t_end, da, origin)
                # A group ACK is emitted by the decode callback at actual signal
                # completion; its small negative modeled margin is clock-alignment
                # error, not a physical overlap.  Non-group ACKs have no such causal
                # guarantee and are the strict start-time-overlap population.
                if origin != 'group':
                    strict_overlaps.append(event)
                if ta < t_end - GUARD:
                    collisions.append(event)
                break
    if collisions:
        print(f"  !! {len(collisions)} COLLISION(S) — receiver transmitted over an in-progress burst:")
        for n, t0, ta, t_end, da, origin in collisions:
            print(f"     burst #{n}: on air [{t0:.2f} .. {t_end:.2f}]  ACK at {ta:.2f} "
                  f"(+{ta-t0:.2f}s in, {t_end-ta:.2f}s before the burst ended, "
                  f"mask={da['fm']}, origin={origin})")
    else:
        print("  clean — every ACK landed at/after its burst's end, in the listening gap")
    near_tail = [event for event in strict_overlaps if event not in collisions]
    if near_tail:
        print(f"  !! strict start-time overlap count: {len(strict_overlaps)} "
              f"({len(collisions)} exceed the {GUARD:.1f}s confidence guard; "
              f"{len(near_tail)} begin inside the final {GUARD:.1f}s and are alignment-sensitive):")
        for n, t0, ta, t_end, da, origin in near_tail:
            print(f"     burst #{n}: ACK at {ta:.2f}, {t_end-ta:.3f}s before modeled "
                  f"signal end (mask={da['fm']}, origin={origin})")
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

    # ---------- TONE-ACK ACCEPTANCE ----------
    # Keep protocol-semantic rejection separate from PHY decode failure. A
    # CRC-valid but impossible ACK now leaves the monitor armed; counting those
    # events proves whether the pre-commit gate was active in a live transfer.
    print("\n## TONE ACK ACCEPTANCE")
    rejected_candidates = sum(int(d.get('count') or 0) for _, d in semantic_rejects)
    print(f"  committed transmissions: {len(acktx)}   accepted detections: {len(seen)}")
    print(f"  CRC-valid semantic rejection events: {len(semantic_rejects)} "
          f"(candidates reported by searches: {rejected_candidates})")
    if ack_quality:
        peaks = [float(d['peak']) for _, d in ack_quality]
        confs = [float(d['conf']) for _, d in ack_quality]
        hamming = [int(d['hamming']) for _, d in ack_quality]
        print("  accepted normalized peak min/median/max: "
              f"{min(peaks):.3f} / {statistics.median(peaks):.3f} / {max(peaks):.3f}")
        print("  accepted min-confidence min/median/max:  "
              f"{min(confs):.3f} / {statistics.median(confs):.3f} / {max(confs):.3f}")
        print(f"  Hamming-corrected accepted bursts: {sum(v > 0 for v in hamming)} "
              f"(corrected blocks total: {sum(hamming)})")
    else:
        print("  detailed peak/confidence/Hamming telemetry unavailable in this log")

    if acktx:
        raw_ack_s = [int(d.get('s') or 0) / 48000.0 for _, d in acktx
                     if int(d.get('s') or 0) > 0]
        keyed_ack_s = [ack_keydown_seconds(d) for _, d in acktx]
        if raw_ack_s and keyed_ack_s:
            print("  median ACK vector duration: "
                  f"raw tone {statistics.median(raw_ack_s):.3f}s; "
                  f"committed keyed samples {statistics.median(keyed_ack_s):.3f}s")

    # ---------- TURNAROUND DECOMPOSITION ----------
    # Break the scheduling gap into logged boundaries. In commit-backed logs,
    # `ackCommit` is the App accepting the waveform into its software audio
    # queue. It is deliberately NOT called "on air": SDL/device buffering and
    # the simulator/channel path live downstream of that timestamp.
    print("\n## TURNAROUND DECOMPOSITION  (end-of-TX -> next key-down)")
    print(f"  {'#':>3} {'txEnd':>8} {'rxDecode':>9} {'ackCommit':>9} "
          f"{'ackSeen':>8} {'nextTX':>7} {'gap':>6}")
    seg = {
        'rx_latency': [], 'ack_commit': [], 'detect': [], 'rekey': [], 'gap': [],
        # Optional marker-level split of commit -> detection. These are log
        # boundaries, not inferred RF boundaries: tx_queue_idle means the App
        # FIFO drained, and audio_arrived is a chunk-RMS threshold crossing.
        'commit_to_queue_idle': [],
        'queue_idle_to_activity': [],
        'activity_to_accept': [],
    }
    tx_queue_idle = [t for t, k, _ in R
                     if k == 'tx_queue_idle' and (t_done is None or t <= t_done)]
    audio_arrivals = [t for t, k, _ in Sa
                      if k == 'audio_arrived' and (t_done is None or t <= t_done)]
    for i, (t0, _) in enumerate(txs):
        t_end = t0 + signal_airtimes[i]
        t1 = txs[i + 1][0] if i + 1 < len(txs) else None
        group_event = groups_by_tx.get(i)
        g = group_event[0] if group_event else None
        atx_record = next(((t, ack_data) for (t, ack_data), origin
                           in zip(acktx, ack_origins)
                           if g is not None and origin == 'group' and
                           abs(ack_data.get('_render_t', t) - g) <= 0.050), None)
        atx = atx_record[0] if atx_record else None
        sn = next((x for x in seen if atx is not None and x > atx
                   and (t1 is None or x <= t1)), None)
        if t1 is None:
            continue
        seg['gap'].append(t1 - t_end)
        if g is not None:
            seg['rx_latency'].append(g - t_end)          # burst end -> receiver finishes decode
        if g is not None and atx is not None:
            seg['ack_commit'].append(atx - g)            # decode -> App audio-queue commit
        if atx is not None and sn is not None:
            seg['detect'].append(sn - atx)               # queue commit -> sender accepts ACK

            # Pair the first receiver software-queue drain after this commit,
            # bounded by the committed vector duration plus scheduler slack.
            keyed_s = ack_keydown_seconds(atx_record[1])
            idle = next((x for x in tx_queue_idle
                         if atx <= x <= atx + keyed_s + 0.5), None)

            # AudioActivity is chunk-level and may already be high, so use the
            # nearest preceding ARRIVED marker and tolerate one cadence beyond
            # this ACK's raw tone duration. Missing markers remain missing rather
            # than being fabricated from the detection timestamp.
            raw_s = int(atx_record[1].get('s') or 0) / 48000.0
            arrivals = [x for x in audio_arrivals
                        if atx <= x <= sn and sn - x <= raw_s + 0.25]
            activity = arrivals[-1] if arrivals else None
            if idle is not None:
                seg['commit_to_queue_idle'].append(idle - atx)
            if idle is not None and activity is not None and activity >= idle:
                seg['queue_idle_to_activity'].append(activity - idle)
            if activity is not None:
                seg['activity_to_accept'].append(sn - activity)
        if sn is not None:
            seg['rekey'].append(t1 - sn)                 # sender detects -> next key-down
        if i < 6:
            print(f"  {i+1:>3} {t_end:8.2f} "
                  f"{(g-t_end) if g is not None else float('nan'):9.2f} "
                  f"{(atx-g) if (g is not None and atx is not None) else float('nan'):9.2f} "
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
                        ('ack_commit', 'RX decode -> ACK queue commit'),
                        ('detect',     'ACK commit -> sender accepts'),
                        ('rekey',      'sender accepts -> next TX   ')):
        v = med(seg[name])
        pct = (100.0 * v / tot) if (tot and tot == tot and v == v) else float('nan')
        print(f"     {label} {v:6.2f}s  ({pct:5.1f}% of the gap)  n={len(seg[name])}")

    if ack_commits_present:
        print("\n  ACK commit is audio-queue acceptance, NOT an on-air timestamp.")
        marker_labels = (
            ('commit_to_queue_idle', 'commit -> receiver SW queue empty'),
            ('queue_idle_to_activity', 'SW queue empty -> sender activity marker'),
            ('activity_to_accept', 'sender activity marker -> ACK accepted'),
        )
        if any(seg[name] for name, _ in marker_labels):
            print("  Marker-level split of commit -> accept (medians; queue empty != DAC empty):")
            for name, label in marker_labels:
                if seg[name]:
                    print(f"     {label:43} {med(seg[name]):6.3f}s  n={len(seg[name])}")
            print("  Cross-host marker time uses the burst-consensus clock offset; that offset")
            print("  includes the forward audio path, so it is accounting—not one-way propagation.")


    print("\n## RETRANSMISSIONS — what and why")
    detailed_retx = [d for t, k, d in Sa
                     if k == 'retx_detail' and
                     (t_done is None or t <= t_done)]
    retx_causes = Counter(d['cause'].lower() for d in detailed_retx)
    rq = [(t, d) for t, k, d in Sa
          if k == 'requeue' and (t_done is None or t <= t_done)]
    partials = [(t, d) for t, d in groups if d['all'] == '0' and d['ok'] != '0']
    zeros = [(t, d) for t, d in groups if d['ok'] == '0']
    missing_group_txs = [i for i in range(len(txs)) if i not in groups_by_tx]
    print(f"  rate-change requeues : {len(rq)}  (chunks re-queued: {sum(int(d['n']) for _, d in rq)})")
    for t, d in rq:
        print(f"     t={t:7.2f}  re-queued {d['n']} chunks, acked={d['acked']}, resume_offset={d['off']}")
    print(f"  partial groups (SACK hole -> selective resend): {len(partials)}")
    for t, d in partials[:12]:
        print(f"     t={t:7.2f}  {d['ok']}/{d['tot']} decoded  q={d['q']}")
    print(f"  FULL craters (0/N, whole group lost)          : {len(zeros)}")
    for t, d in zeros[:12]:
        print(f"     t={t:7.2f}  0/{d['tot']}")
    print(f"  physical bursts with NO group callback        : {len(missing_group_txs)}"
          f"  ({', '.join('#'+str(i+1) for i in missing_group_txs) or 'none'})")
    print(f"  timer-generated SACK repeats                  : {origin_counts['timer_sack']}")
    print(f"  standalone-DATA ACKs                          : {origin_counts['standalone_data']}")
    print(f"  decoder-forced re-anchors                     : "
          f"{len([1 for t,k,_ in R if k=='reanchor' and (t_done is None or t <= t_done)])}")
    print(f"  anchored-burst backstops                      : "
          f"{len([1 for t,k,_ in R if k=='backstop' and (t_done is None or t <= t_done)])}")
    if post_groups:
        post_zeros = sum(1 for _, d in post_groups if d['ok'] == '0')
        print(f"  post-completion groups (excluded above)       : {len(post_groups)}"
              f"  (0/N={post_zeros})")

    # Per-frame-position failure profile.  This is descriptive evidence only:
    # flat-looking rates from a small number of bursts cannot rule stale CSI in
    # or out, especially when failures are correlated within a burst.
    pos = defaultdict(lambda: [0, 0])
    for t, k, d in R:
        if k == 'frame' and (t_done is None or t <= t_done):
            pos[int(d['i'])][1] += 1
            if d['res'] == 'OK':
                pos[int(d['i'])][0] += 1
    if pos:
        print("  per-frame-position decode rate (descriptive; does not by itself rule stale CSI in or out):")
        for i in sorted(pos):
            ok, tot = pos[i]
            print(f"     frame {i}: {ok:>4}/{tot:<4} ({100.0*ok/tot:5.1f}%)")

    # ---------- AIRTIME BUDGET ----------
    # Sender signal airtime comes from the emitted sample count.  PA key-down adds both
    # configured guards (default 150/50 ms).  Reverse ACK airtime is reported separately;
    # omitting it hid the cost of timer-SACK/cached repair transmissions.
    print("\n## AIRTIME BUDGET")
    if cfgs:
        print(f"  per-frame airtime (from ARQ config): {frame_ms_at(txs[-1][0]) if txs else 0:.0f} ms"
              f"   full-chirp bursts {len(fulls)} / light-LTS {len(lights)}")
    span_end = t_done if t_done is not None else (
        max((t + kd for (t, _), kd in zip(txs, keydowns)), default=0.0))
    span = (span_end - txs[0][0]) if txs else 0.0
    kd_tot_emitted = sum(keydowns)
    # The delivery span ends at the receiver's completion callback, which can
    # precede the sender's final silent tail (and differs by the bounded
    # cross-clock alignment residual).  Duty over that span must count only the
    # overlap, not charge samples after its endpoint to its denominator.
    kd_tot_in_span = keydown_overlap_seconds(
        txs, keydowns, txs[0][0] if txs else None, span_end)
    kd_after_completion = max(0.0, kd_tot_emitted - kd_tot_in_span)
    if kd_after_completion < 1e-9:
        kd_after_completion = 0.0
    ack_keydowns = [ack_keydown_seconds(d) for _, d in acktx]
    ack_kd_tot = sum(ack_keydowns)
    if cycles:
        cyc_mean = sum(cycles) / len(cycles)
        cycle_kd = keydowns[:-1] if len(keydowns) > 1 else keydowns
        kd_mean = (sum(cycle_kd) / len(cycle_kd)) if cycle_kd else 0.0
        print(f"  mean cycle        : {cyc_mean:6.2f}s")
        print(f"  mean key-down     : {kd_mean:6.2f}s  ({100.0*kd_mean/cyc_mean:.1f}% of cycle)")
        print(f"  mean turnaround   : {cyc_mean-kd_mean:6.2f}s  ({100.0*(cyc_mean-kd_mean)/cyc_mean:.1f}% of cycle)")
    if span > 0:
        duty = 100.0 * kd_tot_in_span / span
        print(f"  sender PA DUTY    : {duty:5.1f}%  (key-down overlap "
              f"{kd_tot_in_span:.1f}s of {span:.1f}s first-key-down -> completion span)")
        if kd_after_completion > 0.001:
            print(f"  sender key-down after RX completion: {kd_after_completion:.3f}s "
                  "(excluded from delivery-span duty/headroom)")
        guard_note = ("exact committed keyed samples" if ack_commits_present else
                      "default 150/50ms guards assumed")
        print(f"  reverse ACK TX    : {ack_kd_tot:5.1f}s  ({len(acktx)} transmissions; "
              f"{guard_note})")
        if duty > 60:
            print(f"     ^^ a real 100W final derates near ~50% duty for digital modes —"
                  f" this would need a duty governor on hardware")

    # ---------- RUNG / CHANNEL ----------
    print("\n## RUNG + CHANNEL")
    rungs = [(t, d['rung'], d['snr']) for t, k, d in R
             if k == 'mode' and (t_done is None or t <= t_done)]
    changes = [(t, rung, float(snr)) for t, rung, snr in rungs]
    rung_window_end = (t_done if t_done is not None else
                       (groups[-1][0] if groups else None))
    rung_segments, rung_transitions = rung_transfer_timeline(
        changes, t_first_tx, rung_window_end)
    latent_n = len([1 for t, k, _ in R
                    if k == 'latent' and (t_done is None or t <= t_done)])
    print(f"  rung state announcements through completion: {len(rungs)} "
          "(the first/current state is not a transition)")
    for t, r, s in rungs[:16]:
        print(f"     t={t:7.2f}  {r:<14} nearby RX SNR {s}")
    print(f"  physical-transfer rung transitions: {len(rung_transitions)}")
    print(f"  selector decisions: LATENT="
          f"{latent_n}"
          f"  RX-authority="
          f"{len([1 for t,k,_ in R if k=='verdict' and (t_done is None or t <= t_done)])}"
          f"   EVM demotes: "
          f"{len([1 for t,k,_ in R if k=='evm_demote' and (t_done is None or t <= t_done)])}")
    if latent_n:
        print("  LATENT decisions consume proven group outcomes (k/M), not the nearby SNR labels")
    sel = Counter(d['cls'] for t, k, d in R
                  if k == 'selectivity' and (t_done is None or t <= t_done))
    if sel:
        print(f"  channel class (freq-selectivity): {dict(sel)}")
    alc = [float(d['h']) for t, k, d in R
           if k == 'alc' and (t_done is None or t <= t_done)]
    if alc:
        alc.sort()
        print(f"  ALC headroom dB: min {alc[0]:.1f}  med {alc[len(alc)//2]:.1f}  max {alc[-1]:.1f}")

    # ---------- PHASES + PROGRESS ----------
    print("\n## PHASES + PROGRESS")
    def first(kind, src=R):
        return next((t for t, k, _ in src if k == kind), None)
    t_ping, t_conn = first('ping'), first('connect')
    t_recv_marker = first('recv_start')
    time_bases = transfer_time_bases(
        t_done,
        float(done[-1]['secs']) if done else None,
        t_first_tx,
        t_recv_marker)
    t_app_origin = time_bases['app_origin_t']
    for label, t in (("first PING", t_ping), ("CONNECTED", t_conn),
                     ("first data key-down", t_first_tx),
                     ("app RX timer origin", t_app_origin)):
        if t is not None:
            suffix = (f" ({time_bases['app_origin_evidence']})"
                      if label == "app RX timer origin" else "")
            print(f"  {label:<18} t={t:7.2f}s{suffix}")
    if t_conn is not None and t_first_tx is not None:
        print(f"  CONNECTED -> first data key-down: {t_first_tx-t_conn:.2f}s")
    if (t_first_tx is not None and
            time_bases['pre_app_keydown_s'] is not None):
        print("  first data key-down -> app RX timer origin: "
              f"{time_bases['pre_app_keydown_s']:.2f}s "
              "(already-occupied channel time excluded from app goodput timer)")
    prog = [(t, int(d['got']), int(d['tot'])) for t, k, d in R if k == 'progress']
    # Rate baseline: prefer the logged RX-start; else the sender's first burst. NEVER use the
    # first progress mark itself — that makes elapsed 0 and the rate meaningless/infinite.
    base = t_app_origin if t_app_origin is not None else t_first_tx
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
        print(f"     COMPLETE  {d['bytes']} bytes in {d['secs']}s  =>  {d['kbps']} kbps")

    # ---------- RUNG DWELL: where the transfer actually spent its time ----------
    print("\n## RUNG DWELL  (physical first-key-down -> completion span)")
    # Cause attribution: an EVM demote or an RX-authority verdict just before a
    # true transition owns it.  A state already active at first key-down is
    # explicitly labelled as the initial state, not as a transfer transition.
    evm = [t for t, k, _ in R if k == 'evm_demote']
    verd = [t for t, k, _ in R if k == 'verdict']
    dwell = defaultdict(float)
    if rung_segments:
        transition_times = {t for t, _, _ in rung_transitions}
        for i, (seg_start, seg_end, rung, snr, announced_t) in enumerate(rung_segments):
            dur = seg_end - seg_start
            dwell[rung] += dur
            if rung == 'UNKNOWN':
                cause = "no MODE state observed yet"
            elif seg_start in transition_times:
                if any(abs(seg_start - x) < 4.0 for x in evm):
                    cause = "EVM-DEMOTE"
                elif any(abs(seg_start - x) < 4.0 for x in verd):
                    cause = "RX-authority verdict"
                else:
                    cause = "state announcement"
            elif (t_first_tx is not None and
                  abs(seg_start - t_first_tx) < 1e-9 and
                  announced_t is not None and announced_t <= seg_start):
                cause = ("initial state at first data key-down; announced "
                         f"t={announced_t:.2f}s")
            else:
                cause = "first observed state"
            print(f"  t={seg_start:7.2f}..{seg_end:7.2f}s  {rung:<14} "
                  f"nearby RX SNR {snr:5.1f}   held {dur:6.2f}s   {cause}")
        total = sum(dwell.values()) or 1.0
        print("\n  dwell summary:")
        for rung, secs in sorted(dwell.items(), key=lambda kv: -kv[1]):
            print(f"     {rung:<14} {secs:7.2f}s  "
                  f"({100.0*secs/total:5.1f}% of the physical transfer span)")
        print(f"  physical-transfer rung transitions: {len(rung_transitions)} "
              f"across {total:.1f}s; state segments: {len(rung_segments)}")

    # ---------- CHANNEL CLASS + DRIVE ----------
    ep = [(t, d['e']) for t, k, d in Sa if k == 'epoch']
    dr = [(t, d) for t, k, d in Sa if k == 'drive']
    drive_summary = drive_transition_summary(dr)
    if ep or dr:
        print("\n## ARQ EPOCHS + TX DRIVE")
        for t, e in ep:
            print(f"  t={t:7.2f}s  MOVE-EPOCH -> {e}   (seq space regridded; in-flight frames abandoned)")
        if drive_summary:
            peak_db = drive_summary['peak_db_from_start']
            final_db = drive_summary['final_db_from_start']
            peak_db_text = f"{peak_db:+.2f} dB" if peak_db is not None else "n/a"
            final_db_text = f"{final_db:+.2f} dB" if final_db is not None else "n/a"
            print(
                "  tx_drive operational: "
                f"start={drive_summary['start']:.3f} "
                f"min={drive_summary['minimum']:.3f} "
                f"peak={drive_summary['peak']:.3f} "
                f"final-before-reset={drive_summary['final_before_reset']:.3f}; "
                f"{drive_summary['advisory_moves']} advisory move(s), "
                f"peak excursion {peak_db_text}, net {final_db_text}; "
                f"{drive_summary['lifecycle_resets']} lifecycle reset(s) excluded")

    # ---------- EFFICIENCY ----------
    physical_span_kbps = None
    sender_keydown_payload_kbps = None
    keyed_to_physical_span_pct = None
    non_keyed_gap_headroom_kbps = None
    if done and txs:
        d = done[-1]
        app_kbps = float(d['kbps'])
        payload_bits = int(d['bytes']) * 8
        onair = kd_tot_in_span if kd_tot_in_span else 1.0
        sender_keydown_payload_kbps = payload_bits / onair / 1000.0
        physical_span_s = time_bases['physical_span_s']
        if physical_span_s and physical_span_s > 0.0:
            physical_span_kbps = payload_bits / physical_span_s / 1000.0
            keyed_to_physical_span_pct = 100.0 * kd_tot_in_span / physical_span_s
            non_keyed_gap_headroom_kbps = (
                sender_keydown_payload_kbps - physical_span_kbps)
        print("\n## EFFICIENCY")
        print(f"  app-reported delivered : {app_kbps:.2f} kbps over {d['secs']}s "
              "(FILE_START/app timer -> completion)")
        if physical_span_kbps is not None:
            print(f"  physical-span delivered: {physical_span_kbps:.2f} kbps over "
                  f"{physical_span_s:.2f}s "
                  "(first data key-down -> completion)")
        print(f"  sender-keydown payload : {sender_keydown_payload_kbps:.2f} kbps  "
              "(delivered payload / sender DATA key-down within physical span)")
        if keyed_to_physical_span_pct is not None:
            print(f"  keyed-to-physical-span efficiency: "
                  f"{keyed_to_physical_span_pct:.1f}%  "
                  "(sender DATA key-down / physical transfer span)")
            headroom_pct = (100.0 * non_keyed_gap_headroom_kbps /
                            physical_span_kbps)
            print("  => headroom if every non-keyed interval vanished: "
                  f"{non_keyed_gap_headroom_kbps:.2f} kbps "
                  f"({headroom_pct:.0f}% over physical-span rate; "
                  "retransmitted key-down is still charged)")

    if a.json:
        with open(a.json, 'w') as fh:
            json.dump({
                'sender': a.sender, 'receiver': a.receiver, 'offset': off,
                'groups_sent': len(txs), 'groups_decoded': len(groups),
                'groups_physically_matched': len(groups_by_tx),
                'bursts_without_group_callback': len(missing_group_txs),
                'unmatched_group_callbacks': len(unmatched_groups),
                'post_completion_sender_bursts': len(post_txs),
                'post_completion_groups': len(post_groups),
                'post_completion_acks': len(post_acktx),
                'collisions': len(collisions),
                'strict_ack_start_overlaps': len(strict_overlaps),
                'ack_tx_evidence': ('audio_commit' if ack_commits_present
                                    else 'legacy_render_proxy'),
                'ack_rendered_not_committed': uncommitted_ack_renders,
                'ack_provenance': dict(origin_counts),
                'ack_accepted_detections': len(seen),
                'ack_semantic_rejection_events': len(semantic_rejects),
                'ack_semantic_rejected_candidates': rejected_candidates,
                'ack_hamming_corrected_bursts': sum(
                    int(d['hamming']) > 0 for _, d in ack_quality),
                'ack_turnaround_markers_median_s': {
                    name: (statistics.median(seg[name]) if seg[name] else None)
                    for name in (
                        'detect', 'commit_to_queue_idle',
                        'queue_idle_to_activity', 'activity_to_accept')
                },
                'requeues': len(rq), 'requeued_chunks': sum(int(x['n']) for _, x in rq),
                'requeued_bytes': sum(int(x['bytes']) for _, x in rq if x.get('bytes')),
                'retransmission_entries': len(detailed_retx),
                'retransmission_causes': dict(retx_causes),
                'nack_retransmission_entries': retx_causes.get('nack', 0),
                'timeout_retransmission_entries': retx_causes.get('timeout', 0),
                'crater_regrades': len([1 for t, k, _ in R
                                        if k == 'regrade' and (t_done is None or t <= t_done)]),
                'partial_groups': len(partials), 'full_craters': len(zeros),
                'mean_cycle_s': (sum(cycles)/len(cycles)) if cycles else None,
                'duty_pct': (100.0*kd_tot_in_span/span) if span > 0 else None,
                'sender_keydown_s': kd_tot_emitted,
                'sender_keydown_within_physical_span_s': kd_tot_in_span,
                'sender_keydown_after_completion_s': kd_after_completion,
                'reverse_ack_keydown_s': ack_kd_tot,
                'app_reported_duration_s': (
                    float(done[-1]['secs']) if done else None),
                'app_timer_origin_t': time_bases['app_origin_t'],
                'app_timer_origin_evidence': time_bases['app_origin_evidence'],
                'pre_app_keydown_s': time_bases['pre_app_keydown_s'],
                'physical_transfer_span_s': time_bases['physical_span_s'],
                'physical_span_kbps': physical_span_kbps,
                'sender_keydown_payload_kbps': sender_keydown_payload_kbps,
                'keyed_to_physical_span_efficiency_pct': keyed_to_physical_span_pct,
                'non_keyed_gap_headroom_kbps': non_keyed_gap_headroom_kbps,
                'rung_state_announcements': len(rungs),
                'physical_transfer_rung_transitions': len(rung_transitions),
                'tx_drive': drive_summary,
                'kbps': float(done[-1]['kbps']) if done else 0.0,
                **alignment_meta,
            }, fh, indent=1)
        print(f"\n(json -> {a.json})")


if __name__ == '__main__':
    main()
