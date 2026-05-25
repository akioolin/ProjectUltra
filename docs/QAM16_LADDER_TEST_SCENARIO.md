# QAM16 Speed-Ladder — Canonical Test Scenario

**Status:** active workstream (2026-05-25). Owner: QAM16 ladder.
**Do not lose this** — it is THE end-to-end scenario for validating every QAM16 rate rung.

## Strategic direction (why this matters)
The endgame is **coherent everywhere**: if QAM16/coherent OFDM can be made to work across all conditions (AWGN → Good → Moderate fading, full rate ladder), the plan is to **retire differential (DQPSK/DBPSK/MC-DPSK) entirely.** So: (a) focus is QAM16/coherent — do not invest in differential paths; (b) differential/other-waveform regressions are **not blockers** for this work (don't chase them, don't gate on them); (c) the bar for coherent is "works in all conditions," because it's meant to replace, not coexist with, differential.

## Principle (non-negotiable)
- Validate through the **real GUI auto-path** (`ultra_gui` + OTASim), because real-HF plug-in is driven by the **ladder**, not by flags. **NEVER `--expert`-force the modulation/rate.** To exercise a specific QAM16 rung, make the **ladder select it** for the test condition (edit the ladder, code-rate-agnostically). Forcing bypasses the exact code that ships.
- **Code-rate-agnostic everywhere.** "There might be more code rates." No hardcoded per-rate constants. Ladder selection, frame airtime, ARQ window/RTO, and turn-around/settle guards must all **derive from a rate descriptor**, not switch on `R1_4`/`R1_2`/etc. (Ties to task #141: the fixed-ms guards in `connection.hpp` must become airtime-derived.)

## The scenario (one full realistic QSO)
1. **Connect** — ALPHA auto-connects, BRAVO auto-accepts.
2. **Bidirectional chat** — **both** sides send **2–3 automated messages each** (exercises half-duplex turn-taking).
3. **File transfer** — **one** side (ALPHA) sends a **10 KB** file (exercises link-hold + ARQ).
4. **Disconnect** — initiator disconnects cleanly.
5. **Clean exit** — both stations exit.

Sequencing must be: **messages (both) → file (one side) → disconnect.** No hang at any boundary.

## Flags (all already exist in ultra_gui)
- BRAVO: `--auto-accept --auto-send-message --auto-message-count 3 --auto-message-interval 8 --exit-after <s>`
- ALPHA: `--auto-connect BRAVO --connect-delay 5 --auto-send-message --auto-message-count 3 --auto-message-interval 8 --auto-send-file <10KB> --auto-disconnect-after <s> --exit-after <s>`
- `--auto-disconnect-after` is wired (app.cpp:2834, gated on scenario_payload_sent_). Verify it fires **after** the file completes.

## Channel / rate selection
- OTASim `--lobby-channel awgn --lobby-snr-db 20` for the AWGN ladder bring-up.
- The QAM16 rung under test is chosen by the **ladder** for that condition (not `--expert`).

## Pass gate (per rung)
- PASS = all messages delivered both directions + 10 KB file received **CRC-OK** + clean disconnect + clean exit, no hang.
- Then **maximize goodput** for that rung (reduce overhead: warm-sync efficiency, ARQ, preamble — code-rate-agnostically).
- Climb the ladder: **R1/4 → R1/2 → R2/3 → R3/4**, each: works → max speed → next.
- **This GUI scenario is THE gate.** Do **not** gate QAM16 work on the full `ctest` (it spans other waveforms / code rates — DQPSK, etc. — not the current objective; `DecodeBenchReplay` is pre-existing-red and unrelated). Do not break QAM16/AWGN paths, but don't chase non-QAM16 regressions right now.

## Runner
- A bash script (`tools/qam16_ladder_scenario.sh`, code-rate-/channel-/SNR-parameterized) brings up OTASim + the two GUIs with the flags above and reports PASS/goodput/retx. Reuses the proven `/tmp/launch_clean_gui.sh` real-audio (NO `SDL_AUDIODRIVER=dummy`) pattern. Keep `caffeinate -dimsu` running for unattended GUI runs.

## MANDATORY: fail-fast timeouts (never run forever)
- The runner must **exit immediately on SUCCESS** (all messages delivered + file CRC-OK + clean disconnect) — a passing run takes only as long as it needs.
- `--exit-after` / wall-clock ceiling is the **FAILURE ceiling** and must be **sized to the expected scenario duration × ~1.5**, NOT a fixed oversized value (no more `--exit-after 360` for a 2-minute scenario). A failing/hung run must abort in reasonable time, not waste minutes.
- The expected duration is **rate-aware (code-rate-agnostic)**: derive it from `handshake (~25 s) + N messages + payload_bytes / selected_rate_throughput + disconnect + margin`. R1/4 legitimately takes longer than R3/4, so the ceiling scales with the rate — never hardcode one timeout for all rates.
- Poll for the success markers (msgs both ways, `Received OK … CRC`, disconnect) and for hard-failure markers (`max retries exceeded`, `transfer failed`) and break on either; only fall through to the ceiling if neither fires.

## MANDATORY: full debug logging on every run
- **Every** scenario run launches BOTH stations with `--log-level debug --log-category all --log-file <per-station>.log`. This is non-negotiable: a failure that isn't captured at full debug is not debuggable, and the whole point of the scenario is to diagnose *why* a rung fails (warm-sync misses, CW failures, ARQ/retx, turn-taking). Never run a scenario at reduced log level. Keep the alpha.log + bravo.log from every run for post-mortem.
