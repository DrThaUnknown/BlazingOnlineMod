# Online play (co-op and realtime PvP)

Status 2026-10-13: both halves exist. The realtime half is `realtime/` (Photon NameServer 5058,
Master 5055, GameServer 5056/udp in one process), rewritten against the client protocol traced
from the binary in **docs/photon-client-trace.md**, which also explains why the first version never
connected. Verified with `realtime/test/realtime.test.ts` and `node tools/smoke_realtime.ts` (two
simulated players, live container); not yet on a phone. Ranked matches: the client pairs players
in Photon lobbies; `rtpvp2/starthost` registers the pair (`server/src/game/rtpvpMatch.ts`) and each
side reports one `rtpvp2/result`.

## What the server does (HTTP, done)

`server/src/game/multi.ts`, `server/src/api/handlers/mission.ts`:

| Endpoint | Who | What the server does |
|---|---|---|
| `mission/startmultihost` | host | Host's battle = a normal solo battle (stamina, drops, hash). Rolls a separate hash + drops for every real player in `guest_player_id_list`, returned in `guest_player_info_list`. |
| `mission/startmultiguest` | guest | Confirms the pending battle and spends the guest's stamina (INFERRED placement). |
| `mission/resultmultihost` | host | Same rules as `resultsolo`. |
| `mission/resultmultiguest` | guest | Guest's drops (minus `exclude_drop_units`) and player exp. |
| `mission/tapmulti` | both | Telemetry, ignored. |

The server stays authoritative: rewards come from the hashes it issued, never from the client.

## What is missing: the realtime layer (Photon)

Co-op rooms (`ExitGames.Client.MultiplayCode.MultiplayManager` / `MultiplayClient`,
`PhotonRoomCollector`) and realtime PvP (`rtpvp*`) run over **Photon Realtime** (Exit Games
LoadBalancing: name server -> master server -> game server, ENet over UDP).

From the 2.28.0 binary:

- `LoadBalancingClient..ctor` (@0xF8B04C) sets the name-server and master-server defaults from two
  string literals, `0x2DFF168` and `0x2DFF170` (`tools/dataxrefs.py` finds the loads at
  0xF8B088 / 0xF8B0D4).
- The official client had `ns.exitgamescloud.com:5058` / `app-eu.exitgamescloud.com:5055` there
  (Photon Cloud; docs/discovery.md section 5.3). Rebirth's build has **`127.0.0.1:15058`** /
  **`127.0.0.1:15055`**: Rebirth simply pointed Photon at a dead local port. Its engine dylib
  contains no Photon server, so Rebirth has no online play at all.
- Flow: `MultiplayManager.CreateClient` -> `ConnectToNameServer` (@0xF8B2CC), then
  `ConnectToRegionMaster(region)` (@0xF8B3D4). The app id is passed into `MultiplayClient`'s
  constructor; candidates per environment are in docs/discovery.md section 5.1. Regions come from
  master data `photonRegionsCodes` / `photonDefaultRegionGlobal`.
- Invites travel as mail: `MailData.room_id`, `room_pass`, `cluster_name`.

Client Photon library (dump): an old Photon3Unity3D. `ConnectionProtocol` has only Udp = 0 and
Tcp = 1 (no WebSocket), with `EnetPeer`/`SocketUdp` and `TPeer`/`SocketTcp`, and there is no `Protocol18` class, so the
wire format is **Protocol16**. `LoadBalancingClient(protocol = 0)` means UDP/ENet by default.

**Decision (2026-10-12): option 2**, our own server in Node, because the target is Linux and the
Photon Server SDK is Windows-only.

## Options for the realtime server

1. **Photon Server SDK (self-hosted).** Exit Games' own server, Windows-only, free licence for 20
   concurrent users, which is enough for a small LAN group. It runs the stock LoadBalancing
   application (NameServer + Master + GameServer). It needs a download plus licence registration
   by the owner, regions configured to match `photonRegionsCodes`, and the two literals repointed
   to `192.168.1.137:5058` / `:5055`.
2. **Our own Photon-compatible server in Node.** ENet-style reliable UDP plus Photon's binary
   serialisation (Protocol 1.6/1.8), plus the LoadBalancing operations the client uses (Authenticate,
   JoinLobby, CreateGame, JoinGame, RaiseEvent, room properties). It fits the "one stack, moves to
   Linux" goal, but is a substantial project.

Both need the IPA patched: `127.0.0.1:15058` is 15 characters and `192.168.1.137:5058` is 18, so the
literal cannot be patched in place. `tools/repoint_ipa.py` has to rewrite the literal's entry in
`global-metadata.dat` (append the new string and update its offset and length).

## Internal: matchmaker loop and room creation

The `server` process runs its own matchmaker loop (`server/src/game/matchmaker.ts`,
`matchmakerTick`, wired into `server/src/main.ts` via `setInterval` every 1500ms). Each tick it
calls `findMatch` over every player with a non-null `rtpvp.queuedAt` (joined by
`api/rtpvp2/matching.json`), picks the closest-ranked pair whose points gap fits an
INFERRED band that widens with wait time, clears both players' `queuedAt`, and records the pairing
in the in-memory `roomPairings` map (consumed by the later realtime-PvP result flow to check that
a reported result's caller actually belongs to the room).

`server` and `realtime` are separate processes (separate containers in `docker-compose.yml`), so
the matchmaker can't call `RoomRegistry` directly. Instead the realtime process exposes a tiny
internal-only HTTP endpoint, `POST /internal/rooms`, alongside its UDP socket
(`realtime/src/internal/roomApi.ts`, started from `realtime/src/main.ts`, default port **5059**
via `REALTIME_INTERNAL_PORT`). Body: `{ roomId: string, playerIds: number[], kind?: "pvp" | "coop" }`.
It calls `registry.createRoom(roomId, playerIds[0])` then `registry.joinRoom(roomId, id)` for
every remaining id, on the exact same `RoomRegistry` instance the UDP transport uses, so a player
who then connects over Photon and does `JoinGame` on that room id finds it already populated.

This endpoint is **never exposed outside the Docker network** -- unlike the UDP ports (5058/5055),
which are deliberately published for the phone to reach directly, port 5059 has no `ports:` mapping
in `docker-compose.yml`'s `realtime` service, so it's reachable only container-to-container (the
`server` container talks to it as `http://realtime:5059`, `REALTIME_INTERNAL_URL`). There is no
auth on it; it must stay LAN/Docker-internal only.
