// The host server on the phone: the mod/host bundle (embedded as host_js.c, or a newer verified
// copy from the update channel later) runs in JavaScriptCore, dlopen()ed at runtime. This file
// provides the global `__bo` bindings declared in mod/host/src/jsc-entry.ts:
//   log, now, random, setInterval/clearInterval, udpBind/udpSend/udpClose/udpPort,
//   httpListen/httpRespond/httpClose, readFile/writeFile.
// All JavaScript runs on one serial GCD queue; socket threads hand events to it.
// Binary data crosses as "binary strings" (one UTF-16 unit per byte).
#include "bo.h"

// ---- JavaScriptCore C API (resolved with dlsym) ----
typedef const struct OpaqueJSContext *JSContextRef;
typedef struct OpaqueJSContext *JSGlobalContextRef;
typedef struct OpaqueJSString *JSStringRef;
typedef struct OpaqueJSValue *JSObjectRef;
typedef const struct OpaqueJSValue *JSValueRef;
typedef JSValueRef (*JSCallback)(JSContextRef, JSObjectRef, JSObjectRef, size_t, const JSValueRef[], JSValueRef *);

static struct {
  JSGlobalContextRef (*GlobalContextCreate)(void *);
  JSObjectRef (*ContextGetGlobalObject)(JSContextRef);
  JSStringRef (*StringCreateWithUTF8CString)(const char *);
  JSStringRef (*StringCreateWithCharacters)(const uint16_t *, size_t);
  void (*StringRelease)(JSStringRef);
  size_t (*StringGetLength)(JSStringRef);
  const uint16_t *(*StringGetCharactersPtr)(JSStringRef);
  size_t (*StringGetMaximumUTF8CStringSize)(JSStringRef);
  size_t (*StringGetUTF8CString)(JSStringRef, char *, size_t);
  JSObjectRef (*ObjectMake)(JSContextRef, void *, void *);
  JSObjectRef (*ObjectMakeFunctionWithCallback)(JSContextRef, JSStringRef, JSCallback);
  void (*ObjectSetProperty)(JSContextRef, JSObjectRef, JSStringRef, JSValueRef, unsigned, JSValueRef *);
  JSValueRef (*ObjectGetProperty)(JSContextRef, JSObjectRef, JSStringRef, JSValueRef *);
  JSValueRef (*ObjectCallAsFunction)(JSContextRef, JSObjectRef, JSObjectRef, size_t, const JSValueRef[], JSValueRef *);
  JSValueRef (*EvaluateScript)(JSContextRef, JSStringRef, JSObjectRef, JSStringRef, int, JSValueRef *);
  double (*ValueToNumber)(JSContextRef, JSValueRef, JSValueRef *);
  JSStringRef (*ValueToStringCopy)(JSContextRef, JSValueRef, JSValueRef *);
  JSObjectRef (*ValueToObject)(JSContextRef, JSValueRef, JSValueRef *);
  JSValueRef (*ValueMakeNumber)(JSContextRef, double);
  JSValueRef (*ValueMakeString)(JSContextRef, JSStringRef);
  JSValueRef (*ValueMakeNull)(JSContextRef);
  JSValueRef (*ValueMakeUndefined)(JSContextRef);
  bool (*ValueIsObject)(JSContextRef, JSValueRef);
  void (*ValueProtect)(JSContextRef, JSValueRef);
  void (*ValueUnprotect)(JSContextRef, JSValueRef);
} J;

static JSGlobalContextRef g_ctx;
static dispatch_queue_t g_q;
static bool g_running;

extern const unsigned char bo_host_js[];
extern const unsigned int bo_host_js_len;

static bool load_jsc(void) {
  void *h = dlopen("/System/Library/Frameworks/JavaScriptCore.framework/JavaScriptCore", RTLD_NOW);
  if (!h) { bo_log("host: JavaScriptCore not available: %s", dlerror()); return false; }
#define SYM(field, name) if (!(*(void **)&J.field = dlsym(h, name))) { bo_log("host: missing %s", name); return false; }
  SYM(GlobalContextCreate, "JSGlobalContextCreate");
  SYM(ContextGetGlobalObject, "JSContextGetGlobalObject");
  SYM(StringCreateWithUTF8CString, "JSStringCreateWithUTF8CString");
  SYM(StringCreateWithCharacters, "JSStringCreateWithCharacters");
  SYM(StringRelease, "JSStringRelease");
  SYM(StringGetLength, "JSStringGetLength");
  SYM(StringGetCharactersPtr, "JSStringGetCharactersPtr");
  SYM(StringGetMaximumUTF8CStringSize, "JSStringGetMaximumUTF8CStringSize");
  SYM(StringGetUTF8CString, "JSStringGetUTF8CString");
  SYM(ObjectMake, "JSObjectMake");
  SYM(ObjectMakeFunctionWithCallback, "JSObjectMakeFunctionWithCallback");
  SYM(ObjectSetProperty, "JSObjectSetProperty");
  SYM(ObjectGetProperty, "JSObjectGetProperty");
  SYM(ObjectCallAsFunction, "JSObjectCallAsFunction");
  SYM(EvaluateScript, "JSEvaluateScript");
  SYM(ValueToNumber, "JSValueToNumber");
  SYM(ValueToStringCopy, "JSValueToStringCopy");
  SYM(ValueToObject, "JSValueToObject");
  SYM(ValueMakeNumber, "JSValueMakeNumber");
  SYM(ValueMakeString, "JSValueMakeString");
  SYM(ValueMakeNull, "JSValueMakeNull");
  SYM(ValueMakeUndefined, "JSValueMakeUndefined");
  SYM(ValueIsObject, "JSValueIsObject");
  SYM(ValueProtect, "JSValueProtect");
  SYM(ValueUnprotect, "JSValueUnprotect");
#undef SYM
  return true;
}

// ---- value helpers (call only on g_q) ----
static char *js_cstr(JSValueRef v) {
  JSStringRef s = J.ValueToStringCopy(g_ctx, v, NULL);
  if (!s) return strdup("");
  size_t n = J.StringGetMaximumUTF8CStringSize(s);
  char *out = malloc(n);
  J.StringGetUTF8CString(s, out, n);
  J.StringRelease(s);
  return out;
}

// Binary string -> bytes (low byte of each UTF-16 unit).
static uint8_t *js_bytes(JSValueRef v, size_t *len) {
  JSStringRef s = J.ValueToStringCopy(g_ctx, v, NULL);
  if (!s) { *len = 0; return malloc(1); }
  size_t n = J.StringGetLength(s);
  const uint16_t *c = J.StringGetCharactersPtr(s);
  uint8_t *out = malloc(n ? n : 1);
  for (size_t i = 0; i < n; i++) out[i] = (uint8_t)c[i];
  J.StringRelease(s);
  *len = n;
  return out;
}

static JSValueRef js_binstr(const uint8_t *p, size_t n) {
  uint16_t *u = malloc((n ? n : 1) * sizeof *u);
  for (size_t i = 0; i < n; i++) u[i] = p[i];
  JSStringRef s = J.StringCreateWithCharacters(u, n);
  free(u);
  JSValueRef v = J.ValueMakeString(g_ctx, s);
  J.StringRelease(s);
  return v;
}

static JSValueRef js_str(const char *s) {
  JSStringRef js = J.StringCreateWithUTF8CString(s);
  JSValueRef v = J.ValueMakeString(g_ctx, js);
  J.StringRelease(js);
  return v;
}

static double num(const JSValueRef *argv, size_t argc, size_t i) {
  return i < argc ? J.ValueToNumber(g_ctx, argv[i], NULL) : 0;
}

static void call_global(const char *name, size_t argc, const JSValueRef *argv) {
  JSStringRef n = J.StringCreateWithUTF8CString(name);
  JSValueRef fn = J.ObjectGetProperty(g_ctx, J.ContextGetGlobalObject(g_ctx), n, NULL);
  J.StringRelease(n);
  if (!fn || !J.ValueIsObject(g_ctx, fn)) return;
  JSValueRef exc = NULL;
  J.ObjectCallAsFunction(g_ctx, J.ValueToObject(g_ctx, fn, NULL), NULL, argc, argv, &exc);
  if (exc) { char *m = js_cstr(exc); bo_log("host: %s threw: %s", name, m); free(m); }
}

// ---- bindings ----
#define BINDING(name) static JSValueRef name(JSContextRef c, JSObjectRef f, JSObjectRef t, size_t argc, const JSValueRef argv[], JSValueRef *exc)
#define UNUSED (void)c; (void)f; (void)t; (void)exc

BINDING(b_log) { UNUSED; if (argc) { char *m = js_cstr(argv[0]); bo_log("host: %s", m); free(m); } return J.ValueMakeUndefined(c); }

BINDING(b_now) {
  UNUSED; (void)argc; (void)argv;
  struct { long sec; int usec; } tv;
  gettimeofday(&tv, NULL);
  return J.ValueMakeNumber(c, (double)tv.sec * 1000.0 + tv.usec / 1000);
}

BINDING(b_random) {
  UNUSED;
  size_t n = (size_t)num(argv, argc, 0);
  if (n > 4096) n = 4096;
  uint8_t buf[4096];
  arc4random_buf(buf, n);
  return js_binstr(buf, n);
}

// Timers: a thread per interval posts the callback onto the JS queue.
typedef struct { int id; double ms; JSValueRef fn; volatile bool stop; } timer_t_;
static timer_t_ *g_timers[64];

static void timer_fire(void *arg) {
  timer_t_ *t = arg;
  if (t->stop || !g_running) return;
  JSValueRef exc = NULL;
  J.ObjectCallAsFunction(g_ctx, J.ValueToObject(g_ctx, t->fn, NULL), NULL, 0, NULL, &exc);
  if (exc) { char *m = js_cstr(exc); bo_log("host: timer threw: %s", m); free(m); }
}

static void *timer_loop(void *arg) {
  timer_t_ *t = arg;
  while (!t->stop) {
    usleep((uint32_t)(t->ms * 1000));
    if (!t->stop) dispatch_async_f(g_q, t, timer_fire);
  }
  return NULL;
}

BINDING(b_set_interval) {
  UNUSED;
  for (int i = 0; i < 64; i++) {
    if (g_timers[i]) continue;
    timer_t_ *tm = calloc(1, sizeof *tm);
    tm->id = i + 1;
    tm->ms = num(argv, argc, 1);
    if (tm->ms < 10) tm->ms = 10;
    tm->fn = argv[0];
    J.ValueProtect(g_ctx, tm->fn);
    g_timers[i] = tm;
    pthread_t th;
    pthread_create(&th, NULL, timer_loop, tm);
    pthread_detach(th);
    return J.ValueMakeNumber(c, tm->id);
  }
  return J.ValueMakeNumber(c, -1);
}

BINDING(b_clear_interval) {
  UNUSED;
  int id = (int)num(argv, argc, 0);
  if (id >= 1 && id <= 64 && g_timers[id - 1]) {
    g_timers[id - 1]->stop = true; // the thread exits on its own; the struct is left to it
    g_timers[id - 1] = NULL;
  }
  return J.ValueMakeUndefined(c);
}

// UDP: one receive thread per socket.
typedef struct { int fd; int port; int handle; volatile bool closed; } udp_t;
static udp_t *g_udp[16];

typedef struct { int handle; uint8_t *data; size_t len; char addr[32]; int port; } udp_msg;

static void udp_deliver(void *arg) {
  udp_msg *m = arg;
  if (g_running && g_udp[m->handle - 1] && !g_udp[m->handle - 1]->closed) {
    JSValueRef args[4] = { J.ValueMakeNumber(g_ctx, m->handle), js_binstr(m->data, m->len), js_str(m->addr), J.ValueMakeNumber(g_ctx, m->port) };
    call_global("__bo_onUdp", 4, args);
  }
  free(m->data);
  free(m);
}

static void *udp_loop(void *arg) {
  udp_t *u = arg;
  uint8_t buf[2048];
  while (!u->closed) {
    struct sockaddr_in from;
    socklen_t fl = sizeof from;
    ssize_t n = recvfrom(u->fd, buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
    if (n <= 0) { if (u->closed) break; continue; }
    udp_msg *m = calloc(1, sizeof *m);
    m->handle = u->handle;
    m->data = malloc((size_t)n);
    memcpy(m->data, buf, (size_t)n);
    m->len = (size_t)n;
    inet_ntop(AF_INET, &from.sin_addr, m->addr, sizeof m->addr);
    m->port = bo_htons(from.sin_port);
    dispatch_async_f(g_q, m, udp_deliver);
  }
  return NULL;
}

BINDING(b_udp_bind) {
  UNUSED;
  int port = (int)num(argv, argc, 0);
  for (int i = 0; i < 16; i++) {
    if (g_udp[i]) continue;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = { sizeof a, AF_INET, bo_htons((uint16_t)port), { 0 }, {0} };
    if (fd < 0 || bind(fd, (struct sockaddr *)&a, sizeof a) != 0) {
      bo_log("host: cannot bind udp %d", port);
      if (fd >= 0) close(fd);
      return J.ValueMakeNumber(c, -1);
    }
    udp_t *u = calloc(1, sizeof *u);
    u->fd = fd;
    u->port = port;
    u->handle = i + 1;
    g_udp[i] = u;
    pthread_t th;
    pthread_create(&th, NULL, udp_loop, u);
    pthread_detach(th);
    return J.ValueMakeNumber(c, u->handle);
  }
  return J.ValueMakeNumber(c, -1);
}

BINDING(b_udp_port) { UNUSED; int h = (int)num(argv, argc, 0); return J.ValueMakeNumber(c, h >= 1 && h <= 16 && g_udp[h - 1] ? g_udp[h - 1]->port : 0); }

BINDING(b_udp_send) {
  UNUSED;
  int h = (int)num(argv, argc, 0);
  if (h < 1 || h > 16 || !g_udp[h - 1] || argc < 4) return J.ValueMakeUndefined(c);
  size_t n;
  uint8_t *data = js_bytes(argv[1], &n);
  char *addr = js_cstr(argv[3]);
  struct sockaddr_in to = { sizeof to, AF_INET, bo_htons((uint16_t)num(argv, argc, 2)), { 0 }, {0} };
  if (inet_pton(AF_INET, addr, &to.sin_addr) == 1) sendto(g_udp[h - 1]->fd, data, n, 0, (struct sockaddr *)&to, sizeof to);
  free(data);
  free(addr);
  return J.ValueMakeUndefined(c);
}

BINDING(b_udp_close) {
  UNUSED;
  int h = (int)num(argv, argc, 0);
  if (h >= 1 && h <= 16 && g_udp[h - 1]) {
    g_udp[h - 1]->closed = true;
    close(g_udp[h - 1]->fd);
    g_udp[h - 1] = NULL;
  }
  return J.ValueMakeUndefined(c);
}

// HTTP: an accept thread per listener; each request is handed to JS, which answers by id.
typedef struct { int fd; int handle; volatile bool closed; } http_t;
static http_t *g_http[4];
static int g_pending_fd[256]; // request id (1-based, cycling) -> client fd
static int g_next_req;
static pthread_mutex_t g_req_lock;

typedef struct { int handle, req; char *method, *path, *body; } http_msg;

static void http_deliver(void *arg) {
  http_msg *m = arg;
  if (g_running) {
    JSValueRef args[5] = { J.ValueMakeNumber(g_ctx, m->handle), J.ValueMakeNumber(g_ctx, m->req), js_str(m->method), js_str(m->path), js_str(m->body) };
    call_global("__bo_onHttp", 5, args);
  }
  free(m->method); free(m->path); free(m->body); free(m);
}

typedef struct { int fd; int handle; } conn_arg;

static void *http_conn(void *arg) {
  conn_arg *ca = arg;
  buf_t raw = {0}, body = {0};
  size_t head = 0;
  if (bo_read_message(ca->fd, &raw, &head, &body, false)) {
    const char *sp1 = strchr(raw.data, ' '), *sp2 = sp1 ? strchr(sp1 + 1, ' ') : NULL;
    if (sp1 && sp2) {
      pthread_mutex_lock(&g_req_lock);
      int req = g_next_req = g_next_req % 256 + 1;
      if (g_pending_fd[req - 1] > 0) close(g_pending_fd[req - 1]); // abandoned long ago
      g_pending_fd[req - 1] = ca->fd;
      pthread_mutex_unlock(&g_req_lock);
      http_msg *m = calloc(1, sizeof *m);
      m->handle = ca->handle;
      m->req = req;
      m->method = calloc(1, (size_t)(sp1 - raw.data) + 1);
      memcpy(m->method, raw.data, (size_t)(sp1 - raw.data));
      m->path = calloc(1, (size_t)(sp2 - sp1));
      memcpy(m->path, sp1 + 1, (size_t)(sp2 - sp1 - 1));
      m->body = strdup(body.data ? body.data : "");
      dispatch_async_f(g_q, m, http_deliver);
      free(raw.data); free(body.data); free(ca);
      return NULL;
    }
  }
  close(ca->fd);
  free(raw.data); free(body.data); free(ca);
  return NULL;
}

static void *http_accept(void *arg) {
  http_t *h = arg;
  while (!h->closed) {
    int c = accept(h->fd, NULL, NULL);
    if (c < 0) { if (h->closed) break; usleep(50000); continue; }
    int one = 1;
    setsockopt(c, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
    conn_arg *ca = malloc(sizeof *ca);
    ca->fd = c;
    ca->handle = h->handle;
    pthread_t th;
    if (pthread_create(&th, NULL, http_conn, ca) == 0) pthread_detach(th);
    else { close(c); free(ca); }
  }
  return NULL;
}

BINDING(b_http_listen) {
  UNUSED;
  int port = (int)num(argv, argc, 0);
  for (int i = 0; i < 4; i++) {
    if (g_http[i]) continue;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = { sizeof a, AF_INET, bo_htons((uint16_t)port), { 0 }, {0} };
    if (fd < 0 || bind(fd, (struct sockaddr *)&a, sizeof a) != 0 || listen(fd, 32) != 0) {
      bo_log("host: cannot listen on http %d", port);
      if (fd >= 0) close(fd);
      return J.ValueMakeNumber(c, -1);
    }
    http_t *h = calloc(1, sizeof *h);
    h->fd = fd;
    h->handle = i + 1;
    g_http[i] = h;
    pthread_t th;
    pthread_create(&th, NULL, http_accept, h);
    pthread_detach(th);
    return J.ValueMakeNumber(c, h->handle);
  }
  return J.ValueMakeNumber(c, -1);
}

BINDING(b_http_respond) {
  UNUSED;
  int req = (int)num(argv, argc, 0);
  if (req < 1 || req > 256 || argc < 4) return J.ValueMakeUndefined(c);
  pthread_mutex_lock(&g_req_lock);
  int fd = g_pending_fd[req - 1];
  g_pending_fd[req - 1] = 0;
  pthread_mutex_unlock(&g_req_lock);
  if (fd <= 0) return J.ValueMakeUndefined(c);
  char *type = js_cstr(argv[2]), *body = js_cstr(argv[3]);
  char head[256];
  int status = (int)num(argv, argc, 1);
  size_t len = strlen(body);
  int hn = snprintf(head, sizeof head, "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                    status, status == 200 ? "OK" : "Error", type, len);
  bo_send_all(fd, head, (size_t)hn);
  bo_send_all(fd, body, len);
  close(fd);
  free(type); free(body);
  return J.ValueMakeUndefined(c);
}

BINDING(b_http_close) {
  UNUSED;
  int h = (int)num(argv, argc, 0);
  if (h >= 1 && h <= 4 && g_http[h - 1]) {
    g_http[h - 1]->closed = true;
    close(g_http[h - 1]->fd);
    g_http[h - 1] = NULL;
  }
  return J.ValueMakeUndefined(c);
}

// Files live in Documents/BlazingOnline/host/<name>; names are plain file names only.
static bool safe_name(const char *n) { return n[0] && !strchr(n, '/') && strcmp(n, ".") && strcmp(n, ".."); }

BINDING(b_read_file) {
  UNUSED;
  if (!argc) return J.ValueMakeNull(c);
  char *name = js_cstr(argv[0]), path[1200];
  JSValueRef out = J.ValueMakeNull(c);
  if (safe_name(name)) {
    snprintf(path, sizeof path, "%s/host/%s", bo_dir(), name);
    FILE *fp = fopen(path, "r");
    if (fp) {
      buf_t b = {0};
      char tmp[8192];
      size_t n;
      while ((n = fread(tmp, 1, sizeof tmp, fp)) > 0) buf_add(&b, tmp, n);
      fclose(fp);
      out = js_str(b.data ? b.data : "");
      free(b.data);
    }
  }
  free(name);
  return out;
}

BINDING(b_write_file) {
  UNUSED;
  if (argc < 2) return J.ValueMakeUndefined(c);
  char *name = js_cstr(argv[0]), *data = js_cstr(argv[1]), path[1200];
  if (safe_name(name)) {
    snprintf(path, sizeof path, "%s/host", bo_dir());
    mkdir(path, 0755);
    snprintf(path, sizeof path, "%s/host/%s", bo_dir(), name);
    FILE *fp = fopen(path, "w");
    if (fp) { fwrite(data, 1, strlen(data), fp); fclose(fp); }
  }
  free(name); free(data);
  return J.ValueMakeUndefined(c);
}

// ---- lifecycle ----
static void define(JSObjectRef obj, const char *name, JSCallback cb) {
  JSStringRef n = J.StringCreateWithUTF8CString(name);
  J.ObjectSetProperty(g_ctx, obj, n, J.ObjectMakeFunctionWithCallback(g_ctx, n, cb), 0, NULL);
  J.StringRelease(n);
}

static char g_start_addr[64];

static void boot(void *arg) {
  (void)arg;
  if (!g_ctx) {
    g_ctx = J.GlobalContextCreate(NULL);
    JSObjectRef bo = J.ObjectMake(g_ctx, NULL, NULL);
    define(bo, "log", b_log);
    define(bo, "now", b_now);
    define(bo, "random", b_random);
    define(bo, "setInterval", b_set_interval);
    define(bo, "clearInterval", b_clear_interval);
    define(bo, "udpBind", b_udp_bind);
    define(bo, "udpPort", b_udp_port);
    define(bo, "udpSend", b_udp_send);
    define(bo, "udpClose", b_udp_close);
    define(bo, "httpListen", b_http_listen);
    define(bo, "httpRespond", b_http_respond);
    define(bo, "httpClose", b_http_close);
    define(bo, "readFile", b_read_file);
    define(bo, "writeFile", b_write_file);
    JSStringRef n = J.StringCreateWithUTF8CString("__bo");
    J.ObjectSetProperty(g_ctx, J.ContextGetGlobalObject(g_ctx), n, bo, 0, NULL);
    J.StringRelease(n);

    char *src = malloc(bo_host_js_len + 1);
    memcpy(src, bo_host_js, bo_host_js_len);
    src[bo_host_js_len] = 0;
    JSStringRef script = J.StringCreateWithUTF8CString(src), url = J.StringCreateWithUTF8CString("host.js");
    free(src);
    JSValueRef exc = NULL;
    J.EvaluateScript(g_ctx, script, NULL, url, 1, &exc);
    J.StringRelease(script);
    J.StringRelease(url);
    if (exc) { char *m = js_cstr(exc); bo_log("host: bundle failed to load: %s", m); free(m); g_running = false; return; }
  }
  JSValueRef args[1] = { js_str(g_start_addr) };
  call_global("__bo_start", 1, args);
}

static void halt(void *arg) {
  (void)arg;
  call_global("__bo_stop", 0, NULL);
}

// This phone's Wi-Fi IPv4 address (en0), or "" when not on Wi-Fi.
static void lan_address(char *out, size_t n) {
  out[0] = 0;
  struct ifaddrs *list = NULL;
  if (getifaddrs(&list) != 0) return;
  for (struct ifaddrs *i = list; i; i = i->ifa_next) {
    if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || strcmp(i->ifa_name, "en0")) continue;
    inet_ntop(AF_INET, &((struct sockaddr_in *)i->ifa_addr)->sin_addr, out, (socklen_t)n);
    break;
  }
  freeifaddrs(list);
}

bool bo_host_start(char *address_out, size_t n) {
  lan_address(g_start_addr, sizeof g_start_addr);
  if (address_out) snprintf(address_out, n, "%s", g_start_addr);
  if (!g_start_addr[0]) { bo_log("host: no Wi-Fi address; join a Wi-Fi network to host"); return false; }
  if (g_running) return true;
  if (!J.GlobalContextCreate && !load_jsc()) return false;
  if (!g_q) {
    g_q = dispatch_queue_create("blazing-online.host", NULL);
    pthread_mutex_init(&g_req_lock, NULL);
  }
  g_running = true;
  dispatch_async_f(g_q, NULL, boot);
  bo_log("host: starting on %s", g_start_addr);
  return true;
}

void bo_host_stop(void) {
  if (!g_running) return;
  dispatch_async_f(g_q, NULL, halt);
  g_running = false;
}

bool bo_host_running(void) { return g_running; }
