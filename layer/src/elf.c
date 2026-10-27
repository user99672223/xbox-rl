/*
 * elf.c - ELF64 loader for the guest image (Wine's unix-side loader + musl).
 *
 * We map the image so it can run NATIVELY; nothing is interpreted. Structure
 * and semantics follow Blink's loader (vendor/blink/blink/loader.c,
 * LoadElfLoadSegment/LoadElf/LoadInterpreter/... pinned in vendor.pins): per-
 * PT_LOAD page-skew handling, zeroing the partial page after p_filesz, bss as
 * extra anonymous pages, PT_INTERP handled by loading the interpreter at its
 * own bias and starting there with AT_BASE set, PT_PHDR/AT_PHDR derivation.
 * Blink's own loader cannot be compiled in place: it is welded to Blink's
 * emulated-machine page tables (ReserveVirtual/CopyToUser), so this is a
 * clean-room port of the logic onto our mmap layer. The initial stack/auxv is
 * built the way Linux's binfmt_elf does it.
 *
 * Supported: ET_EXEC and ET_DYN (static-PIE or PIE + PT_INTERP), x86-64 only.
 * Extras for the B<->C seam:
 *   - undefined dynamic symbols named `xrl_syscall` (and a few siblings) in the
 *     main image are resolved at load time (GLOB_DAT / JUMP_SLOT / 64) when the
 *     image has no interpreter, for images that import the thunk by symbol;
 *   - the exported dynsym `xrl_wineserver_main` (if present and not stripped)
 *     is returned so boot can start the wineserver thread hook.
 */
#include "xrl_internal.h"

#define EI_NIDENT 16
#pragma pack(push, 1)
typedef struct { u8 ident[EI_NIDENT]; u16 type, machine; u32 version; u64 entry, phoff, shoff; u32 flags; u16 ehsize, phentsize, phnum, shentsize, shnum, shstrndx; } Ehdr;
typedef struct { u32 type, flags; u64 offset, vaddr, paddr, filesz, memsz, align; } Phdr;
typedef struct { u32 name, type; u64 flags, addr, offset, size; u32 link, info; u64 addralign, entsize; } Shdr;
typedef struct { u32 name; u8 info, other; u16 shndx; u64 value, size; } Sym;
typedef struct { u64 offset, info; s64 addend; } Rela;
typedef struct { s64 tag; u64 val; } Dyn;
#pragma pack(pop)

#define PT_LOAD 1
#define PT_DYNAMIC 2
#define PT_INTERP 3
#define PT_PHDR 6
#define PT_TLS 7
#define PF_X 1
#define PF_W 2
#define PF_R 4
#define ET_EXEC 2
#define ET_DYN 3
#define EM_X86_64 62
#define DT_NULL 0
#define DT_PLTRELSZ 2
#define DT_STRTAB 5
#define DT_SYMTAB 6
#define DT_RELA 7
#define DT_RELASZ 8
#define DT_JMPREL 23
#define SHT_DYNSYM 11
#define R_X86_64_64 1
#define R_X86_64_GLOB_DAT 6
#define R_X86_64_JUMP_SLOT 7

static int fd_pread_all(int fd, void *dst, u64 n, s64 off)
{
    xfile *f = xrl_fd_get(fd);
    if (!f) return -1;
    u64 done = 0;
    while (done < n) {
        slong r = f->ops->read(f, (char *)dst + done, n - done, off + (s64)done);
        if (r <= 0) { xfile_unref(f); return -1; }
        done += (u64)r;
    }
    xfile_unref(f);
    return 0;
}

static int prot_of(u32 fl) { return (fl & PF_R ? LX_PROT_READ : 0) | (fl & PF_W ? LX_PROT_WRITE : 0) | (fl & PF_X ? LX_PROT_EXEC : 0); }

typedef struct loaded {
    u64 bias, min_vaddr, max_vaddr, entry, phdr_addr, phnum, phent;
    int type;
    char interp[256];
    Phdr *ph;           /* malloc'd copy */
    Ehdr eh;
    int fd;
} loaded;

static int map_segment(int fd, const Phdr *p, u64 bias)
{
    u64 va = p->vaddr + bias;
    u64 start = ALIGN_DOWN(va, LX_PAGE);
    u64 skew = va - start;
    u64 off = p->offset - skew;
    u64 file_end = va + p->filesz;
    u64 mem_end = ALIGN_UP(va + p->memsz, LX_PAGE);
    int prot = prot_of(p->flags);
    if ((p->offset & (LX_PAGE - 1)) != skew) { LOGE("elf: PT_LOAD vaddr/offset skew mismatch"); return -1; }
    if (p->filesz) {
        u64 flen = ALIGN_UP(file_end, LX_PAGE) - start;
        slong r = xrl_mem_mmap(start, flen, prot | LX_PROT_WRITE, LX_MAP_PRIVATE | LX_MAP_FIXED, fd, (s64)off);
        if (r < 0 || (u64)r != start) { LOGE("elf: mmap segment %llx+%llx failed (%lld)", start, flen, r); return -1; }
        /* bytes between p_filesz and the page end must be zero, not file data */
        u64 tail = ALIGN_UP(file_end, LX_PAGE) - file_end;
        if (tail && xrl_zerou(file_end, (size_t)tail)) return -1;
        if (p->memsz > p->filesz && (file_end & (LX_PAGE - 1))) {}   /* covered by zeroing */
        if (prot != (prot | LX_PROT_WRITE) && xrl_mem_mprotect(start, flen, prot)) return -1;
        if (prot & LX_PROT_EXEC) xrl_rewrite_fd_mapped(fd, start, flen, (s64)off, prot);   /* foreign images: rewrite.c */
        start += flen;
    }
    if (start < mem_end) {
        slong r = xrl_mem_mmap(start, mem_end - start, prot, LX_MAP_PRIVATE | LX_MAP_ANONYMOUS | LX_MAP_FIXED, -1, 0);
        if (r < 0) { LOGE("elf: bss mmap failed (%lld)", r); return -1; }
    }
    return 0;
}

static int load_one(const char *gpath, int is_interp, loaded *L)
{
    memset(L, 0, sizeof *L);
    L->fd = (int)xrl_vfs_open(LX_AT_FDCWD, gpath, LX_O_RDONLY | LX_O_CLOEXEC, 0);
    if (L->fd < 0) { LOGE("elf: cannot open %s (%d)", gpath, L->fd); return -1; }
    if (fd_pread_all(L->fd, &L->eh, sizeof L->eh, 0)) { LOGE("elf: %s too short", gpath); return -1; }
    Ehdr *e = &L->eh;
    if (memcmp(e->ident, "\177ELF", 4) || e->ident[4] != 2 || e->ident[5] != 1 || e->machine != EM_X86_64 ||
        (e->type != ET_EXEC && e->type != ET_DYN) || e->phentsize != sizeof(Phdr) || e->phnum == 0 || e->phnum > 128) {
        LOGE("elf: %s is not a supported x86-64 ELF64 executable/shared object", gpath);
        return -1;
    }
    L->type = e->type;
    L->ph = (Phdr *)malloc(e->phnum * sizeof(Phdr));
    if (fd_pread_all(L->fd, L->ph, e->phnum * sizeof(Phdr), (s64)e->phoff)) { LOGE("elf: cannot read program headers"); return -1; }
    L->min_vaddr = ~0ull; L->max_vaddr = 0;
    for (int i = 0; i < e->phnum; i++) {
        Phdr *p = &L->ph[i];
        if (p->type == PT_LOAD) {
            if (p->vaddr < L->min_vaddr) L->min_vaddr = p->vaddr;
            if (p->vaddr + p->memsz > L->max_vaddr) L->max_vaddr = p->vaddr + p->memsz;
        } else if (p->type == PT_INTERP && !is_interp) {
            if (p->filesz >= sizeof L->interp || fd_pread_all(L->fd, L->interp, p->filesz, (s64)p->offset)) return -1;
            L->interp[p->filesz] = 0;
        }
    }
    if (L->min_vaddr == ~0ull) { LOGE("elf: no PT_LOAD"); return -1; }
    u64 lo = ALIGN_DOWN(L->min_vaddr, LX_PAGE), hi = ALIGN_UP(L->max_vaddr, LX_PAGE);
    if (e->type == ET_DYN) {
        slong base = xrl_mem_mmap(0, hi - lo, LX_PROT_NONE, LX_MAP_PRIVATE | LX_MAP_ANONYMOUS, -1, 0);
        if (base < 0) { LOGE("elf: cannot reserve %llx bytes for %s", hi - lo, gpath); return -1; }
        L->bias = (u64)base - lo;
    } else L->bias = 0;
    for (int i = 0; i < e->phnum; i++) if (L->ph[i].type == PT_LOAD && map_segment(L->fd, &L->ph[i], L->bias)) return -1;
    L->entry = e->entry + L->bias;
    L->phent = e->phentsize; L->phnum = e->phnum;
    L->phdr_addr = 0;
    for (int i = 0; i < e->phnum; i++) if (L->ph[i].type == PT_PHDR) L->phdr_addr = L->ph[i].vaddr + L->bias;
    if (!L->phdr_addr) L->phdr_addr = L->min_vaddr + L->bias + e->phoff;
    LOGI("elf: loaded %s%s type=%s bias=%llx span=[%llx,%llx) entry=%llx", gpath, is_interp ? " (interpreter)" : "",
         e->type == ET_DYN ? "DYN" : "EXEC", L->bias, lo + L->bias, hi + L->bias, L->entry);
    return 0;
}

/* Exports we can satisfy by symbol name. */
static u64 export_lookup(const char *name)
{
    if (!strcmp(name, "xrl_syscall")) return (u64)(uintptr_t)&xrl_syscall;
    if (!strcmp(name, "xrl_guest_ctx_ptr")) return (u64)(uintptr_t)G.guest_ctx;
    return 0;
}

/* Resolve undefined dynsyms (xrl_*) in an interpreter-less image. */
static void resolve_imports(const loaded *L)
{
    for (int i = 0; i < L->eh.phnum; i++) {
        if (L->ph[i].type != PT_DYNAMIC) continue;
        u64 dyn_va = L->ph[i].vaddr + L->bias;
        u64 symtab = 0, strtab = 0, rela = 0, relasz = 0, jmprel = 0, pltrelsz = 0;
        for (Dyn *d = (Dyn *)dyn_va; d->tag != DT_NULL; d++) {
            u64 v = d->val;
            switch (d->tag) {
            case DT_SYMTAB: symtab = v; break;
            case DT_STRTAB: strtab = v; break;
            case DT_RELA: rela = v; break;
            case DT_RELASZ: relasz = v; break;
            case DT_JMPREL: jmprel = v; break;
            case DT_PLTRELSZ: pltrelsz = v; break;
            }
        }
        if (!symtab || !strtab) return;
        if (symtab < L->bias) { /* some linkers store absolute vaddrs: add bias */ }
        if (L->type == ET_DYN) { symtab += L->bias; strtab += L->bias; rela += rela ? L->bias : 0; jmprel += jmprel ? L->bias : 0; }
        struct { u64 a, n; } tabs[2] = { { rela, relasz }, { jmprel, pltrelsz } };
        int resolved = 0;
        for (int t = 0; t < 2; t++) {
            if (!tabs[t].a) continue;
            for (Rela *r = (Rela *)tabs[t].a; (u64)r < tabs[t].a + tabs[t].n; r++) {
                u32 type = (u32)r->info, sym = (u32)(r->info >> 32);
                if (!sym || (type != R_X86_64_64 && type != R_X86_64_GLOB_DAT && type != R_X86_64_JUMP_SLOT)) continue;
                Sym *s = (Sym *)(symtab + sym * sizeof(Sym));
                if (s->shndx != 0) continue;     /* defined locally */
                const char *nm = (const char *)(strtab + s->name);
                u64 v = export_lookup(nm);
                if (!v) continue;
                *(u64 *)(r->offset + L->bias) = v + (type == R_X86_64_64 ? (u64)r->addend : 0);
                resolved++;
            }
        }
        if (resolved) LOGI("elf: resolved %d xrl_* import relocation(s) in main image", resolved);
    }
}

/* Find a *defined* dynsym in a mapped image through its PT_DYNAMIC segment
 * (DT_SYMTAB/DT_STRTAB, symbol count from DT_HASH or DT_GNU_HASH). Unlike
 * find_symbol() this works on stripped images, which is what we ship. */
#define DT_HASH 4
#define DT_GNU_HASH 0x6ffffef5
static u64 dyn_find_symbol(const loaded *L, const char *want)
{
    for (int i = 0; i < L->eh.phnum; i++) {
        if (L->ph[i].type != PT_DYNAMIC) continue;
        u64 dyn_va = L->ph[i].vaddr + L->bias;
        u64 symtab = 0, strtab = 0, hash = 0, gnuhash = 0;
        for (Dyn *d = (Dyn *)dyn_va; d->tag != DT_NULL; d++) {
            switch (d->tag) {
            case DT_SYMTAB: symtab = d->val; break;
            case DT_STRTAB: strtab = d->val; break;
            case DT_HASH: hash = d->val; break;
            case DT_GNU_HASH: gnuhash = d->val; break;
            }
        }
        if (!symtab || !strtab) return 0;
        if (L->type == ET_DYN) {
            symtab += L->bias; strtab += L->bias;
            if (hash) hash += L->bias;
            if (gnuhash) gnuhash += L->bias;
        }
        u64 nsyms = 0;
        if (hash) {
            nsyms = ((const u32 *)hash)[1];                 /* nchain */
        } else if (gnuhash) {
            const u32 *h = (const u32 *)gnuhash;
            u32 nbuckets = h[0], symoff = h[1], bloom = h[2];
            const u32 *buckets = (const u32 *)(gnuhash + 16 + (u64)bloom * 8);
            const u32 *chain = buckets + nbuckets;
            u32 last = 0;
            for (u32 b = 0; b < nbuckets; b++) if (buckets[b] > last) last = buckets[b];
            if (last >= symoff) { while (!(chain[last - symoff] & 1)) last++; nsyms = (u64)last + 1; }
            else nsyms = symoff;
        }
        if (!nsyms || nsyms > (1u << 20)) return 0;
        for (u64 k = 1; k < nsyms; k++) {
            const Sym *s = (const Sym *)(symtab + k * sizeof(Sym));
            if (!s->shndx) continue;
            if (!strcmp((const char *)(strtab + s->name), want))
                return L->type == ET_DYN ? s->value + L->bias : s->value;
        }
    }
    return 0;
}

/* B's patched musl reads the gateway from an 8-byte data symbol `__xrl_gateway`
 * that the loader fills before any guest instruction runs (musl's own start-up
 * issues syscalls). Fill it in every image that defines it. */
static int install_gateway(const loaded *L, const char *what)
{
    u64 slot = dyn_find_symbol(L, "__xrl_gateway");
    if (!slot) return 0;
    *(u64 *)slot = (u64)(uintptr_t)&xrl_syscall;
    LOGI("elf: __xrl_gateway in %s at %llx -> xrl_syscall", what, slot);
    return 1;
}

/* find an exported dynsym by name via section headers (works if not stripped) */
static u64 find_symbol(const loaded *L, const char *want)
{
    Ehdr *e = (Ehdr *)&L->eh;
    if (!e->shoff || e->shentsize != sizeof(Shdr) || e->shnum == 0 || e->shnum > 256) return 0;
    Shdr *sh = (Shdr *)malloc(e->shnum * sizeof(Shdr));
    u64 res = 0;
    if (!fd_pread_all(L->fd, sh, e->shnum * sizeof(Shdr), (s64)e->shoff)) {
        for (int i = 0; i < e->shnum && !res; i++) {
            if (sh[i].type != SHT_DYNSYM || sh[i].link >= e->shnum || sh[i].size > (4u << 20)) continue;
            Shdr *st = &sh[sh[i].link];
            if (st->size > (4u << 20)) continue;
            Sym *syms = (Sym *)malloc((size_t)sh[i].size);
            char *strs = (char *)malloc((size_t)st->size + 1);
            if (!fd_pread_all(L->fd, syms, sh[i].size, (s64)sh[i].offset) && !fd_pread_all(L->fd, strs, st->size, (s64)st->offset)) {
                strs[st->size] = 0;
                for (size_t k = 0; k < sh[i].size / sizeof(Sym); k++)
                    if (syms[k].shndx && syms[k].name < st->size && !strcmp(strs + syms[k].name, want)) { res = syms[k].value + L->bias; break; }
            }
            free(syms); free(strs);
        }
    }
    free(sh);
    return res;
}

int xrl_elf_load(const char *gpath, xrl_elf_image *out)
{
    loaded main, interp;
    memset(out, 0, sizeof *out);
    if (load_one(gpath, 0, &main)) return -1;
    out->is_dyn = main.type == ET_DYN;
    out->load_bias = main.bias;
    out->prog_entry = main.entry;
    out->phdr = main.phdr_addr; out->phent = main.phent; out->phnum = main.phnum;
    out->entry = main.entry;
    u64 end = ALIGN_UP(main.max_vaddr + main.bias, LX_PAGE);
    out->brk_start = end;
    if (main.interp[0]) {
        strcpy_s(out->interp, sizeof out->interp, main.interp);
        if (load_one(main.interp, 1, &interp)) return -1;
        out->base = interp.bias;
        out->entry = interp.entry;
        out->interp_lo = ALIGN_DOWN(interp.min_vaddr + interp.bias, LX_PAGE);
        out->interp_hi = ALIGN_UP(interp.max_vaddr + interp.bias, LX_PAGE);
        install_gateway(&interp, "interpreter");
        free(interp.ph);
        xrl_fd_close(interp.fd);
    } else {
        resolve_imports(&main);
    }
    install_gateway(&main, "main image");
    out->exports_xrl_wineserver_main = find_symbol(&main, "xrl_wineserver_main");
    free(main.ph);
    xrl_fd_close(main.fd);
    return 0;
}

/* ---------------------------------------------------------------- stack */
#define AT_NULL 0
#define AT_PHDR 3
#define AT_PHENT 4
#define AT_PHNUM 5
#define AT_PAGESZ 6
#define AT_BASE 7
#define AT_FLAGS 8
#define AT_ENTRY 9
#define AT_UID 11
#define AT_EUID 12
#define AT_GID 13
#define AT_EGID 14
#define AT_PLATFORM 15
#define AT_HWCAP 16
#define AT_CLKTCK 17
#define AT_SECURE 23
#define AT_RANDOM 25
#define AT_HWCAP2 26
#define AT_EXECFN 31
#define AT_MINSIGSTKSZ 51

u64 xrl_elf_build_stack(const xrl_elf_image *img, u64 *out_lo, u64 *out_hi)
{
    const u64 size = 8ull << 20;
    u64 base = xrl_mem_stack_alloc(size);
    if (!base) return 0;
    u64 top = base + size;
    *out_lo = base; *out_hi = top;

    /* strings at the top */
    u64 sp = top - 16;
    u8 rnd[16]; xrl_rng_fill(rnd, 16);
    sp -= 16; u64 p_random = sp; memcpy((void *)sp, rnd, 16);
    const char *plat = "x86_64";
    sp -= 8; u64 p_plat = sp; memcpy((void *)sp, plat, strlen(plat) + 1);
    size_t el = strlen(G.exec_path) + 1;
    sp -= (el + 7) & ~7ull; u64 p_exec = sp; memcpy((void *)sp, G.exec_path, el);
    u64 *envp_s = (u64 *)calloc(G.envc + 1, 8), *argv_s = (u64 *)calloc(G.argc + 1, 8);
    for (int i = G.envc - 1; i >= 0; i--) { size_t l = strlen(G.envp[i]) + 1; sp -= l; memcpy((void *)sp, G.envp[i], l); envp_s[i] = sp; }
    for (int i = G.argc - 1; i >= 0; i--) { size_t l = strlen(G.argv[i]) + 1; sp -= l; memcpy((void *)sp, G.argv[i], l); argv_s[i] = sp; }
    sp &= ~15ull;

    u64 aux[40][2];
    int na = 0;
#define AUX(t, v) do { aux[na][0] = (t); aux[na][1] = (u64)(v); na++; } while (0)
    AUX(AT_PHDR, img->phdr); AUX(AT_PHENT, img->phent); AUX(AT_PHNUM, img->phnum);
    AUX(AT_PAGESZ, 4096); AUX(AT_BASE, img->base); AUX(AT_FLAGS, 0); AUX(AT_ENTRY, img->prog_entry);
    AUX(AT_UID, 1000); AUX(AT_EUID, 1000); AUX(AT_GID, 1000); AUX(AT_EGID, 1000);
    AUX(AT_PLATFORM, p_plat);
    int cpuid1[4]; __cpuid(cpuid1, 1);
    AUX(AT_HWCAP, (u32)cpuid1[3]);
    AUX(AT_HWCAP2, 0);   /* deliberately NOT advertising HWCAP2_FSGSBASE: Wine would try wrgsbase on its TEB */
    AUX(AT_CLKTCK, 100); AUX(AT_SECURE, 0); AUX(AT_RANDOM, p_random); AUX(AT_EXECFN, p_exec);
    AUX(AT_MINSIGSTKSZ, 2048);
    AUX(AT_XRL_SYSCALL, (u64)(uintptr_t)&xrl_syscall);
    AUX(AT_XRL_CTX, (u64)(uintptr_t)G.guest_ctx);
    AUX(AT_XRL_ABI, XRL_ABI_VERSION);
    AUX(AT_NULL, 0);
#undef AUX
    size_t nwords = 1 + (size_t)G.argc + 1 + (size_t)G.envc + 1 + (size_t)na * 2;
    sp -= nwords * 8;
    sp &= ~15ull;
    u64 *w = (u64 *)sp;
    size_t k = 0;
    w[k++] = (u64)G.argc;
    for (int i = 0; i < G.argc; i++) w[k++] = argv_s[i];
    w[k++] = 0;
    for (int i = 0; i < G.envc; i++) w[k++] = envp_s[i];
    w[k++] = 0;
    for (int i = 0; i < na; i++) { w[k++] = aux[i][0]; w[k++] = aux[i][1]; }
    free(envp_s); free(argv_s);
    return sp;
}
