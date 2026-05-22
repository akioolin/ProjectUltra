#!/usr/bin/env python3
"""Run forced coherent OFDM ladder sweeps and summarize cli_simulator output.

The script is intentionally measurement-only: it shells out to cli_simulator,
saves raw logs, and extracts the same on-air/ARQ/decoder counters printed by the
simulator summary.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import csv
import os
import re
import signal
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from statistics import mean
from typing import Iterable


ANSI_RE = re.compile(r"\x1b\[[0-9;]*m")
GOODPUT_RE = re.compile(
    r"On-air goodput:\s+(?P<bytes>\d+) bytes in (?P<seconds>[0-9.]+)s =\s+"
    r"(?P<bps>\d+) bps"
)
SECTION_RE = re.compile(r"---\s+(?P<label>ALPHA|BRAVO)(?:\s+\([^)]*\))?\s+---")
KEYVAL_RE = re.compile(r"([A-Za-z_]+)=([0-9.]+)")
SUCCESS_RE = re.compile(r"frame_success=([0-9.]+)%")


@dataclass(frozen=True)
class Case:
    channel: str
    mod: str
    rate: str
    snr: float
    file_size: int
    seed: int

    @property
    def key(self) -> str:
        snr_token = ("%g" % self.snr).replace(".", "p")
        return (
            f"{self.channel}_{self.mod}_{self.rate}_snr{snr_token}_"
            f"file{self.file_size}_seed{self.seed}"
        )


def parse_list(value: str) -> list[str]:
    return [part.strip() for part in value.split(",") if part.strip()]


def parse_float_list(value: str) -> list[float]:
    return [float(part) for part in parse_list(value)]


def parse_int_list(value: str) -> list[int]:
    return [int(part) for part in parse_list(value)]


def parse_cells(value: str) -> list[tuple[str, str]]:
    cells: list[tuple[str, str]] = []
    for token in parse_list(value):
        if ":" not in token:
            raise argparse.ArgumentTypeError(
                f"cell '{token}' must use mod:rate, e.g. qam16:r1_2"
            )
        mod, rate = token.split(":", 1)
        cells.append((mod.strip(), rate.strip()))
    return cells


def strip_ansi(text: str) -> str:
    return ANSI_RE.sub("", text)


def parse_log(text: str) -> dict[str, str | int | float]:
    clean = strip_ansi(text)
    out: dict[str, str | int | float] = {}

    goodput = GOODPUT_RE.search(clean)
    if goodput:
        out["goodput_bps"] = int(goodput.group("bps"))
        out["on_air_seconds"] = float(goodput.group("seconds"))

    out["test_passed"] = "yes" if "TEST PASSED" in clean else "no"
    out["test_failed"] = "yes" if "TEST FAILED" in clean else "no"

    current_section: str | None = None
    for line in clean.splitlines():
        section = SECTION_RE.search(line)
        if section:
            current_section = section.group("label").lower()
            continue
        if current_section not in {"alpha", "bravo"}:
            continue

        prefix = current_section
        if "ARQ:" in line:
            for key, value in KEYVAL_RE.findall(line):
                out[f"{prefix}_arq_{key}"] = int(float(value))
        elif "RX:" in line:
            for key, value in KEYVAL_RE.findall(line):
                if key in {"frames_decoded", "frames_failed"}:
                    out[f"{prefix}_rx_{key}"] = int(float(value))
        elif "Rate: frame_success=" in line:
            match = SUCCESS_RE.search(line)
            if match:
                out[f"{prefix}_frame_success_pct"] = float(match.group(1))

    return out


def command_for(sim: Path, case: Case) -> list[str]:
    return [
        str(sim),
        "--expert",
        "--mod",
        case.mod,
        "--rate",
        case.rate,
        "--channel",
        case.channel,
        "--snr",
        "%g" % case.snr,
        "--file",
        str(case.file_size),
        "--seed",
        str(case.seed),
    ]


def run_case(
    sim: Path,
    out_dir: Path,
    timeout_sec: int,
    case: Case,
    reuse_logs: bool,
) -> dict[str, str | int | float]:
    cmd = command_for(sim, case)
    log_path = out_dir / "logs" / f"{case.key}.log"
    log_path.parent.mkdir(parents=True, exist_ok=True)

    if reuse_logs and log_path.exists():
        clean = log_path.read_text(encoding="utf-8", errors="replace")
        parsed = parse_log(clean)
        reused_status = "reused"
        if parsed.get("test_passed") != "yes":
            reused_status = "reused_incomplete"
        parsed.update(
            {
                "channel": case.channel,
                "mod": case.mod,
                "rate": case.rate,
                "snr": case.snr,
                "file_size": case.file_size,
                "seed": case.seed,
                "status": reused_status,
                "returncode": "",
                "command": " ".join(cmd),
                "log_path": str(log_path),
            }
        )
        return parsed

    timed_out = False
    proc = subprocess.Popen(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        cwd=Path.cwd(),
        start_new_session=True,
    )
    try:
        stdout, _ = proc.communicate(timeout=timeout_sec)
    except subprocess.TimeoutExpired:
        timed_out = True
        try:
            os.killpg(proc.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        try:
            stdout, _ = proc.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(proc.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            stdout, _ = proc.communicate()

    clean = strip_ansi(stdout)
    log_path.write_text(clean, encoding="utf-8")

    parsed = parse_log(clean)
    parsed.update(
        {
            "channel": case.channel,
            "mod": case.mod,
            "rate": case.rate,
            "snr": case.snr,
            "file_size": case.file_size,
            "seed": case.seed,
            "status": "timeout" if timed_out else ("ok" if proc.returncode == 0 else "error"),
            "returncode": -1 if timed_out else proc.returncode,
            "command": " ".join(cmd),
            "log_path": str(log_path),
        }
    )
    return parsed


CSV_FIELDS = [
    "channel",
    "mod",
    "rate",
    "snr",
    "file_size",
    "seed",
    "status",
    "returncode",
    "test_passed",
    "goodput_bps",
    "on_air_seconds",
    "alpha_arq_frames_sent",
    "alpha_arq_retransmissions",
    "alpha_arq_timeouts",
    "alpha_arq_failed",
    "bravo_rx_frames_decoded",
    "bravo_rx_frames_failed",
    "bravo_frame_success_pct",
    "command",
    "log_path",
]


def csv_value(row: dict[str, str | int | float], field: str) -> str | int | float:
    return row.get(field, "")


def write_csv(path: Path, rows: list[dict[str, str | int | float]]) -> None:
    with path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=CSV_FIELDS)
        writer.writeheader()
        for row in rows:
            writer.writerow({field: csv_value(row, field) for field in CSV_FIELDS})


def group_rows(
    rows: Iterable[dict[str, str | int | float]]
) -> dict[tuple[str, str, str, float, int], list[dict[str, str | int | float]]]:
    groups: dict[tuple[str, str, str, float, int], list[dict[str, str | int | float]]] = {}
    for row in rows:
        key = (
            str(row["channel"]),
            str(row["mod"]),
            str(row["rate"]),
            float(row["snr"]),
            int(row["file_size"]),
        )
        groups.setdefault(key, []).append(row)
    return groups


def fmt_float(value: float) -> str:
    return "%g" % value


def write_markdown(path: Path, rows: list[dict[str, str | int | float]]) -> None:
    groups = group_rows(rows)
    lines: list[str] = [
        "# Coherent Ladder Sweep Summary",
        "",
        "Generated by `tools/sweep_coherent_ladder.py`.",
        "",
        "## Aggregate",
        "",
        "| channel | mod | rate | snr | file | seeds | mean bps | spread | retx mean | timeouts mean | frame success | failed seeds |",
        "|---|---|---|---:|---:|---|---:|---:|---:|---:|---|---:|",
    ]
    for key in sorted(groups):
        channel, mod, rate, snr, file_size = key
        group = sorted(groups[key], key=lambda row: int(row["seed"]))
        goodputs = [
            int(row["goodput_bps"])
            for row in group
            if str(row.get("goodput_bps", "")).strip()
        ]
        retx = [
            int(row["alpha_arq_retransmissions"])
            for row in group
            if str(row.get("alpha_arq_retransmissions", "")).strip()
        ]
        timeouts = [
            int(row["alpha_arq_timeouts"])
            for row in group
            if str(row.get("alpha_arq_timeouts", "")).strip()
        ]
        success = [
            float(row["bravo_frame_success_pct"])
            for row in group
            if str(row.get("bravo_frame_success_pct", "")).strip()
        ]
        failed_seeds = sum(1 for row in group if row.get("test_passed") != "yes")
        mean_bps = round(mean(goodputs)) if goodputs else ""
        spread = (max(goodputs) - min(goodputs)) if goodputs else ""
        retx_mean = f"{mean(retx):.1f}" if retx else ""
        timeout_mean = f"{mean(timeouts):.1f}" if timeouts else ""
        success_text = (
            f"{min(success):.1f}-{max(success):.1f}%" if success else ""
        )
        seeds = ",".join(str(int(row["seed"])) for row in group)
        lines.append(
            f"| {channel} | {mod} | {rate} | {fmt_float(snr)} | {file_size} | {seeds} | "
            f"{mean_bps} | {spread} | {retx_mean} | {timeout_mean} | "
            f"{success_text} | {failed_seeds} |"
        )

    lines.extend(
        [
            "",
            "## Per Seed",
            "",
            "| channel | mod | rate | snr | file | seed | status | bps | retx | timeouts | frames_failed | frame_success |",
            "|---|---|---|---:|---:|---:|---|---:|---:|---:|---:|---:|",
        ]
    )
    for row in sorted(
        rows,
        key=lambda r: (
            str(r["channel"]),
            float(r["snr"]),
            str(r["mod"]),
            str(r["rate"]),
            int(r["file_size"]),
            int(r["seed"]),
        ),
    ):
        lines.append(
            f"| {row['channel']} | {row['mod']} | {row['rate']} | "
            f"{fmt_float(float(row['snr']))} | {row['file_size']} | {row['seed']} | "
            f"{row.get('status', '')} | {row.get('goodput_bps', '')} | "
            f"{row.get('alpha_arq_retransmissions', '')} | {row.get('alpha_arq_timeouts', '')} | "
            f"{row.get('bravo_rx_frames_failed', '')} | {row.get('bravo_frame_success_pct', '')} |"
        )

    lines.extend(["", "## Commands", ""])
    for row in sorted(rows, key=lambda r: str(r["command"])):
        lines.append(f"- `{row['command']}`")

    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def build_cases(args: argparse.Namespace) -> list[Case]:
    cells = parse_cells(args.cells)
    snrs = parse_float_list(args.snrs)
    seeds = parse_int_list(args.seeds)
    channels = parse_list(args.channels)
    cases: list[Case] = []
    for channel in channels:
        for mod, rate in cells:
            for snr in snrs:
                for seed in seeds:
                    cases.append(Case(channel, mod, rate, snr, args.file_size, seed))
    return cases


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sim", default="./build/cli_simulator", help="cli_simulator path")
    parser.add_argument(
        "--out-dir",
        default="/tmp/coherent_ladder_floor_map_2026_05_22",
        help="output directory for logs, CSV, and summary markdown",
    )
    parser.add_argument("--channels", default="good", help="comma-separated channels")
    parser.add_argument(
        "--cells",
        default="qpsk:r1_2,8psk:r1_2,qam16:r1_2",
        help="comma-separated mod:rate cells, e.g. qpsk:r1_2,8psk:r2_3",
    )
    parser.add_argument("--snrs", default="20,17,14,11,8", help="comma-separated SNR dB values")
    parser.add_argument("--seeds", default="42,43,44", help="comma-separated seeds")
    parser.add_argument("--file-size", type=int, default=5120, help="file size in bytes")
    parser.add_argument("--jobs", type=int, default=1, help="parallel simulator jobs")
    parser.add_argument("--timeout-sec", type=int, default=420, help="per-run timeout")
    parser.add_argument(
        "--reuse-logs",
        action="store_true",
        help="reuse matching existing raw logs instead of rerunning those cases",
    )
    args = parser.parse_args()

    sim = Path(args.sim)
    if not sim.exists():
        print(f"simulator not found: {sim}", file=sys.stderr)
        return 2

    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    cases = build_cases(args)
    print(f"Running {len(cases)} cases into {out_dir}", flush=True)

    def record(row: dict[str, str | int | float]) -> None:
        rows.append(row)
        write_csv(out_dir / "results.csv", rows)
        write_markdown(out_dir / "summary.md", rows)

    rows: list[dict[str, str | int | float]] = []
    if args.jobs <= 1:
        for index, case in enumerate(cases, start=1):
            print(f"[{index}/{len(cases)}] {case.key}", flush=True)
            record(run_case(sim, out_dir, args.timeout_sec, case, args.reuse_logs))
    else:
        with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as executor:
            future_map = {
                executor.submit(
                    run_case, sim, out_dir, args.timeout_sec, case, args.reuse_logs
                ): case
                for case in cases
            }
            completed = 0
            for future in concurrent.futures.as_completed(future_map):
                completed += 1
                case = future_map[future]
                print(f"[{completed}/{len(cases)}] {case.key}", flush=True)
                record(future.result())

    rows.sort(
        key=lambda r: (
            str(r["channel"]),
            float(r["snr"]),
            str(r["mod"]),
            str(r["rate"]),
            int(r["file_size"]),
            int(r["seed"]),
        )
    )
    write_csv(out_dir / "results.csv", rows)
    write_markdown(out_dir / "summary.md", rows)
    print(f"Wrote {out_dir / 'results.csv'}")
    print(f"Wrote {out_dir / 'summary.md'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
