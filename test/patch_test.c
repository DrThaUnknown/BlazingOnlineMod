// Off-device test of the startup patch: crypto.c + proxy.c's json_set/patch_startup, compiled for
// Linux with CCCrypt shimmed onto OpenSSL. Reads an encrypted body on stdin, writes the patched
// encrypted body to stdout; test/run.sh checks it against vendor/shared/codec.ts.
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdbool.h>

int CCCrypt(uint32_t op, uint32_t alg, uint32_t options, const void *key, size_t keyLength, const void *iv,
            const void *in, size_t inLen, void *out, size_t outAvail, size_t *moved) {
  (void)alg; (void)options; (void)keyLength; (void)outAvail;
  EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new();
  EVP_CipherInit_ex(c, EVP_aes_128_cbc(), NULL, key, iv, op == 0);
  EVP_CIPHER_CTX_set_padding(c, 0);
  int a = 0, b = 0;
  EVP_CipherUpdate(c, out, &a, in, (int)inLen);
  EVP_CipherFinal_ex(c, (unsigned char *)out + a, &b);
  EVP_CIPHER_CTX_free(c);
  *moved = (size_t)(a + b);
  return 0;
}
void bo_log(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap); fputc('\n', stderr); }

#define kCCEncrypt 0
#define kCCDecrypt 1
#define kCCAlgorithmAES 0
typedef int32_t CCCryptorStatus;
char *bo_decrypt_body(const char *b64, size_t len, size_t *out_len);
char *bo_encrypt_body(const char *plain, size_t len, size_t *out_len);
#define BO_TEST 1
#include "../src/crypto_impl.inc"
#include "../src/patch_impl.inc"


static char *recode_test(const char *body, size_t len, const char *path, bool request, bool host, size_t *out_len) {
  size_t plain_len;
  char *plain = bo_decrypt_body(body, len, &plain_len);
  if (!plain) return NULL;
  char *t = request ? transform_request(path, plain, host) : transform_response(path, plain, host);
  free(plain);
  char *enc = bo_encrypt_body(t, strlen(t), out_len);
  free(t);
  return enc;
}

static int fails;
static void expect(bool ok, const char *what) { if (!ok) { fprintf(stderr, "FAIL: %s\n", what); fails++; } }

// Player-id translation and host revisions on plain JSON.
static void id_tests(void) {
  char *r = transform_request("/api/base/auth.json", "{\"uuid\":\"DEV-1\",\"player_id\":0}", false);
  free(r);
  char *a = transform_response("/api/base/auth.json", "{\"player_id\":10000000001,\"ssid\":\"x\",\"date\":\"2026-10-13T21:30:00-04:00\",\"main_revision\":7,\"assetbundle_revision\":9}", false);
  expect(g_local_id == 10000000001ull && g_online_id >= 20000000000ull, "auth learns local id and derives an online id");
  char want[64];
  snprintf(want, sizeof want, "\"player_id\":%llu", (unsigned long long)g_online_id);
  expect(strstr(a, want) != NULL, "auth answer shows the online id");
  free(a);
  // Game -> engine: online id back to the engine's own; other numbers untouched.
  char req[256];
  snprintf(req, sizeof req, "{\"player_id\":%llu,\"mission_id\":100001,\"x\":\"%llu\"}", (unsigned long long)g_online_id, (unsigned long long)g_online_id);
  char *e = transform_request("/api/mission/startsolo.json", req, false);
  expect(strstr(e, "\"player_id\":10000000001,") != NULL && strstr(e, "\"mission_id\":100001") != NULL, "engine sees its own id, other numbers kept");
  free(e);
  // Game -> host: keeps the online id.
  char *h = transform_request("/api/rtpvp2/matching.json", req, true);
  expect(strstr(h, want) != NULL, "host sees the online id");
  free(h);
  // Engine answers: local id -> online id, but not inside longer numbers.
  char *p = transform_response("/api/base/player.json", "{\"u_player\":{\"player_id\":10000000001},\"big\":100000000012,\"s\":\"10000000001\"}", false);
  expect(strstr(p, want) != NULL && strstr(p, "100000000012") && strstr(p, "\"s\":\"10000000001\""), "player answer translated, longer numbers and strings untouched");
  free(p);
  // Host answers get this phone's engine revisions.
  char *q = transform_response("/api/rtpvp2/top.json", "{\"status_code\":200,\"date\":\"2026-10-14T01:30:00.000Z\",\"main_revision\":0,\"assetbundle_revision\":0}", true);
  expect(strstr(q, "\"main_revision\":7") && strstr(q, "\"assetbundle_revision\":9"), "host answer carries the engine revisions");
  expect(strstr(q, "\"date\":\"2026-10-13T21:30:00-04:00\"") != NULL, "host answer carries the engine's local date, not the host clock");
  free(q);
  // Only top-level date/revision count: startup's tables carry their own nested "date" fields.
  char *st = transform_response("/api/base/startup.json",
                                "{\"m_login_bonus\":{\"1\":{\"date\":\"2000-01-01T00:00:00\",\"main_revision\":1}},\"date\":\"2026-10-13T21:45:00-04:00\",\"main_revision\":7}", false);
  free(st);
  char *q2 = transform_response("/api/rtpvp2/top.json", "{\"status_code\":200,\"date\":\"x\",\"main_revision\":0}", true);
  expect(strstr(q2, "\"date\":\"2026-10-13T21:45:00-04:00\"") && strstr(q2, "\"main_revision\":7"), "nested date/revision ignored, top-level used");
  free(q2);
  // With nothing saved, base/player gets seven empty slots (the game's editor needs them).
  char *empty = transform_response("/api/base/player.json", "{\"u_status\":{}}", false);
  expect(strstr(empty, "\"7\":{\"team_no\":7") && strstr(empty, "\"1\":{\"team_no\":1,") && strstr(empty, "\"main_team_flag\":1"), "seven empty team slots when nothing is saved");
  free(empty);
  // The Ultimate Showdown team is kept on the phone and shown again in base/player.
  char *ans = team_save("{\"player_id\":1,\"u_chara_team_rtpvp2\":{\"1\":{\"team_no\":1,\"member1_chara_no\":61,\"main_team_flag\":1}}}");
  expect(strstr(ans, "\"status_code\":200") && strstr(ans, "\"main_revision\":7"), "team save answered locally with engine revisions");
  free(ans);
  char *pl = transform_response("/api/base/player.json", "{\"u_chara_team_rtpvp2\":{},\"u_status\":{\"coin\":5}}", false);
  expect(strstr(pl, "\"member1_chara_no\":61") && strstr(pl, "\"coin\":5"), "base/player shows the saved team, rest untouched");
  free(pl);
  // A later save of only slot 2 keeps slot 1.
  char *ans2 = team_save("{\"u_chara_team_rtpvp2\":{\"2\":{\"team_no\":2,\"member1_chara_no\":77,\"main_team_flag\":0}}}");
  free(ans2);
  char *pl2 = transform_response("/api/base/player.json", "{}", false);
  expect(strstr(pl2, "\"member1_chara_no\":61") && strstr(pl2, "\"member1_chara_no\":77"), "partial team save keeps the other slots");
  free(pl2);
  // The engine's u_status rides along on host PvP results (the result screen shows exp as new minus old).
  char *pr = transform_response("/api/base/player.json", "{\"u_status\":{\"player_exp\":1500,\"player_rank\":99}}", false);
  free(pr);
  char *res = transform_response("/api/rtpvp2/result.json", "{\"status_code\":200,\"u_rtpvp2\":{}}", true);
  expect(strstr(res, "\"u_status\":{\"player_exp\":1500,\"player_rank\":99}") != NULL, "host result carries the engine's u_status");
  free(res);
  // Offline: startup is not patched.
  g_test_online = false;
  char *o = transform_response("/api/base/startup.json", "{\"m_rtpvp2_open\":{}}", false);
  expect(strstr(o, "\"m_rtpvp2_open\":{}") != NULL, "offline startup left alone");
  free(o);
  g_test_online = true;
}

int main(int argc, char **argv) {
  if (argc > 1 && !strcmp(argv[1], "ids")) {
    id_tests();
    if (!fails) fprintf(stderr, "PASS: id translation, host revisions, offline startup\n");
    return fails ? 1 : 0;
  }
  static char in[1 << 24];
  size_t n = fread(in, 1, sizeof in - 1, stdin);
  in[n] = 0;
  size_t out_len;
  char *out = recode_test(in, n, "/api/base/startup.json", false, false, &out_len);
  if (!out) return 1;
  fwrite(out, 1, out_len, stdout);
  return 0;
}
