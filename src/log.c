#include "bo.h"

static char g_dir[1024];
static pthread_mutex_t g_log_lock;

const char *bo_dir(void) {
  if (!g_dir[0]) {
    const char *home = getenv("HOME");
    snprintf(g_dir, sizeof g_dir, "%s/Documents/BlazingOnline", home ? home : ".");
    mkdir(g_dir, 0755);
  }
  return g_dir;
}

void bo_log_init(void) {
  pthread_mutex_init(&g_log_lock, NULL);
}

void bo_log(const char *fmt, ...) {
  char line[2048];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(line, sizeof line - 1, fmt, ap);
  va_end(ap);
  if (n < 0) return;
  if (n > (int)sizeof line - 2) n = sizeof line - 2;
  line[n++] = '\n';
  char path[1100];
  snprintf(path, sizeof path, "%s/online.log", bo_dir());
  pthread_mutex_lock(&g_log_lock);
  FILE *f = fopen(path, "a");
  if (f) {
    char stamp[32];
    int s = snprintf(stamp, sizeof stamp, "%ld ", (long)time(NULL));
    fwrite(stamp, 1, (size_t)s, f);
    fwrite(line, 1, (size_t)n, f);
    fclose(f);
  }
  pthread_mutex_unlock(&g_log_lock);
}
