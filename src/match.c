// Breaks the league matchmaking lockstep. Ranked matching alternates a host phase (create a room,
// wait) and a guest phase (scan the lobby, join), each lasting Random.Range(min, max) seconds from
// playerparam -- but the shipped values are min = max = 5, so two phones that start together create
// rooms together, leave together and scan empty lobbies together, and both end up against bots
// (0.2.4 test: B saw A's room and joined just as A left it).
//
// The client's code is signed and cannot be patched, so this writes the phase lengths into the live
// MultiplayManager instead, a fresh random length each time a phase starts. Offsets are from the
// 2.28.0 il2cpp dump (docs/photon-client-trace.md, "League matching phases"):
//   MultiplayManager.UpdateLeagueMatchingPhaseInterval @0xF64714 -- called once per matching session
//     (PageLeagueMatching.Initialize); stores phaseStart (+0xA4), guest seconds (+0x90), host (+0x94).
//   ChangeLeagueMatchingPhase @0xF65B38 -- phase (+0xA0, 1 = host) and phaseStart (+0xA4) = TickCount.
//   get_IsLeagueMatchingPhaseTime @0xF65ABC -- (TickCount - phaseStart) / 1000 >= seconds.
// The instance is SingleObject<MultiplayManager>'s static (get_instance @0x1632198: class + 0xB8 =
// static fields, [0] = instance), found through the class pointer il2cpp stores in its metadata slot.
// Every pointer is read safely and the instance's class name is checked (game.c), so a mismatched
// binary only means the fix stays off.
#include "bo.h"

#define RVA_SINGLE_TYPEINFO 0x2DB0F28 // SingleObject<MultiplayManager>_TypeInfo
#define F_GUEST_SECONDS 0x90
#define F_HOST_SECONDS 0x94
#define F_PHASE 0xA0
#define F_PHASE_START 0xA4
#define MIN_SECONDS 3
#define MAX_SECONDS 8

static uintptr_t manager(void) { return bo_singleton(RVA_SINGLE_TYPEINFO, "MultiplayManager"); }

static int32_t pick(void) {
  uint32_t r;
  arc4random_buf(&r, sizeof r);
  return MIN_SECONDS + (int32_t)(r % (MAX_SECONDS - MIN_SECONDS + 1));
}

static void *match_loop(void *arg) {
  (void)arg;
  uintptr_t seen = 0;
  int32_t last_start = 0;
  for (;;) {
    usleep(200000);
    if (!g_cfg.online || !g_cfg.host[0]) continue;
    uintptr_t m = manager();
    if (!m) continue;
    if (m != seen) { bo_log("match: MultiplayManager found at %p", (void *)m); seen = m; last_start = 0; }
    int32_t f[6]; // +0x90 guest, +0x94 host, +0x98, +0x9C, +0xA0 phase, +0xA4 phaseStart
    if (!bo_rd(m + F_GUEST_SECONDS, f, sizeof f)) continue;
    int32_t start = f[(F_PHASE_START - F_GUEST_SECONDS) / 4];
    if (start == last_start) continue;
    last_start = start;
    // A new phase (or a new matching session) began: give this phone's next phases their own lengths.
    int32_t host = pick(), guest = pick();
    *(volatile int32_t *)(m + F_HOST_SECONDS) = host;
    *(volatile int32_t *)(m + F_GUEST_SECONDS) = guest;
    bo_log("match: phase %d started; host phase %ds, guest phase %ds (were %d/%d)",
           f[(F_PHASE - F_GUEST_SECONDS) / 4], host, guest, f[1], f[0]);
  }
  return NULL;
}

void bo_match_install(void) {
  if (!bo_game_base()) { bo_log("match: game image not found; matchmaking fix off"); return; }
  pthread_t t;
  if (pthread_create(&t, NULL, match_loop, NULL) == 0) pthread_detach(t);
}
