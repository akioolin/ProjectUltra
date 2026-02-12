#!/usr/bin/env python3
"""
Verify OFDM internal CFO correction dumps produced by ULTRA_DUMP_CFO_PREFIX.

Each dump index i is expected to have:
  <prefix>_i_pre.cf32   (after mixer/downconversion, before CFO correction)
  <prefix>_i_post.cf32  (after CFO correction)
  <prefix>_i_meta.txt   (optional, includes cfo_hz)

The script estimates the applied correction from phase slope of:
  post * conj(pre)
and checks it matches -expected_cfo_hz within tolerance.
"""

from __future__ import annotations

import argparse
import math
import re
import sys
from pathlib import Path
from typing import Dict, List, Optional, Tuple

import numpy as np


PRE_RE = re.compile(r"_(\d+)_pre\.cf32$")


def parse_meta(path: Path) -> Dict[str, str]:
    out: Dict[str, str] = {}
    if not path.exists():
        return out
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if "=" not in line:
            continue
        key, val = line.split("=", 1)
        out[key.strip()] = val.strip()
    return out


def weighted_fit_slope(n: np.ndarray, y: np.ndarray, w: np.ndarray) -> Tuple[float, float]:
    ws = float(np.sum(w))
    if ws <= 0.0:
        raise ValueError("non-positive weight sum")
    n_mean = float(np.sum(w * n) / ws)
    y_mean = float(np.sum(w * y) / ws)
    den = float(np.sum(w * (n - n_mean) ** 2))
    if den <= 1e-15:
        raise ValueError("degenerate weighted fit")
    num = float(np.sum(w * (n - n_mean) * (y - y_mean)))
    slope = num / den
    intercept = y_mean - slope * n_mean
    return slope, intercept


def estimate_applied_cfo_hz(pre: np.ndarray, post: np.ndarray, sample_rate: float) -> Tuple[float, float, int]:
    n = min(pre.size, post.size)
    if n < 32:
        raise ValueError("dump too short")

    pre = pre[:n]
    post = post[:n]
    ratio = post * np.conj(pre)

    mag = np.abs(pre).astype(np.float64)
    p20 = float(np.percentile(mag, 20.0))
    mask = mag > p20
    keep = int(np.sum(mask))
    if keep < max(64, int(0.2 * n)):
        raise ValueError("not enough energetic samples for estimate")

    idx = np.flatnonzero(mask).astype(np.float64)
    phase = np.unwrap(np.angle(ratio[mask]).astype(np.float64))
    weights = np.abs(ratio[mask]).astype(np.float64)

    slope, intercept = weighted_fit_slope(idx, phase, weights)
    applied_hz = slope * sample_rate / (2.0 * math.pi)

    fit = intercept + slope * idx
    residual = np.angle(np.exp(1j * (phase - fit)))
    residual_rms_deg = float(np.degrees(np.sqrt(np.mean(residual * residual))))
    return float(applied_hz), residual_rms_deg, keep


def collect_indices(prefix: str) -> List[int]:
    parent = Path(prefix).parent
    stem = Path(prefix).name
    indices: List[int] = []
    for p in sorted(parent.glob(f"{stem}_*_pre.cf32")):
        m = PRE_RE.search(p.name)
        if m:
            indices.append(int(m.group(1)))
    return sorted(set(indices))


def get_expected_cfo(meta: Dict[str, str], cli_expected: Optional[float]) -> Optional[float]:
    if cli_expected is not None:
        return cli_expected
    val = meta.get("cfo_hz")
    if val is None:
        return None
    try:
        return float(val)
    except ValueError:
        return None


def main() -> int:
    ap = argparse.ArgumentParser(description="Verify internal OFDM CFO correction from pre/post dumps")
    ap.add_argument("--prefix", required=True, help="Dump prefix (same as ULTRA_DUMP_CFO_PREFIX)")
    ap.add_argument("--expected-cfo", type=float, default=None,
                    help="Expected injected CFO in Hz (if omitted, read cfo_hz from meta)")
    ap.add_argument("--sample-rate", type=float, default=48000.0, help="Sample rate in Hz")
    ap.add_argument("--tolerance", type=float, default=0.5,
                    help="Allowed |applied - (-expected)| in Hz")
    ap.add_argument("--max-dumps", type=int, default=0, help="Optional cap of dumps to evaluate (0=all)")
    args = ap.parse_args()

    indices = collect_indices(args.prefix)
    if not indices:
        print(f"[FAIL] no dumps found for prefix: {args.prefix}")
        return 2

    if args.max_dumps > 0:
        indices = indices[:args.max_dumps]

    print(f"prefix={args.prefix}")
    print(f"dumps={indices}")
    print(f"sample_rate={args.sample_rate}")
    print(f"tolerance_hz={args.tolerance}")
    print()

    pass_count = 0
    total = 0

    for i in indices:
        total += 1
        pre_path = Path(f"{args.prefix}_{i}_pre.cf32")
        post_path = Path(f"{args.prefix}_{i}_post.cf32")
        meta_path = Path(f"{args.prefix}_{i}_meta.txt")

        if not pre_path.exists() or not post_path.exists():
            print(f"[FAIL] dump {i}: missing pre/post file")
            continue

        meta = parse_meta(meta_path)
        expected = get_expected_cfo(meta, args.expected_cfo)
        if expected is None:
            print(f"[FAIL] dump {i}: expected CFO not available (use --expected-cfo)")
            continue

        pre = np.fromfile(pre_path, dtype=np.complex64)
        post = np.fromfile(post_path, dtype=np.complex64)
        if pre.size == 0 or post.size == 0:
            print(f"[FAIL] dump {i}: empty dump file")
            continue

        try:
            applied_hz, residual_rms_deg, keep = estimate_applied_cfo_hz(pre, post, args.sample_rate)
        except Exception as exc:
            print(f"[FAIL] dump {i}: estimate failed: {exc}")
            continue

        target_hz = -expected
        err_hz = applied_hz - target_hz
        ok = abs(err_hz) <= args.tolerance
        status = "PASS" if ok else "FAIL"
        print(
            f"[{status}] dump {i}: expected={expected:.3f}Hz target={target_hz:.3f}Hz "
            f"applied={applied_hz:.3f}Hz err={err_hz:+.3f}Hz "
            f"resid_rms={residual_rms_deg:.2f}deg keep={keep}"
        )
        if ok:
            pass_count += 1

    print()
    print(f"summary: {pass_count}/{total} dumps within tolerance")
    return 0 if pass_count == total else 1


if __name__ == "__main__":
    raise SystemExit(main())
