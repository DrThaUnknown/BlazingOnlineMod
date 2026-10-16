# Compatibility

## What Blazing Online runs inside
`BlazingOnline.dylib` is injected into the official Blazing:Rebirth offline IPA, with or without
Blazing:Forge. At launch it logs (Files > Blazing:Rebirth > BlazingOnline > `online.log`):

```
app: <bundle id> <version> (build <n>); protocol <n>
Blazing Online <ver> loaded; ... Rebirth loader present|absent, Blazing Forge present|absent
```
and warns if `BlazingLoader.dylib` is not loaded (not Rebirth/Forge: routing will not work).

## Tested combinations
| Mod | Host (JS) | Protocol | Container | Result |
|-----|-----------|----------|-----------|--------|
| 0.2.6 | 0.2.6 | none | Blazing:Forge 1.8.7 (bundle `dev.blazingrebirth.ios`, build 18700, engine `ios-onphone-43-startup-migration-app11-data7`) | works (reported by owner) |
| 0.3.1 | 0.3.1 | 1 | Blazing:Forge 1.8.7 | build verified (same load commands as Forge's 0.2.6 dylib); on-phone test pending |

Update this table after each phone test. The mod patches game code located at runtime
(`src/match.c`, `src/rebind.c`) against Rebirth 2.28.0; a different game build is untested.

## Host / guest versions
`GET /healthz` on the host answers `ok bo=<version> proto=<protocol>`.
Test Connection (menu) compares `proto` with the phone's `BO_PROTOCOL`:
- equal: connected;
- different: refused, the message names both versions ("Both phones need the same mod version");
- missing (pre-0.3.1 host): accepted with a warning.

Bump `BO_PROTOCOL` (`src/bo.h`) **and** `PROTOCOL` (`host/src/version.ts`) whenever the HTTP API
or Photon room data changes in a way an old peer would misread. `host/test/version.test.ts`
fails if the two constants or the two versions drift.

## Handing the mod to the Forge developer
Give them `build/BlazingOnline.dylib` (from `./build.sh`) plus the version in `src/bo.h`. It is a
drop-in for `Frameworks/BlazingOnline.dylib`; install name and linked libraries match the 0.2.6
dylib in Forge 1.8.7. Everyone playing together should use the same mod version.
