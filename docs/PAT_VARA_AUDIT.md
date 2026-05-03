# Pat VARA TCP TNC Expectations Audit

Sources used:

- pat-vara v1.2.0 local source: `/tmp/patvara_src/*.go`
- Pat 1.0.0 local module cache for app startup/listener behavior: `/Users/mathieuvachon/go/pkg/mod/github.com/la5nta/pat@v1.0.0`
- wl2k-go v1.0.1 local module cache for B2F behavior: `/Users/mathieuvachon/go/pkg/mod/github.com/la5nta/wl2k-go@v1.0.1`
- ProjectUltra TNC source under `src/tnc/`

## Executive Summary

Pat-vara is not a command/response protocol client in the strict sense. It writes commands as `COMMAND ...\r` and then a single command-reader goroutine publishes every modem line to prefix-based subscribers.

Hard requirements:

- TNC -> Pat command-port lines must be CR-terminated with `\r`, not `\r\n`.
- `VERSION` must answer with a line beginning `VERSION`, normally `VERSION 4.9.0\r`; `OK\r` alone is not accepted for `VERSION`.
- `CONNECT` must eventually produce `CONNECTED <initiator> <responder> ...\r` or `DISCONNECTED\r`; `OK\r` alone is not enough.
- `DISCONNECT` must eventually produce `DISCONNECTED\r`; `OK\r` alone is not enough.
- `BUFFER N\r` must represent bytes accepted from Pat but not yet fully transmitted; Pat's `Flush()` and write throttling depend on it.
- The data port is an opaque bidirectional byte stream. Pat-vara has no VARA data-port framing, marker byte, or end-of-frame event.

Important non-blocker:

- `BITRATE (0) 2300 BPS\r` is not parsed by pat-vara. It only produces the "got a vara command I wasn't expecting" debug line when `VARA_DEBUG` is enabled. It does not break Pat's state machine.

B2F correction:

- Pat's `>` prefix in logs is not on the wire. When Pat logs `>FF`, it writes `FF\r`.
- In the no-message case, the listening/master Pat side should usually answer remote `FF\r` with `FQ\r`, not another `FF\r`, because `remoteNoMsgs` is already true.

## Wire Basics

Pat -> TNC:

- `writeCmd` appends exactly `\r`: `/tmp/patvara_src/vara.go:226-233`.

TNC -> Pat:

- `cmdListen` splits incoming bytes only on `\r`: `/tmp/patvara_src/vara.go:241-263`.
- It does not trim `\n`. If the TNC sends `OK\r\nCONNECTED ...\r\n`, Pat sees lines like `"\nCONNECTED ..."` and they do not match exact or prefix dispatch. Use CR only.

Dispatcher:

- Prefix matching is done by `strings.HasPrefix`: `/tmp/patvara_src/pubsub.go:20-29`.
- Every non-empty command line is first handled by `handleCmd`, then published: `/tmp/patvara_src/vara.go:257-263`.

## 1. Async Events From TNC To Pat

All formats below are command-port lines terminated by `\r`.

| Event | Exact wire format | Pat behavior | Expected timing |
|---|---|---|---|
| PTT on/off | `PTT ON\r`, `PTT OFF\r` | Calls injected rig PTT controller. `/tmp/patvara_src/vara.go:273-278` | Whenever modem starts/stops transmitting. Optional if Pat has no PTT controller. |
| Busy on/off | `BUSY ON\r`, `BUSY OFF\r` | Sets `m.busy`; outgoing dial can wait if busy func is configured. `/tmp/patvara_src/vara.go:279-282`, `/tmp/patvara_src/transport.go:58-63` | Whenever channel busy detector changes. Missing means Pat always sees channel idle. |
| OK/WRONG | `OK\r`, `WRONG\r` | Generic no-op, except `VERSION` waits for `WRONG` as "not implemented". `/tmp/patvara_src/vara.go:283-284`, `/tmp/patvara_src/vara.go:359-369` | May be sent after accepted/rejected commands, but most commands do not wait for it. |
| Keepalive | `IAMALIVE\r` | No-op, but read deadline is two minutes. `/tmp/patvara_src/vara.go:245-247`, `/tmp/patvara_src/vara.go:285-286` | VARA spec comment says every 60 seconds. Must be under two minutes during idle command connection. |
| Pending inbound | `PENDING\r` | No-op in pat-vara v1.2.0. `/tmp/patvara_src/vara.go:287-290` | Optional pre-connect indication. `CONNECTED` is what actually creates inbound `net.Conn`. |
| Cancel pending | `CANCELPENDING\r` | No-op. `/tmp/patvara_src/vara.go:287-290` | Optional pending-call cancellation. |
| Link registration | `LINK UNREGISTERED\r`, `LINK REGISTERED\r` | No-op. `/tmp/patvara_src/vara.go:291-292` | Optional. |
| Encryption status | `ENCRYPTION DISABLED\r`, `ENCRYPTION READY\r`, `UNENCRYPTED LINK\r`, `ENCRYPTED LINK\r` | No-op. `/tmp/patvara_src/vara.go:293-296` | Optional. |
| Disconnect | `DISCONNECTED\r` | Sets disconnected, resets buffer count, resets bandwidth. Wakes close/dial/flush subscribers. `/tmp/patvara_src/vara.go:297-332` | Required when RF connect attempt fails or active link is closed. For graceful close, emit only after queued TX data is actually handled. |
| Buffer | `BUFFER <decimal-bytes>\r` | Sets buffer count via `Atoi(strings.TrimPrefix("BUFFER "))`. `/tmp/patvara_src/vara.go:300-302`, `/tmp/patvara_src/buffer_count.go:41-43` | Required whenever TX backlog changes. `BUFFER 0` is the signal `Flush()` waits for. |
| Connected | `CONNECTED <src> <dst> [anything...]\r` | Requires at least 3 space-separated fields. Uses only `<src>` and `<dst>`. `/tmp/patvara_src/vara.go:304-306`, `/tmp/patvara_src/vara.go:334-353` | Required on link establishment. For outbound, `<src>` must equal Pat's MYCALL. For inbound, `<dst>` must equal Pat's MYCALL. |
| Registered owner | `REGISTERED ...\r` | Prefix accepted; logs owner if second token exists. `/tmp/patvara_src/vara.go:308-314` | Optional. |
| Version | `VERSION ...\r` | Prefix accepted generally; `Version()` trims `VERSION `. `/tmp/patvara_src/vara.go:315-317`, `/tmp/patvara_src/vara.go:359-369` | Required response to `VERSION`. |

Events Pat does **not** parse:

- `SN ...\r`
- `BITRATE ...\r`
- `IDLE ...\r`
- Any end-of-frame/end-of-packet event

Unknown events reach `/tmp/patvara_src/vara.go:318`, which calls `debugPrint`; that only logs when `VARA_DEBUG` is enabled (`/tmp/patvara_src/debug.go:17-21`).

## 2. Pat Commands And Required Responses

Pat writes commands but usually does not synchronously read the next line as that command's response. Responses are just async events.

| Command | Pat source | Required response for Pat |
|---|---|---|
| `PUBLIC ON` | Startup: `/tmp/patvara_src/vara.go:121-123` | No awaited response. `OK\r` is fine. Avoid `WRONG\r` because it could race with later `Version()` if delivered late. |
| `CWID ON` | HF startup only: `/tmp/patvara_src/vara.go:125-129` | No awaited response. `OK\r` is fine. |
| `COMPRESSION TEXT` | Startup: `/tmp/patvara_src/vara.go:131-133` | No awaited response. `OK\r` is fine. Must remain data-port transparent to Pat. |
| `MYCALL <call>` | Startup: `/tmp/patvara_src/vara.go:135-137` | No awaited response. `OK\r` is fine. |
| `LISTEN OFF` | Startup and listener close: `/tmp/patvara_src/vara.go:139-141`, `/tmp/patvara_src/listener.go:47-51` | No awaited response. `OK\r` is fine. |
| `LISTEN ON` | Listener open: `/tmp/patvara_src/listener.go:19-26` | No awaited response. Inbound connection is delivered by later `CONNECTED <caller> <mycall> ...\r`. |
| `VERSION` | `/tmp/patvara_src/vara.go:359-369` | Must return `VERSION <text>\r` or `WRONG\r`. `OK\r` alone is ignored and causes a hang unless the modem later closes. |
| `BW500`, `BW2300`, `BW2750` | `/tmp/patvara_src/transport.go:133-140`; default reset on disconnect `/tmp/patvara_src/vara.go:328-332` | No awaited response. `OK\r` is fine. |
| `WINLINK SESSION` | Outgoing HF non-P2P: `/tmp/patvara_src/transport.go:43-55` | No awaited response. `OK\r` is fine. |
| `P2P SESSION` | Outgoing HF P2P: `/tmp/patvara_src/transport.go:43-55` | No awaited response. `OK\r` is fine. |
| `CONNECT <mycall> <target>` | `/tmp/patvara_src/transport.go:65-103` | Must eventually publish `CONNECTED ...\r` or `DISCONNECTED\r`. `OK\r` is ignored by the dial wait. |
| `DISCONNECT` | `/tmp/patvara_src/vara.go:175-195`, `/tmp/patvara_src/conn.go:99-127`, `/tmp/patvara_src/transport.go:110-120` | Must eventually publish `DISCONNECTED\r`. `OK\r` alone is ignored. |
| `ABORT` | `/tmp/patvara_src/transport.go:123-130` | Pat fakes its own `DISCONNECTED` after writing `ABORT`; no awaited modem response. `OK\r` is harmless. |
| `CHAT` | Not sent by pat-vara v1.2.0 (`rg writeCmd` found no use). | No Pat requirement. |
| `IGNOREKISSDCD` | Not sent by pat-vara v1.2.0. | No Pat requirement. |
| `RETRIES` | Not sent by pat-vara v1.2.0. | No Pat requirement. |
| `CALLINT` | Not sent by pat-vara v1.2.0. | No Pat requirement. |
| `BUFFER` | Not sent by pat-vara v1.2.0. Pat consumes async `BUFFER N`. | If supporting other clients, answer `BUFFER <decimal>\r`. |
| `SN` | Not sent by pat-vara v1.2.0. Pat does not consume async `SN`. | No Pat requirement. |
| `BITRATE` | Not sent by pat-vara v1.2.0. Pat does not consume async `BITRATE`. | No Pat requirement and no canonical Pat parse format. |

Answer to "is `OK\r` universally accepted?":

- For setup/no-op commands, yes in practice because Pat does not wait for command-specific output.
- For `VERSION`, no: Pat waits for `VERSION...` or `WRONG`.
- For `CONNECT`, no: Pat waits for `CONNECTED...` or `DISCONNECTED`.
- For `DISCONNECT`, no: Pat waits for `DISCONNECTED`.
- For `BUFFER`/`SN`/`BITRATE`, Pat v1.2.0 does not send those queries.

## 3. Data Port Semantics

Pat-vara treats the data port as a raw `net.Conn`.

- Reads call `v.dataConn.Read(b)` directly: `/tmp/patvara_src/conn.go:134-181`.
- Writes call `v.dataConn.Write(b)` directly: `/tmp/patvara_src/conn.go:184-248`.
- There is no VARA-side data-port sentinel, frame header, length prefix, or end-of-frame marker in pat-vara.

B2F itself is an in-band protocol carried over that raw stream:

- Text lines are CR-terminated, e.g. `FF\r`, `FQ\r`, `FS ...\r`.
- Binary compressed message blocks use B2F bytes `SOH=0x01`, `STX=0x02`, `EOT=0x04`: `/Users/mathieuvachon/go/pkg/mod/github.com/la5nta/wl2k-go@v1.0.1/fbb/b2f.go:48-53`, `/Users/mathieuvachon/go/pkg/mod/github.com/la5nta/wl2k-go@v1.0.1/fbb/b2f.go:431-515`.
- Those B2F bytes are application payload. The TNC must not interpret them as VARA framing.

ProjectUltra's current 1-byte `0x00`/`0x01` payload marker is not a Pat or VARA data-port feature. It is an Ultra-over-air wrapper. It is safe for local Pat only if it is always stripped before bytes are written to Pat's data socket and if both Ultra peers agree on the wrapper.

## 4. Unexpected Command Path And BITRATE

Path:

- `handleCmd` recognizes exact strings and a few prefixes: `/tmp/patvara_src/vara.go:272-319`.
- If no case matches, it calls `debugPrint("got a vara command I wasn't expecting: %q", c)`: `/tmp/patvara_src/vara.go:318`.
- `debugPrint` is disabled unless `VARA_DEBUG` parses true: `/tmp/patvara_src/debug.go:10-21`.
- The line is still published after handling: `/tmp/patvara_src/vara.go:262-263`.

Effect of `BITRATE (0) 2300 BPS\r`:

- Pat has no `BITRATE` parser or subscriber.
- It does not mutate connection state, buffer count, busy state, or PTT.
- It is benign except for debug noise.

Canonical BITRATE format for Pat:

- None in pat-vara v1.2.0. There is no accepted `BITRATE` prefix in the source.

## 5. B2F Handshake And Completion

Relevant Pat/wl2k behavior:

- `Session.Exchange` runs B2F over the raw `net.Conn` and closes the connection at the end: `/Users/mathieuvachon/go/pkg/mod/github.com/la5nta/wl2k-go@v1.0.1/fbb/wl2k.go:228-294`.
- The `>` prefix in Pat's protocol log is logging only: `/Users/mathieuvachon/go/pkg/mod/github.com/la5nta/wl2k-go@v1.0.1/fbb/b2f.go:63`. The actual write is `FF\r` or `FQ\r`: `/Users/mathieuvachon/go/pkg/mod/github.com/la5nta/wl2k-go@v1.0.1/fbb/b2f.go:64`.
- If there are no outbound messages, Pat sends `FF\r`; if it already received remote `FF`, it sends `FQ\r`: `/Users/mathieuvachon/go/pkg/mod/github.com/la5nta/wl2k-go@v1.0.1/fbb/b2f.go:55-65`.
- Receiving `FF` sets `remoteNoMsgs=true` and turns the session around: `/Users/mathieuvachon/go/pkg/mod/github.com/la5nta/wl2k-go@v1.0.1/fbb/b2f.go:235-237`.
- Receiving `FQ` marks quit received: `/Users/mathieuvachon/go/pkg/mod/github.com/la5nta/wl2k-go@v1.0.1/fbb/b2f.go:239-241`.

Specific answers:

- Does Pat-Mac need `BUFFER 0` before it sends `DISCONNECT`?  
  Not in the no-proposal `FF`/`FQ` path. `Exchange` ends and `conn.Close()` sends `DISCONNECT` after a two-second last-write guard, then waits for `DISCONNECTED`: `/tmp/patvara_src/conn.go:106-127`. For actual compressed message transfers, `writeCompressed` calls `Flush()` after EOT, and `Flush()` waits for `BUFFER 0`: `/Users/mathieuvachon/go/pkg/mod/github.com/la5nta/wl2k-go@v1.0.1/fbb/b2f.go:517-521`, `/tmp/patvara_src/conn.go:30-63`.

- Does Pat-Pi need any TNC event before writing its own response after receiving `FF`?  
  No. It only needs the data-port read to return the CR-terminated B2F line. If it has no outbound messages and has already received remote `FF`, the expected wire response is `FQ\r`, not `FF\r`.

- Is there an `IDLE` or end-of-frame event?  
  No. Pat-vara does not parse one. `BUFFER 0` is the TX-drain signal; inbound data completion is just the B2F CR line or B2F binary block framing on the data stream.

## 6. Pat Startup Ritual

Pat-vara modem startup:

1. Opens command TCP connection: `/tmp/patvara_src/vara.go:107-113`.
2. Opens data TCP connection: `/tmp/patvara_src/vara.go:115-119`.
3. Writes startup commands: `PUBLIC ON`, `CWID ON` for HF, `COMPRESSION TEXT`, `MYCALL`, `LISTEN OFF`: `/tmp/patvara_src/vara.go:121-141`.
4. Starts command reader goroutine: `/tmp/patvara_src/vara.go:144-146`.
5. Pat app calls `m.Version()` and logs `VARA modem (%s) initialized`: `/Users/mathieuvachon/go/pkg/mod/github.com/la5nta/pat@v1.0.0/app/connect.go:390-413`.
6. For a listener, Pat then sends `LISTEN ON`: `/tmp/patvara_src/listener.go:19-26`, `/Users/mathieuvachon/go/pkg/mod/github.com/la5nta/pat@v1.0.0/app/listen.go:185-192`.

Pat does not intentionally open multiple command connections for a single `Modem`. It does retry listeners in the background after failures:

- Retry loop logs `Listener varahf failed: ...` and sleeps one second: `/Users/mathieuvachon/go/pkg/mod/github.com/la5nta/pat@v1.0.0/app/listener_hub.go:66-85`.

Interpretation of observed `VARA modem () initialized` then `Listener varahf failed: modem closed`:

- Empty parentheses means `m.Version()` returned empty and Pat ignored the error at `/Users/mathieuvachon/go/pkg/mod/github.com/la5nta/pat@v1.0.0/app/connect.go:411-412`.
- `Version()` returns empty if it receives `WRONG` or if the pubsub channel closes and the receive yields the zero string: `/tmp/patvara_src/vara.go:359-369`.
- `modem closed` from listener means `m.closed` was set before `m.Listen()`: `/tmp/patvara_src/listener.go:19-23`.
- This points to the first command connection being closed or `VERSION` being rejected, not to normal Pat multi-command-client behavior.

ProjectUltra single-client behavior:

- A new command client evicts the old command client, closes the data client, and resets session state: `src/tnc/tnc_server.cpp:429-431`, `src/tnc/tnc_server.cpp:536-546`.
- That is compatible with normal Pat's one-command/one-data-connection model, but an overlapping retry or a second Pat process will reset the active session.

## Implementation Gaps / Bugs Found In `src/tnc/`

### 1. Unsolicited `BITRATE` Is Not A Pat Event

What's wrong:

- ProjectUltra emits `BITRATE (0) <bps> BPS\r`, including immediately after `CONNECTED`.
- Pat-vara v1.2.0 does not recognize any `BITRATE` line. There is no canonical Pat parse format.

Where in our code:

- Emission format: `src/tnc/tnc_session.cpp:506-513`.
- Emitted after connect: `src/tnc/tnc_bridge.cpp:428-431`.
- Event plumbing: `src/tnc/tnc_server.cpp:271-275`, `src/tnc/tnc_server.cpp:738-740`.

Where in pat-vara source:

- Recognized events omit `BITRATE`: `/tmp/patvara_src/vara.go:272-319`.
- Unknown command debug path: `/tmp/patvara_src/vara.go:318`.

Severity:

- Cosmetic for Pat. It does not break connection, B2F, buffer accounting, or disconnect.

Suggested fix:

- Stop emitting unsolicited `BITRATE` on Pat-compatible sessions, especially immediately after `CONNECTED`.
- Keep `BITRATE` only as a ProjectUltra/diagnostic extension or answer it only when explicitly queried by a non-Pat client.

### 2. `SN` Is Also Not A Pat Event

What's wrong:

- ProjectUltra can emit `SN <db>\r`.
- Pat-vara v1.2.0 does not recognize `SN`.

Where in our code:

- Emission format: `src/tnc/tnc_session.cpp:499-503`.
- Async emission when chat is enabled: `src/tnc/tnc_session.cpp:350-354`.
- Query response: `src/tnc/tnc_session.cpp:731-738`.

Where in pat-vara source:

- Recognized events omit `SN`: `/tmp/patvara_src/vara.go:272-319`.

Severity:

- Cosmetic for Pat. Current Pat does not send `CHAT ON` or `SN`, so this should not appear unless another client enables it or a user queries it.

Suggested fix:

- Do not emit unsolicited `SN` in Pat compatibility mode.
- If kept for diagnostics, document it as a non-Pat extension.

### 3. Missing `BUSY ON/OFF`

What's wrong:

- Pat-vara recognizes `BUSY ON\r` and `BUSY OFF\r` and can use this to delay outgoing dials.
- ProjectUltra has no `BUSY` event type or emission path in the TNC layer.

Where in our code:

- Event enum has no Busy event: `src/tnc/tnc_events.hpp:24-36`.
- Dispatch has no Busy case: `src/tnc/tnc_server.cpp:721-750`.

Where in pat-vara source:

- Busy state parsing: `/tmp/patvara_src/vara.go:279-282`.
- Outgoing dial checks busy func: `/tmp/patvara_src/transport.go:58-63`.

Severity:

- Nice-to-have for Pat. Missing `BUSY` does not explain a post-B2F hang; it only means Pat assumes the channel is clear.

Suggested fix:

- If ProjectUltra has carrier/busy detection, add `BUSY ON/OFF\r` emission on state changes.
- If not, leave absent rather than fabricating it.

### 4. `BUFFER N` Does Not Appear To Include TNCSession's Local Staging Buffer

What's wrong:

- Pat's `conn.Write` increments its local buffer count by bytes written to the data TCP socket before the TNC has necessarily handed those bytes to the modem: `/tmp/patvara_src/conn.go:244-248`.
- Pat's `Flush()` trusts async `BUFFER 0` as "all accepted bytes have transmitted": `/tmp/patvara_src/conn.go:30-63`.
- ProjectUltra buffers data-port bytes locally for up to 200 ms before calling `modem_.sendBinary`.
- `BUFFER` responses/events appear to report only ProtocolEngine backlog, not `TNCSession::data_tx_buffer_`.

Where in our code:

- Local staging append: `src/tnc/tnc_session.cpp:252-260`.
- Quiet-period flush to modem: `src/tnc/tnc_session.cpp:390-424`.
- `BUFFER` query uses only modem backlog: `src/tnc/tnc_session.cpp:723-729`.
- Async backlog polling uses only engine backlog: `src/tnc/tnc_bridge.cpp:357-367`.

Where in pat-vara source:

- `Write` increments buffer count before TCP write returns: `/tmp/patvara_src/conn.go:244-248`.
- `Flush` waits until parsed `BUFFER` count reaches zero: `/tmp/patvara_src/conn.go:30-63`.
- B2F compressed message send calls `Flush`: `/Users/mathieuvachon/go/pkg/mod/github.com/la5nta/wl2k-go@v1.0.1/fbb/b2f.go:517-521`.

Severity:

- Blocker for correct message-transfer flush semantics.
- Probably not the direct cause of a no-proposal `FF`/`FQ` hang, because that path does not call `Flush()`, but it will matter as soon as real messages are transferred.

Suggested fix:

- Make `BUFFER N` include all bytes accepted from Pat but not yet fully transmitted, including TNCSession's local staging buffer and ProtocolEngine backlog.
- Alternatively, remove the 200 ms staging delay for Pat data or force-flush the staging buffer before reporting/querying `BUFFER 0`.

### 5. `COMPRESSION TEXT` Enables A Non-VARA Ultra Payload Marker

What's wrong:

- Pat sends `COMPRESSION TEXT` as a normal VARA startup command.
- ProjectUltra interprets any non-`OFF` compression mode as enabling a custom deflate wrapper with a leading `0x00` raw marker or `0x01` deflate marker on modem-side payloads.
- Pat-vara expects the data TCP stream to be transparent. The marker is not a Pat/VARA data-port feature.

Where in our code:

- Marker definitions and comment: `src/tnc/tnc_session.cpp:20-25`.
- RX marker stripping/decompression: `src/tnc/tnc_session.cpp:303-329`.
- TX marker insertion/compression: `src/tnc/tnc_session.cpp:398-424`.
- `COMPRESSION TEXT` enables it: `src/tnc/tnc_session.cpp:654-674`.

Where in pat-vara source:

- Pat writes `COMPRESSION TEXT` at startup: `/tmp/patvara_src/vara.go:131-133`.
- Data port write/read are raw TCP stream operations: `/tmp/patvara_src/conn.go:134-181`, `/tmp/patvara_src/conn.go:184-248`.

Severity:

- Blocker for VARA-compatible semantics with any peer that does not implement the exact Ultra marker wrapper.
- For current Ultra-to-Ultra tests it can work only because both TNCs agree on the private marker and strip it before Pat sees data.

Suggested fix:

- Treat VARA `COMPRESSION TEXT/FILES/ON/OFF` as a modem compatibility setting, not as permission to alter the data-port wire protocol visible between TNCs.
- If Ultra compression is desired, negotiate it explicitly at the Ultra protocol layer, not solely from Pat's VARA `COMPRESSION` command.
- The minimal compatibility fix is to make `COMPRESSION TEXT` a no-op/OK for Pat sessions and send raw B2F bytes over the Ultra link.

### 6. No Evidence Pat Needs An `IDLE` Or End-Of-Frame Event

What's wrong:

- Nothing in ProjectUltra for this item. This is a negative finding.

Where in our code:

- No action needed.

Where in pat-vara source:

- Recognized events list has no `IDLE`/EOF/end-frame event: `/tmp/patvara_src/vara.go:272-319`.

Severity:

- Not a bug.

Suggested fix:

- Do not add an `IDLE` or end-of-frame command-port event for Pat. It will be unknown noise.

## Notes On Current ProjectUltra Emissions

Current ProjectUltra lines that match Pat:

- `OK\r`: `src/tnc/tnc_session.cpp:442-445`
- `WRONG\r`: `src/tnc/tnc_session.cpp:448-451`
- `VERSION 4.9.0\r`: `src/tnc/tnc_session.cpp:454-464`
- `CONNECTED <src> <dst> <bw>\r`: `src/tnc/tnc_session.cpp:467-471`; Pat ignores the extra bandwidth token.
- `DISCONNECTED\r`: `src/tnc/tnc_session.cpp:474-477`
- `PTT ON/OFF\r`: `src/tnc/tnc_session.cpp:480-483`
- `BUFFER N\r`: `src/tnc/tnc_session.cpp:486-496`
- `IAMALIVE\r`: `src/tnc/tnc_session.cpp:516-519`
- `PENDING\r`, `CANCELPENDING\r`: `src/tnc/tnc_session.cpp:522-530`

Current ProjectUltra lines that Pat does not recognize:

- `SN <db>\r`: `src/tnc/tnc_session.cpp:499-503`
- `BITRATE (0) <bps> BPS\r`: `src/tnc/tnc_session.cpp:506-513`
- `STATS ...\r`: `src/tnc/tnc_session.cpp:796-815` if a Pat user explicitly sends `STATS`; Pat itself does not.

## Most Likely Explanations For The Observed Hang

Based on source alone:

1. `BITRATE (0) 2300 BPS` is not the blocker. It is unknown debug noise only.
2. If the Pi Pat truly received `FF\r` on its data socket and had no outbound messages, it should write `FQ\r` with no TNC event prerequisite.
3. If Pi Pat does not write `FQ\r`, suspect that the `FF\r` bytes are not reaching Pat's data-port `Read` exactly as a CR-terminated B2F line, or that the Pi B2F session is not in `handleInbound` at that point.
4. For no-message teardown, Mac Pat does not need `BUFFER 0` before sending `DISCONNECT`; it needs `DISCONNECTED` after it asks to close.
5. For future real message transfers, `BUFFER` accounting must include all local TNC staging bytes or Pat's B2F `Flush()` can return early.
