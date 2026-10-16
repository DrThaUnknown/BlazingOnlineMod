// The Blazing Online menu: a native UIAlertController with the host address and Online/Offline.
// Opened with a four-finger hold (Rebirth's own overlay already uses the three-finger hold), and
// shown once automatically on the first launch so players can find it.
#include "bo.h"

typedef struct { double x, y, w, h; } bo_rect;

static id g_target;                // instance of our gesture-target class
static id g_alert;                 // the alert currently shown (to read its text field)
void (*bo_menu_changed)(void);     // rebirth_menu.c: refresh its screen after a change

static id nsstr(const char *s) { return MSG(id, id, SEL, const char *)(CLS("NSString"), SELN("stringWithUTF8String:"), s); }
static const char *cstr(id s) { return s ? MSG(const char *, id, SEL)(s, SELN("UTF8String")) : ""; }

static id key_window(void) {
  id app = MSG(id, id, SEL)(CLS("UIApplication"), SELN("sharedApplication"));
  id win = MSG(id, id, SEL)(app, SELN("keyWindow"));
  if (!win) {
    id wins = MSG(id, id, SEL)(app, SELN("windows"));
    win = MSG(id, id, SEL)(wins, SELN("firstObject"));
  }
  return win;
}

static id top_controller(void) {
  id vc = MSG(id, id, SEL)(key_window(), SELN("rootViewController"));
  for (id next; vc && (next = MSG(id, id, SEL)(vc, SELN("presentedViewController"))); ) vc = next;
  return vc;
}

// The league windows come from startup.json, which the game fetches at login: a change between
// online and offline applies at the next title screen. The menu offers to go there (game.c).
static void ask_title_later(void *arg);
static const char *APPLY_HINT = "Go back to the title screen to apply it (Other menu, Title), no restart needed.";

static void apply(bool online) {
  id fields = MSG(id, id, SEL)(g_alert, SELN("textFields"));
  id field = MSG(id, id, SEL)(fields, SELN("firstObject"));
  bo_set_online(online, cstr(MSG(id, id, SEL)(field, SELN("text"))));
}

void bo_set_online(bool online, const char *text) {
  bool was = g_cfg.online;
  size_t i = 0;
  while (text && text[i] && text[i] != ' ' && i < sizeof g_cfg.host - 1) { g_cfg.host[i] = text[i]; i++; }
  if (text) g_cfg.host[i] = 0;
  g_cfg.online = online && g_cfg.host[0];
  bo_config_save();
  bo_log("menu: %s (host %s)", g_cfg.online ? "online" : "offline", g_cfg.host[0] ? g_cfg.host : "-");
  if (g_cfg.online) bo_lan_poke();
  if (was != g_cfg.online) dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC / 2), MAIN_QUEUE, NULL, ask_title_later);
  if (bo_menu_changed) bo_menu_changed();
}

const char *bo_status_text(char *buf, size_t n) {
  snprintf(buf, n, "%s%s%s", g_cfg.online ? "Online, host " : "Offline", g_cfg.online ? g_cfg.host : "", bo_host_running() ? " (hosting)" : "");
  return buf;
}

void bo_notice(const char *title, const char *msg) {
  id n = MSG(id, id, SEL, id, id, long)(CLS("UIAlertController"), SELN("alertControllerWithTitle:message:preferredStyle:"),
                                         nsstr(title), nsstr(msg), 1);
  id ok = MSG(id, id, SEL, id, long, void (^)(id))(CLS("UIAlertAction"), SELN("actionWithTitle:style:handler:"), nsstr("OK"), 0, (void (^)(id))0);
  MSG(void, id, SEL, id)(n, SELN("addAction:"), ok);
  id vc = top_controller();
  if (vc) MSG(void, id, SEL, id, signed char, id)(vc, SELN("presentViewController:animated:completion:"), n, YES, nil);
}

static void title_now(void *arg) {
  (void)arg;
  if (!bo_game_to_title()) bo_notice("Go to the title screen", APPLY_HINT);
}

// "Return to Title" closes every menu (ours, Rebirth's) and then takes the game to its title screen.
void bo_ask_title(const char *title, const char *msg) {
  char full[640];
  snprintf(full, sizeof full, "%s%sThe game goes back to the title screen and logs in again to apply it. No restart needed.", msg, msg[0] ? "\n\n" : "");
  id a = MSG(id, id, SEL, id, id, long)(CLS("UIAlertController"), SELN("alertControllerWithTitle:message:preferredStyle:"),
                                         nsstr(title), nsstr(full), 1);
  id go = MSG(id, id, SEL, id, long, void (^)(id))(CLS("UIAlertAction"), SELN("actionWithTitle:style:handler:"), nsstr("Return to Title"), 0, ^(id x) {
    (void)x;
    id root = MSG(id, id, SEL)(key_window(), SELN("rootViewController"));
    MSG(void, id, SEL, signed char, id)(root, SELN("dismissViewControllerAnimated:completion:"), YES, nil);
    dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC * 6 / 10), MAIN_QUEUE, NULL, title_now);
  });
  id later = MSG(id, id, SEL, id, long, void (^)(id))(CLS("UIAlertAction"), SELN("actionWithTitle:style:handler:"), nsstr("Later"), 1, (void (^)(id))0);
  MSG(void, id, SEL, id)(a, SELN("addAction:"), go);
  MSG(void, id, SEL, id)(a, SELN("addAction:"), later);
  id vc = top_controller();
  if (vc) MSG(void, id, SEL, id, signed char, id)(vc, SELN("presentViewController:animated:completion:"), a, YES, nil);
}

static void ask_title_later(void *arg) {
  (void)arg;
  bo_ask_title(g_cfg.online ? "Online" : "Offline", g_cfg.online ? "Ranked and friend battles now go to the host." : "PvP goes back to Rebirth's own offline mode.");
}

// Host on this phone: start the in-mod server and point this phone's own game at it.
static void toggle_hosting(void *arg) {
  (void)arg;
  if (bo_host_running()) {
    bo_host_stop();
    g_cfg.hosting = false;
    bo_config_save();
    bo_log("menu: hosting stopped");
    bo_notice("Hosting stopped", "Other players can no longer join this phone. To play offline, also Go Offline.");
    if (bo_menu_changed) bo_menu_changed();
    return;
  }
  bool was = g_cfg.online;
  char addr[64], msg[512];
  if (!bo_host_start(addr, sizeof addr)) {
    bo_notice("Can't host", "Connect this iPhone to Wi-Fi first. Other players must be on the same Wi-Fi.");
    return;
  }
  snprintf(g_cfg.host, sizeof g_cfg.host, "%s", addr);
  g_cfg.online = true;
  g_cfg.hosting = true;
  bo_lan_poke();
  bo_config_save();
  snprintf(msg, sizeof msg, "Other players enter this address in their Blazing Online menu:\n\n%s\n\nKeep the game open while hosting.", addr);
  bo_log("menu: hosting on %s", addr);
  if (bo_menu_changed) bo_menu_changed();
  if (was) bo_notice("Hosting", msg);
  else bo_ask_title("Hosting", msg); // this phone was offline: it joins its own server after a title trip
}

static void test_done(bool ok, const char *msg) {
  bo_notice(ok ? "Connection OK" : "Connection failed", msg);
}

void bo_toggle_hosting(void) { dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC / 2), MAIN_QUEUE, NULL, toggle_hosting); }

static void run_test(void *arg) {
  (void)arg;
  bo_test_host(test_done);
}

void bo_run_test(void) { dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC / 2), MAIN_QUEUE, NULL, run_test); }

static void show_menu(void) {
  if (g_alert) return;
  char msg[512];
  snprintf(msg, sizeof msg,
           "Version %s\nNow: %s%s%s\n\nEnter the host's address and Go Online, or Host so others can join you. "
           "Missions, summons and saves stay on this phone either way. Switching applies at the next title screen.",
           BO_VERSION, g_cfg.online ? "online, host " : "offline", g_cfg.online ? g_cfg.host : "",
           bo_host_running() ? " (hosting)" : "");
  g_alert = MSG(id, id, SEL, id, id, long)(CLS("UIAlertController"), SELN("alertControllerWithTitle:message:preferredStyle:"),
                                           nsstr("Blazing Online"), nsstr(msg), 1);
  MSG(void, id, SEL, void (^)(id))(g_alert, SELN("addTextFieldWithConfigurationHandler:"), ^(id field) {
    MSG(void, id, SEL, id)(field, SELN("setPlaceholder:"), nsstr("e.g. 192.168.1.137"));
    MSG(void, id, SEL, id)(field, SELN("setText:"), nsstr(g_cfg.host));
    MSG(void, id, SEL, long)(field, SELN("setKeyboardType:"), 2); // UIKeyboardTypeNumbersAndPunctuation
  });
  id online = MSG(id, id, SEL, id, long, void (^)(id))(CLS("UIAlertAction"), SELN("actionWithTitle:style:handler:"),
                                                     nsstr("Go Online"), 0, ^(id a) { (void)a; apply(true); g_alert = nil; });
  id offline = MSG(id, id, SEL, id, long, void (^)(id))(CLS("UIAlertAction"), SELN("actionWithTitle:style:handler:"),
                                                      nsstr("Go Offline"), 0, ^(id a) { (void)a; apply(false); g_alert = nil; });
  id cancel = MSG(id, id, SEL, id, long, void (^)(id))(CLS("UIAlertAction"), SELN("actionWithTitle:style:handler:"),
                                                     nsstr("Close"), 1, ^(id a) { (void)a; g_alert = nil; });
  id host = MSG(id, id, SEL, id, long, void (^)(id))(CLS("UIAlertAction"), SELN("actionWithTitle:style:handler:"),
                                                   nsstr(bo_host_running() ? "Stop Hosting" : "Host on This Phone"), 0, ^(id a) {
    (void)a;
    g_alert = nil;
    // Let this alert finish dismissing before presenting the next one.
    dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC / 2), MAIN_QUEUE, NULL, toggle_hosting);
  });
  MSG(void, id, SEL, id)(g_alert, SELN("addAction:"), online);
  MSG(void, id, SEL, id)(g_alert, SELN("addAction:"), host);
  id test = MSG(id, id, SEL, id, long, void (^)(id))(CLS("UIAlertAction"), SELN("actionWithTitle:style:handler:"),
                                                   nsstr("Test Connection"), 0, ^(id a) {
    (void)a;
    apply(g_cfg.online); // keep the mode, take the typed address
    g_alert = nil;
    dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC / 2), MAIN_QUEUE, NULL, run_test);
  });
  MSG(void, id, SEL, id)(g_alert, SELN("addAction:"), test);
  MSG(void, id, SEL, id)(g_alert, SELN("addAction:"), offline);
  MSG(void, id, SEL, id)(g_alert, SELN("addAction:"), cancel);
  id vc = top_controller();
  if (!vc) { g_alert = nil; return; }
  MSG(void, id, SEL, id, signed char, id)(vc, SELN("presentViewController:animated:completion:"), g_alert, YES, nil);
}

static void on_gesture(id self, SEL cmd, id recognizer) {
  (void)self; (void)cmd;
  if (MSG(long, id, SEL)(recognizer, SELN("state")) == 1) show_menu(); // UIGestureRecognizerStateBegan
}

static void attach(void *ctx) {
  (void)ctx;
  id win = key_window();
  if (!win) { dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC), MAIN_QUEUE, NULL, attach); return; }
  Class cls = objc_getClass("BOMenuTarget");
  if (!cls) {
    cls = objc_allocateClassPair(objc_getClass("NSObject"), "BOMenuTarget", 0);
    class_addMethod(cls, SELN("onGesture:"), (IMP)on_gesture, "v@:@");
    objc_registerClassPair(cls);
  }
  g_target = MSG(id, id, SEL)(MSG(id, id, SEL)((id)cls, SELN("alloc")), SELN("init"));
  id g = MSG(id, id, SEL)(CLS("UILongPressGestureRecognizer"), SELN("alloc"));
  g = MSG(id, id, SEL, id, SEL)(g, SELN("initWithTarget:action:"), g_target, SELN("onGesture:"));
  MSG(void, id, SEL, unsigned long)(g, SELN("setNumberOfTouchesRequired:"), 4);
  MSG(void, id, SEL, double)(g, SELN("setMinimumPressDuration:"), 0.6);
  MSG(void, id, SEL, signed char)(g, SELN("setCancelsTouchesInView:"), NO);
  MSG(void, id, SEL, id)(win, SELN("addGestureRecognizer:"), g);
  bo_log("menu: four-finger hold attached");
  bo_rebirth_menu_install();
  if (g_cfg.online || g_cfg.hosting) bo_lan_poke(); // the app is on screen now, so iOS can ask
  if (!g_cfg.host[0]) show_menu(); // first launch: let the player find it
}

void bo_menu_install(void) {
  // Wait for Unity's window and Rebirth's first-run setup screen.
  dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, 8 * NSEC_PER_SEC), MAIN_QUEUE, NULL, attach);
}
