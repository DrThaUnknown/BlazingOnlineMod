// Blazing Online entry point. Injected into the official Blazing:Rebirth offline IPA with
// Sideloadly, next to (or without) Blazing Forge. See docs/superpowers/specs/2026-10-13-blazing-online-mod-design.md.
#include "bo.h"

static bool image_loaded(const char *needle) {
  for (uint32_t i = 0, n = _dyld_image_count(); i < n; i++) {
    const char *name = _dyld_get_image_name(i);
    if (name && strstr(name, needle)) return true;
  }
  return false;
}

__attribute__((constructor)) static void bo_init(void) {
  bo_log_init();
  bo_config_load();
  bo_engine_port_detect();
  // BlazingLoader dlopen()s the engine after this constructor runs, so check for its loader instead.
  bo_log("Blazing Online %s loaded; engine port %d; %s%s; Rebirth loader %s, Blazing Forge %s",
         BO_VERSION, g_engine_port, g_cfg.online ? "online, host " : "offline", g_cfg.online ? g_cfg.host : "",
         image_loaded("BlazingLoader.dylib") ? "present" : "absent",
         image_loaded("BlazingForge.dylib") ? "present" : "absent");
  int port = bo_proxy_start();
  bo_log("proxy: %s", port ? "listening on 127.0.0.1:18181" : "failed to start");
  bo_net_install();
  bo_http_install();
  bo_menu_install();
  bo_match_install();
  if (g_cfg.hosting) {
    char addr[64];
    // The Wi-Fi address can change between sessions; follow it so this phone joins its own server.
    if (bo_host_start(addr, sizeof addr) && addr[0] && strcmp(addr, g_cfg.host)) {
      snprintf(g_cfg.host, sizeof g_cfg.host, "%s", addr);
      bo_config_save();
    }
  }
}
