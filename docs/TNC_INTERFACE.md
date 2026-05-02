# Ultra TNC Interface

`ultra_tnc` exposes a VARA-compatible TCP TNC shell backed by ProjectUltra's
ProtocolEngine, StreamingEncoder/StreamingDecoder, and SDL audio I/O.

## Run

```bash
./build/ultra_tnc \
  --audio-output "USB Audio Device" \
  --audio-input "USB Audio Device" \
  --callsign N0CALL \
  --port 8300
```

The command port is `--port` and the binary data port is `--port + 1`.
By default, `ultra_tnc` binds to `127.0.0.1:8300` and `127.0.0.1:8301`.
Use `--bind 0.0.0.0` only when you intentionally want LAN access.

For TCP-only smoke tests, use `--audio-output none --audio-input none`.

## Pat Configuration

Point Pat at the command port:

```toml
[[ax25]]
  port = "ultra"

[[transport.vara]]
  address = "127.0.0.1:8300"
```

If your Pat build separates command and data ports, use:

- Command: `127.0.0.1:8300`
- Data: `127.0.0.1:8301`

## Supported VARA Commands

The TNC shell accepts the Phase 1/2 command set:

- `VERSION`
- `MYCALL <call> [secondary...]`
- `LISTEN ON|OFF|CQ`
- `CONNECT <src> <dst>`
- `DISCONNECT`
- `ABORT`
- `BW500`, `BW2300`, `BW2750`
- `BUFFER`
- `SN`
- `BITRATE`
- `COMPRESSION`, `CHAT`, `CWID`
- `PUBLIC`, `P2P`, `WINLINK`, `IGNOREKISSDCD`, `RETRIES`, `CALLINT`

Bandwidth mapping:

- `BW500` selects `OFDM_NARROW`
- `BW2300` selects `OFDM_CHIRP`
- `BW2750` is accepted for VARA compatibility and maps to `OFDM_CHIRP`

Binary payload bytes are sent on the data port while connected. Received
ProtocolEngine binary payloads are emitted on the data port.

## Quick Check

```bash
./build/ultra_tnc --audio-output none --audio-input none --port 18300
printf 'VERSION\r' | nc 127.0.0.1 18300
```

Expected reply:

```text
VARA version 4.9.0 registered
```
