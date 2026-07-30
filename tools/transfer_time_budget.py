#!/usr/bin/env python3
"""Account for EVERY second of a file transfer, from the receiver's own logs.

WHY THIS EXISTS
---------------
docs/F163_TIME_BUDGET_2026_07_06.md did this once, by hand, for one rig transfer, and
it remains the single most useful document about throughput on this modem: it showed
that only 30.4% of wall clock carries fresh data, that receiver decode lag is ZERO, and
that the ladder's up-switches "bought zero throughput upside, only stall downside".

It was never repeatable. So every later throughput question got answered by hypothesis
instead, and on 2026-07-29/30 five consecutive mechanisms were proposed, built and
measured neutral-or-worse on hardware -- every one of them resting on an assumption
about where time went. This makes the accounting a command instead of an archaeology
project.

THE RULE THAT MAKES IT TRUSTWORTHY
----------------------------------
The categories MUST sum to the measured wall clock. Anything left over is printed as
UNACCOUNTED, loudly, with its share. A budget that silently absorbs its residual is
worse than no budget: it looks like understanding. F163 reconciled to 99.98% and named
its 0.1 s residual; anything materially worse than that means the model is wrong, not
that the run was odd.

INPUTS
------
  --phy   ULTRA_PHY_DIAG_LOG output (per-frame `event=burst_logical` lines carry
          diag_ms, so inter-frame gaps are measured, not modelled)
  --gui   logs/gui.log (mode changes, ARQ config, transfer completion)

WHAT IT CANNOT SEE, STATED UP FRONT
-----------------------------------
This reads the RECEIVER's clock. Sender-side keydown (its own T/R, PA ramp, carrier
sense deferral) is invisible here and lands inside the gap categories. F163 paired both
stations' logs by content to separate those; this tool deliberately does not guess at
them -- it labels the gaps by what preceded them and leaves attribution honest.
"""
import argparse
import re
import sys
from collections import defaultdict


def parse_phy(path):
    """Per-frame records from the burst_logical diagnostic lines."""
    frames = []
    with open(path, errors="replace") as fh:
        for line in fh:
            if "event=burst_logical" not in line:
                continue
            d = {}
            for tok in line.split():
                if "=" in tok:
                    k, _, v = tok.partition("=")
                    d[k] = v
            try:
                rec = {
                    "t": float(d["diag_ms"]) / 1000.0,
                    "group": int(d.get("group", -1)),
                    "cw_ok": int(d.get("cw_ok", 0)),
                    "cw_fail": int(d.get("cw_fail", 0)),
                    "success": d.get("success") == "1",
                    "snr": float(d.get("ofdm_internal_snr_db", "nan")),
                }
            except (KeyError, ValueError):
                continue
            frames.append(rec)
    frames.sort(key=lambda r: r["t"])
    return frames


def parse_gui(path):
    """Mode changes and the frame cadence the modem itself reported."""
    modes = []
    data_ms = None
    ack_ms = None
    complete = None
    ts = re.compile(r"^\[\s*([0-9]+\.[0-9]+)\]")
    try:
        fh = open(path, errors="replace")
    except OSError:
        return modes, data_ms, ack_ms, complete
    with fh:
        for line in fh:
            m = ts.match(line)
            t = float(m.group(1)) if m else None
            mm = re.search(r"MODE_CHANGE: OFDM (\S+) (R\d/\d)", line)
            if mm and t is not None:
                modes.append((t, mm.group(1), mm.group(2)))
            cfg = re.search(r"data=(\d+)ms.*?ack=(\d+)ms", line)
            if cfg:
                data_ms = int(cfg.group(1))
                ack_ms = int(cfg.group(2))
            tc = re.search(r"Transfer complete ([0-9.]+)s", line)
            if tc:
                complete = float(tc.group(1))
    return modes, data_ms, ack_ms, complete


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--phy", required=True)
    ap.add_argument("--gui", default="logs/gui.log")
    ap.add_argument("--bytes", type=int, default=51200)
    args = ap.parse_args()

    frames = parse_phy(args.phy)
    if len(frames) < 4:
        print(f"ERROR: only {len(frames)} frames in {args.phy} — nothing to account for")
        return 1
    modes, data_ms, ack_ms, complete = parse_gui(args.gui)

    # Frame airtime: prefer the modem's OWN reported cadence over any constant of ours.
    # Falls back to the modal inter-frame gap, which is measured from this very run.
    if data_ms:
        frame_s = data_ms / 1000.0
        frame_src = f"modem-reported data_ms={data_ms}"
    else:
        gaps = [round(frames[i]["t"] - frames[i - 1]["t"], 2)
                for i in range(1, len(frames))]
        modal = defaultdict(int)
        for g in gaps:
            if 0.3 < g < 3.0:
                modal[g] += 1
        frame_s = max(modal, key=modal.get) if modal else 1.237
        frame_src = f"modal inter-frame gap={frame_s:.3f}s"

    # --- GROUP granularity, one clock ------------------------------------------
    # The phy log emits every frame of a group in one loop at GROUP COMPLETION, so all
    # frames in a group share a timestamp (verified: group 0 frames all at 48958-48959 ms).
    # Per-frame arrival times therefore DO NOT EXIST in this log, and a frame-granular
    # model silently mis-attributes: it collapses within-group gaps to ~0 (undercounting
    # airtime) and lets each between-group gap swallow the whole cycle (overcounting
    # stalls). A first version of this tool did exactly that and reconciled to -52%.
    #
    # Group granularity is sound because the group is what the ARQ actually cycles on:
    # burst airtime, then one turnaround, then the next burst.
    #
    # ONE CLOCK ONLY: everything below is phy diag_ms. The gui log's "Transfer complete
    # Ns" is measured from transfer start, not process start, and mixing the two produced
    # a stall timestamped after the end of its own window.
    groups = []
    for f in frames:
        if groups and groups[-1]["group"] == f["group"] and abs(f["t"] - groups[-1]["t"]) < 2.0:
            groups[-1]["n"] += 1
            groups[-1]["ok"] += 1 if f["success"] else 0
        else:
            groups.append({"group": f["group"], "t": f["t"], "n": 1,
                           "ok": 1 if f["success"] else 0})
    if len(groups) < 3:
        print(f"ERROR: only {len(groups)} groups — nothing to account for")
        return 1

    t0 = groups[0]["t"]
    last_air = groups[-1]["n"] * frame_s
    window = (groups[-1]["t"] - t0) + last_air

    airtime = 0.0
    turn_gap = 0.0
    stall_gap = 0.0
    n_turn = 0
    n_stall = 0
    stalls = []
    delivered = 0
    failed = 0
    # A turnaround is one receiver-decode + tail-SACK + tone + T/R. Anything beyond a
    # generous allowance for that is a stall; the split point is stated, not tuned.
    turn_allow = 2.0 * frame_s + 2.5
    for k, g in enumerate(groups):
        air = g["n"] * frame_s
        airtime += air
        delivered += g["ok"]
        failed += g["n"] - g["ok"]
        if k + 1 < len(groups):
            interval = groups[k + 1]["t"] - g["t"]
            gap = interval - air
            if gap <= 0:
                continue
            if gap <= turn_allow:
                turn_gap += gap
                n_turn += 1
            else:
                turn_gap += turn_allow
                n_turn += 1
                stall_gap += gap - turn_allow
                n_stall += 1
                stalls.append((g["t"] - t0, gap - turn_allow))
    useful_air = delivered * frame_s
    wasted_air = failed * frame_s


    print(f"=== TRANSFER TIME BUDGET ===")
    print(f"  groups          : {len(groups)}   frames: {len(frames)} (delivered {delivered}, failed {failed})")
    print(f"  frame airtime   : {frame_s:.3f}s  [{frame_src}]")
    print(f"  window          : {window:.1f}s")
    if modes:
        print(f"  rungs used      : " +
              ", ".join(f"{m}{r}@{t:.0f}s" for t, m, r in modes[:6]))
    print()
    rows = [
        ("Useful data airtime (frames delivered)", useful_air),
        ("Wasted data airtime (frames failed)", wasted_air),
        ("ACK turnaround gaps", turn_gap),
        ("Stall / RTO dead air", stall_gap),
    ]
    total = sum(v for _, v in rows)
    unacc = window - total
    print(f"  {'category':<42}{'seconds':>10}{'share':>9}")
    print("  " + "-" * 61)
    for name, v in rows:
        print(f"  {name:<42}{v:>10.1f}{100*v/window:>8.1f}%")
    print("  " + "-" * 61)
    print(f"  {'SUM of categories':<42}{total:>10.1f}{100*total/window:>8.1f}%")
    tag = "OK" if abs(unacc) < 0.05 * window else "*** MODEL INCOMPLETE ***"
    print(f"  {'UNACCOUNTED':<42}{unacc:>10.1f}{100*unacc/window:>8.1f}%   {tag}")
    print()
    print(f"  turnarounds: {n_turn}  (mean {turn_gap/n_turn:.2f}s each)"
          if n_turn else "  turnarounds: 0")
    print(f"  stalls:      {n_stall}" + (f"  (total {stall_gap:.1f}s)" if n_stall else ""))
    for t, d in sorted(stalls, key=lambda x: -x[1])[:5]:
        print(f"     stall {d:5.1f}s at t+{t:.1f}s")
    print()
    goodput = args.bytes * 8 / window
    ceiling = args.bytes * 8 / useful_air if useful_air else 0
    print(f"  delivered goodput      : {goodput:.0f} bps over {window:.1f}s")
    print(f"  pure-airtime ceiling   : {ceiling:.0f} bps  (if zero gaps/losses)")
    print(f"  efficiency vs ceiling  : {100*goodput/ceiling:.0f}%" if ceiling else "")
    return 0


if __name__ == "__main__":
    sys.exit(main())
