// HTTP routing. The official client's server table points at Rebirth's engine
// (http://127.0.0.1:<port>/, port read from BlazingEngine.json). Every request the game builds goes
// through -[NSURLRequest initWithURL:cachePolicy:timeoutInterval:] or -[NSMutableURLRequest setURL:];
// both are swizzled so game API calls (api/...) go to the local proxy (proxy.c), which decides
// between the engine and the host. Assets and everything else go to the engine untouched.
#include "bo.h"

static id (*orig_init)(id, SEL, id, unsigned long, double);
static void (*orig_set_url)(id, SEL, id);

static id make_url(const char *s) {
  id str = MSG(id, id, SEL, const char *)(CLS("NSString"), SELN("stringWithUTF8String:"), s);
  return MSG(id, id, SEL, id)(CLS("NSURL"), SELN("URLWithString:"), str);
}

// Returns a replacement NSURL, or nil to keep the original.
static id route(id url) {
  if (!url) return nil;
  id abs = MSG(id, id, SEL)(url, SELN("absoluteString"));
  const char *s = abs ? MSG(const char *, id, SEL)(abs, SELN("UTF8String")) : NULL;
  char engine[48];
  int elen = snprintf(engine, sizeof engine, "http://127.0.0.1:%d/", g_engine_port);
  if (!s || strncmp(s, engine, (size_t)elen) != 0) return nil;
  const char *rest = s + elen;
  char out[2048];
  if (strncmp(rest, "api/", 4) != 0) return nil;
  snprintf(out, sizeof out, "http://127.0.0.1:%d/%s", BO_PROXY_PORT, rest);
  return make_url(out);
}

static id hook_init(id self, SEL cmd, id url, unsigned long policy, double timeout) {
  id routed = route(url);
  return orig_init(self, cmd, routed ? routed : url, policy, timeout);
}

static void hook_set_url(id self, SEL cmd, id url) {
  id routed = route(url);
  orig_set_url(self, cmd, routed ? routed : url);
}

void bo_http_install(void) {
  Method m1 = class_getInstanceMethod(objc_getClass("NSURLRequest"), SELN("initWithURL:cachePolicy:timeoutInterval:"));
  Method m2 = class_getInstanceMethod(objc_getClass("NSMutableURLRequest"), SELN("setURL:"));
  if (m1) orig_init = (id(*)(id, SEL, id, unsigned long, double))method_setImplementation(m1, (IMP)hook_init);
  if (m2) orig_set_url = (void (*)(id, SEL, id))method_setImplementation(m2, (IMP)hook_set_url);
  bo_log("http: routing installed (init %s, setURL %s)", m1 ? "hooked" : "missing", m2 ? "hooked" : "missing");
}
