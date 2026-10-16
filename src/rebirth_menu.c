// A "Blazing Online" page inside Rebirth's own menu (BlazingOverlay.dylib, the floating menu
// button), so players find online play next to Rebirth's settings. The four-finger menu (menu.c)
// stays as the fallback when the overlay is missing.
//
// Rebirth's menu, from BlazingOverlay's Objective-C metadata (docs/blazing-online-menu.md):
//   BZTable (UITableViewController): -setSections:(NSArray<BZSection>) stores and reloads the table;
//     -reload builds the sections; -refreshState (pull to refresh) ends refreshing and calls -reload
//     (BZPlaceholderScreen @0x348e0); -prompt:text:placeholder:then: calls then(trimmed text) only
//     when the text is not empty.
//   BZSection +header:footer:rows:.  BZRow +nav:value:symbol:tint:open: (kind 0: the table calls
//     open() with no arguments and pushes the view controller it returns, @0x3325c),
//     +toggle:on:changed: (changed(BOOL isOn)), +button:destructive:tap: (tap()), +value:detail:.
//   BZRootScreen (the first page) -reload: we call the original, then insert our section.
#include "bo.h"

static id nsstr(const char *s) { return MSG(id, id, SEL, const char *)(CLS("NSString"), SELN("stringWithUTF8String:"), s); }
static const char *cstr(id s) { return s ? MSG(const char *, id, SEL)(s, SELN("UTF8String")) : ""; }

static IMP g_root_reload;  // BZRootScreen's own -reload
static id g_screen;        // our page, retained while it may be on screen

static id row_value(const char *title, const char *detail) {
  return MSG(id, id, SEL, id, id)(CLS("BZRow"), SELN("value:detail:"), nsstr(title), nsstr(detail));
}
static id row_button(const char *title, void (^tap)(void)) {
  return MSG(id, id, SEL, id, signed char, void (^)(void))(CLS("BZRow"), SELN("button:destructive:tap:"), nsstr(title), NO, tap);
}
static id row_toggle(const char *title, bool on, void (^changed)(signed char)) {
  return MSG(id, id, SEL, id, signed char, void (^)(signed char))(CLS("BZRow"), SELN("toggle:on:changed:"), nsstr(title), on, changed);
}
static id section(const char *header, const char *footer, id *rows, unsigned long n) {
  id arr = MSG(id, id, SEL, id *, unsigned long)(CLS("NSArray"), SELN("arrayWithObjects:count:"), rows, n);
  return MSG(id, id, SEL, id, id, id)(CLS("BZSection"), SELN("header:footer:rows:"), header ? nsstr(header) : nil, footer ? nsstr(footer) : nil, arr);
}

static void ask_host(id table) {
  MSG(void, id, SEL, id, id, id, void (^)(id))(table, SELN("prompt:text:placeholder:then:"), nsstr("Host Address"),
                                               nsstr(g_cfg.host), nsstr("e.g. 192.168.1.106"), ^(id text) {
    bo_set_online(true, cstr(text));
  });
}

// Our page's -reload: build the sections from the current state.
static void screen_reload(id self, SEL cmd) {
  (void)cmd;
  if (self != g_screen) {
    MSG(id, id, SEL)(self, SELN("retain"));
    if (g_screen) MSG(void, id, SEL)(g_screen, SELN("release"));
    g_screen = self;
  }
  char status[160], addr[96], hostfoot[256];
  bo_status_text(status, sizeof status);
  snprintf(addr, sizeof addr, "Host Address: %s", g_cfg.host[0] ? g_cfg.host : "not set");
  if (bo_host_running())
    snprintf(hostfoot, sizeof hostfoot, "Hosting. Other players enter %s as their host address. Keep the game open while others play.", g_cfg.host);
  else
    snprintf(hostfoot, sizeof hostfoot, "Run the online server on this iPhone. Other players on the same Wi-Fi join with this phone's address.");

  id status_rows[] = { row_value("Now", status), row_value("Version", BO_VERSION) };
  id play_rows[] = {
    row_toggle("Online", g_cfg.online, ^(signed char on) {
      if (on && !g_cfg.host[0]) { ask_host(self); MSG(void, id, SEL)(self, SELN("reload")); return; }
      bo_set_online(on, NULL);
    }),
    row_button(addr, ^{ ask_host(self); }),
    row_button("Test Connection", ^{ bo_run_test(); }),
  };
  id host_rows[] = {
    row_toggle("Host on This Phone", bo_host_running(), ^(signed char on) {
      if ((bool)on != bo_host_running()) bo_toggle_hosting();
    }),
  };
  id sections[] = {
    section(NULL, NULL, status_rows, 2),
    section("Play Online", "Ranked and friend battles against players on the same Wi-Fi. Missions, summons and saves stay on this iPhone. "
                           "Switching online or offline offers to return to the title screen to apply it; no restart needed.",
            play_rows, 3),
    section("Host", hostfoot, host_rows, 1),
  };
  id arr = MSG(id, id, SEL, id *, unsigned long)(CLS("NSArray"), SELN("arrayWithObjects:count:"), sections, 3);
  MSG(void, id, SEL, id)(self, SELN("setSections:"), arr);
}

static void screen_refresh(id self, SEL cmd) {
  (void)cmd;
  id rc = MSG(id, id, SEL)(self, SELN("refreshControl"));
  if (rc) MSG(void, id, SEL)(rc, SELN("endRefreshing"));
  screen_reload(self, SELN("reload"));
}

static id make_screen(void) {
  Class cls = objc_getClass("BOOnlineScreen");
  if (!cls) {
    cls = objc_allocateClassPair(objc_getClass("BZTable"), "BOOnlineScreen", 0);
    class_addMethod(cls, SELN("reload"), (IMP)screen_reload, "v16@0:8");
    class_addMethod(cls, SELN("refreshState"), (IMP)screen_refresh, "v16@0:8");
    objc_registerClassPair(cls);
  }
  id vc = MSG(id, id, SEL)(MSG(id, id, SEL)((id)cls, SELN("alloc")), SELN("init"));
  MSG(void, id, SEL, id)(vc, SELN("setTitle:"), nsstr("Blazing Online"));
  return MSG(id, id, SEL)(vc, SELN("autorelease")); // the table retains what open() returns
}

static void refresh_visible(void) {
  if (g_screen) MSG(void, id, SEL)(g_screen, SELN("reload"));
}

// BZRootScreen -reload: Rebirth's sections, then ours as the second section.
static void root_reload(id self, SEL cmd) {
  ((void (*)(id, SEL))g_root_reload)(self, cmd);
  id sections = MSG(id, id, SEL)(self, SELN("sections"));
  id list = MSG(id, id, SEL, id)(CLS("NSMutableArray"), SELN("arrayWithArray:"), sections);
  char status[160];
  id row = MSG(id, id, SEL, id, id, id, id, id (^)(void))(CLS("BZRow"), SELN("nav:value:symbol:tint:open:"), nsstr("Blazing Online"),
                                                          nsstr(bo_status_text(status, sizeof status)),
                                                          nsstr("antenna.radiowaves.left.and.right"), nil, ^id { return make_screen(); });
  id rows[] = { row };
  unsigned long n = MSG(unsigned long, id, SEL)(list, SELN("count"));
  MSG(void, id, SEL, id, unsigned long)(list, SELN("insertObject:atIndex:"), section("Online", NULL, rows, 1), n ? 1 : 0);
  MSG(void, id, SEL, id)(self, SELN("setSections:"), list);
}

static int g_tries;

static void install(void *arg) {
  (void)arg;
  Class root = objc_getClass("BZRootScreen");
  bool ok = root && objc_getClass("BZTable") && objc_getClass("BZRow") && objc_getClass("BZSection") &&
            class_getClassMethod(objc_getClass("BZRow"), SELN("nav:value:symbol:tint:open:")) &&
            class_getInstanceMethod(objc_getClass("BZTable"), SELN("prompt:text:placeholder:then:"));
  if (!ok) {
    // BlazingLoader loads the overlay after this dylib; give it a minute.
    if (++g_tries < 20) dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC), MAIN_QUEUE, NULL, install);
    else bo_log("rebirth menu: Rebirth's menu not found; use the four-finger hold");
    return;
  }
  Method m = class_getInstanceMethod(root, SELN("reload"));
  g_root_reload = method_setImplementation(m, (IMP)root_reload);
  bo_menu_changed = refresh_visible;
  bo_log("rebirth menu: Blazing Online page added to Rebirth's menu");
}

void bo_rebirth_menu_install(void) { install(NULL); }
