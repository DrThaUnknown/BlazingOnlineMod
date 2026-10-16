// Local HTTP proxy on 127.0.0.1:18181. http.c sends every game API call here (assets go straight
// to the engine). Multiplayer calls are forwarded to the host when online, everything else to
// Rebirth's engine; bodies are decrypted, transformed (patch_impl.inc: player ids, startup league
// windows, revisions) and re-encrypted. One thread per request.
#include "bo.h"

#include "patch_impl.inc"

// Encrypted body -> transformed encrypted body; NULL when it is not a game body (left as is).
static char *recode(const char *body, size_t len, const char *path, bool request, bool host, size_t *out_len) {
  if (!len) return NULL;
  size_t plain_len;
  char *plain = bo_decrypt_body(body, len, &plain_len);
  if (!plain) return NULL;
  char *t = request ? transform_request(path, plain, host) : transform_response(path, plain, host);
  free(plain);
  char *enc = bo_encrypt_body(t, strlen(t), out_len);
  free(t);
  return enc;
}

// The engine/online id mapping, persisted (patch_impl.inc g_on_ids_learned).
static void ids_file(char *out, size_t n) { snprintf(out, n, "%s/ids.txt", bo_dir()); }

static void ids_save(void) {
  char path[1100], line[64];
  ids_file(path, sizeof path);
  FILE *f = fopen(path, "w");
  if (!f) return;
  int n = snprintf(line, sizeof line, "%llu %llu\n", (unsigned long long)g_local_id, (unsigned long long)g_online_id);
  fwrite(line, 1, (size_t)n, f);
  fclose(f);
}

static void ids_load(void) {
  char path[1100], buf[64] = {0};
  ids_file(path, sizeof path);
  FILE *f = fopen(path, "r");
  if (!f) return;
  fread(buf, 1, sizeof buf - 1, f);
  fclose(f);
  char *end;
  uint64_t local = (uint64_t)strtol(buf, &end, 10), online = (uint64_t)strtol(end, NULL, 10);
  if (local && online) { g_local_id = local; g_online_id = online; }
}

static void team_file(char *out, size_t n) { snprintf(out, n, "%s/team-rtpvp2.json", bo_dir()); }

static void team_load(void) {
  char path[1100];
  team_file(path, sizeof path);
  FILE *f = fopen(path, "r");
  if (!f) return;
  buf_t b = {0};
  char tmp[4096];
  size_t n;
  while ((n = fread(tmp, 1, sizeof tmp, f)) > 0) buf_add(&b, tmp, n);
  fclose(f);
  if (b.data && b.data[0] == '{') g_team_json = b.data;
  else free(b.data);
}

static void send_plain_answer(int cfd, const char *answer) {
  size_t enc_len = 0;
  char *enc = bo_encrypt_body(answer, strlen(answer), &enc_len);
  char head[160];
  int hn = snprintf(head, sizeof head, "HTTP/1.1 200 OK\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n", enc_len);
  bo_send_all(cfd, head, (size_t)hn);
  if (enc) bo_send_all(cfd, enc, enc_len);
  free(enc);
}

// The Ultimate Showdown team save never leaves the phone (patch_impl.inc team_save).
static void answer_team_save(int cfd, const char *body, size_t len) {
  size_t plain_len, enc_len = 0;
  char *plain = len ? bo_decrypt_body(body, len, &plain_len) : NULL;
  char *answer = team_save(plain ? plain : "{}");
  if (g_team_json) {
    char path[1100];
    team_file(path, sizeof path);
    FILE *f = fopen(path, "w");
    if (f) { fwrite(g_team_json, 1, strlen(g_team_json), f); fclose(f); }
  }
  char *enc = bo_encrypt_body(answer, strlen(answer), &enc_len);
  char head[160];
  int hn = snprintf(head, sizeof head, "HTTP/1.1 200 OK\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n", enc_len);
  bo_send_all(cfd, head, (size_t)hn);
  if (enc) bo_send_all(cfd, enc, enc_len);
  bo_log("proxy: Ultimate Showdown team saved on this phone");
  free(plain); free(answer); free(enc);
}

// Copies the header lines of an HTTP message, skipping hop-by-hop and length headers.
static void copy_headers(buf_t *out, const char *msg, size_t head_len, bool request) {
  const char *h = strstr(msg, "\r\n") + 2, *end = msg + head_len - 2;
  while (h < end) {
    const char *eol = strstr(h, "\r\n");
    if (!eol) break;
    bool skip = !strncmp(h, "Content-Length:", 15) || !strncmp(h, "Transfer-Encoding:", 18) || !strncmp(h, "Connection:", 11);
    if (request) skip = skip || !strncmp(h, "Host:", 5) || !strncmp(h, "Accept-Encoding:", 16);
    if (!skip) buf_add(out, h, (size_t)(eol - h) + 2);
    h = eol + 2;
  }
}

static void *handle_conn(void *arg) {
  int cfd = (int)(intptr_t)arg;
  buf_t req = {0}, req_body = {0}, resp = {0}, resp_body = {0};
  size_t req_head = 0, resp_head = 0, enc_len = 0;
  int ufd = -1;
  char *req_enc = NULL, *resp_enc = NULL;
  char line[1200];
  if (!bo_read_message(cfd, &req, &req_head, &req_body, false)) goto done;

  // Request line: METHOD SP PATH SP VERSION
  char method[16] = {0}, path[1024] = {0};
  const char *sp1 = strchr(req.data, ' ');
  const char *sp2 = sp1 ? strchr(sp1 + 1, ' ') : NULL;
  if (!sp1 || !sp2 || sp1 - req.data >= (long)sizeof method || sp2 - sp1 - 1 >= (long)sizeof path) goto done;
  memcpy(method, req.data, (size_t)(sp1 - req.data));
  memcpy(path, sp1 + 1, (size_t)(sp2 - sp1 - 1));

  if (strstr(path, "api/chara/teamrtpvp2.json")) {
    answer_team_save(cfd, req_body.data ? req_body.data : "", req_body.len);
    goto done;
  }

  // Multiplayer calls go to the host when online; everything else to this phone's engine.
  bool to_host = BO_ONLINE && is_multiplayer(path);
  struct sockaddr_in dst = { sizeof(struct sockaddr_in), AF_INET, bo_htons((uint16_t)g_engine_port), { 0x0100007fu }, {0} };
  char host_hdr[96];
  snprintf(host_hdr, sizeof host_hdr, "127.0.0.1:%d", g_engine_port);
  if (to_host) {
    dst.sin_port = bo_htons(BO_HOST_HTTP_PORT);
    if (inet_pton(AF_INET, g_cfg.host, &dst.sin_addr) != 1) { bo_log("proxy: bad host address %s", g_cfg.host); goto done; }
    snprintf(host_hdr, sizeof host_hdr, "%s:%d", g_cfg.host, BO_HOST_HTTP_PORT);
  }
  ufd = socket(AF_INET, SOCK_STREAM, 0);
  int one = 1;
  setsockopt(ufd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
  if (connect(ufd, (struct sockaddr *)&dst, sizeof dst) != 0) {
    bo_log("proxy: %s not reachable for %s (error %d: %s)", to_host ? "host" : "engine", path, *__error(), bo_errname(*__error()));
    if (to_host && background_read(path)) { char *ok = local_ok(); send_plain_answer(cfd, ok); free(ok); }
    goto done;
  }

  const char *body = req_body.data ? req_body.data : "";
  size_t body_len = req_body.len;
  if ((req_enc = recode(body, body_len, path, true, to_host, &enc_len))) { body = req_enc; body_len = enc_len; }

  buf_t fwd = {0};
  snprintf(line, sizeof line, "%s %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n", method, path, host_hdr);
  buf_str(&fwd, line);
  copy_headers(&fwd, req.data, req_head, true);
  snprintf(line, sizeof line, "Content-Length: %zu\r\n\r\n", body_len);
  buf_str(&fwd, line);
  if (body_len) buf_add(&fwd, body, body_len);
  bool sent = bo_send_all(ufd, fwd.data, fwd.len);
  free(fwd.data);
  if (!sent || !bo_read_message(ufd, &resp, &resp_head, &resp_body, true)) {
    bo_log("proxy: %s answer failed for %s", to_host ? "host" : "engine", path);
    if (to_host && background_read(path)) { char *ok = local_ok(); send_plain_answer(cfd, ok); free(ok); }
    goto done;
  }
  if (to_host) bo_log("proxy: %s -> host (%.3s)", path, resp.data + 9);

  body = resp_body.data ? resp_body.data : "";
  body_len = resp_body.len;
  if ((resp_enc = recode(body, body_len, path, false, to_host, &enc_len))) { body = resp_enc; body_len = enc_len; }

  // Answer with the upstream status line and headers, a fresh Content-Length and no chunking.
  buf_t out = {0};
  const char *eol = strstr(resp.data, "\r\n");
  buf_add(&out, resp.data, (size_t)(eol - resp.data) + 2);
  copy_headers(&out, resp.data, resp_head, false);
  snprintf(line, sizeof line, "Content-Length: %zu\r\nConnection: close\r\n\r\n", body_len);
  buf_str(&out, line);
  buf_add(&out, body, body_len);
  bo_send_all(cfd, out.data, out.len);
  free(out.data);

done:
  if (ufd >= 0) close(ufd);
  close(cfd);
  free(req_enc); free(resp_enc);
  free(req.data); free(req_body.data); free(resp.data); free(resp_body.data);
  return NULL;
}

static int g_listen_fd = -1;

static void *accept_loop(void *arg) {
  (void)arg;
  for (;;) {
    int c = accept(g_listen_fd, NULL, NULL);
    if (c < 0) { usleep(100000); continue; }
    int one = 1;
    setsockopt(c, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
    pthread_t t;
    if (pthread_create(&t, NULL, handle_conn, (void *)(intptr_t)c) == 0) pthread_detach(t);
    else close(c);
  }
  return NULL;
}

int bo_proxy_start(void) {
  team_load();
  ids_load();
  g_on_ids_learned = ids_save;
  g_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (g_listen_fd < 0) return 0;
  int one = 1;
  setsockopt(g_listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  struct sockaddr_in a = { sizeof(struct sockaddr_in), AF_INET, bo_htons(BO_PROXY_PORT), { 0x0100007fu }, {0} };
  if (bind(g_listen_fd, (struct sockaddr *)&a, sizeof a) != 0 || listen(g_listen_fd, 16) != 0) {
    bo_log("proxy: cannot listen on 127.0.0.1:%d", BO_PROXY_PORT);
    close(g_listen_fd);
    return 0;
  }
  pthread_t t;
  pthread_create(&t, NULL, accept_loop, NULL);
  pthread_detach(t);
  return BO_PROXY_PORT;
}
