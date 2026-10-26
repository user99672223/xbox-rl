/* A Windows program that issues raw LINUX system calls, the way Epic's EAC bootstrapper does on Wine.
 *
 * On Linux (Proton) a `syscall` instruction in a PE image is a Linux system call: the bootstrapper uses it
 * for memfd_create/write (to hold the anti-cheat module) and calls libc's dlopen through a small
 * Windows-ABI -> SysV-ABI trampoline. The Windows kernel would execute that `syscall` as a *Windows*
 * system call with a Linux number, so the layer must see it (layer/src/dbt.c).
 *
 * What it does, exactly as the bootstrapper does (RocketLeague_EAC.exe 0x14004a7a0 / 0x14004b9f0):
 *   - the 21-byte `syscall3` stub sits in the image (case "image") and is also copied into a page that
 *     VirtualAlloc returns at 0x7001_0000_0000 + n * 4 GiB, PAGE_EXECUTE_READWRITE (case "heap");
 *   - calls go through a function pointer, rcx = number, rdx/r8/r9 = first three arguments.
 * The name of this program contains "_eac" so that the default XRL_DBT policy applies to it.
 *
 * Every check is something only a Linux kernel answers: uname says "Linux", memfd_create + write + read
 * round-trips, /proc/self/fd/N opens the memfd, mmap of it works, a second thread gets its own tid.
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

typedef long long (*sys3_fn)(long long nr, long long a, long long b, long long c);

extern void rawsys_stub(void), rawsys_stub_end(void);
__asm__(
    ".text\n.globl rawsys_stub\nrawsys_stub:\n"
    "  push %rdi\n  push %rsi\n  mov %rcx,%rax\n  mov %rdx,%rdi\n  push %rdx\n  mov %r8,%rsi\n  mov %r9,%rdx\n"
    "  syscall\n  pop %rdx\n  pop %rsi\n  pop %rdi\n  ret\n"
    ".globl rawsys_stub_end\nrawsys_stub_end:\n  int3\n");

#define SYS_read 0
#define SYS_write 1
#define SYS_open 2
#define SYS_close 3
#define SYS_lseek 8
#define SYS_mmap 9
#define SYS_munmap 11
#define SYS_getpid 39
#define SYS_uname 63
#define SYS_gettid 186
#define SYS_memfd_create 319

static int g_fail;
static void check(int ok, const char *what, long long v)
{
    printf("rawsys: %-4s %s (%lld)\n", ok ? "ok" : "FAIL", what, v);
    if (!ok) g_fail++;
}

static sys3_fn g_sys;
static DWORD WINAPI thread_main(void *arg)
{
    *(long long *)arg = g_sys(SYS_gettid, 0, 0, 0);
    return 0;
}

static void run_case(const char *tag, sys3_fn sys)
{
    char msg[96];
    g_sys = sys;
    printf("rawsys: --- case %s (stub at %p)\n", tag, (void *)sys);

    struct { char sysname[65], nodename[65], release[65], version[65], machine[65], domain[65]; } un;
    memset(&un, 0, sizeof un);
    long long r = sys(SYS_uname, (long long)(uintptr_t)&un, 0, 0);
    snprintf(msg, sizeof msg, "uname: sysname \"%.8s\" machine \"%.8s\"", un.sysname, un.machine);
    check(r == 0 && !strcmp(un.sysname, "Linux") && !strcmp(un.machine, "x86_64"), msg, r);

    long long pid = sys(SYS_getpid, 0, 0, 0), tid = sys(SYS_gettid, 0, 0, 0);
    check(pid > 0 && tid > 0 && pid == sys(SYS_getpid, 0, 0, 0), "getpid() and gettid() answer, getpid() is stable", pid);

    long long tid2 = -1;
    HANDLE th = CreateThread(NULL, 0, thread_main, &tid2, 0, NULL);
    WaitForSingleObject(th, 10000);
    CloseHandle(th);
    check(tid2 > 0 && tid2 != tid && tid2 != pid, "a second thread's gettid() differs", tid2);

    /* memfd_create + write + lseek + read, as the bootstrapper stores the module */
    static const char payload[] = "Easy Anti-Cheat module bytes would be here";
    long long fd = sys(SYS_memfd_create, (long long)(uintptr_t)"xrl_rawsys", 2 /* MFD_ALLOW_SEALING */, 0);
    check(fd >= 0, "memfd_create(MFD_ALLOW_SEALING)", fd);
    if (fd >= 0) {
        long long w = sys(SYS_write, fd, (long long)(uintptr_t)payload, sizeof payload);
        check(w == (long long)sizeof payload, "write(memfd)", w);
        long long off = sys(SYS_lseek, fd, 0, 0);
        char back[sizeof payload] = {0};
        long long rd = sys(SYS_read, fd, (long long)(uintptr_t)back, sizeof back);
        check(off == 0 && rd == (long long)sizeof payload && !memcmp(back, payload, sizeof payload), "lseek + read(memfd) round trip", rd);

        char path[64];
        snprintf(path, sizeof path, "/proc/self/fd/%d", (int)fd);
        long long fd2 = sys(SYS_open, (long long)(uintptr_t)path, 0 /* O_RDONLY */, 0);
        char back2[sizeof payload] = {0};
        long long rd2 = fd2 >= 0 ? sys(SYS_read, fd2, (long long)(uintptr_t)back2, sizeof back2) : -1;
        check(fd2 >= 0 && rd2 == (long long)sizeof payload && !memcmp(back2, payload, sizeof payload), "open(/proc/self/fd/N) reads the memfd", fd2);
        if (fd2 >= 0) sys(SYS_close, fd2, 0, 0);

        /* mmap(NULL, len, PROT_READ, MAP_PRIVATE, fd, 0): six arguments, so go through the 3-argument stub's
         * big brother: the bootstrapper maps nothing itself (dlopen does), here it only needs the first three
         * to stay right. Check the file offset is where write left it instead. */
        long long end = sys(SYS_lseek, fd, 0, 2 /* SEEK_END */);
        check(end == (long long)sizeof payload, "lseek(SEEK_END) is the payload size", end);
        sys(SYS_close, fd, 0, 0);
    }

    /* a call the Windows kernel cannot know: an unassigned number is ENOSYS (-38) on Linux */
    long long bad = sys(0x3fff, 0, 0, 0);
    check(bad == -38, "an unassigned number returns -ENOSYS", bad);
}

static long long sys_pid(void *stub) { return ((sys3_fn)stub)(SYS_getpid, 0, 0, 0); }

/* /proc/self/maps through raw syscalls (the bootstrapper reads it to find libc): print what mentions libc */
static void show_maps(sys3_fn sys)
{
    static char buf[1 << 20];
    long long fd = sys(SYS_open, (long long)(uintptr_t)"/proc/self/maps", 0, 0);
    check(fd >= 0, "open(/proc/self/maps)", fd);
    if (fd < 0) return;
    size_t n = 0;
    for (;;) {
        long long r = sys(SYS_read, fd, (long long)(uintptr_t)buf + (long long)n, (long long)(sizeof buf - 1 - n));
        if (r <= 0) break;
        n += (size_t)r;
    }
    buf[n] = 0;
    sys(SYS_close, fd, 0, 0);
    int lines = 0;
    for (char *l = strtok(buf, "\n"); l; l = strtok(NULL, "\n")) {
        lines++;
        if (strstr(l, "libc") || strstr(l, "musl") || strstr(l, "gcompat") || strstr(l, "memfd") || strstr(l, "libgcc")) printf("rawsys: maps: %s\n", l);
    }
    printf("rawsys: maps: %d lines\n", lines);
}

/* ------------------------------------------------------------------------------------------------------------
 * Finding dlopen the way the bootstrapper does (RocketLeague_EAC.exe 0x14001c850 / 0x14001c470), then loading
 * a glibc-built module from a memfd through it:
 *   1. read /proc/<getpid>/maps with raw syscalls; the first line whose path contains "/libc-" or "libc.so"
 *      (case-insensitive) is libc; its start address (offset 0) is the ELF header;
 *   2. walk that image's PT_DYNAMIC by hand: DT_HASH / DT_GNU_HASH, DT_SYMTAB, DT_STRTAB are taken as
 *      absolute pointers (glibc's ld.so leaves them so), look up "dlopen" and "dlsym" (GNU hash only works
 *      with the bootstrapper's own hash: djb2 for both tables);
 *   3. call them through a Windows-ABI -> SysV-ABI trampoline: rcx = function, rdx/r8/r9 = three arguments.
 */
typedef long long (*call3_fn)(void *fn, long long a, long long b, long long c);
extern void rawsys_call3(void), rawsys_call3_end(void);
__asm__(
    ".text\n.globl rawsys_call3\nrawsys_call3:\n"
    "  push %rdi\n  push %rsi\n  mov %rcx,%rax\n  mov %rdx,%rdi\n  push %rdx\n  mov %r8,%rsi\n  mov %r9,%rdx\n"
    "  call *%rax\n  pop %rdx\n  pop %rsi\n  pop %rdi\n  ret\n"
    ".globl rawsys_call3_end\nrawsys_call3_end:\n  int3\n");

typedef struct { unsigned char ident[16]; unsigned short type, machine; unsigned version; uint64_t entry, phoff, shoff;
                 unsigned flags; unsigned short ehsize, phentsize, phnum, shentsize, shnum, shstrndx; } Ehdr;
typedef struct { unsigned type, flags; uint64_t offset, vaddr, paddr, filesz, memsz, align; } Phdr;
typedef struct { int64_t tag; uint64_t val; } Dyn;
typedef struct { unsigned name; unsigned char info, other; unsigned short shndx; uint64_t value, size; } Sym;

static int ci_contains(const char *s, const char *needle)
{
    for (; *s; s++) {
        const char *a = s, *b = needle;
        while (*a && *b && ((*a | 0x20) == (*b | 0x20))) a++, b++;
        if (!*b) return 1;
    }
    return 0;
}

/* the bootstrapper's find_libc(): returns the base address, and the line it matched in `line` */
static uint64_t find_libc(sys3_fn sys, char *line, size_t cap)
{
    static char buf[1 << 20];
    char path[64];
    snprintf(path, sizeof path, "/proc/%lld/maps", sys(SYS_getpid, 0, 0, 0));
    long long fd = sys(SYS_open, (long long)(uintptr_t)path, 0, 0);
    if (fd < 0) return 0;
    size_t n = 0;
    for (;;) {
        long long r = sys(SYS_read, fd, (long long)(uintptr_t)buf + (long long)n, (long long)(sizeof buf - 1 - n));
        if (r <= 0) break;
        n += (size_t)r;
    }
    buf[n] = 0;
    sys(SYS_close, fd, 0, 0);
    for (char *l = strtok(buf, "\n"); l; l = strtok(NULL, "\n")) {
        char *tok[6]; int nt = 0;
        char *copy = _strdup(l), *p = copy;
        while (nt < 6) { while (*p == ' ') p++; if (!*p) break; tok[nt++] = p; while (*p && *p != ' ') p++; if (*p) *p++ = 0; }
        if (nt == 6 && (ci_contains(tok[5], "/libc-") || ci_contains(tok[5], "libc.so"))) {
            uint64_t start = strtoull(tok[0], NULL, 16), off = strtoull(tok[2], NULL, 16);
            snprintf(line, cap, "%s", l);
            free(copy);
            return start - off;
        }
        free(copy);
    }
    return 0;
}

/* the bootstrapper's symbol lookup in a loaded image; *why says what failed */
static void *elf_sym(uint64_t base, const char *name, const char **why)
{
    const Ehdr *eh = (const Ehdr *)(uintptr_t)base;
    if (memcmp(eh->ident, "\177ELF", 4) || eh->machine != 0x3e || eh->type != 3 || eh->phentsize != sizeof(Phdr)) { *why = "not an x86-64 shared object at that address"; return NULL; }
    const Phdr *ph = (const Phdr *)(uintptr_t)(base + eh->phoff);
    const Dyn *dyn = NULL;
    for (int i = 0; i < eh->phnum; i++) if (ph[i].type == 2) dyn = (const Dyn *)(uintptr_t)(base + ph[i].vaddr);
    if (!dyn) { *why = "no PT_DYNAMIC"; return NULL; }
    uint64_t hash = 0, gnu = 0, str = 0, sym = 0;
    for (; dyn->tag; dyn++) {
        if (dyn->tag == 4) hash = dyn->val;
        else if (dyn->tag == 0x6ffffef5) gnu = dyn->val;
        else if (dyn->tag == 5) str = dyn->val;
        else if (dyn->tag == 6) sym = dyn->val;
    }
    uint64_t tab = gnu ? gnu : hash;
    if (!tab || !str || !sym) { *why = "no hash/symtab/strtab"; return NULL; }
    if (tab < base || str < base || sym < base) { *why = "the dynamic section holds link-time offsets, not absolute addresses"; return NULL; }
    const char *strtab = (const char *)(uintptr_t)str;
    const Sym *syms = (const Sym *)(uintptr_t)sym;
    uint32_t h = 5381;
    for (const char *c = name; *c; c++) h = h * 33 + (unsigned char)*c;       /* the bootstrapper uses djb2 for both tables */
    if (gnu) {
        const uint32_t *t = (const uint32_t *)(uintptr_t)gnu;
        uint32_t nb = t[0], so = t[1], bloom = t[2];
        const uint64_t *bl = (const uint64_t *)(t + 4);
        const uint32_t *bk = (const uint32_t *)(bl + bloom), *ch = bk + nb;
        uint32_t i = bk[h % nb];
        if (i < so) return NULL;
        for (;; i++) {
            uint32_t c = ch[i - so];
            if ((c | 1) == (h | 1) && !strcmp(strtab + syms[i].name, name)) return (void *)(uintptr_t)(base + syms[i].value);
            if (c & 1) break;
        }
        return NULL;
    }
    const uint32_t *t = (const uint32_t *)(uintptr_t)hash;
    uint32_t nb = t[0];
    const uint32_t *bk = t + 2, *ch = bk + nb;
    for (uint32_t i = bk[h % nb]; i; i = ch[i])
        if (!strcmp(strtab + syms[i].name, name)) return (void *)(uintptr_t)(base + syms[i].value);
    return NULL;
}

static unsigned char *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *b = (unsigned char *)malloc((size_t)n);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return NULL; }
    fclose(f);
    *len = (size_t)n;
    return b;
}

static void libc_case(sys3_fn sys, void *call3_page)
{
    char line[512];
    printf("rawsys: --- libc discovery and a glibc-built module from a memfd\n");
    uint64_t base = find_libc(sys, line, sizeof line);
    printf("rawsys: libc line: %s\n", line);
    check(base != 0, "/proc/self/maps has a line that names libc (\"libc.so\" or \"/libc-\")", (long long)base);
    if (!base) return;
    const char *why = "";
    void *p_dlopen = elf_sym(base, "dlopen", &why);
    check(p_dlopen != NULL, why[0] ? why : "dlopen found in libc's symbol table", (long long)(uintptr_t)p_dlopen);
    void *p_dlsym = elf_sym(base, "dlsym", &why);
    check(p_dlsym != NULL, "dlsym found in libc's symbol table", (long long)(uintptr_t)p_dlsym);
    if (!p_dlopen || !p_dlsym) return;
    call3_fn call3 = (call3_fn)call3_page;

    size_t len = 0;
    unsigned char *mod = read_file("Z:\\exectest\\xrl_mod.so", &len);
    check(mod != NULL, "read Z:\\exectest\\xrl_mod.so (host-built, glibc ABI)", (long long)len);
    if (!mod) return;
    long long fd = sys(SYS_memfd_create, (long long)(uintptr_t)"xrl_mod", 2, 0);
    long long w = fd >= 0 ? sys(SYS_write, fd, (long long)(uintptr_t)mod, (long long)len) : -1;
    check(w == (long long)len, "the module is written into a memfd", w);
    char fdpath[64];
    snprintf(fdpath, sizeof fdpath, "/proc/self/fd/%d", (int)fd);
    long long h = call3(p_dlopen, (long long)(uintptr_t)fdpath, 2 /* RTLD_NOW */, 0);
    check(h != 0, "dlopen(\"/proc/self/fd/N\") of the memfd", h);
    if (!h) {
        void *p_dlerror = elf_sym(base, "dlerror", &why);
        const char *e = p_dlerror ? (const char *)(uintptr_t)call3(p_dlerror, 0, 0, 0) : NULL;
        printf("rawsys: dlerror: %s\n", e ? e : "(none)");
        return;
    }
    void *f_ans = (void *)(uintptr_t)call3(p_dlsym, h, (long long)(uintptr_t)"xrl_mod_answer", 0);
    void *f_pid = (void *)(uintptr_t)call3(p_dlsym, h, (long long)(uintptr_t)"xrl_mod_getpid", 0);
    void *f_fmt = (void *)(uintptr_t)call3(p_dlsym, h, (long long)(uintptr_t)"xrl_mod_fmt", 0);
    void *f_ctor = (void *)(uintptr_t)call3(p_dlsym, h, (long long)(uintptr_t)"xrl_mod_ctor", 0);
    check(f_ans && f_pid && f_fmt && f_ctor, "dlsym finds the module's four functions", (long long)(uintptr_t)f_ans);
    if (!(f_ans && f_pid && f_fmt && f_ctor)) return;
    long long a = call3(f_ans, 20, 0, 0);
    check((int)a == 41, "module function runs (xrl_mod_answer(20) == 41)", a);
    long long cr = call3(f_ctor, 0, 0, 0);
    check((int)cr == 7, "the module's constructor ran at dlopen", cr);
    long long mp = call3(f_pid, 0, 0, 0), pid = sys(SYS_getpid, 0, 0, 0);
    check(mp == pid, "the module's own raw syscall (getpid) is answered by the layer", mp);
    char out[32] = {0};
    long long fm = call3(f_fmt, (long long)(uintptr_t)out, sizeof out, 42);
    check(!strcmp(out, "v=42"), "a glibc-only symbol (__snprintf_chk) is answered by the shim", fm);

    void *f_ctx = (void *)(uintptr_t)call3(p_dlsym, h, (long long)(uintptr_t)"xrl_mod_ctx", 0);
    void *f_bt = (void *)(uintptr_t)call3(p_dlsym, h, (long long)(uintptr_t)"xrl_mod_backtrace", 0);
    long long cx = f_ctx ? call3(f_ctx, 0, 0, 0) : -1;
    check((int)cx == 7107, "getcontext/makecontext/swapcontext (the shim) switch stacks and resume", cx);
    long long bt = f_bt ? call3(f_bt, 0, 0, 0) : -1;
    check(bt >= 1, "libgcc_s's unwinder (_Unwind_Backtrace, _dl_find_object) walks frames of the module", bt);

    /* the code the program sees is the file's: look at the module's own function bytes (it contains `syscall`) */
    const Ehdr *eh = (const Ehdr *)mod;
    const Phdr *ph = (const Phdr *)(mod + eh->phoff);
    uint64_t fn_off = 0, rel = (uint64_t)(uintptr_t)f_pid;
    /* the module's load base: its dlsym result minus the symbol's link-time address (found in the file's dynsym) */
    uint64_t mbase = 0;
    {
        const Dyn *dyn = NULL; uint64_t symoff = 0, stroff = 0, hashoff = 0, gnuoff = 0;
        for (int i = 0; i < eh->phnum; i++) if (ph[i].type == 2) dyn = (const Dyn *)(mod + ph[i].offset);
        for (; dyn && dyn->tag; dyn++) { if (dyn->tag == 6) symoff = dyn->val; if (dyn->tag == 5) stroff = dyn->val; if (dyn->tag == 4) hashoff = dyn->val; if (dyn->tag == 0x6ffffef5) gnuoff = dyn->val; }
        (void)hashoff; (void)gnuoff;
        /* the five exported symbols are the first dynsym entries after the null one; find ours by name */
        const Sym *sy = (const Sym *)(mod + symoff);
        for (unsigned i = 1; i < (stroff - symoff) / sizeof(Sym); i++)      /* dynstr follows dynsym */
            if (!strcmp((const char *)(mod + stroff) + sy[i].name, "xrl_mod_getpid")) { mbase = rel - sy[i].value; fn_off = sy[i].value; }
    }
    for (int i = 0; i < eh->phnum && fn_off; i++)
        if (ph[i].type == 1 && fn_off >= ph[i].vaddr && fn_off < ph[i].vaddr + ph[i].filesz) { fn_off = fn_off - ph[i].vaddr + ph[i].offset; break; }
    (void)mbase;
    check(fn_off && !memcmp((void *)(uintptr_t)rel, mod + fn_off, 24), "the module's code in memory is byte-identical to the file (its `syscall` is not patched)", (long long)fn_off);
    free(mod);
}

int main(void)
{
    size_t n = (size_t)((char *)rawsys_stub_end - (char *)rawsys_stub);
    printf("rawsys: stub is %u bytes (the bootstrapper's is 21)\n", (unsigned)n);

    run_case("image", (sys3_fn)(void *)rawsys_stub);
    show_maps((sys3_fn)(void *)rawsys_stub);

    /* the bootstrapper: try 0x7001_0000_0000, step 4 GiB, ten times; keep the first allocation, which must be
     * >= 0x7000_0000_0000 */
    void *page = NULL;
    uint64_t hint = 0x700100000000ull;
    int tries = 0;
    for (; tries < 10 && !page; tries++, hint += 0x100000000ull)
        page = VirtualAlloc((void *)(uintptr_t)hint, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    printf("rawsys: VirtualAlloc(RWX) after %d tries -> %p\n", tries, page);
    check(page && (uintptr_t)page >= 0x700000000000ull, "the helper page is at or above 0x7000_0000_0000", (long long)(uintptr_t)page);
    if (page) {
        memcpy(page, (void *)rawsys_stub, n);
        run_case("heap", (sys3_fn)page);

        /* generated code that changes: a page that was translated is written again (a JIT, a patched stub).
         * Each version must run as written: mov eax,39 (getpid) / mov eax,0x3fff (ENOSYS), over and over,
         * more often than the layer has translation slots. */
        static const unsigned char A[] = { 0xb8, 0x27, 0, 0, 0, 0x0f, 0x05, 0xc3 };
        static const unsigned char B[] = { 0xb8, 0xff, 0x3f, 0, 0, 0x0f, 0x05, 0xc3 };
        long long pid = sys_pid(page);
        int bad = 0;
        for (int i = 0; i < 8; i++) {
            memcpy((char *)page + 0x100, (i & 1) ? B : A, 8);
            long long v = ((long long (*)(void))((char *)page + 0x100))();
            if ((i & 1) ? v != -38 : v != pid) bad++;
        }
        check(!bad, "a page of generated code rewritten 8 times always runs the latest version", bad);

        libc_case((sys3_fn)page, (void *)rawsys_call3);
    }

    printf("rawsys: %s (%d failed)\n", g_fail ? "FAILED" : "RAWSYS-OK", g_fail);
    fflush(stdout);
    return g_fail ? 1 : 0;
}
