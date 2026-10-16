# Porting Blazing:Rebirth to a server-authoritative LAN server

Date: 2026-10-12. Status: approved, in progress.

## Decision

The owner wants **online multiplayer and online solo missions** — solo meaning "playing a mission
by yourself against our server", not offline play. Nothing runs the game logic on the phone.

So:

- **Our TypeScript server is the sole authority.** Unchanged from the existing hard rule.
- **The official 2.28.0 client stays unmodified**, with Rebirth's dylibs stripped as
  `tools/repoint_ipa.py` already does.
- **Blazing:Rebirth is a reference implementation and content donor, never a shipped runtime.**

## Why Rebirth cannot ship

`BlazingEngine.dylib` (98.6 MB) is an embedded ASP.NET Core / Kestrel HTTP server that runs
*inside the app*. Its own config (`BlazingEngine.json`) shows the shape:

```
"net":   "loopback"
"allow": ["127.0.0.1"]
"args":  ["--contentRoot", "{data}/content", "--urls", "http://127.0.0.1:18080"]

NARUTO_PUBLIC_BASE_URL = http://127.0.0.1:18080
NARUTO_ASSET_BASE_URL  = http://127.0.0.1:18080/assets/
NARUTO_DATABASE_PATH   = {home}/Documents/BlazingEngine/save.db
NARUTO_GAMEPLAY_MODE   = full
```

It is the backend, on the device, with a local SQLite save. Two phones each running one share
nothing, so keeping it is incompatible with multiplayer. It is also iOS arm64, so it cannot be
moved to the LAN box.

What it *is* good for: it answers protocol questions authoritatively, and its content is authored
as plain JSON we already have extracted under `build/rebirth-config/config/`.

## Plan

**0 — Diagnostics.** Install `build/enable-private-logging.mobileconfig`; one script that pulls
syslog + SmartBeat + screenshot around a test run. Blocking: the game's own Unity log lines are
`<private>`, so client-side faults (the battle white squares, jutsus) cannot currently be
diagnosed at all. See `docs/battle-assets.md`.

**1 — The oracle.** Repack the Rebirth IPA unstripped with the engine bound to the LAN
(`--urls http://0.0.0.0:18080`, `allow` extended), then record every endpoint's real
request/response into a fixture corpus and diff our server against it. This closes the remaining
`INFERRED` entries in `docs/open-questions.md` by observation rather than guesswork.

**2 — Content import.** Port Rebirth's catalogs into our server as data. Each import is a
generator under `tools/`, with the output committed and guarded by a test against the real master.

**3 — Authority and multiplayer.** Friends, support characters, shared state, PvP.

## Content inventory (sub-project 2)

Under `build/rebirth-config/config/`, ~32 MB authored as JSON:

| Catalog | Size | Covers |
|---|---|---|
| `core/missions/mission-objectives-2.28.0.json` | 7.2 MB | mission objectives / sub-missions |
| `catalogs/sroad-implementation-2.28.0.json` | 9.4 MB | Shinobi Road |
| `core/characters/cache-valid-characters-2.28.0.json` | 3.9 MB | summonable/valid characters |
| `catalogs/achievements-2.28.0.json` | 3.5 MB | achievements |
| `core/characters/unit-experience-2.28.0.json` | 2.4 MB | unit exp curves |
| `catalogs/story-implementation-2.28.0.json` | 2.3 MB | story stages and banners |
| `catalogs/event-missions-2.28.0.json` | 1.8 MB | event missions |
| `summons/` | 0.6 MB | banners, pools, step-ups, rotation |
| `banners/artwork-2.28.0.json` | 129 KB | banner artwork ids |
| `core/missions/mission-coin-rewards`, `mission-lucky-drops` | 26 KB | ryo and lucky drops |
| `catalogs/{news,presents,private-jewel-shop,featured,changelog,events}.json` | small | home-screen content |

### Done

**Summons.** `tools/import_rebirth_summons.py` turns `summons/rotation.json` plus its banner and
pool files into `server/config/gacha.json`. Rebirth's banner files carry the client's
`GashaRareData` fields in camelCase, so the import is an explicit rename (note the client's own
`addtion` typo, and `startsAt`/`endsAt` → `op_date`/`ed_date`) plus a pool lookup. The import
*merges*: hand-written banners the rotation does not define are kept, because the tutorial summon
points at banner 1 and dropping it would fail `api/gasha/tutorial.json` mid-tutorial.

Result: 13 banners live (1 hand-written, 3 festival, 9 step-up), 603 pool units, all resolving
against the master.

Two things this surfaced:

1. **A free-summon hole.** `Gacha.price` returned `once_consume_num ?? 0` for a single pull
   without checking one was offered. Step-up banners carry `once_consume_num: 0` and sell only the
   multi-pull, so a client asking for `count: 1` on any of the nine step-ups got a **free summon**.
   A pull mode priced at 0 now means "not offered", not "free". `first_free_flag` is the real
   free-pull mechanism and is still unimplemented, so no banner may resolve to a zero charge.
2. **Tests were running on shipped content.** `testServer` paired the real `gacha.json` with
   `fixtureMaster`, so importing real unit ids emptied every pool and broke four suites. Tests now
   use `fixtureGacha()` alongside `fixtureMaster()`, and `server/test/content.test.ts` separately
   checks the *shipped* config against the *real* master — the pairing that actually breaks in
   production.

### Next

Events, news, presents and jewel shop (small, high visibility on the home screen), then mission
objectives and coin rewards, then story and S-Road.

## Testing

- Fixture suites stay on `fixtureMaster` + `fixtureGacha`; no test reads shipped content except
  `content.test.ts`, which skips when the build outputs are absent.
- Every importer gets `--check` so CI can prove the committed output matches its source.
