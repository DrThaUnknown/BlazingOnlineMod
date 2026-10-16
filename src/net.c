// Photon redirect. Rebirth's client points Photon at dead loopback ports (127.0.0.1:15058 name
// server, :15055 master; docs/online.md). When online, those go to the host's realtime server on
// 5058/5055; the host hands out 127.0.0.1:15055/15056 as its master/game addresses, so those come
// here too and reach whatever address this player entered (LAN, VPN or the internet).
// Rebound only in the game binary (rebind.c), so the engine and other add-ons are untouched.
#include "bo.h"

static int (*orig_connect)(int, const struct sockaddr *, socklen_t);
static ssize_t (*orig_sendto)(int, const void *, size_t, int, const struct sockaddr *, socklen_t);

#define LOOPBACK 0x0100007fu // 127.0.0.1 in network byte order

// Returns true and fills `out` when `in` is one of Rebirth's dead Photon endpoints.
static bool photon_target(const struct sockaddr *in, socklen_t len, struct sockaddr_in *out) {
  if (!g_cfg.online || !g_cfg.host[0] || !in || len < sizeof(struct sockaddr_in) || in->sa_family != AF_INET) return false;
  const struct sockaddr_in *sin = (const struct sockaddr_in *)in;
  if (sin->sin_addr.s_addr != LOOPBACK) return false;
  uint16_t port = bo_htons(sin->sin_port);
  // 15058 name server (Rebirth's config); 15055/15056 master/game as a 0.3+ host hands them out.
  uint16_t to = port == 15058 ? 5058 : port == 15055 ? 5055 : port == 15056 ? 5056 : 0;
  if (!to) return false;
  *out = *sin;
  out->sin_port = bo_htons(to);
  if (inet_pton(AF_INET, g_cfg.host, &out->sin_addr) != 1) return false;
  return true;
}

static int hook_connect(int fd, const struct sockaddr *addr, socklen_t len) {
  struct sockaddr_in to;
  if (photon_target(addr, len, &to)) {
    bo_log("photon: connect %u -> %s:%u", bo_htons(((const struct sockaddr_in *)addr)->sin_port), g_cfg.host, bo_htons(to.sin_port));
    return orig_connect(fd, (const struct sockaddr *)&to, sizeof to);
  }
  return orig_connect(fd, addr, len);
}

static ssize_t hook_sendto(int fd, const void *buf, size_t n, int flags, const struct sockaddr *addr, socklen_t len) {
  struct sockaddr_in to;
  if (photon_target(addr, len, &to)) return orig_sendto(fd, buf, n, flags, (const struct sockaddr *)&to, sizeof to);
  return orig_sendto(fd, buf, n, flags, addr, len);
}

void bo_net_install(void) {
  bo_rebinding list[] = {
    { "connect", (void *)hook_connect, (void **)&orig_connect },
    { "sendto", (void *)hook_sendto, (void **)&orig_sendto },
  };
  int replaced = bo_rebind_symbols(list, 2);
  // dyld binds "lazy" pointers at launch on current iOS, so the saved originals may already be the
  // real functions; fall back to them when a symbol was not imported at all.
  if (!orig_connect) orig_connect = connect;
  if (!orig_sendto) orig_sendto = sendto;
  bo_log("net: photon redirect %s (%d symbol pointers in the game rebound)", replaced ? "installed" : "NOT installed", replaced);
}
