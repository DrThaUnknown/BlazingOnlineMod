// Symbol-pointer rebinding (the fishhook technique): an image calls an imported function through a
// pointer in its lazy/non-lazy symbol-pointer sections (__la_symbol_ptr, __nl_symbol_ptr, __got).
// Rewriting that pointer redirects every call the image makes, without touching its code. The game
// binary (IL2CPP, built 2020 with classic dyld info, no chained fixups) is rebound this way so its
// socket calls reach net.c. Our own image is skipped so the hooks can call the originals.
#include "bo.h"

extern const struct mach_header_64 __dso_handle; // our own image

static int g_replaced;

static void rebind_section(const struct section_64 *sect, intptr_t slide, const struct nlist_64 *symtab,
                           const char *strtab, const uint32_t *indirect, const bo_rebinding *list, int count) {
  const uint32_t *indices = indirect + sect->reserved1;
  void **slots = (void **)(slide + sect->addr);
  uint64_t n = sect->size / sizeof(void *);
  for (uint64_t i = 0; i < n; i++) {
    uint32_t symi = indices[i];
    if (symi == INDIRECT_SYMBOL_ABS || symi == INDIRECT_SYMBOL_LOCAL || symi == (INDIRECT_SYMBOL_LOCAL | INDIRECT_SYMBOL_ABS)) continue;
    const char *name = strtab + symtab[symi].n_strx;
    if (name[0] != '_') continue;
    for (int r = 0; r < count; r++) {
      if (strcmp(name + 1, list[r].name) != 0) continue;
      if (slots[i] == list[r].replacement) break;
      if (list[r].original && !*list[r].original) *list[r].original = slots[i];
      // __DATA_CONST is read-only after dyld finishes; make the page writable first.
      uintptr_t page = (uintptr_t)&slots[i] & ~(uintptr_t)0x3fff;
      vm_protect(mach_task_self_, page, 0x4000, 0, PROT_READ | PROT_WRITE | VM_PROT_COPY);
      slots[i] = list[r].replacement;
      g_replaced++;
      break;
    }
  }
}

static void rebind_image(const struct mach_header_64 *header, intptr_t slide, const bo_rebinding *list, int count) {
  const struct segment_command_64 *linkedit = NULL;
  const struct symtab_command *symtab_cmd = NULL;
  const struct dysymtab_command *dysymtab_cmd = NULL;
  const uint8_t *p = (const uint8_t *)(header + 1);
  for (uint32_t i = 0; i < header->ncmds; i++) {
    const struct load_command *lc = (const struct load_command *)p;
    if (lc->cmd == LC_SEGMENT_64 && strcmp(((const struct segment_command_64 *)lc)->segname, "__LINKEDIT") == 0)
      linkedit = (const struct segment_command_64 *)lc;
    else if (lc->cmd == LC_SYMTAB) symtab_cmd = (const struct symtab_command *)lc;
    else if (lc->cmd == LC_DYSYMTAB) dysymtab_cmd = (const struct dysymtab_command *)lc;
    p += lc->cmdsize;
  }
  if (!linkedit || !symtab_cmd || !dysymtab_cmd || !dysymtab_cmd->nindirectsyms) return;

  uintptr_t base = (uintptr_t)slide + linkedit->vmaddr - linkedit->fileoff;
  const struct nlist_64 *symtab = (const struct nlist_64 *)(base + symtab_cmd->symoff);
  const char *strtab = (const char *)(base + symtab_cmd->stroff);
  const uint32_t *indirect = (const uint32_t *)(base + dysymtab_cmd->indirectsymoff);

  p = (const uint8_t *)(header + 1);
  for (uint32_t i = 0; i < header->ncmds; i++) {
    const struct load_command *lc = (const struct load_command *)p;
    if (lc->cmd == LC_SEGMENT_64) {
      const struct segment_command_64 *seg = (const struct segment_command_64 *)lc;
      if (strcmp(seg->segname, "__DATA") == 0 || strcmp(seg->segname, "__DATA_CONST") == 0) {
        const struct section_64 *sect = (const struct section_64 *)(seg + 1);
        for (uint32_t s = 0; s < seg->nsects; s++) {
          uint32_t type = sect[s].flags & SECTION_TYPE;
          if (type == S_LAZY_SYMBOL_POINTERS || type == S_NON_LAZY_SYMBOL_POINTERS)
            rebind_section(&sect[s], slide, symtab, strtab, indirect, list, count);
        }
      }
    }
    p += lc->cmdsize;
  }
}

int bo_rebind_symbols(const bo_rebinding *list, int count) {
  g_replaced = 0;
  uint32_t n = _dyld_image_count();
  for (uint32_t i = 0; i < n; i++) {
    const struct mach_header_64 *h = _dyld_get_image_header(i);
    if (!h || h == &__dso_handle) continue;
    const char *name = _dyld_get_image_name(i);
    // Only the game itself: system libraries call their own internals, and other add-ons (Rebirth's
    // engine, Blazing Forge) must keep talking to the real network stack.
    if (!name || !strstr(name, ".app/") || strstr(name, "/Frameworks/")) continue;
    rebind_image(h, _dyld_get_image_vmaddr_slide(i), list, count);
  }
  return g_replaced;
}
