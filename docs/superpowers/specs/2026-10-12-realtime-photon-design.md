# Realtime PvP and co-op rooms: a Photon-compatible server in Node

Date: 2026-10-12. Status: approved, not yet implemented.

## Decision

Build our own Photon-compatible realtime server in Node, as a new process alongside the existing
HTTP game server, to serve two client features that currently do nothing:

- **Ninja World Ultimate Showdown** (the client's real-time ranked PvP ladder — distinct from
  Phantom Castle, which is async and already working).
- **Realtime co-op rooms** (the live-session half of `mission/startmultihost|startmultiguest`; the
  HTTP/reward half of co-op is already implemented and does not change).

This supersedes the open question in `docs/online.md` ("Photon Server SDK vs our own Node
server") in favor of our own server: the Photon Server SDK is Windows-only, and the project's
stated direction is one Node stack that moves to a Linux box. Building on Windows now would mean
rebuilding this entire piece later; building it in Node once is more total work up front but zero
work thrown away.

## Scope

In scope:
- A new realtime server process speaking enough of Photon's published wire protocol (Protocol16
  binary encoding + its ENet-style reliable-UDP transport) to satisfy the 2.28.0 client's
  connection handshake and the operations below. **Not** the full Photon LoadBalancing spec —
  only what `MultiplayManager`/`MultiplayClient` actually call for these two features.
- Real queue-based matchmaking for Ninja World Ultimate Showdown, as a single points ladder.
  Seasonal structure (Training Time vs Qualifier League vs Tiers F→S vs Five Kage League) is
  explicitly deferred — see Non-goals.
- Co-op room creation/join, tied to the existing `mission/startmulti*` HTTP flow via mail invites.
- The one client-side binary change this requires: repointing the two Photon server-address
  literals, extending `tools/repoint_ipa.py`.
- A validation plan that proves wire correctness against the real published Photon SDK before any
  on-device test.

Non-goals (explicitly deferred, not forgotten):
- Battle simulation. Our server never understands combat — it relays `RaiseEvent` bytes between
  the two clients in a room and trusts them to agree on their own outcome, exactly like real
  Photon's own architecture, and exactly like this project's existing Phantom Castle trust model
  (`resultPvp` already trusts a client-reported `win_lose`).
- Seasons, tiers, promotion/demotion, the Five Kage League bracket, Qualifier League gating,
  Training Time as a distinct unranked mode. These are real game-design layers on top of today's
  matchmaking core and can be added later without touching the transport or the queue.
- Real geographic regions / separate name-server, master-server, game-server processes. Real
  Photon's three-tier topology exists to route traffic across data centers; one LAN server
  collapses all three roles into one process while still answering the client's
  `ConnectToNameServer` → `ConnectToRegionMaster` handshake so its state machine proceeds normally.
- Spectating, replays, rematches, voice/chat.

## Architecture

A new process (tentatively `realtime/`, a new `docker-compose` service) next to the existing
`server`. It does not duplicate game state: it reads/writes through the same Postgres-backed
`Stores` module the HTTP server already uses, and authenticates clients against the same `ssid`
session tokens `server/src/store/types.ts` already issues — there is no separate login for the
realtime server.

Reasons to keep it a separate process rather than folding into `server`:
- The transport is fundamentally different (long-lived raw UDP socket + binary framing vs
  request/response HTTP) and doesn't belong in the same event loop/listener as Fastify.
- A bug in this new, unproven protocol implementation should not be able to take down missions,
  shop, or anything else already working.

It can still live in the same monorepo/workspace and import the same TypeScript modules
(`game/pvp.ts`'s rank/standing patterns, `Stores`, etc.) — this is a process boundary, not a
rewrite of anything existing.

## Protocol layer

Two pieces, both documented by Exit Games (Photon's maker) for anyone building tools or bots
against Photon — this is implementing a published third-party protocol, not blind reverse
engineering of this one client's private format:

1. **Protocol16**: Photon's binary value encoding. A one-byte type tag followed by type-specific
   bytes, for primitives (bool, byte, short, int, float, double, string), Photon's `Hashtable`
   (its typed dictionary), arrays, and a handful of custom types. Used for both operation
   parameters (requests/responses) and event payloads.
2. **The ENet-style transport**: a custom reliable-UDP scheme — connect/disconnect handshake,
   sequenced commands with acks and retransmission, fragmentation for payloads larger than one
   datagram, and periodic keep-alive pings.

Only the operations the client actually calls need implementing:
`Authenticate`, `JoinLobby`, `CreateGame`, `JoinGame`, `RaiseEvent`, `Leave`, room-property
get/set, plus the transport-level connect/ack/ping/disconnect commands underneath all of them.

### De-risking plan

Per this project's own rule ("get client-side evidence before theorizing," and "minimize phone
round-trips"), wire-format correctness should not be discovered by trial and error against the
real IPA on a phone. Validation proceeds in two stages:

1. Build a throwaway PC test client using Photon's own freely available Unity/PC SDK, and point it
   at our server. If a *real* Photon client is satisfied by our server's handshake and operations,
   our protocol implementation is very likely correct — this is testable entirely on the PC, with
   no phone involved, and can iterate as fast as any other code.
2. Only once step 1 passes: extend `tools/repoint_ipa.py` to patch the two address literals,
   install the repointed IPA, and test the real client against the real server.

This also means a feasibility spike — checking whether an existing open-source Node/JS Photon
client or server implementation exists and can be adapted or learned from — is worth doing at the
start of implementation, before writing the transport from a blank file. It does not change this
design; it may reduce its cost.

## Matchmaking and ranking data model

A new state bucket, kept separate from Phantom Castle's existing `pvp` standing (different ladder,
different currency — Clash Coins live at Nekobaa's shop; this ladder's Rank Points are a distinct
resource):

```ts
rtpvp: Record<string, { points: number; winCount: number; lossCount: number }>  // per rtpvp_id
```

mirrored in Postgres the same way `pvp`/`pvpTeams`/everything else already is.

Flow:
1. Client calls an HTTP endpoint (filling in today's `api/rtpvp2/*` stubs) to join the queue.
2. A matchmaker loop pairs queued players within a points-range band, widening the band the
   longer a player waits, so nobody waits forever for an exact match.
3. Once paired, the server creates a room on the realtime process and tells both clients (via the
   existing HTTP response, or a Photon lobby instruction — exact mechanism is an implementation
   decision, not a design one) to join it.
4. The live match is pure relay (see Non-goals).
5. After the match, the result flow — same trust model as Phantom Castle's `resultPvp` — records
   win/loss and awards Rank Points on the single ladder.

## Co-op rooms

No queue, no ranking. A host picks a mission and creates a room; the invite travels as mail
(`MailData.room_id`, `room_pass` — mail already exists, `game/social.ts`). The guest's client
joins that room by id from the mail. Once both are connected it is pure relay, same as PvP.

The reward/result side needs **no new work**: `mission/startmultihost|startmultiguest
|resultmultihost|resultmultiguest` (`game/multi.ts`, already implemented) already handle stamina,
drops and rewards server-side for this flow. This design only adds the live room underneath what
already exists.

### Disconnect handling

- **PvP**: a disconnected player auto-forfeits (loss recorded) after a grace period. Without this,
  a losing player could dodge a loss by simply closing the app.
- **Co-op**: a dropped guest does not penalize the host; the host's mission can continue solo (or
  the guest's slot simply ends), since the host's own stamina/rewards flow is independent per
  `game/multi.ts`'s existing split between host and guest result handling.

## Client-side change required

The 2.28.0 binary has two Photon server-address literals. Rebirth's build points them at dead
loopback ports (`127.0.0.1:15058` name server, `127.0.0.1:15055` master server — its engine has no
Photon server at all, so this was never reachable even in Rebirth). They need to become
`192.168.1.137:5058` / `192.168.1.137:5055`.

Unlike the HTTP server URL (a fixed-length literal patched in place today), the new addresses are
longer strings, so `tools/repoint_ipa.py` needs a new capability: append the new string into
`global-metadata.dat` and rewrite that literal's offset and length, rather than patching in place.
This is a natural extension of a tool that already exists, not a new tool.

## Open risks

- **Wire-format correctness** is the main unknown. Mitigated by the two-stage validation plan
  above — PC-SDK-first, phone second.
- **ENet-style transport complexity**: reliable-UDP with fragmentation and acks is real protocol
  work, not a thin wrapper. The feasibility spike (existing Node/JS implementations) may reduce
  this; if none exist, it is still scoped to only what the client's handshake needs, not a general
  ENet reimplementation.
- **Matchmaking fairness at low population**: a real queue with a widening points band degrades
  gracefully to "anyone currently queued" when few players are online, so this does not need a
  separate small-scale code path.

## Out of scope for this spec, tracked for later

Seasonal tier/league structure (see Non-goals) gets its own design once the matchmaking core is
working and tested, since it is additive game-design logic, not a transport concern.
