// Reading and calling into the game (il2cpp, BNEI0249 2.28.0). The game's code is signed and can't
// be patched, but its data can be read and written and its functions called. Every pointer is read
// with mach_vm_read_overwrite, so a different binary only turns a feature off, never crashes.
// Offsets: docs/photon-client-trace.md (League matching phases), docs/blazing-online-menu.md.
#include "bo.h"

#define CLASS_NAME 0x10           // Il2CppClass: image, gc_desc, name, namespaze
#define CLASS_STATIC_FIELDS 0xB8  // as SingleObject<T>.get_instance @0x1632198 reads it

#define RVA_SINGLE_SCENEMANAGER 0x2DB01F8 // SingleObject<SceneManager>_TypeInfo (UIManager.get_sm @0x1509B90)
#define RVA_LOADSCENE 0x13BFD8C           // SceneManager.LoadScene(SceneList, TransitMode, bool, bool, Action)
#define SCENE_BOOT 17                     // SceneList.SceneBoot
#define TRANSIT_TITLE 100                 // as the Other menu's Title confirm passes it (@0x151F1A4)

static uintptr_t g_base;

uintptr_t bo_game_base(void) {
  if (g_base) return g_base;
  for (uint32_t i = 0, n = _dyld_image_count(); i < n; i++) {
    const char *name = _dyld_get_image_name(i);
    if (name && strstr(name, "/BNEI0249")) return g_base = (uintptr_t)_dyld_get_image_header(i);
  }
  return 0;
}

bool bo_rd(uintptr_t addr, void *out, size_t n) {
  uint64_t got = 0;
  return addr && mach_vm_read_overwrite(mach_task_self_, addr, n, (uint64_t)(uintptr_t)out, &got) == 0 && got == n;
}

// A heap/image pointer, not null or an il2cpp metadata token not yet resolved (those are odd).
static bool ptr_ok(uintptr_t v) { return v >= 0x100000000ull && v < 0x1000000000000ull && !(v & 7); }

uintptr_t bo_rdptr(uintptr_t addr) {
  uintptr_t v = 0;
  return bo_rd(addr, &v, sizeof v) && ptr_ok(v) ? v : 0;
}

// The instance behind SingleObject<T>, given the RVA of SingleObject<T>_TypeInfo, if its class is
// named `class_name`.
uintptr_t bo_singleton(uintptr_t rva_single_typeinfo, const char *class_name) {
  uintptr_t base = bo_game_base();
  uintptr_t cls = base ? bo_rdptr(base + rva_single_typeinfo) : 0;
  uintptr_t statics = cls ? bo_rdptr(cls + CLASS_STATIC_FIELDS) : 0;
  uintptr_t inst = statics ? bo_rdptr(statics) : 0;
  uintptr_t klass = inst ? bo_rdptr(inst) : 0;
  uintptr_t name = klass ? bo_rdptr(klass + CLASS_NAME) : 0;
  char buf[64] = {0};
  size_t want = strlen(class_name) + 1;
  if (!name || want > sizeof buf || !bo_rd(name, buf, want)) return 0;
  return memcmp(buf, class_name, want) == 0 ? inst : 0;
}

// What the Other menu's Title button does after its confirm (UIManager.<CheckTitleMoveDialog>
// b__128_0): SceneManager.LoadScene(SceneBoot, 100). The game logs in again from the title screen,
// which fetches startup.json with the new league windows. Main thread only.
bool bo_game_to_title(void) {
  uintptr_t sm = bo_singleton(RVA_SINGLE_SCENEMANAGER, "SceneManager");
  if (!sm) { bo_log("game: SceneManager not found; can't go to the title screen"); return false; }
  void (*load)(uintptr_t, int32_t, int32_t, bool, bool, void *, void *) = (void *)(bo_game_base() + RVA_LOADSCENE);
  bo_log("game: back to the title screen");
  load(sm, SCENE_BOOT, TRANSIT_TITLE, false, false, NULL, NULL);
  return true;
}
