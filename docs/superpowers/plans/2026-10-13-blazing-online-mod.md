# Blazing Online mod: implementation plan

Spec: `docs/superpowers/specs/2026-10-13-blazing-online-mod-design.md`. Code lives in `mod/`.

## Milestone 1: the client side works on the official IPA (one phone test)

Native dylib (C, Objective-C runtime via `objc_msgSend`, no SDK; built in Docker):

1. `log` -- `Documents/BlazingOnline/online.log`, one line per event.
2. `config` -- `Documents/BlazingOnline/config.json` `{ "online": bool, "host": "a.b.c.d" }`.
3. `rebind` -- fishhook-style rebinding of the main executable's symbol pointers.
4. `net` -- rebind `connect`/`sendto`: `127.0.0.1:15058|15055` -> `host:5058|5055` when online.
5. `crypto` -- AES-128-CBC (CommonCrypto `CCCrypt`) + base64, matching `shared/src/codec.ts`.
6. `proxy` -- local HTTP/1.1 proxy on `127.0.0.1:18181`; `api/base/startup.json` is fetched
   from the engine, league windows opened, re-encrypted. Other paths pass through untouched.
7. `http` -- swizzle `-[NSURLRequest initWithURL:cachePolicy:timeoutInterval:]` and
   `-[NSMutableURLRequest setURL:]`: startup -> local proxy; multiplayer paths -> `host:8180`
   when online; everything else unchanged (local engine).
8. `menu` -- three-finger long press -> UIAlertController: host address, Online / Offline.
9. `main` -- constructor wiring it together; logs whether Forge and Rebirth's engine are loaded.

Verify in Docker: builds, symbols, load commands. On the phone (one trip): loads with and without
Forge, menu opens, Ultimate Showdown screen opens offline, routing lines appear in the log.

## Milestone 2: the host server

10. `mod/host/` TypeScript: multiplayer HTTP API (rtpvp2/*, player/detail, teamrtpvp2, co-op
    multi) keyed by the player id/uuid each phone's own engine issued, plus the Photon
    LoadBalancing server ported from `realtime/src/photon` onto a small platform interface
    (udp, http, timers, storage). Runs under Node for tests.
11. Native JavaScriptCore host: `dlopen` JavaScriptCore, expose the platform interface (UDP via
    BSD sockets + GCD, HTTP listener on 8180, timers, file storage), load the bundle.
12. Menu: Host (shows this phone's address) / Join.

## Milestone 3: remote updates

13. Signed bundle channel: manifest URL, Ed25519 signature check, cached bundle, fallback to the
    built-in bundle.

## Milestone 4: host rules and session features

14. Rules: bans/allows, boosts, Kage League set, regulation JSON presets, Draft vs Import mode;
    enforced on Photon room join (actor `ParamsJson.teamCharacters`) and on `starthost`.
15. Session leaderboard (ELO), ping display, match-log export.

## Milestone 5: team names and more slots (client hooks).
