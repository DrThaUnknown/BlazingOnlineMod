# Photon client trace (2.28.0, from the binary)

Date: 2026-10-13. Everything here is read from the 2.28.0 iOS binary (`Payload/BNEI0249.app/BNEI0249`
from `build/Blazing-Private-LAN-photon.ipa`) with `tools/disasm.py` + `build/il2cpp-ios-2.28.0/script.json`.
It supersedes the guesses in `photon-protocol.md` wherever they disagree. Addresses are RVAs.

## Why realtime PvP never connected (root causes, in order)

Container log of a real attempt: Connect -> our VerifyConnect -> client `SendReliable(Init)` -> our
ack + InitResponse -> client only pings, never acks the InitResponse, and disconnects at 15 s.

1. **Reliable replies carried `commandFlags = 0`.** `NCommand..ctor` (@0x1AD7C04) reads byte 2 of the
   command header into `commandFlags` (+0x10). `ReceiveIncomingCommands` (@0x1AD6F9C) only creates an
   ack when `commandFlags & 1`. `QueueIncomingCommand` (@0x1AD9070) sends a flags-0 command down the
   unreliable path (@0x1AD9698), which orders it after reliable seq `reliableSequenceNumber` (we sent
   1, which never arrives), so it is never dispatched. Every reply we sent was silently dropped.
2. **Wrong `PhotonCodes`.** dump.cs shows field *offsets* 0..3; the values are set in
   `PhotonCodes..cctor` (@0x1ADD350) as one 32-bit store of `0x00010201`:
   `ClientKey = 1, ModeKey = 2, ServerKey = 1, InitEncryption = 0`. We waited for op 3.
3. **Message framing.** All messages are `[0xF3][msgType][body]`. `SerializeOperationRequest`
   (@0x1AD6138) is called with `setType = false`, so there is **no** `0x71`/`0x70`/`0x65` tag:
   - request: `[F3][02][opCode][paramTable]` (internal: `[F3][06]...`)
   - response: `[F3][03][opCode][returnCode:i16][debugMsg: typed][paramTable]`
     (`DeserializeOperationResponse` @0x1ADCE48; internal: `[F3][07]...`)
   - event: `[F3][04][code][paramTable]` (`DeserializeEventData` @0x1ADCFE4)
   - paramTable: `[count:i16]` then `[key:byte][typed value]`.
4. `GameEnteredOnGameServer` casts `252 ActorList` to `int[]` (Protocol16 `n`), not an object array.
5. The game matches players **inside Photon** (lobby room lists), not via our HTTP matchmaker; see below.

## Transport facts

- Command header: `[type][channel][flags][reserved][length:i32][reliableSeq:i32]`, unreliable adds
  `[unreliableSeq:i32]`, fragment adds `[startSeq][fragmentCount][fragmentNumber][totalLength][fragmentOffset]`
  (all i32, header 32 bytes). Types 1..8 only; `CT_EG_SERVERTIME = 12` exists but `ExecuteCommand`
  ignores it.
- Ack body: `[ackedReliableSeq:i32][ackedSentTime:i32]`, on the acked command's channel.
- `SocketUdp.ReceiveLoop` (@0x1AE6194) receives into `new byte[mtu]` (1200 from the Connect body), so
  no datagram we send may exceed the client's MTU; large messages must be fragmented.

## Encryption (every connection: NameServer, Master, GameServer)

- `LoadBalancingClient.OnStatusChanged` (@0xF8E01C): on `Connect` it **always** calls
  `EstablishEncryption()`; if a secret is held it also sends `OpAuthenticate` (token) immediately.
- `ExchangeKeysForEncryption` (@0x1ADC344): internal op 0, `{1: clientPublicKey byte[]}`, msgType 6.
- `PeerBase.DeriveSharedKey` (@0x1ADC480): needs internal response op 0, rc 0, `{1: serverKey byte[]}`;
  then fires `EncryptionEstablished` (1048).
- `DiffieHellmanCryptoProvider` (@0x1AEED60): Oakley 768-bit prime, generator 22, 160-bit secret.
  `DeriveSharedKey` (@0x1AEEFE0): `key = SHA256(BigInteger(shared).GetBytes())` (minimal big-endian, so
  strip leading zeros), `RijndaelManaged`, IV = 16 zero bytes, `Padding = PKCS7`, mode CBC.
- `OpAuthenticate` (@0xF903B8): with a secret, `{221: secret}` unencrypted; otherwise
  `{220 appVersion, 224 appId, 210 region?, 225 userId?, 217 authType?, 216/214 params}` **encrypted**.
- Encrypted request: `[F3][0x80|type][AES(opCode + paramTable)]` (`SerializeOperationToMessage` @0x1AD5C84).

## LoadBalancing state machine (`LoadBalancingClient.OnOperationResponse` @0xF8D5F8)

Opcode jump table at 0x24ACF8C (220..230):

| Op | Server | What the client reads |
|---|---|---|
| 220 GetRegions | Name | `210` string[], `230` string[] |
| 230 Authenticate | Name | `230` master address (string) -> reconnect; `221` secret (any server) |
| 230 Authenticate | Master | -> ConnectedToMaster, auto `OpJoinLobby(CurrentLobby)` |
| 230 Authenticate | Game | replays Create/Join on the game server |
| 227/226/225 | Master | `230` game-server address, `255` room name -> reconnect |
| 227/226/225 | Game | `GameEnteredOnGameServer` (@0xF8CF48): `254` int, `249` Hashtable, `248` Hashtable, `252` int[] |
| 229 JoinLobby | Master | -> JoinedLobby |
| 254 Leave | Game | -> disconnect, back to master |

Error codes the client special-cases: 32758 (GameDoesNotExist), 32760, 32765, 32766 (0x7FF6..0x7FFE).

`MultiplayClient.OnStatusChanged` (@0xF62410) calls `OpGetRegions` once encryption is up on the
NameServer; the game then authenticates with a region.

Events (`LoadBalancingClient.OnEvent` @0xF8E4A0): 230/229 GameList(Update) `{222: Hashtable name->props}`
(props with `251: true` = removed); 226 AppStats `{229,228,227: int}`; 255 Join `{254 int, 249 Hashtable,
252 int[]}`; 254 Leave `{254 int, 233 bool?}`; 253 PropertiesChanged `{253 int, 251 Hashtable}`.
`RoomInfo.CacheProperties` reads 255/252 as **byte**, 253/254/251 as bool, 250 as string[].

## Game layer

- `MultiplayClient.ExcuteConnect(lobbyID)` (@0xF5D914): `CurrentLobby = TypedLobby(lobbyID + "_" + …)`,
  `ConnectToRegionMaster(regionCode)`.
- `MultiplayClient.SetCreateRoom` (@0xF5F864): custom props `pn` (player name), `ps` (password),
  `cm` (comment), all listed in the lobby; `OpCreateRoom(name, options, CurrentLobby)`.
- `MultiplayManager` league matching alternates host/guest phases (`ChangeLeagueMatchingPhase`,
  `SetLeagueParameter`): hosts `CreateRoomForLeague`, guests pick a room from the lobby list
  (`GetRoomInfoDataFromNameLeague` parses the room name). The server only has to be a faithful
  lobby + room server; it does not pair players itself.

## League matching phases (why two phones fight bots)

Traced 2026-10-13 after the 0.2.4 test, where both phones searched ranked at the same time and each
got a bot. The host log showed them creating rooms together, leaving together, and scanning empty
lobbies together. B saw A's room once and sent JoinGame just as A left it.

- `UIManager.ConnectMultiLeague` -> `MultiplayManager.SetLeagueParameter` (@0xF66410) stores the
  playerparam values: +0x78 `leagueMatchingTime` (ms, 180000), +0x7C/+0x80 host phase min/max,
  +0x84/+0x88 guest phase min/max (seconds). Shipped: **all four = 5**.
- `UpdateLeagueMatchingPhaseInterval` (@0xF64714) is called **once per matching session**, from
  `PageLeagueMatching.Initialize`. It sets +0xA4 = TickCount, guest seconds +0x90 =
  `Random.Range(+0x84, +0x88)` and host seconds +0x94 = `Random.Range(+0x7C, +0x80)`. With min=max
  both are always 5.
- `ChangeLeagueMatchingPhase(phase)` (@0xF65B38, called from `UIManager.MultiplayCallbackLeague
  Host/GuestPhase`) sets +0xA0 = phase (1 = host) and +0xA4 = TickCount.
- `get_IsLeagueMatchingPhaseTime` (@0xF65ABC): `(TickCount - +0xA4) / 1000 >= (phase == 1 ? +0x94 : +0x90)`.
- `get_IsLeagueMatchingTimeOutPhase` (@0xF65B64): the whole search is capped by +0x78 against the
  LoadBalancingClient's start tick (+0x124).

So every client runs an identical 5 s host / 5 s guest square wave from the moment it taps Search.
Two players who tap at about the same time stay in phase and never meet, and the client falls back to
a CPU opponent after 180 s.

Fix (mod 0.2.5, `mod/src/match.c`): signed code can't be patched, so the dylib finds the live
instance through data. `SingleObject<MultiplayManager>_TypeInfo` (slot RVA 0x2DB0F28) -> class
+0xB8 (static fields) -> [0] = instance (the same path `get_instance` @0x1632198 takes through its
rgctx). The instance is validated by checking that its klass equals `MultiplayManager_TypeInfo`
(slot RVA 0x2DB3178). Every time +0xA4 changes (a new phase or session), the dylib writes fresh
random 3..8 s lengths into +0x90/+0x94. The 3..8 range is our choice, not the game's. Because the
two phones' phase boundaries now drift independently, one ends up hosting while the other scans.

First on-device result (0.2.5, 2026-10-14): the phase fix worked. A and B were paired after about
45 s and played a real ranked battle through turn 5. Then B dropped out mid-battle (cause unknown,
because B's log was not pulled) and A was force-quit. Because the host lived in A's process and kept
matches only in memory, both clients' unsent `rtpvp2/result` (resent at every login) were refused
with 400, and that blocked the login with "error in the requested data". Host 0.2.6: matches are
persisted in host-state.json, and a result with no matching match is answered 200 with zero points.
