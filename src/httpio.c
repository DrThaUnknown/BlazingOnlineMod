// HTTP/1.1 message reading and writing shared by the proxy (proxy.c) and the host listener (jsc.c).
#include "bo.h"

bool bo_send_all(int fd, const char *p, size_t n) {
  while (n) {
    ssize_t w = send(fd, p, n, 0);
    if (w <= 0) return false;
    p += w;
    n -= (size_t)w;
  }
  return true;
}

static const char *find_header_end(const char *s, size_t n) {
  for (size_t i = 0; i + 3 < n; i++)
    if (s[i] == '\r' && s[i + 1] == '\n' && s[i + 2] == '\r' && s[i + 3] == '\n') return s + i + 4;
  return NULL;
}

// Case-insensitive header lookup within [headers, end).
static const char *header_value(const char *headers, const char *end, const char *name) {
  size_t nl = strlen(name);
  for (const char *p = headers; p < end; ) {
    const char *eol = strstr(p, "\r\n");
    if (!eol || eol > end) break;
    if ((size_t)(eol - p) > nl && p[nl] == ':') {
      size_t i = 0;
      for (; i < nl; i++) {
        char a = p[i], c = name[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (c >= 'A' && c <= 'Z') c += 32;
        if (a != c) break;
      }
      if (i == nl) {
        p += nl + 1;
        while (*p == ' ') p++;
        return p;
      }
    }
    p = eol + 2;
  }
  return NULL;
}

// Reads one full HTTP message (headers + body by Content-Length, chunked, or until close).
// Returns false on error. `head_len` is the header block length; the decoded body is appended to
// `body` (chunked encoding removed).
bool bo_read_message(int fd, buf_t *raw, size_t *head_len, buf_t *body, bool is_response) {
  char tmp[16384];
  const char *hend = NULL;
  while (!(hend = find_header_end(raw->data ? raw->data : "", raw->len))) {
    ssize_t r = recv(fd, tmp, sizeof tmp, 0);
    if (r <= 0) return false;
    buf_add(raw, tmp, (size_t)r);
  }
  *head_len = (size_t)(hend - raw->data);
  const char *cl = header_value(raw->data, hend, "Content-Length");
  const char *te = header_value(raw->data, hend, "Transfer-Encoding");
  if (te && !strncmp(te, "chunked", 7)) {
    size_t pos = *head_len;
    for (;;) {
      // Make sure a full chunk-size line is buffered.
      const char *eol;
      while (!(eol = strstr(raw->data + pos, "\r\n"))) {
        ssize_t r = recv(fd, tmp, sizeof tmp, 0);
        if (r <= 0) return false;
        buf_add(raw, tmp, (size_t)r);
      }
      size_t size = (size_t)strtol(raw->data + pos, NULL, 16);
      size_t data_at = (size_t)(eol - raw->data) + 2;
      while (raw->len < data_at + size + 2) {
        ssize_t r = recv(fd, tmp, sizeof tmp, 0);
        if (r <= 0) return false;
        buf_add(raw, tmp, (size_t)r);
      }
      if (size == 0) return true;
      buf_add(body, raw->data + data_at, size);
      pos = data_at + size + 2;
    }
  }
  if (cl) {
    size_t want = (size_t)strtol(cl, NULL, 10);
    while (raw->len - *head_len < want) {
      ssize_t r = recv(fd, tmp, sizeof tmp, 0);
      if (r <= 0) return false;
      buf_add(raw, tmp, (size_t)r);
    }
    buf_add(body, raw->data + *head_len, want);
    return true;
  }
  if (!is_response) return true; // a request without a body
  buf_add(body, raw->data + *head_len, raw->len - *head_len);
  for (;;) {
    ssize_t r = recv(fd, tmp, sizeof tmp, 0);
    if (r <= 0) return true;
    buf_add(body, tmp, (size_t)r);
  }
}

