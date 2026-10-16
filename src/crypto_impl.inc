// Portable body of crypto.c, shared with mod/test/patch_test.c.

static const char KEY16[] = "78U#5e8!4@481eeU";
static const char IV16[] = "@#5U552&91127e)!";
static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int b64val(int c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

static uint8_t *b64decode(const char *in, size_t len, size_t *out_len) {
  uint8_t *out = malloc(len / 4 * 3 + 3);
  size_t o = 0;
  uint32_t acc = 0;
  int bits = 0;
  for (size_t i = 0; i < len; i++) {
    int v = b64val((unsigned char)in[i]);
    if (v < 0) continue; // skips quotes, '=', whitespace
    acc = (acc << 6) | (uint32_t)v;
    bits += 6;
    if (bits >= 8) { bits -= 8; out[o++] = (uint8_t)(acc >> bits); }
  }
  *out_len = o;
  return out;
}

static char *b64encode(const uint8_t *in, size_t len, size_t *out_len) {
  char *out = malloc((len + 2) / 3 * 4 + 1);
  size_t o = 0;
  for (size_t i = 0; i < len; i += 3) {
    uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < len ? (uint32_t)in[i + 1] << 8 : 0) | (i + 2 < len ? in[i + 2] : 0);
    out[o++] = B64[v >> 18 & 63];
    out[o++] = B64[v >> 12 & 63];
    out[o++] = i + 1 < len ? B64[v >> 6 & 63] : '=';
    out[o++] = i + 2 < len ? B64[v & 63] : '=';
  }
  out[o] = 0;
  *out_len = o;
  return out;
}

char *bo_decrypt_body(const char *b64, size_t len, size_t *out_len) {
  size_t raw_len;
  uint8_t *raw = b64decode(b64, len, &raw_len);
  if (!raw_len || raw_len % 16) { free(raw); return NULL; }
  char *plain = malloc(raw_len + 1);
  size_t moved = 0;
  if (CCCrypt(kCCDecrypt, kCCAlgorithmAES, 0, KEY16, 16, IV16, raw, raw_len, plain, raw_len, &moved) != 0) {
    free(raw); free(plain); return NULL;
  }
  free(raw);
  while (moved > 0 && plain[moved - 1] == 0) moved--;
  plain[moved] = 0;
  *out_len = moved;
  return plain;
}

char *bo_encrypt_body(const char *plain, size_t len, size_t *out_len) {
  size_t padded = (len + 15) / 16 * 16;
  if (!padded) padded = 16;
  uint8_t *buf = calloc(1, padded), *enc = malloc(padded);
  memcpy(buf, plain, len);
  size_t moved = 0;
  CCCryptorStatus st = CCCrypt(kCCEncrypt, kCCAlgorithmAES, 0, KEY16, 16, IV16, buf, padded, enc, padded, &moved);
  free(buf);
  if (st != 0) { free(enc); return NULL; }
  char *out = b64encode(enc, moved, out_len);
  free(enc);
  return out;
}
