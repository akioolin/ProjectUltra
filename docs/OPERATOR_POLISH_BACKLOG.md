# Operator polish backlog

Codex polish-priorities review, 2026-05-08 morning. Captured here so it
doesn't get re-discovered. None of these affect modem signal-processing
behavior — they are all operator-facing first-run / friction items.

Status legend: 🔴 not started · 🟡 partial · ✅ done

## Ranked list

### 1. 🔴 Operator-clean logging by default
**Wrong now:**
* Source builds default to `DEBUG` unless `NDEBUG` is set
  (`include/ultra/logging.hpp:51`).
* `ultra_tnc` has no `--log-level` flag.
* Even `INFO` level carries CW/LLR/ARQ internals — operators see modem
  autopsy by default.

**What "good" looks like:** the default console output shows startup,
audio/PTT setup, connect/disconnect, mode changes, throughput, warnings.
DSP/ARQ internals require `--log-level debug --log-category demod
--log-file ...`. Most operators never need to see them.

**Effort:** 1–2 days.

**Why it ranks #1:** first-run trust killer — users cannot distinguish
healthy operation from a debug stack trace.

### 2. 🔴 Ship the actual operator artifact
**Wrong now:**
* Release-bundle list in `.github/workflows/build-matrix.yml:215`
  *excludes* `ultra_tnc` and *includes* lab tools instead.
* Packaging scripts are GUI-only.

**What "good" looks like:** alpha bundles contain `ultra_tnc`,
`ultra_gui`, a sample config file, and a tiny `RUNNING.md`. Simulators
and bench tools become separate developer artifacts.

**Effort:** 0.5–1 day.

**Why it ranks #2:** the headless-TNC + Pat/Winlink path is the main
value to operators. Shipping it broken is the main blocker.

### 3. 🔴 One-page operator runbook
**Wrong now:**
* `docs/README.md` opens with project goals and agent docs, not
  "how to operate".
* `docs/TNC_INTERFACE.md:93` still expects a stale `VARA version...`
  output line that no longer matches reality.

**What "good" looks like:** a single-page `RUNNING.md` covering install,
audio-device discovery, config file, Pat setup, PTT, a smoke test, the
`STATS` query, and log levels. Operators should not have to mine ~14k
lines of engineering docs.

**Effort:** 1 day.

### 4. 🔴 Fix setup error messages
**Wrong now:**
* `tools/ultra_tnc.cpp:91` says only `"Failed to open audio output/input"`
  on audio failure — no device name, no next-step hint.
* `tools/ultra_tnc.conf.example:16` claims `--help` reports devices, but
  the actual flag is `--list-audio-devices`.

**What "good" looks like:** audio errors name the failing device and the
exact command to list devices. Config-file diagnostics name the line +
expected value when parsing fails.

**Effort:** 0.5–1 day.

### 5. 🔴 Guard expert PHY knobs
**Wrong now:**
* `ultra_tnc --mod` exposes `qam16` / `qam32` / `qam64`
  (`tools/ultra_tnc_config.cpp:93`) even though the README says high-order
  modes are not on the production ladder.

**What "good" looks like:** normal `--help` shows `auto` / `dqpsk` only.
Experimental modes require an explicit `--expert` flag or print a
loud forced-mode warning. Prevents self-inflicted bad reports.

**Effort:** 0.5–1 day.

### 6. 🟡 Purge stale public facts
**Wrong now:**
* The exact stale references Codex flagged on 2026-05-08 morning have
  partly been fixed by the overnight cleanup batch (CTest count, OFDM_COX
  status, FFTW fallback type, README update date).
* Remaining: cross-check rate docs vs the operator parsers (e.g., the
  rate ladder mentions or implies modes the parsers reject).

**Effort:** 0.5 day for the residual sweep.

### 7. 🔴 Split operator tools from lab tools
**Wrong now:** README presents `ultra_tnc`, GUI, simulator, and the raw
frame CLI at the same level — readers don't know which is for them.

**What "good" looks like:** operator path comes first (TNC + GUI). Lab /
simulator / bench tools are clearly marked **diagnostic**.

**Effort:** 0.5 day.

## Top 3 to ship first (Codex's call, agreed)

1. Logging operator profile (#1)
2. Release bundle with `ultra_tnc` + sample config (#2)
3. One-page operator runbook (#3)

Together: ~3 days of careful, operator-first work. None of it touches
modem signal-processing or the wire format. ctest stays green by
construction.

## What was already cleaned up overnight (so we don't re-do it)

* CTest count refs corrected.
* OFDM_COX default/forced status reconciled across docs.
* `BUILD_SYSTEM.md` FFTW fallback corrected to Cooley-Tukey.
* README OTA status refreshed.
* Historical session reports moved to `docs/archive/`.
* Naming-policy compliance pass on TNC docs.
* Local artifact gate fixed.

## Source
Codex polish-priorities opinion turn, transcript at
`/tmp/codex_polish.log` (will rotate; the full ranked list above is
the durable copy).
