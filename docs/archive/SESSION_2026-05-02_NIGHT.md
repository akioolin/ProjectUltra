# Session summary — 2026-05-02 evening / 2026-05-03 night

## What shipped

10 commits on `main` since `8f7a43c`. In rough order:

| Commit | What |
|---|---|
| `e142797` | TNC: emit BUFFER N events on tick (Pat needs this) |
| `ae355cd` | Codex pat-vara audit committed as PAT_VARA_AUDIT.md |
| `6679641` | TNC: BUFFER N includes TX staging buffer |
| `72e3ba7` | TNC: comprehensive Pat E2E validation T1-T7 PASS |
| `442f3a5` | TNC: T8 root cause traced upstream to pat-vara |
| `08bf2b6` | ultra_tnc: hardware PTT (RTS/DTR) + config file |
| `05148b6` | ultra_tnc: --list-audio-devices |
| `3c338f0` | docs: MODEM_IMPROVEMENT_BACKLOG.md |
| `e9b3a93` | HARQ: fix soft-combine to sum LLRs (was averaging) |
| `1f18683` | HARQ: enable by default + add Key fields |
| `01d72a2` | HARQ: Codex review fixes — carrier_count_hash + audit |
| `882b418` | HARQ audit: append 3-seed A/B + log verification |

## Real bugs found and fixed

1. **VERSION format wrong for pat-vara dispatcher** (Mercury-style → "VERSION 4.9.0")
2. **CONNECTED line ordering** — listener side never saw inbound
3. **BUFFER N never emitted** spontaneously, only on query — Pat hangs waiting for BUFFER 0
4. **BUFFER N missed TX staging buffer** — Pat's Flush() could return early
5. **Chase combining was averaging LLRs not summing** — HARQ effectively a no-op
6. **HARQ disabled by default** in ultra_tnc — buggy code wasn't even being exercised
7. **HARQ Key missing PHY fields** — modulation/interleave/carrier-count changes could corrupt-combine

## Validation that real Pat clients work end-to-end

Mac (Pat 1.0.0) ↔ Pi5 (Pat 0.15.1) over USB cable, real audio, byte-exact:

- Empty CONNECT/DISCONNECT (13 s)
- 41 B text Mac→Pi (21 s)
- 2.6 KB text Mac→Pi gzipped to 357 B (50 s)
- 1 KB binary attachment Mac→Pi (56 s)
- 47 B reverse Pi→Mac (37 s)
- Bidirectional in single session (57 s)
- 12.5 KB Lorem ipsum Mac→Pi gzipped to 448 B, 28× ratio (50 s)
- 3× sequential connects: 1/3 (upstream pat-vara listener bug, documented)

## Headless deployment now possible

```bash
# /home/pi/.config/ultra_tnc/config
callsign        = N0CALL
audio_output    = USB Audio Device, USB Audio
audio_input     = USB Audio Device, USB Audio
ptt_serial_port = /dev/ttyUSB0
ptt_serial_line = rts
```

```bash
$ ultra_tnc
Loaded config: /home/pi/.config/ultra_tnc/config
Hardware PTT enabled on /dev/ttyUSB0 @ 9600 baud, line=rts
ultra_tnc listening on 127.0.0.1:8300 (data 8301)
```

A Pi running this is a working open-source VARA-equivalent TNC for
Pat / Winlink Express / BPQ32 clients.

## What's NOT done (open work)

- **OTA / real radio testing** — every test was over USB cable. Need
  ionospheric channel validation before declaring production-ready.
- **Pat ↔ Winlink Express** on Windows — spec-compatible, not tested.
- **HARQ throughput claim quantified on real hardware** — math fix
  shipped, code path engages on retx, but multi-seed simulator sweep
  was inconclusive on whether it materially reduces retx count.
- **Modem improvement backlog (#2-#7)** — notch filter, low-rate
  fallback, per-burst adaptation, bit loading, in-frame training,
  IR-HARQ. None started; all sized in `docs/MODEM_IMPROVEMENT_BACKLOG.md`.
- **Pat upstream patches** for the listener-Accept race (T8 finding)
  and pat-vara DEBUG silencing.

## Honest framing

The night's work was 80% Pat/TNC integration plus a real HARQ math
bug fix. The HARQ fix is mathematically correct and verifiably engages
in production code. Whether it produces measurable throughput
improvement on real channels is open — needs OTA + bigger seed sweep.

Two AI collaboration documents committed for future reference:
`docs/PAT_VARA_AUDIT.md` (Codex's full pat-vara expectations spec) and
`docs/HARQ_AUDIT_2026-05-02.md` (this audit's findings + Codex review).

If I were to recommend the next morning's work: validate the headless
ultra_tnc on a real radio (one of: VOX-driven, SignaLink, hardware-PTT
serial cable), and let real conditions surface what's actually broken.
The backlog items will land more cleanly once OTA tells us which ones
matter most.
