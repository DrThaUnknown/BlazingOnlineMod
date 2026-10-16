// Portable request/response transforms of the local proxy (proxy.c), shared with
// mod/test/patch_test.c. Works on decrypted JSON text; proxy.c handles HTTP and crypto.
//
// 1. Player ids. Every Rebirth engine numbers its first account 10000000001, so two phones would
//    meet online with the same id (the league code keys rooms and players on it). The game is shown
//    a unique online id derived from the device uuid; the local engine keeps seeing its own id.
// 2. Startup. When online, the engine's empty m_rtpvp_open / m_rtpvp2_open tables get the league
//    row that opens Ultimate Showdown (offline, Rebirth's closed screens are kept).
// 3. Revisions. Host answers carry the host's main/assetbundle revision; they are replaced with
//    this phone's engine values so the game never thinks it needs a data update.

#ifndef BO_TEST
#define BO_ONLINE (g_cfg.online && g_cfg.host[0])
#else
static bool g_test_online = true;
#define BO_ONLINE g_test_online
#endif

// Qualifier league open (season type 2, real matching); final and CPU exhibition windows closed.
// The same row that fixed bot-only ranked matching on our server (server/config/startup.json).
#define LEAGUE_ROW \
  "{\"1\":{\"rtpvp2_id\":1,\"news_id_e\":0,\"news_id_p\":0,\"news_id_f\":0,\"news_id_a\":0," \
  "\"op_date\":\"2000-01-01T00:00:00Z\",\"ed_date\":\"2099-12-31T23:59:59Z\"," \
  "\"op_date_p\":\"2000-01-01T00:00:00Z\",\"ed_date_p\":\"2099-12-31T23:59:59Z\"," \
  "\"op_date_f\":\"2000-01-01T00:00:00Z\",\"ed_date_f\":\"2000-01-02T00:00:00Z\"," \
  "\"op_date_c\":\"2000-01-01T00:00:00Z\",\"ed_date_c\":\"2000-01-02T00:00:00Z\"," \
  "\"hours\":\"111111111111111111111111\"}}"

static const char *const MULTIPLAYER[] = {
  "api/rtpvp2/", "api/rtpvp/", "api/player/detail.json",
  "api/mission/startmulti", "api/mission/resultmulti", "api/mission/tapmulti",
};

#include "buf.h"

static uint64_t g_local_id, g_online_id;     // the engine's player id and the one the game sees
// Called when the mapping is first learned, so proxy.c can persist it: the game sends the online id
// it remembers from the last session before this session's login (seen in Rebirth's engine log).
static void (*g_on_ids_learned)(void);
static char g_uuid[80];                      // device uuid, from request bodies
static char g_main_rev[32], g_asset_rev[32]; // last revision tokens the engine sent
// Last `date` token the engine sent. Host answers get it instead of the host's own clock: the engine
// dates in local time and the game treats a jump in the response date as a day change (back to the
// title, forever, when the two disagree about the day; owner's 0.2.0 test).
static char g_date[64];
// The engine's latest top-level u_status (rank, exp, stamina...). The host does not own account state,
// but the PvP result screen shows exp gained as new minus old u_status, so a result without it showed
// negative exp (owner's 0.2.2 test). It is added to host result answers.
static char *g_status_json;

static bool is_multiplayer(const char *path) {
  for (size_t i = 0; i < sizeof MULTIPLAYER / sizeof *MULTIPLAYER; i++)
    if (strstr(path, MULTIPLAYER[i])) return true;
  return false;
}

// FNV-1a 64: the online id is 20000000000 + h % 8999999999, stable per device and engine account.
static uint64_t online_id_for(const char *uuid, uint64_t local) {
  uint64_t h = 1469598103934665603ull;
  char key[128];
  int n = snprintf(key, sizeof key, "%s:%llu", uuid, (unsigned long long)local);
  for (int i = 0; i < n; i++) { h ^= (uint8_t)key[i]; h *= 1099511628211ull; }
  return 20000000000ull + h % 8999999999ull;
}

static bool is_digit(char c) { return c >= '0' && c <= '9'; }

// Replaces every JSON number token equal to `from` with `to` (whole tokens only, never digits
// inside a longer number or a string of digits next to letters).
static char *replace_number(const char *json, uint64_t from, uint64_t to) {
  char f[24], t[24];
  int fl = snprintf(f, sizeof f, "%llu", (unsigned long long)from);
  snprintf(t, sizeof t, "%llu", (unsigned long long)to);
  buf_t out = {0};
  const char *p = json, *hit;
  while ((hit = strstr(p, f))) {
    bool start = hit == json || (!is_digit(hit[-1]) && hit[-1] != '.' && hit[-1] != '"' && (hit[-1] < 'A' || hit[-1] > 'z'));
    char after = hit[fl];
    bool end = !is_digit(after) && after != '.' && after != '"' && (after < 'A' || after > 'z');
    buf_add(&out, p, (size_t)(hit - p));
    buf_str(&out, start && end ? t : f);
    p = hit + fl;
  }
  buf_str(&out, p);
  return out.data;
}

// Returns the end of the JSON value starting at v (object, array, string or scalar).
static const char *json_skip(const char *v) {
  const char *e = v;
  if (*e == '{' || *e == '[') {
    int depth = 0;
    bool in_str = false;
    for (; *e; e++) {
      if (in_str) {
        if (*e == '\\') e++;
        else if (*e == '"') in_str = false;
      } else if (*e == '"') in_str = true;
      else if (*e == '{' || *e == '[') depth++;
      else if (*e == '}' || *e == ']') {
        if (--depth == 0) return e + 1;
      }
    }
    return e;
  }
  if (*e == '"') {
    for (e++; *e && *e != '"'; e++) if (*e == '\\') e++;
    return *e ? e + 1 : e;
  }
  while (*e && *e != ',' && *e != '}' && *e != ']') e++;
  return e;
}

static const char *ws(const char *p) {
  while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++;
  return p;
}

// Value of the TOP-LEVEL key in a JSON object, or NULL. Nested keys of the same name (startup's
// tables have their own "date" fields, for example) are never matched.
static const char *json_find(const char *json, const char *key) {
  const char *p = ws(json);
  if (*p != '{') return NULL;
  p = ws(p + 1);
  size_t kl = strlen(key);
  while (*p == '"') {
    const char *k = p + 1, *ke = k;
    while (*ke && *ke != '"') { if (*ke == '\\') ke++; ke++; }
    if (!*ke) return NULL;
    p = ws(ke + 1);
    if (*p != ':') return NULL;
    const char *v = ws(p + 1);
    if ((size_t)(ke - k) == kl && !memcmp(k, key, kl)) return v;
    p = ws(json_skip(v));
    if (*p != ',') return NULL;
    p = ws(p + 1);
  }
  return NULL;
}

// The raw top-level token for `key` (number, or string with quotes), copied into out.
static bool json_token(const char *json, const char *key, char *out, size_t n) {
  const char *v = json_find(json, key);
  if (!v) return false;
  size_t i = 0;
  if (*v == '"') {
    out[i++] = *v++;
    while (*v && *v != '"' && i < n - 2) out[i++] = *v++;
    if (*v == '"' && i < n - 1) out[i++] = '"';
  } else {
    while (*v && *v != ',' && *v != '}' && *v != ']' && i < n - 1) out[i++] = *v++;
  }
  out[i] = 0;
  return i > 0;
}

// Replaces the JSON value of top-level `"key":` with `value` (or appends the pair when absent).
static char *json_set(const char *json, const char *key, const char *value) {
  char pat[64];
  snprintf(pat, sizeof pat, "\"%s\":", key);
  const char *v = json_find(json, key);
  buf_t out = {0};
  if (!v) {
    const char *end = json + strlen(json);
    while (end > json && end[-1] != '}') end--;
    if (end == json) return strdup(json);
    buf_add(&out, json, (size_t)(end - 1 - json));
    buf_str(&out, end - 1 > json && *ws(json + 1) != '}' ? "," : "");
    buf_str(&out, pat);
    buf_str(&out, value);
    buf_str(&out, "}");
    return out.data;
  }
  const char *e = json_skip(v);
  buf_add(&out, json, (size_t)(v - json));
  buf_str(&out, value);
  buf_str(&out, e);
  return out.data;
}

static char *swap(char *old, char *next) { free(old); return next; }
static char *team_inject(char *plain_response, const char *path);
static char *json_raw(const char *json, const char *key);

// Request body (decrypted) on its way out. Returns a new string.
static char *transform_request(const char *path, const char *plain, bool to_host) {
  char uuid[80];
  if (json_token(plain, "uuid", uuid, sizeof uuid) && uuid[0] == '"') {
    size_t n = strlen(uuid);
    if (n > 2 && n - 2 < sizeof g_uuid) { memcpy(g_uuid, uuid + 1, n - 2); g_uuid[n - 2] = 0; }
  }
  (void)path;
  if (!to_host && g_online_id) return replace_number(plain, g_online_id, g_local_id);
  return strdup(plain);
}

// Response body (decrypted) on its way back to the game. Returns a new string.
static char *transform_response(const char *path, const char *plain, bool from_host) {
  char *out = strdup(plain);
  if (from_host) {
    if (g_main_rev[0]) out = swap(out, json_set(out, "main_revision", g_main_rev));
    if (g_asset_rev[0]) out = swap(out, json_set(out, "assetbundle_revision", g_asset_rev));
    if (g_date[0]) out = swap(out, json_set(out, "date", g_date));
    if (g_status_json && strstr(path, "api/rtpvp2/result.json")) out = swap(out, json_set(out, "u_status", g_status_json));
    return out;
  }
  json_token(out, "main_revision", g_main_rev, sizeof g_main_rev);
  json_token(out, "date", g_date, sizeof g_date);
  {
    char *status = json_raw(out, "u_status");
    if (status && status[0] == '{') { free(g_status_json); g_status_json = status; }
    else free(status);
  }
  json_token(out, "assetbundle_revision", g_asset_rev, sizeof g_asset_rev);
  if (strstr(path, "api/base/auth.json") || strstr(path, "api/player/regist.json")) {
    char id[32];
    if (json_token(out, "player_id", id, sizeof id) && is_digit(id[0])) {
      uint64_t local = (uint64_t)strtol(id, NULL, 10);
      if (local && local != g_local_id && local != g_online_id) {
        g_local_id = local;
        g_online_id = online_id_for(g_uuid[0] ? g_uuid : "device", local);
        bo_log("proxy: engine player %llu is %llu online", (unsigned long long)g_local_id, (unsigned long long)g_online_id);
        if (g_on_ids_learned) g_on_ids_learned();
      }
    }
  }
  if (g_online_id) out = swap(out, replace_number(out, g_local_id, g_online_id));
  out = team_inject(out, path);
  if (BO_ONLINE && strstr(path, "api/base/startup.json")) {
    out = swap(out, json_set(out, "m_rtpvp_open", LEAGUE_ROW));
    out = swap(out, json_set(out, "m_rtpvp2_open", LEAGUE_ROW));
    bo_log("proxy: startup patched (league windows open)");
  }
  return out;
}

// ---- Ultimate Showdown team (api/chara/teamrtpvp2.json), kept on this phone ----
// Rebirth's engine has no PvP, so it neither stores this team nor returns it in base/player. The
// proxy answers the save itself (proxy.c), remembers the team here and adds it to the engine's
// base/player answers, so the game shows the saved team again after every restart.
static char *g_team_json; // raw JSON object of u_chara_team_rtpvp2, or NULL

// Raw JSON value of top-level `"key":` (object, array or scalar), newly allocated; NULL if absent.
static char *json_raw(const char *json, const char *key) {
  const char *v = json_find(json, key);
  if (!v) return NULL;
  const char *e = json_skip(v);
  char *out = malloc((size_t)(e - v) + 1);
  memcpy(out, v, (size_t)(e - v));
  out[e - v] = 0;
  return out;
}

// Seven empty Ultimate Showdown team slots, team 1 the main one: what our PC server sends when a
// player has saved nothing (server/src/game/state.ts padTeams). Without any slots the game's team
// editor shows unusable placeholder teams (full, level 9999, cannot be changed; owner's 0.2.1 test).
#define TEAM_SLOTS 7
static void empty_slot(buf_t *b, int n) {
  char t[200];
  snprintf(t, sizeof t, "{\"team_no\":%d,\"member1_chara_no\":0,\"member2_chara_no\":0,\"member3_chara_no\":0,"
           "\"member4_chara_no\":0,\"member5_chara_no\":0,\"member6_chara_no\":0,\"main_team_flag\":%d}", n, n == 1);
  buf_str(b, t);
}

// Builds all seven slots: each from `latest` if it has that slot, else `previous`, else empty.
static char *team_merge(const char *latest, const char *previous) {
  buf_t out = {0};
  buf_str(&out, "{");
  for (int n = 1; n <= TEAM_SLOTS; n++) {
    char key[4];
    snprintf(key, sizeof key, "%d", n);
    char *slot = latest ? json_raw(latest, key) : NULL;
    if (!slot && previous) slot = json_raw(previous, key);
    if (n > 1) buf_str(&out, ",");
    buf_str(&out, "\"");
    buf_str(&out, key);
    buf_str(&out, "\":");
    if (slot && slot[0] == '{') buf_str(&out, slot);
    else empty_slot(&out, n);
    free(slot);
  }
  buf_str(&out, "}");
  return out.data;
}

/** A plain empty success answer with this phone's engine revisions and date. */
static char *local_ok(void) {
  buf_t out = {0};
  buf_str(&out, "{\"status_code\":200,\"reason_phrase\":\"OK\",\"error_message\":\"\",\"achievement_id_list\":[],"
                "\"achievement_badge_count\":0,\"panel_badge_count\":0,\"main_revision\":");
  buf_str(&out, g_main_rev[0] ? g_main_rev : "0");
  buf_str(&out, ",\"assetbundle_revision\":");
  buf_str(&out, g_asset_rev[0] ? g_asset_rev : "0");
  if (g_date[0]) {
    buf_str(&out, ",\"date\":");
    buf_str(&out, g_date);
  }
  buf_str(&out, "}");
  return out.data;
}

/** The save request: remembers the team; returns the plain JSON answer for the game. */
static char *team_save(const char *plain_request) {
  char *team = json_raw(plain_request, "u_chara_team_rtpvp2");
  if (team && team[0] == '{') {
    char *merged = team_merge(team, g_team_json);
    free(g_team_json);
    g_team_json = merged;
  }
  free(team);
  return local_ok();
}

// Home-screen PvP reads that must never block normal play when the host is unreachable.
static bool background_read(const char *path) {
  return strstr(path, "api/rtpvp2/toplight.json") || strstr(path, "api/rtpvp2/checktitlereward.json");
}

/** Adds the remembered team to an engine base/player answer. */
static char *team_inject(char *plain_response, const char *path) {
  if (!strstr(path, "api/base/player.json")) return plain_response;
  char *teams = team_merge(NULL, g_team_json); // saved slots, the rest empty
  char *out = json_set(plain_response, "u_chara_team_rtpvp2", teams);
  free(teams);
  return swap(plain_response, out);
}
