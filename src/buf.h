// Growable byte buffer shared by the proxy, the host's HTTP side and the off-device tests.
#pragma once

typedef struct { char *data; size_t len, cap; } buf_t;

static inline void buf_add(buf_t *b, const void *p, size_t n) {
  if (b->len + n + 1 > b->cap) {
    b->cap = (b->len + n + 1) * 2;
    b->data = realloc(b->data, b->cap);
  }
  memcpy(b->data + b->len, p, n);
  b->len += n;
  b->data[b->len] = 0;
}

static inline void buf_str(buf_t *b, const char *s) { buf_add(b, s, strlen(s)); }

