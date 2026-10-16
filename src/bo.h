// Blazing Online: shared declarations between the mod's native modules.
#pragma once
#include "sys.h"
#include "buf.h"

#define BO_VERSION "0.3.1"

// log.c -- Documents/BlazingOnline/online.log
void bo_log_init(void);
void bo_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
const char *bo_dir(void); // Documents/BlazingOnline (created on first use)

// config.c -- Documents/BlazingOnline/config.json
typedef struct {
  bool online;
  char host[64]; // IPv4 of the host phone/PC
  bool hosting;  // this phone runs the host server (restarted at launch)
} bo_config;
extern bo_config g_cfg;
void bo_config_load(void);
void bo_config_save(void);

// rebind.c -- rebind imported symbols in already-loaded images
typedef struct { const char *name; void *replacement; void **original; } bo_rebinding;
int bo_rebind_symbols(const bo_rebinding *list, int count); // returns pointers replaced

// crypto.c -- the game's HTTP body codec (shared/src/codec.ts)
char *bo_decrypt_body(const char *b64, size_t len, size_t *out_len);
char *bo_encrypt_body(const char *plain, size_t len, size_t *out_len);

// httpio.c -- one HTTP message: headers + body (Content-Length, chunked or until close)
bool bo_send_all(int fd, const char *p, size_t n);
bool bo_read_message(int fd, buf_t *raw, size_t *head_len, buf_t *body, bool is_response);

// jsc.c -- the host server (mod/host bundle) in JavaScriptCore
bool bo_host_start(char *address_out, size_t n); // starts hosting; fills this phone's LAN address
void bo_host_stop(void);
bool bo_host_running(void);

// net.c / http.c / proxy.c / menu.c
void bo_net_install(void);
void bo_http_install(void);
int bo_proxy_start(void); // returns the port, 0 on failure
void bo_menu_install(void);
// menu.c actions, shared with rebirth_menu.c (main thread)
void bo_notice(const char *title, const char *msg);
void bo_ask_title(const char *title, const char *msg); // offers Return to Title (game.c)
void bo_set_online(bool online, const char *host); // host NULL keeps the saved address
void bo_toggle_hosting(void);                     // starts/stops hosting and explains it
void bo_run_test(void);                           // Test Connection, result in a notice
const char *bo_status_text(char *buf, size_t n);
extern void (*bo_menu_changed)(void);
void bo_rebirth_menu_install(void); // rebirth_menu.c: a Blazing Online page in Rebirth's own menu
// game.c -- reading and calling into the game (il2cpp)
uintptr_t bo_game_base(void);
bool bo_rd(uintptr_t addr, void *out, size_t n);
uintptr_t bo_rdptr(uintptr_t addr);
uintptr_t bo_singleton(uintptr_t rva_single_typeinfo, const char *class_name);
bool bo_game_to_title(void); // the Other menu's Title, without its confirm (main thread)
void bo_match_install(void); // match.c: random league matching phase lengths (breaks the lockstep)

// Rebirth's engine port, read from its BlazingEngine.json at startup (config.c); 18080 by default.
extern int g_engine_port;
void bo_engine_port_detect(void);
#define BO_PROXY_PORT 18181
#define BO_HOST_HTTP_PORT 8180

// lan.c -- local network: permission prompt, connection test, error names
void bo_lan_poke(void);                                 // makes iOS ask for Local Network access
const char *bo_errname(int err);                        // short explanation of a socket error
void bo_test_host(void (*done)(bool ok, const char *msg)); // TCP check of the host (main thread callback)
