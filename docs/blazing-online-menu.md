# Blazing Online: menus, live switching, remote players

## Rebirth's menu (BlazingOverlay.dylib), as dumped 2026-10-14

From the Objective-C metadata (`objcdump.py`-style walk of `__objc_classlist`) and disassembly:

- `BZTable : UITableViewController`
  - `-setSections:` (@0x2eca0) copies the sections, then calls `reloadData` and `updateBackground`.
  - `-reload` (@0x2ec3c) only reloads the table view. Subclasses override it to build their sections.
  - `-refreshState` is the pull-to-refresh handler. `BZPlaceholderScreen` (@0x348e0) does
    `endRefreshing` and then `reload`.
  - `-prompt:text:placeholder:then:` calls `then(trimmed NSString)` only when the text is non-empty
    (@0x304f8).
  - `-tableView:didSelectRowAtIndexPath:` (@0x33138): for a kind 0 row it calls `make()` with no
    arguments and pushes the returned controller onto `navigationController`, then releases it
    (so `make` returns +0). For kinds 5/6/7 it calls `tap()` with no arguments.
  - `-switched:` calls `row.switched(BOOL isOn)`.
- `BZSection +header:footer:rows:`
- `BZRow` factories:
  - `nav:value:symbol:tint:open:` (kind 0; `open` is stored as `make`)
  - `toggle:on:changed:`
  - `button:destructive:tap:`
  - `value:detail:`
  - `segments:selected:changed:`
  - `stepper:…`
  - `check:subtitle:checked:tap:`
  - `headerTitle:subtitle:detail:image:statusColor:tap:`
- `BZRootScreen : BZTable` is the first page. Its `-reload` builds Rebirth's sections.

Mod 0.3.0 (`mod/src/rebirth_menu.c`):
- Swizzles `-[BZRootScreen reload]`: it calls the original, then inserts an "Online" section with a
  nav row "Blazing Online" (value = current status).
- That row opens `BOOnlineScreen`, a runtime subclass of `BZTable` with its own `reload` and
  `refreshState`. It has:
  - a status line;
  - an Online toggle;
  - a Host Address prompt;
  - Test Connection;
  - a "Host on This Phone" toggle.
- If the overlay classes are not present within about 60 s, the four-finger menu (menu.c) remains the
  only menu.

## Switching without a restart

Request routing (proxy.c `BO_ONLINE`) and the Photon redirect (net.c) read the config on every call,
and hosting starts and stops in-process (jsc.c). The one piece of state the game caches is the
league windows: `m_rtpvp2_open` comes from `api/base/startup.json`, which the proxy patches only
when online, and the game fetches it at login. Within one launch, `startup.json` has been fetched
again every time the game went back to the title screen (the 0.2.0 day-change loop), so the game's
own Other menu → Title applies a switch. INFERRED: it should be the same through the menu's Title
button; not yet confirmed on the device.

## Players outside the Wi-Fi

From 0.3.0, the host hands out `127.0.0.1:15055/15056` as its Photon master/game addresses instead
of its LAN IP (host.ts `advertisedPortOffset`). Each player's net.c redirects 127.0.0.1:15055,
15056 and 15058 to the host address that player entered. So the same host works over any route the
player can reach it by:

- **VPN (recommended):** Tailscale (or ZeroTier) on every iPhone, and remote players enter the host
  phone's 100.x address. No router changes are needed, and iOS allows incoming tailnet connections
  to the app. INFERRED: untested on device.
- **Router port forwarding** to the host phone: TCP 8180, plus UDP 5055, 5056 and 5058. Remote
  players enter the public IP. This exposes the host to the internet (no authentication beyond the
  game protocol), and a phone's LAN IP changes unless it is reserved in DHCP.

Both 0.2.x clients and 0.2.x hosts are incompatible with 0.3.0 for this reason (a 0.2.x client
does not redirect 15056), so everyone should update together.

### Returning to the title from the menu (0.3.1)

The Other menu's Title button runs `UIManager.OnClickSelectOtherMenu(11)` → `CheckTitleMoveDialog`
(@0x150DC70). That opens the game's confirm, and its Yes action (`<CheckTitleMoveDialog>b__128_0`
@0x151F168) calls `SceneManager.LoadScene(SceneList.SceneBoot = 17, TransitMode 100, false, false,
null)` (@0x13BFD8C). game.c `bo_game_to_title` makes the same call. It finds the SceneManager through
`SingleObject<SceneManager>_TypeInfo` (RVA 0x2DB01F8) → class +0xB8 → [0], and checks the class
name. After an online/offline switch, or after starting to host from offline, the menu asks
"Return to Title". On Yes it dismisses every menu, then calls this. In 0.3.0 the owner had to go to
the title by hand, which was confirmed working (2026-10-14 log: a hosted match after a manual title
trip).
