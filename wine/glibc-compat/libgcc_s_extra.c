/*
 * libgcc_s_extra.c - what GCC's unwinder asks of the dynamic linker, linked into the guest's libgcc_s.so.1
 * (wine/scripts/build-libgcc-s.sh).
 *
 * GCC 13+ looks up the object that holds a pc with glibc's _dl_find_object() and only falls back to
 * dl_iterate_phdr() when it fails. The patched musl (patches/musl/0001) defines a _dl_find_object that always
 * reports "not found", expecting the fallback; with the fallback, _Unwind_Backtrace aborts (uw_init_context_1
 * cannot find the FDE of its own frame; measured on the layer, with and without Wine). So libgcc_s carries its
 * own _dl_find_object, answered from dl_iterate_phdr, and (-Bsymbolic) its own references bind to it, ahead of
 * musl's stub. Only the five fields glibc documents first are filled (flags, map start/end, link map,
 * eh_frame = the PT_GNU_EH_FRAME segment).
 */
#define _GNU_SOURCE
#include <link.h>
#include <stddef.h>
#include <stdint.h>

struct xrl_dlfo { unsigned long long flags; void *map_start, *map_end; void *link_map; void *eh_frame; };
struct dlfo_q { uintptr_t pc; struct xrl_dlfo *r; int found; };

static int dlfo_cb(struct dl_phdr_info *info, size_t sz, void *data)
{
    struct dlfo_q *q = (struct dlfo_q *)data;
    uintptr_t lo = ~(uintptr_t)0, hi = 0;
    const ElfW(Phdr) *eh = NULL;
    int in = 0;
    (void)sz;
    for (int i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr) *p = &info->dlpi_phdr[i];
        if (p->p_type == PT_LOAD) {
            uintptr_t a = info->dlpi_addr + p->p_vaddr, e = a + p->p_memsz;
            if (a < lo) lo = a;
            if (e > hi) hi = e;
            if (q->pc >= a && q->pc < e) in = 1;
        } else if (p->p_type == PT_GNU_EH_FRAME) eh = p;
    }
    if (!in) return 0;
    q->r->flags = 0;
    q->r->map_start = (void *)lo;
    q->r->map_end = (void *)hi;
    q->r->link_map = NULL;
    q->r->eh_frame = eh ? (void *)(info->dlpi_addr + eh->p_vaddr) : NULL;
    q->found = 1;
    return 1;
}

__attribute__((visibility("default"))) int _dl_find_object(void *pc, void *result)
{
    struct dlfo_q q = { (uintptr_t)pc, (struct xrl_dlfo *)result, 0 };
    dl_iterate_phdr(dlfo_cb, &q);
    return q.found ? 0 : -1;
}
