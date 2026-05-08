# Running ProjectUltra Alpha

## 1. Install

Download `projectultra-<platform>.zip` from the GitHub release assets and unzip
it. Use that operator bundle, not the source-code archive. It contains
`ultra_tnc`, `ultra_gui`, `ultra`, `tools/ultra_tnc.conf.example`, and these
operator docs. Install the platform audio/runtime prerequisites:

- macOS: SDL2 runtime if not bundled by the release.
- Linux: SDL2 and FFTW packages from your distro.
- Windows: the bundled `SDL2.dll` plus the Visual C++ runtime if Windows asks.

## 2. Audio Device Discovery

```bash
./ultra_tnc --list-audio-devices
```

Use the exact device names shown under Output and Input. For a TCP-only check
without a sound card, use `none` for both directions.

## 3. Config File

Start from `tools/ultra_tnc.conf.example`. Set `callsign`, `audio_output`,
`audio_input`, and optional PTT fields. Run with:

```bash
./ultra_tnc --config tools/ultra_tnc.conf.example
```

Command-line flags override config-file values.

## 4. Pat Configuration

Configure Pat for a legacy TCP TNC at `127.0.0.1:8300`. The minimal shape is:

```toml
[[transport.legacy_tnc]]
  address = "127.0.0.1:8300"
```

Keep your callsign, locator, secure login, mailbox, and session setup in Pat's
own configuration. See Pat's project docs at https://getpat.io/ and the current
`pat configure` manpage at
https://manpages.debian.org/testing/pat/pat-winlink-configure.1 for Pat-side
details.

## 5. PTT Setup

For serial RTS/DTR keying:

```bash
./ultra_tnc --config tools/ultra_tnc.conf.example \
  --ptt-serial-port /dev/cu.usbserial-FT001 \
  --ptt-serial-line rts
```

Use `--ptt-serial-line dtr` if your interface keys DTR. Add
`--ptt-inactive-high` only for an inverted interface. Without `--ptt-serial-port`,
ProjectUltra assumes VOX or external/CAT PTT.

## 6. Smoke Test

Terminal 1:

```bash
./ultra_tnc --config tools/ultra_tnc.conf.example \
  --audio-output none --audio-input none --port 18300
```

Expected startup includes `Listening: cmd=127.0.0.1:18300 data=18301`.

Terminal 2:

```bash
{ printf 'VERSION\r'; sleep 0.2; printf 'STATS\r'; sleep 0.2; } | nc 127.0.0.1 18300
```

Expected replies include `VERSION 4.9.0` and one `STATS ... backlog=0` line.
For an on-radio smoke test, run the same `ultra_tnc` command with real audio
devices, connect Pat to `127.0.0.1:8300`, and send a short local/P2P message to
the peer station.

## 7. STATS Query

```bash
{ printf 'STATS\r'; sleep 0.2; } | nc 127.0.0.1 8300
```

Expected shape:

```text
STATS frames_sent=0 frames_recv=0 retx=0 timeouts=0 failed=0 out_of_order=0 rate=R1/4 mod=DQPSK mode=OFDM-COX snr=20 bps=4000 backlog=0
```

Counters change during a real session.

## 8. Log Levels

Default output is operator-clean. Use:

```bash
./ultra_tnc --log-level debug --log-category demod --log-file /tmp/ultra_tnc.log
./ultra_gui --log-level debug --log-file /tmp/ultra_gui.log
./ultra --log-level debug info
```

Levels are `error`, `warn`, `info`, `debug`, and `trace`. Categories include
`operator`, `audio`, `tnc`, `modem`, `demod`, `sync`, `ldpc`, `channel`, and
`all`.
