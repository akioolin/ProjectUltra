#!/usr/bin/env python3
"""Scan a cli_simulator log for diagnostic anomalies.

Print a tail-friendly summary that tells you whether the run hides
sync/CFO/queueing problems behind a TEST PASSED + retx count.

Usage:
    scripts/scan_cli_log.py [LOG_FILE]

Default LOG_FILE is /tmp/manual_run.log.
Exit code: 0 if no anomalies, 1 if any anomaly fired, 2 on bad input.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path


# ---- patterns ----------------------------------------------------------
# Use ANSI-tolerant patterns. cli_simulator emits color codes.
ANSI = re.compile(r"\x1b\[[0-9;]*[a-zA-Z]")
RE_HEADER_SNR = re.compile(r"SNR:\s+(-?\d+(?:\.\d+)?)\s+dB")
RE_HEADER_CHANNEL = re.compile(r"Channel:\s+(AWGN|Good|Moderate|Poor)")
RE_HARQ_ENABLED = re.compile(r"RX soft-combining HARQ ENABLED")
RE_CONNECT_START = re.compile(r"PHASE 1: CONNECTION")
RE_CONNECT_DONE = re.compile(r"Both stations connected!")
RE_SIM_TIME = re.compile(r"\[(\d+(?:\.\d+)?)s sim\]")
RE_FAILSAFE = re.compile(r"Handshake fail-safe triggered")
RE_CW_FAIL = re.compile(r"cw_ok=(\d+),\s*cw_fail=(\d+)")
RE_LTS_CFO = re.compile(r"LTS residual CFO:\s+(-?\d+(?:\.\d+)?)\s+Hz.*chirp gave\s+(-?\d+(?:\.\d+)?)")
RE_TX_PENDING = re.compile(r"TX_pending=(\d+)")
RE_LDPC_FALSE_POS = re.compile(r"LDPC false positive detected")
RE_SR_ARQ_OOW = re.compile(r"SR-ARQ:\s+DATA seq=\d+ outside window")
RE_HARQ_KEY_BUILD = re.compile(r"harq_key_build\s+success=(\d+)\s+failed=(\d+)")
RE_TEST_VERDICT = re.compile(r"TEST (PASSED|FAILED)")
RE_RETX = re.compile(r"retransmissions=(\d+)")

# CFO thresholds (Hz) above which LTS residual is "noisier than the
# channel can plausibly produce". Doppler-based heuristic; conservative.
CFO_BUDGET = {
    "AWGN": 0.20,
    "Good": 0.30,
    "Moderate": 0.80,
    "Poor": 1.50,
}

# TX_pending peak above this many samples flags a deep ARQ-vs-radio race.
# 48000 sps; ARQ window=8 frames at ~1s each = up to 8 × 48000 = 384k samples
# is "expected"; 5 seconds (240k samples) of queue is already worth a flag.
TX_PENDING_FLAG_SAMPLES = 240_000


# ---- helpers ----------------------------------------------------------
def strip_ansi(s: str) -> str:
    return ANSI.sub("", s)


def fmt_hz(values):
    if not values:
        return "—"
    return f"min={min(values):+.2f} max={max(values):+.2f} span={max(values)-min(values):.2f}"


# ---- main scan --------------------------------------------------------
def scan(path: Path) -> int:
    try:
        text = path.read_text(errors="replace")
    except OSError as e:
        print(f"scan_cli_log: cannot read {path}: {e}", file=sys.stderr)
        return 2

    text = strip_ansi(text)
    lines = text.splitlines()

    snr_match = RE_HEADER_SNR.search(text)
    channel_match = RE_HEADER_CHANNEL.search(text)
    snr = snr_match.group(1) if snr_match else "?"
    channel = channel_match.group(1) if channel_match else "?"

    verdict_match = RE_TEST_VERDICT.search(text)
    verdict = verdict_match.group(1) if verdict_match else "INCOMPLETE"

    retx_matches = RE_RETX.findall(text)
    retx_tx = int(retx_matches[0]) if retx_matches else None

    # Handshake duration: from PHASE 1 line to "Both stations connected!".
    handshake_start = None
    handshake_end = None
    for i, line in enumerate(lines):
        if handshake_start is None and RE_CONNECT_START.search(line):
            # Look at the next ~3 lines for a sim-time stamp.
            for j in range(i, min(i + 6, len(lines))):
                m = RE_SIM_TIME.search(lines[j])
                if m:
                    handshake_start = float(m.group(1))
                    break
        if handshake_start is not None and RE_CONNECT_DONE.search(line):
            for j in range(max(0, i - 3), min(i + 4, len(lines))):
                m = RE_SIM_TIME.search(lines[j])
                if m:
                    handshake_end = float(m.group(1))
                    break
            if handshake_end is not None:
                break
    handshake_secs = (handshake_end - handshake_start) if (handshake_start is not None and handshake_end is not None) else None

    failsafe_count = len(RE_FAILSAFE.findall(text))

    cw_total = 0
    cw_uniform_4_of_4 = 0
    for m in RE_CW_FAIL.finditer(text):
        ok, fail = int(m.group(1)), int(m.group(2))
        cw_total += 1
        if ok == 0 and fail == 4:
            cw_uniform_4_of_4 += 1

    lts_values = [float(m.group(1)) for m in RE_LTS_CFO.finditer(text)]
    chirp_values = [float(m.group(2)) for m in RE_LTS_CFO.finditer(text)]
    chirp_zero_count = sum(1 for v in chirp_values if v == 0.0)

    tx_pending_peak = 0
    for m in RE_TX_PENDING.finditer(text):
        v = int(m.group(1))
        if v > tx_pending_peak:
            tx_pending_peak = v

    ldpc_fp = len(RE_LDPC_FALSE_POS.findall(text))
    sr_arq_oow = len(RE_SR_ARQ_OOW.findall(text))

    harq_enabled = bool(RE_HARQ_ENABLED.search(text))
    hkb_match = RE_HARQ_KEY_BUILD.search(text)
    if hkb_match:
        hkb_ok = int(hkb_match.group(1))
        hkb_fail = int(hkb_match.group(2))
    else:
        hkb_ok = hkb_fail = None

    # ---- emit summary ------------------------------------------------
    print()
    print("=" * 64)
    print(f"  CLI_SIM LOG SCAN  -  {path.name}")
    print("=" * 64)
    print(f"  Config:  SNR={snr} dB  Channel={channel}  HARQ={'on' if harq_enabled else 'off'}")
    print(f"  Verdict: TEST {verdict}" + (f"  retx={retx_tx}" if retx_tx is not None else ""))
    print()

    anomalies: list[str] = []

    # 1. Slow handshake.
    if handshake_secs is not None:
        if handshake_secs > 5.0:
            anomalies.append(
                f"Handshake duration: {handshake_secs:.1f}s (expected <5s)"
                + (f", {failsafe_count} fail-safe trigger(s) fired" if failsafe_count else "")
            )
        else:
            print(f"  ✓ Handshake: {handshake_secs:.1f}s")
    elif failsafe_count > 0:
        anomalies.append(f"{failsafe_count} handshake fail-safe trigger(s)")

    # 2. Uniform 4/4 CW failures dominating.
    if cw_total >= 3:
        pct = cw_uniform_4_of_4 * 100.0 / max(1, cw_total)
        if pct >= 60.0:
            anomalies.append(
                f"{cw_uniform_4_of_4}/{cw_total} decode failures are uniform 4/4 ({pct:.0f}%) "
                "-- looks like sync/CFO upstream of LDPC, not noise-limited"
            )
        else:
            print(f"  ✓ CW failure mix: {cw_uniform_4_of_4} uniform / {cw_total} total ({pct:.0f}%)")

    # 3. CFO budget vs channel.
    if lts_values:
        budget = CFO_BUDGET.get(channel, 0.50)
        peak = max(abs(v) for v in lts_values)
        if peak > budget * 3.0:
            anomalies.append(
                f"LTS residual CFO peak ±{peak:.2f} Hz exceeds {channel} channel budget "
                f"(~{budget:.2f} Hz); chirp reported 0.00 on {chirp_zero_count}/{len(chirp_values)} "
                "LTS events -- chirp CFO may be inert, LTS noise being trusted"
            )
        else:
            print(f"  ✓ LTS residual CFO: {fmt_hz(lts_values)}  (budget ±{budget:.2f} Hz)")

    # 4. TX_pending peak.
    if tx_pending_peak > TX_PENDING_FLAG_SAMPLES:
        secs = tx_pending_peak / 48000.0
        anomalies.append(
            f"TX_pending peak {tx_pending_peak} samples ({secs:.1f}s queued at radio) -- "
            "ARQ may be racing frames that haven't gone on-air"
        )
    elif tx_pending_peak > 0:
        print(f"  ✓ TX_pending peak: {tx_pending_peak} ({tx_pending_peak/48000.0:.2f}s)")

    # 5. HARQ verdict.
    if harq_enabled:
        if hkb_ok is None:
            anomalies.append("HARQ ENABLED at startup but no harq_key_build stats found in trailer -- HARQ may never have fired on the data-decode path")
        else:
            miss = hkb_fail * 100.0 / max(1, hkb_ok + hkb_fail)
            note = f"HARQ active: key_build success={hkb_ok} failed={hkb_fail} ({miss:.0f}% miss)"
            if miss > 30.0:
                anomalies.append(note + " -- high miss rate, CW0 failing to identify frame for keying")
            else:
                print(f"  ✓ {note}")

    # 6. Misc warnings worth surfacing.
    if ldpc_fp > 0:
        # Recovery path runs; counts > frame-count is normal under storm.
        print(f"  i LDPC false-positive recovery fired: {ldpc_fp} event(s)")
    if sr_arq_oow > 0:
        print(f"  i SR-ARQ out-of-window DATA: {sr_arq_oow} event(s)")

    print()
    if anomalies:
        print("⚠ ANOMALIES (read the full log):")
        for a in anomalies:
            print(f"  • {a}")
        print()
        print(f"→ Full log: {path}")
        print("=" * 64)
        return 1
    else:
        print("✓ No anomalies above thresholds. Tail is sufficient.")
        print("=" * 64)
        return 0


def main(argv: list[str]) -> int:
    path = Path(argv[1] if len(argv) > 1 else "/tmp/manual_run.log")
    return scan(path)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
