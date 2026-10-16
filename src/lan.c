// Local network helpers. iOS blocks traffic to other devices on the Wi-Fi until the player allows
// "Local Network" for the app; in the 0.2.1 test the prompt never appeared and every connection to
// the host failed silently. bo_lan_poke() sends a tiny UDP broadcast and a datagram to the host,
// the standard way to make iOS show the prompt (the official IPA's Info.plist already carries
// NSLocalNetworkUsageDescription). bo_test_host() backs the menu's Test Connection button.
#include "bo.h"

const char *bo_errname(int err) {
  switch (err) {
    case 0: return "ok";
    case 1: return "not permitted (Local Network access off?)";
    case 49: return "address not available";
    case 50: return "network down";
    case 51: return "network unreachable (not on Wi-Fi?)";
    case 54: return "connection reset";
    case 60: return "timed out (host not running, or another network)";
    case 61: return "refused (host phone not hosting)";
    case 64: return "host is down";
    case 65: return "no route to host (Local Network access off, or another network)";
    default: return "network error";
  }
}

// A request through Apple's URL loading system (NSURLSession). On the owner's iOS the UDP poke
// below never produced the Local Network prompt (0.2.2: Phone B, error 65, no Settings entry),
// while the game's own NSURLSession traffic is what normally trips it, so ask the same way.
static void url_poke(void) {
  if (!g_cfg.host[0]) return;
  char s[128];
  snprintf(s, sizeof s, "http://%s:%d/healthz", g_cfg.host, BO_HOST_HTTP_PORT);
  id str = MSG(id, id, SEL, const char *)(CLS("NSString"), SELN("stringWithUTF8String:"), s);
  id url = MSG(id, id, SEL, id)(CLS("NSURL"), SELN("URLWithString:"), str);
  id session = MSG(id, id, SEL)(CLS("NSURLSession"), SELN("sharedSession"));
  id task = MSG(id, id, SEL, id)(session, SELN("dataTaskWithURL:"), url);
  MSG(void, id, SEL)(task, SELN("resume"));
}

void bo_lan_poke(void) {
  url_poke();
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) return;
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
  const char ping = 0;
  struct sockaddr_in to = { sizeof to, AF_INET, bo_htons(5058), { 0xffffffffu }, {0} };
  sendto(fd, &ping, 1, 0, (struct sockaddr *)&to, sizeof to);
  if (g_cfg.host[0] && inet_pton(AF_INET, g_cfg.host, &to.sin_addr) == 1) sendto(fd, &ping, 1, 0, (struct sockaddr *)&to, sizeof to);
  close(fd);
  bo_log("lan: local network poke sent (iOS asks for Local Network access once)");
}

typedef struct { void (*done)(bool, const char *); bool ok; char msg[256]; } test_job;

static void test_report(void *arg) {
  test_job *j = arg;
  j->done(j->ok, j->msg);
  free(j);
}

static void *test_thread(void *arg) {
  test_job *j = arg;
  struct sockaddr_in to = { sizeof to, AF_INET, bo_htons(BO_HOST_HTTP_PORT), { 0 }, {0} };
  if (!g_cfg.host[0] || inet_pton(AF_INET, g_cfg.host, &to.sin_addr) != 1) {
    snprintf(j->msg, sizeof j->msg, "Enter the host's address first.");
  } else {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct { long sec; int usec; } tv = { 5, 0 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
    if (connect(fd, (struct sockaddr *)&to, sizeof to) != 0) {
      int e = *__error();
      snprintf(j->msg, sizeof j->msg, "Can't reach %s: %s (error %d).", g_cfg.host, bo_errname(e), e);
    } else {
      const char req[] = "GET /healthz HTTP/1.1\r\nHost: host\r\nConnection: close\r\n\r\n";
      char buf[512] = {0};
      bo_send_all(fd, req, sizeof req - 1);
      ssize_t n = recv(fd, buf, sizeof buf - 1, 0);
      j->ok = n > 0 && strstr(buf, "200") != NULL;
      const char *body = j->ok ? strstr(buf, "\r\n\r\n") : NULL;
      const char *pp = body ? strstr(body, "proto=") : NULL;
      int proto = 0;
      if (pp) for (pp += 6; *pp >= '0' && *pp <= '9'; pp++) proto = proto * 10 + (*pp - '0');
      if (!j->ok) {
        snprintf(j->msg, sizeof j->msg, "Reached %s, but it did not answer as a Blazing Online host.", g_cfg.host);
      } else if (pp && proto != BO_PROTOCOL) {
        const char *bp = strstr(body, "bo=");
        char hv[16] = "?";
        if (bp) { int k = 0; for (bp += 3; *bp && *bp != ' ' && *bp != '\r' && *bp != '\n' && k < 15; bp++) hv[k++] = *bp; hv[k] = 0; }
        j->ok = false;
        snprintf(j->msg, sizeof j->msg, "The host at %s runs Blazing Online %s (protocol %d); you have %s (protocol %d). Both phones need the same mod version.",
                 g_cfg.host, hv, proto, BO_VERSION, BO_PROTOCOL);
      } else if (!pp) {
        snprintf(j->msg, sizeof j->msg, "Connected to the host at %s, but it is an older Blazing Online without version info; update it if matches misbehave.", g_cfg.host);
      } else {
        snprintf(j->msg, sizeof j->msg, "Connected to the host at %s (Blazing Online, protocol %d).", g_cfg.host, proto);
      }
    }
    close(fd);
  }
  bo_log("lan: test connection: %s", j->msg);
  dispatch_async_f(MAIN_QUEUE, j, test_report);
  return NULL;
}

void bo_test_host(void (*done)(bool ok, const char *msg)) {
  test_job *j = calloc(1, sizeof *j);
  j->done = done;
  pthread_t t;
  if (pthread_create(&t, NULL, test_thread, j) == 0) pthread_detach(t);
  else free(j);
}
