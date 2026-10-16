# Photon wire protocol reference

Status 2026-10-12: research spike for the realtime-photon plan, Task 1. Every byte value in
`realtime/src/protocol/constants.ts` must trace to a citation in this document. If a later task
needs a value that isn't here, add it here first (with its source), then import it by name —
never hardcode a raw byte in transport/codec code.

Scope reminder from `docs/online.md`: the real iOS client (Photon3Unity3D, bundled in the 2.28.0
binary) has `ConnectionProtocol.Udp = 0` and `.Tcp = 1` only — no WebSocket, no `Protocol18` class.
`LoadBalancingClient(protocol = 0)` defaults to UDP. So the protocol we must speak is **Protocol16
over an ENet-style reliable UDP transport**, not Photon's newer WebSocket/Protocol18 stack. That
narrows which "prior art" below is actually load-bearing (UDP/Protocol16) versus merely useful for
cross-checking operation/event/parameter codes (which are serialization-independent).

## Prior art

Searched npm and GitHub for: `photon-client`, `photon-realtime`, `enet-js`, `photon protocol16`,
`photon packet parser`, `photon protocol16 node.js`.

| Project | Language | License | Client/Server | Verdict |
|---|---|---|---|---|
| [`photon-realtime`](https://www.npmjs.com/package/photon-realtime) (npm, v4.4.0) | JS | "SEE LICENSE IN https://www.photonengine.com/terms" (proprietary Exit Games EULA, not OSS) | Client (official Exit Games SDK) | **Used as primary source for LoadBalancing operation/event/parameter codes** (see below). Not usable as a dependency (license, and it's a WebSocket+JSON-ish client, not the UDP/Protocol16 client we need — see note below). Downloaded the published tarball from the npm registry and read `photon-realtime-module.js` directly; this is the real, officially-published source, not a guess. |
| [`0blu/PhotonPackageParser`](https://github.com/0blu/PhotonPackageParser) | C# | MIT | Neither — a passive packet sniffer/parser for reverse-engineering Photon traffic (built for game RE, e.g. Albion Online-style tools) | **Used as primary source for Protocol16 byte layout and ENet-style command/datagram framing.** Clean room-style reimplementation with its own type/command enums and a from-scratch deserializer (`Protocol16Deserializer.cs`, `PhotonParser.cs`). Has its own test suite (`Protocol16.Tests/`). Not a client or server we could depend on, but directly readable source for exact byte offsets. |
| [`0xN0x/photon-packet-parser`](https://github.com/0xN0x/photon-packet-parser) | JS/Node | unspecified in README | Passive parser, same genre as above | Found, not fetched in depth — `0blu`'s C# parser already gave us verifiable byte-level detail with a readable license; didn't need a second parser for the same facts. Worth returning to if a gap turns up later. |
| [`nyxikitty/photon-server-nodejs`](https://github.com/nyxikitty/photon-server-nodejs) | JS/Node | MIT | Server (self-described "GPBinaryV16 Photon Server implementation", built for an old VRChat build) | **Found, read, and explicitly rejected as a source.** Its `src/protocol/constants.js` disagrees with the authoritative `photon-realtime` figures in several places — e.g. it has `RAISE_EVENT: 255` (actually 253; 255 is `Join`/`LiteOpKey.GameId` territory) and both `LEAVE_ROOM` and `CREATE_ROOM` set to `227`. Low adoption (4 stars, 13 commits, no releases). Its `PHOTON_COMMANDS` enum does independently corroborate the ENet command-type values 1–8 (see below), but is not trustworthy beyond that overlap. **This is a genuine negative finding, not a wasted search**: it's a concrete example of why every value here is cross-checked against at least one source we trust, rather than copied from the first match. |
| [`photonsh/photon-node`](https://github.com/photonsh/photon-node) | JS | MIT | N/A | **False positive.** Interfaces with a SaaS product at photon.sh, unrelated to Exit Games' Photon Engine. Ruled out. |
| [`lsalzman/enet`](https://github.com/lsalzman/enet) | C | MIT | The actual open-source ENet library (not Photon, but the protocol family Photon's transport is modeled on) | **Used as the authoritative source for ENet's own command-type enum and default timeout/ping constants**, and as the baseline to show exactly where Photon's transport diverges from vanilla ENet (see "ENet-style transport" below — the divergence matters, don't assume byte-compatibility). |

**No usable Node/JS Photon *server* implementation exists** that we could depend on or faithfully
mirror: the one Node server we found (`nyxikitty/photon-server-nodejs`) contains internally
inconsistent protocol constants and isn't trustworthy as a reference beyond the command-type
overlap noted above. This project has to build its own Protocol16 codec and ENet-style transport
from the primary sources in the table above, cross-checked against each other — which is exactly
what Tasks 2+ of this plan do.

## Protocol16 encoding

Source: [`0blu/PhotonPackageParser`, `Protocol16/Protocol16Type.cs`](https://github.com/0blu/PhotonPackageParser/blob/master/Protocol16/Protocol16Type.cs)
and [`Protocol16/Protocol16Deserializer.cs`](https://github.com/0blu/PhotonPackageParser/blob/master/Protocol16/Protocol16Deserializer.cs)
(MIT). Cross-checked against `nyxikitty/photon-server-nodejs`'s `src/protocol/constants.js`
`PHOTON_TYPES` object, which agrees on every byte value both sources define (see `nyx_constants.js`
note below) — two independent reverse-engineering efforts landed on the same numbers, which is as
close to confirmation as we can get without a vendor spec. Several of the byte values are the
ASCII code of a letter that reads naturally for the type (`'s'`=0x73 for String, `'i'`=0x69 for
Integer, `'b'`=0x62 for Byte) — but this is **not a reliable derivation method**: several others
don't fit any obvious mnemonic (`Boolean`=0x6F=`'o'`, `Array`=0x79=`'y'`, `ObjectArray`=0x7A=`'z'`,
`IntegerArray`=0x6E=`'n'`, `StringArray`=0x61=`'a'`). Treat every value in the table below as
sourced from the citations, not as something derivable from the type's name.

Every value is a single tag byte followed by type-specific payload. All multi-byte integers are
**big-endian** (confirmed by the byte-swap code in `Protocol16Deserializer.cs`'s
`DeserializeFloat`/`DeserializeDouble`/`DeserializeLong`, which explicitly reverses byte order on
little-endian hosts before calling `BitConverter`).

| Type | Tag (hex) | Tag (dec) | Payload after the tag |
|---|---|---|---|
| Null | `0x2A` | 42 | none |
| Boolean | `0x6F` | 111 | 1 byte: `0` = false, nonzero = true |
| Byte | `0x62` | 98 | 1 byte |
| Short | `0x6B` | 107 | 2 bytes, big-endian signed |
| Integer | `0x69` | 105 | 4 bytes, big-endian signed |
| Float | `0x66` | 102 | 4 bytes, big-endian IEEE-754 |
| Double | `0x64` | 100 | 8 bytes, big-endian IEEE-754 |
| String | `0x73` | 115 | 2-byte big-endian length (as a Short), then that many UTF-8 bytes |
| Hashtable | `0x68` | 104 | 2-byte big-endian short = element count, then for each element: a nested type-tagged key followed by a nested type-tagged value (i.e. every key/value is self-describing — see `DeserializeHashtable`/`DeserializeDictionaryElements`) |
| Array | `0x79` | 121 | 2-byte big-endian short = element count, then **one** type tag byte for the element type, then that many untagged values of that type (homogeneous array; nested `Array`/`ByteArray`/`Dictionary` element types get special-cased recursion) |
| ObjectArray | `0x7A` | 122 | 2-byte big-endian short = element count, then for each element: a type tag byte followed by that element's value (heterogeneous — every element is individually tagged, unlike `Array`) |
| IntegerArray | `0x6E` | 110 | 4-byte big-endian int = element count, then that many untagged 4-byte big-endian ints |
| StringArray | `0x61` | 97 | 2-byte big-endian short = element count, then that many (length-prefixed) strings, each as described under String but without its own leading tag byte |

Also present in the source (not in this plan's required minimum, but captured in
`constants.ts` since downstream codec/transport tasks will need at least `Long` and the three
"envelope" types — `OperationRequest`/`OperationResponse`/`EventData` — to decode a `SendReliable`
command's payload; see "ENet-style transport" below):

| Type | Tag (hex) | Tag (dec) | Payload after the tag |
|---|---|---|---|
| Dictionary | `0x44` | 68 | 1 byte key-type tag, 1 byte value-type tag (`0x00`/`Unknown` for "mixed, tag each element individually"), then 2-byte big-endian short = element count, then elements (tagged individually if key/value type was `Unknown`, untagged otherwise) |
| Long | `0x6C` | 108 | 8 bytes, big-endian signed |
| ByteArray | `0x78` | 120 | 4-byte big-endian int = length, then that many raw bytes (no further tagging) |
| EventData | `0x65` | 101 | 1 byte event code, then a parameter table (2-byte big-endian short = count, then for each entry: 1 byte parameter key, 1 byte value-type tag, tagged value) — see "Operations in scope" |
| OperationRequest | `0x71` | 113 | 1 byte operation code, then a parameter table (same shape as above) |
| OperationResponse | `0x70` | 112 | 1 byte operation code, 2-byte big-endian short return code, a type-tagged debug message (`String` or `Null`), then a parameter table |

Cross-check artifact: `nyxikitty/photon-server-nodejs`'s `src/protocol/constants.js` `PHOTON_TYPES`
(fetched to `nyx_constants.js` in the research scratchpad, not committed — the file itself is
already untrustworthy per the Prior Art table, it's cited here only for the specific byte values it
happens to share with `0blu`'s parser: `NULL 0x2A`, `BYTE 0x62`, `DOUBLE 0x64`, `FLOAT 0x66`,
`HASH_TABLE 0x68`, `INTEGER 0x69`, `SHORT 0x6B`, `LONG 0x6C`, `INT_ARRAY 0x6E`, `BOOLEAN 0x6F`,
`STRING 0x73`, `BYTE_ARRAY 0x78`, `ARRAY 0x79`, `OBJECT_ARRAY 0x7A`, `STRING_ARRAY 0x61`,
`DICTIONARY 0x44` — every one matches `0blu`'s values exactly).

## ENet-style transport

**Important caveat, stated up front:** Photon's transport is *conceptually* ENet (peer-based
reliable/unreliable UDP with channels, sequence numbers and acknowledgements) and its
**command-type numbering matches vanilla ENet exactly** (cross-checked against the real
[lsalzman/enet](https://github.com/lsalzman/enet), MIT), but the **actual datagram and command
header byte layouts are Photon's own, and do not match vanilla ENet's structs field-for-field.**
Do not assume wire-compatibility with libenet beyond the command-type enum. This is a real,
important distinction found during this research, not a guess.

### UDP datagram header (12 bytes)

Source: [`0blu/PhotonPackageParser`, `PhotonParser.cs`](https://github.com/0blu/PhotonPackageParser/blob/master/PhotonPackageParser/PhotonParser.cs)
(`PhotonHeaderLength = 12`, `ReceivePacket`). This is Photon's own outer header — vanilla ENet's
`ENetProtocolHeader` is only 4 bytes (`peerID: uint16`, `sentTime: uint16`,
[`enet/protocol.h` line ~65](https://github.com/lsalzman/enet/blob/master/include/enet/protocol.h)),
so Photon extended it:

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 2 | `peerId` | big-endian short |
| 2 | 1 | `flags` | `0xCC` = CRC-32 check enabled for this datagram; `0x01` = encrypted (the parser we sourced this from explicitly does not support decrypting — out of scope for us too, unencrypted LAN traffic only, matching this project's "LAN-only, no HTTPS yet" state) |
| 3 | 1 | `commandCount` | number of ENet-style commands packed into this datagram |
| 4 | 4 | `timestamp` | big-endian int |
| 8 | 4 | `challenge` | big-endian int; doubles as the CRC-32 slot when `flags == 0xCC` |

CRC handling nuance: the source we have zeroes a 4-byte slot and recomputes a CRC-32 over the whole
datagram when the `0xCC` flag is set, but the exact slot offset in that code path looked
inconsistent on inspection (it reads the expected CRC from offset 0, not offset 8, which doesn't
line up with "the challenge field doubles as the CRC slot"). **Flagging this explicitly as
unverified** rather than asserting a byte offset I'm not confident in — a later task implementing
CRC support should re-derive this from a captured packet, not from this doc.

### Command header (12 bytes), repeated `commandCount` times after the datagram header

Source: same file, `HandleCommand`. Again, Photon's own layout — vanilla ENet's
`ENetProtocolCommandHeader` is only 4 bytes (`command: uint8`, `channelID: uint8`,
`reliableSequenceNumber: uint16`); Photon's is 12:

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 1 | `commandType` | see Command types table below |
| 1 | 1 | `channelId` | |
| 2 | 1 | `commandFlags` | |
| 3 | 1 | — | unused/reserved (skipped by the parser) |
| 4 | 4 | `commandLength` | big-endian int; **includes this 12-byte header itself** (the parser subtracts 12 immediately after reading it) |
| 8 | 4 | `sequenceNumber` | big-endian int |

### Command types

Source: [`0blu/PhotonPackageParser`, `PhotonPackageParser/CommandType.cs`](https://github.com/0blu/PhotonPackageParser/blob/master/PhotonPackageParser/CommandType.cs)
for the values that parser actually implements, cross-checked against vanilla ENet's own
`ENetProtocolCommand` enum ([`enet/protocol.h`](https://github.com/lsalzman/enet/blob/master/include/enet/protocol.h#L25-L38),
MIT) for the full set (0blu's parser only needed to *receive* a subset of commands, so it never
defined `Acknowledge`/`Connect`/`VerifyConnect`/`Ping` — but the ones it does define agree exactly
with ENet's numbering, which is why we trust ENet's numbering for the rest). Triple-checked against
`nyxikitty/photon-server-nodejs`'s `PHOTON_COMMANDS` object, which independently has the identical
values for 1–8 despite disagreeing with everything else in that repo (see Prior Art).

| Command | Value | Source for this exact value |
|---|---|---|
| `Acknowledge` | 1 | ENet `ENET_PROTOCOL_COMMAND_ACKNOWLEDGE`; corroborated by `nyxikitty`'s `ACKNOWLEDGE: 1` |
| `ConnectRequest` (ENet calls this `Connect`) | 2 | ENet `ENET_PROTOCOL_COMMAND_CONNECT`; corroborated by `nyxikitty`'s `CONNECT: 2` |
| `ConnectResponse` (ENet calls this `VerifyConnect`) | 3 | ENet `ENET_PROTOCOL_COMMAND_VERIFY_CONNECT`; corroborated by `nyxikitty`'s `VERIFY_CONNECT: 3` |
| `Disconnect` | 4 | `0blu`'s `CommandType.Disconnect = 4` (direct, used by their parser); matches ENet |
| `Ping` | 5 | ENet `ENET_PROTOCOL_COMMAND_PING`; corroborated by `nyxikitty`'s `PING: 5` |
| `SendReliable` | 6 | `0blu`'s `CommandType.SendReliable = 6` (direct); matches ENet |
| `SendUnreliable` | 7 | `0blu`'s `CommandType.SendUnreliable = 7` (direct); matches ENet |
| `SendFragment` | 8 | `0blu`'s `CommandType.SendFragment = 8` (direct); matches ENet |

**Ping payload format**: vanilla ENet's `ENetProtocolPing` struct is just the command header with
no additional fields (`typedef struct _ENetProtocolPing { ENetProtocolCommandHeader header; }`,
[`enet/protocol.h` lines 141-143](https://github.com/lsalzman/enet/blob/master/include/enet/protocol.h#L141-L143),
MIT) — i.e. a `Ping` command is a bare 12-byte Photon command header with no body, by the same
pattern as `Disconnect` above. **Not independently confirmed against real Photon traffic**
(same caveat as the handshake/acknowledge items below: our Photon-specific sources never needed
to construct a `Ping`), so treat this as the vanilla-ENet baseline, not a Photon-verified fact.

The brief's placeholder constants had `Ping: 6` colliding with `SendReliable: 6`, and
`Acknowledge: 1` colliding with `ConnectRequest: 1` — both were genuinely wrong guesses, corrected
above. All seven required values are now distinct (1, 2, 3, 4, 5, 6, 7), matching the brief's own
instruction not to leave two constants sharing a code without the doc confirming they really do.

### Inner message type (inside a `SendReliable`/`SendUnreliable` command's payload)

A command of type `SendReliable` or `SendUnreliable` doesn't carry a raw Protocol16 value — it
carries one more discriminator byte first, then the Protocol16-tagged `OperationRequest` /
`OperationResponse` / `EventData` structure described above. Source:
[`0blu/PhotonPackageParser`, `MessageType.cs`](https://github.com/0blu/PhotonPackageParser/blob/master/PhotonPackageParser/MessageType.cs)
and `PhotonParser.cs`'s `HandleSendReliable`.

| Message type | Value |
|---|---|
| `OperationRequest` | 2 |
| `OperationResponse` | 3 |
| `Event` | 4 |

### `SendReliable` / `SendUnreliable` / `SendFragment` payload shapes

Source: `PhotonParser.cs`, `HandleCommand`/`HandleSendReliable`/`HandleSendFragment`.

- **`SendReliable`**: after the 12-byte command header — 1 reserved/unused byte, then 1
  message-type byte (table above), then the Protocol16-serialized `OperationRequest` /
  `OperationResponse` / `EventData` payload (length = `commandLength - 12 - 1 - 1`, i.e. the
  header's declared length minus the header itself minus those two bytes).
- **`SendUnreliable`**: identical to `SendReliable`, but with an extra 4-byte unreliable sequence
  number inserted immediately after the 12-byte command header, before the reserved byte and
  message-type byte.
- **`SendFragment`**: used when a payload is too large for one datagram. After the 12-byte command
  header: `startSequenceNumber` (4 bytes), `fragmentCount` (4 bytes), `fragmentNumber` (4 bytes),
  `totalLength` (4 bytes), `fragmentOffset` (4 bytes), then the fragment's slice of the payload
  bytes. The receiver accumulates fragments keyed by `startSequenceNumber` into a buffer of
  `totalLength` bytes at `fragmentOffset`, and once all bytes have arrived, treats the reassembled
  buffer as a `SendReliable` payload (1 reserved byte, 1 message-type byte, Protocol16 payload).
- **`Disconnect`**: command header only, no further payload (per `HandleCommand`'s early return).

### Connect / verify-connect handshake

Source: [`enet/protocol.h`](https://github.com/lsalzman/enet/blob/master/include/enet/protocol.h#L85-L118)
(MIT) for vanilla ENet's `ENetProtocolConnect`/`ENetProtocolVerifyConnect` struct *fields* — **this
describes vanilla ENet's handshake payload, not a confirmed-identical Photon one**; none of our
Photon-specific sources (`0blu`'s parser, the `photon-realtime` module) decode a literal
connect/verify-connect handshake, because both operate one layer up (already-established
peer-to-peer traffic for the parser; the WebSocket+JSON transport for the npm client, which has no
ENet handshake at all — see the transport-mismatch note below). Documenting what we have, flagged
honestly as unverified-for-Photon-specifically:

- `ENetProtocolConnect` fields (after the usual command header): `outgoingPeerID` (uint16),
  `incomingSessionID` (uint8), `outgoingSessionID` (uint8), `mtu` (uint32), `windowSize` (uint32),
  `channelCount` (uint32), `incomingBandwidth` (uint32), `outgoingBandwidth` (uint32),
  `packetThrottleInterval` (uint32), `packetThrottleAcceleration` (uint32),
  `packetThrottleDeceleration` (uint32), `connectID` (uint32), `data` (uint32).
- `ENetProtocolVerifyConnect` has the identical fields minus the trailing `data` field — it's the
  acceptor echoing back (possibly adjusted) connection parameters.
- **NOT VERIFIED for Photon**: whether Photon's actual `ConnectRequest`/`ConnectResponse` commands
  use this exact field layout, given that Photon's *other* headers (datagram, command) already
  diverge from vanilla ENet's. A later task that needs to literally perform this handshake against
  a real Photon client **must** capture and inspect a real connect exchange (e.g. via
  `idevicesyslog`/Wireshark against the oracle IPA per `docs/rebirth-oracle.md`'s tooling) rather
  than trusting this struct layout. Flagging this rather than guessing, per this task's explicit
  instructions.

### Sequence numbers and acknowledgement

- Each command carries its own `sequenceNumber` (reliable sequence number) in its 12-byte command
  header (see above; source: `0blu/PhotonPackageParser`).
- Vanilla ENet acknowledges a reliable command via `ENetProtocolAcknowledge`: command header plus
  `receivedReliableSequenceNumber` (uint16) and `receivedSentTime` (uint16)
  ([`enet/protocol.h`](https://github.com/lsalzman/enet/blob/master/include/enet/protocol.h#L78-L83)).
  **Not independently confirmed against real Photon traffic** — same caveat as the connect
  handshake above; our passive-parser source never needed to construct an `Acknowledge` command
  (it only parses incoming reliable sends), so this is vanilla-ENet-sourced only.

### Retransmission

Source: [`enet/peer.c`](https://github.com/lsalzman/enet/blob/master/peer.c) doc comment on
`enet_peer_timeout` (MIT), quoted near-verbatim: *"Timeout values use an exponential backoff
mechanism, where if a reliable packet is not acknowledged within some multiple of the average RTT
plus a variance tolerance, the timeout will be doubled until it reaches a set limit. If the timeout
is thus at this limit and reliable packets have been sent but not acknowledged within a certain
number of times, the peer will be disconnected."* Vanilla ENet's own defaults
([`enet/enet.h`](https://github.com/lsalzman/enet/blob/master/include/enet/enet.h) lines 214-227):
`ENET_PEER_DEFAULT_ROUND_TRIP_TIME = 500`, `ENET_PEER_PING_INTERVAL = 500`,
`ENET_PEER_TIMEOUT_LIMIT = 32`, `ENET_PEER_TIMEOUT_MINIMUM = 5000`,
`ENET_PEER_TIMEOUT_MAXIMUM = 30000` (all milliseconds except the limit, which is a retry count).

**These are vanilla-ENet defaults, not confirmed Photon defaults** — flagging explicitly, since
Photon is its own reimplementation and may tune these differently server-side. The one
Photon-specific number we could find for the *client* ping behavior came from a web search of
Exit Games' own (bot-walled, couldn't fetch directly) `doc-api.photonengine.com` pages: the .NET/
C++ `PhotonPeer.TimePingInterval` default is reported as **1000ms**. I could not independently
fetch that page myself (it sits behind a Cloudflare/Gcore bot-check that blocked every automated
fetch attempt, including a direct `curl` with a browser User-Agent) — so **this 1000ms figure is a
secondary citation (a search-engine summary of the official docs), not a primary-source read**,
and should be re-verified by a human opening that URL in a real browser, or by capturing real
traffic, before being trusted as load-bearing. It's included here because the task brief asks for
the ping interval and a weakly-sourced, clearly-flagged number is more useful than silence — but
treat it as provisional.

**Transport-mismatch note on the WebSocket client we read:** the official `photon-realtime` npm
package (`photon-realtime-module.js`, v4.4.0) does have a literal, directly-read keep-alive
constant — `keepAliveTimeoutMs = 3000` (3 seconds) — but that package's peer connects over
WebSocket with `ws`, not raw UDP/ENet, and its `TypeExt.*` helpers (`Int`, `Byte`, `String`, etc.)
are no-ops (`return f`), meaning it does **not** do Protocol16 binary framing at all — it's Exit
Games' JSON-oriented transport variant. That 3000ms number is real and directly read from the
source, but it governs a different transport than the one our iOS client uses, so it is **not**
used as the UDP ping-interval value in `constants.ts`; it's recorded here only so a future reader
doesn't rediscover and misapply it.

## Operations in scope

Source: the official `photon-realtime` npm package (v4.4.0), fetched from the npm registry
(`https://registry.npmjs.org/photon-realtime/-/photon-realtime-4.4.0.tgz`) and read directly from
`photon-realtime-module.js` — this is Exit Games' own published client code defining
`Photon.Lite.Constants` (`LiteOpKey`, `LiteOpCode`, `LiteEventCode`) and
`Photon.LoadBalancing.Constants` (`OperationCode`, `EventCode`, `ParameterCode`,
`ActorProperties`, `GameProperties`). These are serialization-independent (the same codes apply
whether the bytes go out as Protocol16/UDP or JSON/WebSocket), so they're valid for our UDP
transport even though this particular package's *transport* isn't.

All "request parameter keys" below are single **bytes** (Protocol16 `Byte`-tagged dictionary keys
inside the operation's parameter table — see "Protocol16 encoding" → `OperationRequest` row), not
strings. The "type" column is the Protocol16 type the value is sent as.

### `Authenticate` — operation code `230`

Request parameters (keys from `ParameterCode`/`LiteOpKey`, sent to the Name Server, Master Server,
and Game Server — each leg sends a subset):

| Key name | Byte | Protocol16 type | Notes |
|---|---|---|---|
| `ApplicationId` | 224 | String | omitted on reconnect-with-secret |
| `AppVersion` | 220 | String | |
| `ClientAuthenticationType` | 217 | Byte | only sent if using custom auth |
| `ClientAuthenticationParams` | 216 | String | only sent if using custom auth |
| `ClientAuthenticationData` | 214 | (any, app-defined) | only sent if using custom auth and data present |
| `UserId` | 225 | String | |
| `Secret` | 221 | String | sent instead of the above when reconnecting with a previously-issued secret |
| `LobbyStats` | 211 | Boolean | optional, Name Server leg only |

Response: standard operation-response envelope (error code + message), plus on success
`vals[Secret]` (221, String) may be present (a resumable-auth token the client should hold onto),
and nickname/userId confirmation fields.

### `JoinLobby` — operation code `229`

Request parameters: `LobbyName` (213, String, optional), `LobbyType` (212, Byte, optional).
Response: error code + message; `vals[Secret]` (221) optionally refreshed.

### `CreateGame` — operation code `227` (a `LoadBalancing.Constants.OperationCode`-level code, not derived from the Lite layer)

Request parameters: `RoomName` (255 — this is `LiteOpKey.GameId`, reused as `ParameterCode.RoomName`,
String), `Broadcast` (250, Boolean), `PlayerProperties` (249 — `LiteOpKey.ActorProperties`,
Hashtable), `JoinMode` (215, Byte, optional — `CreateIfNotExists`/`RejoinOnly`), `GameProperties`
(248, Hashtable, optional — the room's custom properties), `Add` (238, StringArray, optional —
`expectedUsers`).

Response: error code + message; on success, the room's actor (the joining player) is populated from
`vals`, and `vals[GameProperties]` (248) carries the room's property table back.

### `JoinGame` — operation code `226`

Same request-parameter shape as `CreateGame` (`RoomName`, `Broadcast`, `PlayerProperties`,
optional `JoinMode`/`Add`), sent to an already-existing room.

Response: error code + message; on success, `vals[ActorList]` (252 — `LiteOpKey.ActorList`,
IntegerArray of actor numbers already in the room), `vals[PlayerProperties]` (249, Hashtable keyed
by actor number → that actor's property Hashtable, which itself holds `ActorProperties.PlayerName`
at key 255 and `ActorProperties.UserId` at key 253), and `vals[GameProperties]` (248, the room's
property table).

### `RaiseEvent` — operation code `253` (this is `LiteOpCode.RaiseEvent`, reused directly in `LoadBalancing.Constants.OperationCode`)

Request parameters: `Code` (244 — `LiteOpKey.Code`, Byte — the **custom event code** the receiving
clients will see as the `EventData` code), `Data` (245 — `LiteOpKey.Data`, any Protocol16 type —
the event payload), and optionally: `ReceiverGroup` (246, Byte — `Others`=0/`All`=1/`MasterClient`=2),
`Cache` (247, Byte — one of `EventCaching`'s values), `Group` (240, Byte — interest group number),
`ActorList` (252, IntegerArray — reused here as the *target* actor-number list, i.e. "send only to
these actors"), `WebFlags` (234, Byte, optional).

Response: standard envelope only (error code 0 on success); no meaningful `vals`.

### `Leave` — operation code `254` (`LiteOpCode.Leave`, reused directly)

Request: no parameters in the normal case. Response: error code + message; the client disconnects
from the game server on a successful response (per `photon-realtime-module.js`'s
`addResponseListener(OperationCode.Leave, ...)` handler, which calls `a.disconnect()`
unconditionally after logging the response).

### Events in scope

| Event | Code | Source | Payload |
|---|---|---|---|
| `Join` | 255 (`LiteEventCode.Join`) | `photon-realtime-module.js` | `vals` contains the joining actor's number (`ActorNr`, key 254) and its properties |
| `Leave` | 254 (`LiteEventCode.Leave`) | same | `vals[ActorNr]` (254), `vals[IsInactive]` (233, Boolean — true means "suspended, may rejoin" rather than fully gone) |
| custom game events (co-op/PvP relay payloads) | whatever byte the sender passed as `RaiseEvent`'s `Code` parameter (any value not reserved by Photon, i.e. not 255/254/253/252/230/229/228/226/224/223/210) | — | `vals[Data]` (245) carries the relayed payload; this project's realtime server only needs to pass this through untouched (per the plan's "relay only" constraint), never interpret it |

Also present in the real protocol but explicitly **out of scope** for this plan's minimum set
(documented here only so nobody rediscovers and misuses the wrong code later):
`PropertiesChanged` (253 — note this collides numerically with the `RaiseEvent` *operation* code,
but event codes and operation codes are different namespaces, so that's not a conflict),
`Disconnect` (252, as an *event* — distinct from the ENet `Disconnect` *command* described in the
transport section), `AuthEvent` (223), `ErrorInfo` (251), `GameList`/`GameListUpdate`/`QueueState`/
`AppStats`/`LobbyStats` (lobby/stats events, 230/229/228/226/224 respectively).

### `ParameterCode` reference (full exported set, with citations)

`realtime/src/protocol/constants.ts`'s `ParameterCode` export holds a few keys
(`TargetActorNr`, `Properties`) that aren't mentioned by name in the per-operation prose above
because they belong to the `PropertiesChanged` event, which is called out as out-of-scope just
above — but the values are still real, sourced numbers, not guesses, so here is the complete
list with its citation, consolidated in one place to prevent exactly this kind of
exported-but-uncited drift in future edits.

Source for every row: `photon-realtime-module.js` (official `photon-realtime` npm package,
v4.4.0, fetched from `https://registry.npmjs.org/photon-realtime/-/photon-realtime-4.4.0.tgz`),
specifically `Photon.Lite.Constants.LiteOpKey` —

```
g.LiteOpKey = {ActorList:252, ActorNr:254, ActorProperties:249, Add:238, Broadcast:250,
  Cache:247, Code:244, Data:245, GameId:255, GameProperties:248, Group:240, Properties:251,
  ReceiverGroup:246, Remove:239, TargetActorNr:253, RoomTTL:236};
```

— and `Photon.LoadBalancing.Constants.ParameterCode`, which aliases most of its keys directly to
the `LiteOpKey` object above (shown here as `p` — i.e. `p.Properties` means "whatever
`LiteOpKey.Properties` is", namely `251`):

```
d.ParameterCode = {..., RoomName:p.GameId, Broadcast:p.Broadcast, ActorList:p.ActorList,
  ActorNr:p.ActorNr, PlayerProperties:p.ActorProperties, Data:p.Data, Code:p.Code,
  GameProperties:p.GameProperties, Properties:p.Properties, TargetActorNr:p.TargetActorNr,
  ReceiverGroup:p.ReceiverGroup, Cache:p.Cache, Group:p.Group, Remove:p.Remove, Add:p.Add,
  RoomTTL:p.RoomTTL, ..., ClientAuthenticationType:217, ClientAuthenticationParams:216,
  ClientAuthenticationData:214, JoinMode:215, ...};
```

combined with the earlier-cited literal fields (`ApplicationId:224`, `AppVersion:220`,
`UserId:225`, `Secret:221`, `LobbyName:213`, `LobbyType:212`, `LobbyStats:211`, `IsInactive:233`
— all read directly from the same module, see the per-operation tables above for where each is
used).

| Key | Byte | Resolves to |
|---|---|---|
| `ApplicationId` | 224 | literal `ParameterCode` field |
| `AppVersion` | 220 | literal `ParameterCode` field |
| `ClientAuthenticationType` | 217 | literal `ParameterCode` field |
| `ClientAuthenticationParams` | 216 | literal `ParameterCode` field |
| `ClientAuthenticationData` | 214 | literal `ParameterCode` field |
| `UserId` | 225 | literal `ParameterCode` field |
| `Secret` | 221 | literal `ParameterCode` field |
| `LobbyName` | 213 | literal `ParameterCode` field |
| `LobbyType` | 212 | literal `ParameterCode` field |
| `LobbyStats` | 211 | literal `ParameterCode` field |
| `RoomName` | 255 | `ParameterCode.RoomName = LiteOpKey.GameId` |
| `Broadcast` | 250 | `ParameterCode.Broadcast = LiteOpKey.Broadcast` |
| `PlayerProperties` | 249 | `ParameterCode.PlayerProperties = LiteOpKey.ActorProperties` |
| `GameProperties` | 248 | `ParameterCode.GameProperties = LiteOpKey.GameProperties` |
| `JoinMode` | 215 | literal `ParameterCode` field |
| `Add` | 238 | `ParameterCode.Add = LiteOpKey.Add` |
| `ActorList` | 252 | `ParameterCode.ActorList = LiteOpKey.ActorList` |
| `ActorNr` | 254 | `ParameterCode.ActorNr = LiteOpKey.ActorNr` |
| `Code` | 244 | `ParameterCode.Code = LiteOpKey.Code` |
| `Data` | 245 | `ParameterCode.Data = LiteOpKey.Data` |
| `ReceiverGroup` | 246 | `ParameterCode.ReceiverGroup = LiteOpKey.ReceiverGroup` |
| `Cache` | 247 | `ParameterCode.Cache = LiteOpKey.Cache` |
| `Group` | 240 | `ParameterCode.Group = LiteOpKey.Group` |
| `TargetActorNr` | 253 | `ParameterCode.TargetActorNr = LiteOpKey.TargetActorNr` |
| `Properties` | 251 | `ParameterCode.Properties = LiteOpKey.Properties` |
| `IsInactive` | 233 | literal `ParameterCode` field |

`TargetActorNr` (253) and `Properties` (251) are used together in the `PropertiesChanged` event
(out of scope, see above): `vals[TargetActorNr]` names which actor's custom properties changed
(absent/0 means the *room's* properties changed instead), and `vals[Properties]` carries the new
property `Hashtable`. Exported here because a future task extending event handling beyond this
plan's minimum will need them, and because they were already present in `constants.ts` without
this citation — now fixed.

## Open items for later tasks

1. **CRC-32 field offset in the datagram header** (flagged above) — needs a real packet capture to
   resolve, not more reading.
2. **Connect/VerifyConnect handshake field layout for Photon specifically** (flagged above) — the
   struct I have is vanilla ENet's; Photon's actual bytes need a capture against the oracle IPA.
3. **Acknowledge command field layout for Photon specifically** — same caveat.
4. **UDP ping interval** — the 1000ms figure is a secondary (search-summary) citation only; treat
   as provisional until a primary source or packet capture confirms it.
5. Tasks that need more `ParameterCode`/`ActorProperties`/`GameProperties`/`ErrorCode` values than
   are listed here should pull them from the same `photon-realtime-module.js` read documented
   above (it defines the full `LoadBalancing.Constants` object, including `ErrorCode`,
   `MatchmakingMode`, `EventCaching`, `ReceiverGroup`, `CustomAuthenticationType`, `LobbyType`) —
   re-fetch `https://registry.npmjs.org/photon-realtime/-/photon-realtime-4.4.0.tgz` rather than
   re-deriving by guesswork.
