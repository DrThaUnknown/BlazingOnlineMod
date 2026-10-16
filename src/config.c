#include "bo.h"

bo_config g_cfg = { false, "", false };
int g_engine_port = 18080;

// Reads "--urls" "http://127.0.0.1:<port>" from Rebirth's BlazingEngine.json, preferring the
// Documents override the way BlazingLoader does, so a Rebirth update that moves the engine keeps
// the routing working. Leaves 18080 when nothing usable is found.
static bool engine_port_from(const char *path) {
  FILE *f = fopen(path, "r");
  if (!f) return false;
  static char buf[65536];
  size_t n = fread(buf, 1, sizeof buf - 1, f);
  fclose(f);
  buf[n] = 0;
  const char *u = strstr(buf, "http://127.0.0.1:");
  if (!u) return false;
  long port = strtol(u + 17, NULL, 10);
  if (port <= 0 || port > 65535) return false;
  g_engine_port = (int)port;
  return true;
}

void bo_engine_port_detect(void) {
  char path[1200];
  const char *home = getenv("HOME");
  snprintf(path, sizeof path, "%s/Documents/BlazingEngine/BlazingEngine.json", home ? home : ".");
  if (engine_port_from(path)) return;
  const char *exe = _dyld_get_image_name(0); // .../BNEI0249.app/BNEI0249
  const char *slash = exe ? strstr(exe, ".app/") : NULL;
  if (!slash) return;
  snprintf(path, sizeof path, "%.*s.app/Frameworks/BlazingEngine.json", (int)(slash - exe), exe);
  engine_port_from(path);
}

static void config_path(char *out, size_t n) { snprintf(out, n, "%s/config.json", bo_dir()); }

// The file is ours and tiny: {"online":true,"host":"192.168.1.137"}. A full JSON parser is not
// needed; unknown or malformed content simply leaves the defaults (offline).
void bo_config_load(void) {
  char path[1100], buf[1024];
  config_path(path, sizeof path);
  FILE *f = fopen(path, "r");
  if (!f) return;
  size_t n = fread(buf, 1, sizeof buf - 1, f);
  fclose(f);
  buf[n] = 0;
  g_cfg.online = strstr(buf, "\"online\":true") != NULL || strstr(buf, "\"online\": true") != NULL;
  g_cfg.hosting = strstr(buf, "\"hosting\":true") != NULL;
  const char *h = strstr(buf, "\"host\"");
  if (h && (h = strchr(h + 6, '"'))) {
    h++;
    size_t i = 0;
    while (h[i] && h[i] != '"' && i < sizeof g_cfg.host - 1) { g_cfg.host[i] = h[i]; i++; }
    g_cfg.host[i] = 0;
  }
}

void bo_config_save(void) {
  char path[1100], buf[256];
  config_path(path, sizeof path);
  int n = snprintf(buf, sizeof buf, "{\"online\":%s,\"host\":\"%s\",\"hosting\":%s}\n", g_cfg.online ? "true" : "false",
                   g_cfg.host, g_cfg.hosting ? "true" : "false");
  FILE *f = fopen(path, "w");
  if (!f) return;
  fwrite(buf, 1, (size_t)n, f);
  fclose(f);
}
