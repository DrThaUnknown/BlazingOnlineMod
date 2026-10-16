# Blazing Online: an online mod for the official Blazing:Rebirth offline IPA

Date: 2026-10-13. Status: approved 2026-10-13 (host on iPhone directly; no PC-server phase).

## Goal

A downloadable add-on, `BlazingOnline.dylib`, injected into the **unmodified official
Blazing:Rebirth offline IPA** with Sideloadly (exactly like Blazing Forge). It turns Rebirth into an
online game that players run themselves: one player's iPhone hosts, others join from an overlay
menu. It works with or without Blazing Forge installed, and neither needs the other.

## What stays offline (untouched)

Rebirth's own `BlazingEngine` keeps serving everything it already does: missions, story, summons,
enhancement, shop, saves, and the 3.5 GB of local game data and art. Each player keeps their own
Rebirth progress. The mod adds no PvE content.

## What the mod adds

1. **Host / Join** (overlay menu).
   - *Host*: the phone runs an in-mod online server: the Photon realtime server (rooms, lobby
     room lists, event relay; `realtime/` in this repo) and the multiplayer HTTP calls Rebirth's
     engine does not serve for real (`api/rtpvp2/*` matching/start/result/top, `api/player/detail.json`
     for opponents, co-op `mission/*multi*`).
   - *Join*: pick or type the host's address. Only those multiplayer calls and Photon traffic go
     to the host; every other call still goes to the local Rebirth engine.
2. **Host rules** (host's overlay): character bans/allows, stat boosts, the Kage League character
   set, regulation presets as shareable JSON (e.g. no dupes, cost cap, no Blazing Awakened), and
   Draft mode (a temporary maxed box for PvP) vs Import mode (the player's real Rebirth account).
   Enforced on the host: each player's PvP team arrives in the Photon actor properties
   (`ParamsJson.teamCharacters`, seen in live logs), so an illegal team is refused before battle.
3. **Session**: ping to the host before readying, a session leaderboard with ELO (SQLite on the
   host's phone), match-log export (JSON of the relayed turn events).
4. **Client QoL**: team names and more team slots.
5. **Remote updates**: the host server and rules are a signed JavaScript bundle run in iOS's
   built-in JavaScriptCore; the mod fetches and verifies new bundles itself, so most updates need
   no reinstall. Only native changes (new hooks) need a new dylib.

Deferred: meta toggles (pushback cap, True Dodge, Anchor immunity), spectators, desync handling.

## Architecture

```
 joiner's iPhone                                   host's iPhone
 ┌───────────────────────────────┐                 ┌──────────────────────────────────┐
 │ Rebirth client (IL2CPP)       │                 │ Rebirth client + engine (local)  │
 │   HTTP ──► URL hook ──┬─► 127.0.0.1:18080 (own engine: PvE)                       │
 │                      └─► host:8180 (multiplayer API) ──► BlazingOnline host server│
 │   Photon UDP ──► socket hook ──► host:5058/5055/5056 ──►  (JS in JavaScriptCore,  │
 │ BlazingOnline.dylib: hooks, overlay menu, updater │       native UDP/HTTP/SQLite) │
 └───────────────────────────────┘                 └──────────────────────────────────┘
```

- **Verified targets in the official offline IPA**: the client's server table
  (`Data/resources.assets`, 8 entries) is `http://127.0.0.1:18080/`, the local engine;
  Photon points at `127.0.0.1:15058` / `127.0.0.1:15055` (global-metadata). The official
  libraries contain no URL-protocol hook, so ours does not compete with one.
- **HTTP routing**: an Objective-C runtime hook on the client's request path (UnityWebRequest on
  iOS goes through NSURLSession) rewrites only the multiplayer paths to the chosen host. Bodies
  use the game's normal encryption, which our server code already speaks.
- **Startup patch (every phone)**: Rebirth's engine ships empty `m_rtpvp_open` /
  `m_rtpvp2_open` tables (our import of its startup showed both empty), so the client keeps the
  Ultimate Showdown screens closed. The mod routes `api/base/startup.json` through a local proxy
  in the dylib that fetches it from the engine, opens the league windows (qualifier `_p` open,
  final `_f` and CPU exhibition `_c` closed, `hours` all day; the rule that fixed bot-only
  matching on our server, see CLAUDE.md), and re-encrypts it (AES-128-CBC with the game's fixed
  key, `shared/src/codec.ts`; CommonCrypto on iOS).
- **Photon routing**: Rebirth's build points Photon at dead loopback ports (127.0.0.1:15058 /
  15055, docs/online.md); a socket-level hook sends those to the host. The host's Authenticate
  response hands out its own master/game addresses, as the realtime server does today.
- **Identity**: joiners present the player id/uuid/ssid their own Rebirth engine issued; the host
  trusts the LAN session (optional host password) and keys its leaderboard by uuid.
- **Host server**: the existing TypeScript (`realtime/` plus the rtpvp2/rules logic) bundled to
  one JS file; native bindings supply UDP sockets, an HTTP listener, SQLite, AES/DH crypto and
  timers. The same bundle runs under Node for PC testing.
- **Overlay**: the mod's own menu (Rebirth's overlay drives its engine's admin API and is not
  extended). Entry: a floating button and the three-finger hold, as Rebirth's overlay does.
- **Forge coexistence**: Forge edits local content (engine config, BlazingData, icons); the mod
  hooks networking and adds a menu. No shared symbols or files. Forge custom characters are
  ordinary unit ids in the client and in team data, so host rules treat them like any other unit.

## Build

No iOS toolchain on this PC: the dylib is built in Docker (clang + lld, arm64-apple-ios). Frameworks
the game process already loads (libSystem, libobjc, Foundation) are linked dynamically;
JavaScriptCore and libsqlite3 are `dlopen`ed at runtime. Sideloadly signs it on injection.

## Limits

- Same Wi-Fi works as is; over the internet the host needs port forwarding (a relay is future work).
- iOS suspends background apps: the host keeps the game open while hosting.
- Ranked results live on the host's session leaderboard, not in each player's Rebirth save.

## Order of work

1. Docker iOS build; a minimal dylib that loads in the official IPA (with and without Forge) and
   logs to Files.
2. Photon + multiplayer-HTTP routing to a host; verified against the PC realtime server.
3. Host server in JavaScriptCore on the phone (native UDP/HTTP/SQLite bindings); Host/Join menu.
4. Remote update channel (signed bundles).
5. Host rules, regulations, draft mode, leaderboard, ping, match logs.
6. Team names and more slots.
