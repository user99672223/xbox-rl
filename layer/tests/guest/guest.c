/*
 * guest.c - freestanding x86-64 Linux test guest. It plays the role of
 * "Wine+musl with __syscall patched": it finds xrl_syscall via the AT_XRL_SYSCALL
 * auxv entry and calls it with the SysV ABI instead of executing `syscall`.
 * Build (WSL): see build.sh. Exit code = number of failed checks.
 */
typedef unsigned long u64; typedef long s64; typedef unsigned int u32; typedef unsigned char u8; typedef unsigned short u16;
typedef long (*xsc_t)(long, long, long, long, long, long, long);
xsc_t xsc;            /* referenced by clone asm */
static u64 xctx;

#define AT_XRL_SYSCALL 0x5801
#define AT_XRL_CTX 0x5802
#define AT_RANDOM 25
static u64 at_random, at_base, at_phnum, at_hwcap2;
static int fake_host;   /* argv has --fake-host: the desktop test host's fake op tables */

#define sc(n, a, b, c, d, e, f) xsc((n), (long)(a), (long)(b), (long)(c), (long)(d), (long)(e), (long)(f))
#define sc1(n, a) sc(n, a, 0, 0, 0, 0, 0)
#define sc2(n, a, b) sc(n, a, b, 0, 0, 0, 0)
#define sc3(n, a, b, c) sc(n, a, b, c, 0, 0, 0)
#define sc4(n, a, b, c, d) sc(n, a, b, c, d, 0, 0)

void *memset(void *d, int c, unsigned long n) { u8 *p = d; while (n--) *p++ = (u8)c; return d; }
void *memcpy(void *d, const void *s, unsigned long n) { u8 *p = d; const u8 *q = s; while (n--) *p++ = *q++; return d; }
int memcmp(const void *a, const void *b, unsigned long n) { const u8 *p = a, *q = b; while (n--) { if (*p != *q) return *p - *q; p++; q++; } return 0; }
static unsigned long slen(const char *s) { unsigned long n = 0; while (s[n]) n++; return n; }

static void out(const char *s) { sc3(1, 1, s, slen(s)); }
static void outn(const char *label, long v)
{
    char b[96]; int n = 0;
    for (const char *p = label; *p && n < 60; p++) b[n++] = *p;
    b[n++] = '=';
    if (v < 0) { b[n++] = '-'; v = -v; }
    char t[24]; int k = 0; if (!v) t[k++] = '0'; while (v) { t[k++] = '0' + v % 10; v /= 10; }
    while (k) b[n++] = t[--k];
    b[n++] = '\n'; b[n] = 0;
    out(b);
}
static int fails;
#define CHECK(cond, name) do { if (cond) out("  ok   " name "\n"); else { out("  FAIL " name "\n"); fails++; } } while (0)

/* ---- clone wrapper: models the B-patched musl __clone ---------------------- */
long xclone(int (*fn)(void *), void *stack, long flags, void *arg, void *ptid, void *tls, void *ctid);
__asm__(
".text\n.globl xclone\nxclone:\n"
"  push %r12\n  push %r13\n"
"  mov %rdi,%r12\n  mov %rcx,%r13\n"
"  mov 24(%rsp),%rax\n"            /* ctid (7th arg) */
"  and $-16,%rsi\n"
"  mov %rsi,%r10\n  mov %rdx,%r11\n"
"  mov $56,%edi\n  mov %r11,%rsi\n  mov %r10,%rdx\n  mov %r8,%rcx\n  mov %rax,%r8\n"
"  mov xsc(%rip),%rax\n"
"  push $0\n  push $0\n"            /* a6 slot + alignment (parent only) */
"  call *%rax\n"
"  add $16,%rsp\n"
"  test %eax,%eax\n  jnz 1f\n"
"  mov %r13,%rdi\n  xor %ebp,%ebp\n  call *%r12\n"
"  mov %eax,%esi\n  mov $60,%edi\n  mov xsc(%rip),%rax\n  push $0\n  push $0\n  call *%rax\n  ud2\n"
"1: pop %r13\n  pop %r12\n  ret\n");

__asm__(".text\n.globl _start\n_start:\n  xor %ebp,%ebp\n  mov %rsp,%rdi\n  and $-16,%rsp\n  call cstart\n  ud2\n");

/* ---- helpers ------------------------------------------------------------- */
static long futex(volatile int *a, int op, int val, const void *ts, volatile int *a2, int v3) { return sc(202, a, op, val, ts, a2, v3); }
static void wait_ne(volatile int *a, int v)   /* block until *a != v (race-free) */
{
    for (;;) { int c = *a; if (c != v) return; futex(a, 0, c, 0, 0, 0); }
}
static void wait_eq(volatile int *a, int v)   /* block until *a == v */
{
    for (;;) { int c = *a; if (c == v) return; futex(a, 0, c, 0, 0, 0); }
}
static void *mmap_anon(u64 len, int prot) { return (void *)sc(9, 0, len, prot, 0x22, -1, 0); }
static u64 mono_ns(void) { long ts[2]; sc2(228, 1, ts); return (u64)ts[0] * 1000000000ull + (u64)ts[1]; }

/* ---- thread test --------------------------------------------------------- */
struct tls { struct tls *self; u64 magic; u64 pad[6]; };
static volatile int tid_slot[8], go, stop, thread_bad[8], thread_iters[8], pingpong, started;

static int thr(void *arg)
{
    int idx = (int)(long)arg;
    u64 bad = 0, n = 0;
    __atomic_fetch_add(&started, 1, __ATOMIC_SEQ_CST);
    futex(&started, 1, 0x7fffffff, 0, 0, 0);
    while (!stop) {
        /* Only the thread-pointer load form is allowed (tools/xrl-tpfix.py turns
         * it into a gs:[XRL_TP_GS_OFFSET] load); fields go through the pointer. */
        struct tls *p;
        __asm__ volatile("mov %%fs:0, %0" : "=r"(p));
        if (p->self != p || p->magic != 0xfeed0000ull + idx) bad++;
        n++;
        if ((n & 0x3ffff) == 0) sc(24, 0, 0, 0, 0, 0, 0);   /* sched_yield */
    }
    thread_bad[idx] = (int)bad; thread_iters[idx] = (int)(n >> 8);
    return idx;
}

static int pp_thr(void *arg)
{
    (void)arg;
    for (int i = 0; i < 2000; i++) {
        wait_eq(&pingpong, 2 * i + 1);
        __atomic_store_n(&pingpong, 2 * i + 2, __ATOMIC_SEQ_CST);
        futex(&pingpong, 1, 1, 0, 0, 0);
    }
    return 0;
}

static void test_threads(void)
{
    out("threads/futex/FS base:\n");
    enum { N = 6 };
    volatile int ctid[N];
    long flags = 0x100 | 0x200 | 0x400 | 0x800 | 0x10000 | 0x40000 | 0x80000 | 0x200000 | 0x100000;   /* musl pthread_create flags */
    for (int i = 0; i < N; i++) {
        u8 *stk = mmap_anon(256 * 1024, 3);
        struct tls *t = mmap_anon(4096, 3);
        t->self = t; t->magic = 0xfeed0000ull + i;
        ctid[i] = -1;
        long r = xclone(thr, stk + 256 * 1024 - 64, flags, (void *)(long)i, (void *)&tid_slot[i], t, (void *)&ctid[i]);
        if (r <= 0) { outn("clone failed", r); fails++; return; }
    }
    CHECK(1, "6 threads cloned (CLONE_SETTLS|PARENT_SETTID|CHILD_CLEARTID)");
    for (;;) { int c = started; if (c >= N) break; futex(&started, 0, c, 0, 0, 0); }
    CHECK(1, "all threads started (futex wake from threads reached waiters)");
    u64 t0 = mono_ns();
    struct { long s, n; } ts = { 0, 300000000 };
    sc4(35, &ts, 0, 0, 0);  /* nanosleep 300ms while threads hammer %fs: */
    u64 dt = mono_ns() - t0;
    CHECK(dt >= 280000000ull && dt < 2000000000ull, "nanosleep(300ms) within bounds");
    stop = 1;
    for (int i = 0; i < N; i++) {
        wait_eq(&ctid[i], 0);   /* join via CLEARTID wake */
        if (thread_bad[i]) outn("thread bad fs reads", thread_bad[i]);
        CHECK(thread_bad[i] == 0, "thread pointer (TP slot) stable across preemption");
        CHECK(tid_slot[i] > 0 && ctid[i] == 0, "PARENT_SETTID + CHILD_CLEARTID");
    }
    /* futex ping-pong main<->thread */
    {
        u8 *stk = mmap_anon(256 * 1024, 3);
        volatile int c = -1;
        struct tls *t = mmap_anon(4096, 3); t->self = t;
        xclone(pp_thr, stk + 256 * 1024 - 64, flags, 0, 0, t, (void *)&c);
        u64 a = mono_ns();
        for (int i = 0; i < 2000; i++) {
            __atomic_store_n(&pingpong, 2 * i + 1, __ATOMIC_SEQ_CST);
            futex(&pingpong, 1, 1, 0, 0, 0);
            wait_eq(&pingpong, 2 * i + 2);
        }
        u64 b = mono_ns();
        outn("futex pingpong round trips/s", (long)(2000ull * 1000000000ull / (b - a)));
        CHECK(pingpong == 4000, "futex ping-pong 2000 round trips");
        wait_eq(&c, 0);
    }
    /* futex timeout & EAGAIN */
    {
        volatile int w = 5;
        struct { long s, n; } to = { 0, 20000000 };
        u64 a = mono_ns();
        long r = futex(&w, 0, 5, &to, 0, 0);
        u64 b = mono_ns();
        CHECK(r == -110 && b - a >= 18000000ull, "futex wait timeout -> -ETIMEDOUT after >=20ms");
        CHECK(futex(&w, 0, 6, 0, 0, 0) == -11, "futex wait value mismatch -> -EAGAIN");
        CHECK(futex(&w, 1, 1, 0, 0, 0) == 0, "futex wake with no waiters -> 0");
    }
}

static void test_fs(void)
{
    out("file/VFS:\n");
    const char *path = "/tmp/xrl_t.txt";
    long fd = sc4(257, -100, path, 0102 /*O_CREAT|O_RDWR*/, 0644);
    CHECK(fd >= 3, "openat(O_CREAT|O_RDWR)");
    CHECK(sc3(1, fd, "hello world", 11) == 11, "write");
    char buf[32]; memset(buf, 0, sizeof buf);
    CHECK(sc4(17, fd, buf, 5, 6) == 5 && !memcmp(buf, "world", 5), "pread64");
    CHECK(sc3(8, fd, 0, 0) == 0, "lseek(SET)");
    CHECK(sc3(0, fd, buf, 11) == 11 && !memcmp(buf, "hello world", 11), "read");
    u64 st[18]; memset(st, 0, sizeof st);
    CHECK(sc2(5, fd, st) == 0 && st[6] == 11 && (st[3] & 0170000) == 0100000, "fstat size=11 S_ISREG");
    CHECK(sc1(3, fd) == 0, "close");
    CHECK(sc3(2, "/tmp/nonexistent_xyz", 0, 0) == -2, "open missing -> -ENOENT");
    CHECK(sc3(83, "/tmp/xrl_dir", 0755, 0) == 0, "mkdir");
    CHECK(sc2(82, path, "/tmp/xrl_dir/moved.txt") == 0, "rename into subdir");
    long d = sc3(2, "/tmp/xrl_dir", 0200000, 0);
    CHECK(d >= 3, "open directory");
    char db[1024]; long n = sc3(217, d, db, sizeof db);
    int found = 0, off = 0;
    while (off < n) { u16 rl = *(u16 *)(db + off + 16); const char *nm = db + off + 19; if (!memcmp(nm, "moved.txt", 10)) found = 1; off += rl; }
    CHECK(n > 0 && found, "getdents64 lists moved.txt");
    sc1(3, d);
    CHECK(sc2(21, "/tmp/xrl_dir/moved.txt", 0) == 0, "access");
    CHECK(sc1(87, "/tmp/xrl_dir/moved.txt") == 0, "unlink");
    CHECK(sc1(84, "/tmp/xrl_dir") == 0, "rmdir");
    /* symlink table */
    CHECK(sc2(88, "/tmp", "/tmp_link") == 0, "symlink");
    char lb[64]; memset(lb, 0, sizeof lb);
    CHECK(sc3(89, "/tmp_link", lb, 63) == 4 && !memcmp(lb, "/tmp", 4), "readlink");
    long fd2 = sc4(257, -100, "/tmp_link/xrl_viasym.txt", 0102, 0644);
    CHECK(fd2 >= 3, "open through symlink");
    sc1(3, fd2); sc1(87, "/tmp/xrl_viasym.txt"); sc1(87, "/tmp_link");
    /* special names */
    long fz = sc3(2, "/dev/zero", 0, 0); char z[8] = {1,1,1,1,1,1,1,1};
    CHECK(fz >= 0 && sc3(0, fz, z, 8) == 8 && z[0] == 0 && z[7] == 0, "/dev/zero");
    long fr = sc3(2, "/dev/urandom", 0, 0); u8 r1[16], r2[16];
    sc3(0, fr, r1, 16); sc3(0, fr, r2, 16);
    CHECK(memcmp(r1, r2, 16) != 0, "/dev/urandom differs between reads");
    long gr = sc3(318, r1, 16, 0);
    CHECK(gr == 16, "getrandom");
    long fm = sc3(2, "/proc/self/maps", 0, 0); char mb[512]; memset(mb, 0, sizeof mb);
    long mn = sc3(0, fm, mb, 500);
    CHECK(mn > 20, "/proc/self/maps readable");
    long fe = sc3(89, "/proc/self/exe", mb, 256);
    CHECK(fe > 0, "readlink /proc/self/exe");
    CHECK(sc3(2, "/dev/null", 2, 0) >= 0, "/dev/null");
    /* pipes / dup / fcntl / eventfd / epoll */
    int pf[2];
    CHECK(sc2(293, pf, 0) == 0, "pipe2");
    CHECK(sc3(1, pf[1], "xyz", 3) == 3, "pipe write");
    struct { int fd; short ev, rev; } pfd = { pf[0], 1, 0 };
    CHECK(sc3(7, &pfd, 1, 0) == 1 && (pfd.rev & 1), "poll POLLIN on pipe");
    char pb[8]; CHECK(sc3(0, pf[0], pb, 8) == 3, "pipe read");
    CHECK(sc3(72, pf[0], 3, 0) >= 0, "fcntl GETFL");
    long dd = sc2(32, pf[0], 0);
    CHECK(dd >= 0 && dd != pf[0], "dup");
    long ev = sc2(290, 0, 0);
    u64 one = 5; sc3(1, ev, &one, 8); u64 got = 0;
    CHECK(sc3(0, ev, &got, 8) == 8 && got == 5, "eventfd2 write/read");
    long ep = sc1(291, 0);
    struct { u32 e; u64 d; } __attribute__((packed)) epev = { 1, 0x1234 }, outev;
    sc4(233, ep, 1, pf[0], &epev);
    sc3(1, pf[1], "a", 1);
    CHECK(sc4(232, ep, &outev, 1, 100) == 1 && outev.d == 0x1234, "epoll_wait");
    /* socketpair + SCM_RIGHTS */
    int sp[2];
    CHECK(sc4(53, 1, 1, 0, sp) == 0, "socketpair");
    struct { u64 base, len; } iov = { (u64)"X", 1 };
    struct { u64 hdr_len; int lvl, type; int fd; int pad; } cm = { 20, 1, 1, pf[1], 0 };
    struct { u64 name; u32 nl, p0; u64 iov, iovlen, ctl, ctllen; u32 fl, p1; } msg = { 0, 0, 0, (u64)&iov, 1, (u64)&cm, 24, 0, 0 };
    CHECK(sc3(46, sp[0], &msg, 0) == 1, "sendmsg SCM_RIGHTS");
    char rb[4]; struct { u64 base, len; } riov = { (u64)rb, 1 };
    struct { u64 hdr_len; int lvl, type; int fd; int pad; } rcm; memset(&rcm, 0, sizeof rcm);
    struct { u64 name; u32 nl, p0; u64 iov, iovlen, ctl, ctllen; u32 fl, p1; } rmsg = { 0, 0, 0, (u64)&riov, 1, (u64)&rcm, 24, 0, 0 };
    CHECK(sc3(47, sp[1], &rmsg, 0) == 1 && rcm.type == 1 && rcm.fd >= 3, "recvmsg receives fd");
    CHECK(sc3(1, rcm.fd, "q", 1) == 1, "received fd is a live duplicate of the pipe write end");
}

static void test_mem(void)
{
    out("memory:\n");
    u8 *p = mmap_anon(1 << 20, 3);
    CHECK((long)p > 0x10000, "mmap anon 1MiB");
    p[0] = 1; p[(1 << 20) - 1] = 2;
    CHECK(p[0] == 1 && p[(1 << 20) - 1] == 2, "anon memory usable");
    CHECK(sc3(10, p, 1 << 20, 1) == 0, "mprotect -> PROT_READ");
    CHECK(sc3(10, p, 1 << 20, 3) == 0 && (p[5] = 9, 1), "mprotect -> RW");
    long fx = sc(9, 0x140000000ull, 1 << 16, 3, 0x32, -1, 0);
    CHECK(fx == 0x140000000ll, "MAP_FIXED at 0x140000000 (PE image base)");
    if (fx == 0x140000000ll) { *(volatile int *)0x140000000ull = 7; CHECK(*(volatile int *)0x140000000ull == 7, "fixed mapping writable"); }
    u8 *q = (u8 *)sc(9, 0, 1 << 20, 0, 0x22, -1, 0);   /* PROT_NONE reserve */
    CHECK((long)q > 0x10000, "PROT_NONE reserve");
    CHECK(sc3(10, q, 4096 * 4, 3) == 0, "mprotect reserved -> RW (commit)");
    q[100] = 3; CHECK(q[100] == 3, "committed page usable");
    u8 *r = (u8 *)sc(25, p, 1 << 20, 2 << 20, 1, 0, 0);
    CHECK((long)r > 0x10000 && r[5] == 9, "mremap grow+move preserves data");
    CHECK(sc2(11, r, 2 << 20) == 0, "munmap");
    long b0 = sc1(12, 0);
    long b1 = sc1(12, b0 + 65536);
    CHECK(b1 == b0 + 65536, "brk grow");
    *(volatile int *)(b0 + 100) = 5;
    CHECK(sc3(28, q, 4096 * 4, 4) == 0, "madvise(DONTNEED)");
    CHECK(q[100] == 0, "DONTNEED zeroes pages");
    u64 budget = *(u64 *)(xctx + 64);     /* xrl_guest_ctx.guest_mem_budget */
    long big = sc(9, 0, budget + (1ull << 30), 3, 0x22, -1, 0);
    CHECK(big == -12, "mmap beyond guest_mem_budget -> -ENOMEM");
}

static void test_misc(void)
{
    out("misc:\n");
    char un[390]; CHECK(sc1(63, un) == 0 && !memcmp(un, "Linux", 5), "uname");
    long ts[2]; CHECK(sc2(228, 0, ts) == 0 && ts[0] > 1700000000, "clock_gettime(REALTIME)");
    CHECK(sc1(186, 0) > 0, "gettid");
    u64 fsv = 0; CHECK(sc2(158, 0x1003, &fsv) == 0, "arch_prctl(GET_FS)");
    CHECK(sc2(158, 0x1001, 0x1234000) == -1, "arch_prctl(SET_GS) refused (-EPERM) for non-TEB value");
    u64 teb = 0; CHECK(sc2(158, 0x5801, &teb) == 0 && teb > 0x10000, "arch_prctl(XRL_GET_HOST_TEB)");
    CHECK(sc(202, 0, 999, 0, 0, 0, 0) == -38, "unknown futex op -> -ENOSYS");
    CHECK(sc1(999, 0) == -38, "unknown syscall 999 -> -ENOSYS");
    CHECK(sc1(1, 99) == -9, "write(bad fd) -> -EBADF");
    long ssz = sc3(204, 0, 128, un);
    CHECK(ssz > 0, "sched_getaffinity");
    CHECK(at_random != 0 && at_base == 0 && at_phnum > 0 && at_hwcap2 == 0, "auxv present");
    CHECK(xctx != 0 && *(unsigned *)(xctx + 4) == 0x58524c47, "AT_XRL_CTX magic");
    /* host op-table downcalls via generated SysV stubs */
    struct gfx { u32 sz; u32 pad; int (*create)(u64 *); int (*swap)(u64, u64, u32, u32, u32, u32, u64 *); void *gb, *p, *d11; void (*release)(u64); };
    struct gctx { u32 sz, magic, abi, flags; u64 sysc, tlsoff; void (*log)(int, const char *, const char *); struct gfx *g; void *a; void *i; u64 budget; } *c = (void *)xctx;
    struct gfx *g = c->g;
    if (fake_host) {
        u64 dev = 0; int r = g->create(&dev);
        CHECK(r == 0 && dev == 0xD3D12DEull, "downcall gfx.d3d12_create_device (host Win64 fn via SysV stub)");
        u64 sw = 0; r = g->swap(0x10, 0x01, 1920, 1080, 3, 28, &sw);
        CHECK(r == 0 && sw == (0x10 ^ 0x01 ^ ((u64)1920 << 32 | 1080) ^ (3ull << 8) ^ 28), "downcall with 7 args (stack-passed args marshalled)");
        g->release(0x77);
    } else {
        /* real host: one safe end-to-end downcall into the native gfx library
         * (v2 slot 9 = get_adapter_info; the host runs it on its gfx thread) */
        struct gfx2 { u32 sz; u32 pad; u64 slot[10]; } *g2 = (void *)c->g;
        if (g2->sz >= 88 && g2->slot[9]) {
            struct { u32 sz, ven, dev, sub, rev, fl; u64 vram, sysm, shared, luid; unsigned short desc[128]; } ai;
            memset(&ai, 0, sizeof ai);
            int r = ((int (*)(void *))g2->slot[9])(&ai);
            outn("real gfx.get_adapter_info rc", r); outn("  vendor", ai.ven); outn("  device", ai.dev);
            outn("  max feature level", ai.fl); outn("  dedicated VRAM MiB", (long)(ai.vram >> 20));
            CHECK(r == 0 && ai.ven && ai.fl >= 0xb000, "downcall real gfx.get_adapter_info from a guest thread");
        } else {
            out("  skip  real gfx downcall (host table has no get_adapter_info)\n");
        }
    }
    c->log(1, "guest", "log via host stub");
}

/* ---- ABI v2 / B-contract tests ------------------------------------------- */
/* long xclone_rpc(long flags, void *stack, void *ptid, void *ctid, void *tls):
 * models B's musl __clone: the child starts at an explicit resume_pc (7th arg)
 * with rsp = stack; fn/arg/map_base/map_size are pre-placed on that stack, and
 * the child exits with exit(code, map_base, map_size) like musl __unmapself. */
long xclone_rpc(long flags, void *stack, void *ptid, void *ctid, void *tls);
__asm__(
".text\n.globl xclone_rpc\nxclone_rpc:\n"
"  mov %r8,%r9\n  mov %rcx,%r8\n  mov %rdx,%rcx\n  mov %rsi,%rdx\n  mov %rdi,%rsi\n"
"  mov $56,%edi\n"
"  lea xrl_child_entry(%rip),%rax\n"
"  mov xsc(%rip),%r10\n"
"  push %rax\n"                       /* a6 = resume_pc */
"  call *%r10\n"
"  add $8,%rsp\n"
"  ret\n"
"xrl_child_entry:\n"
"  mov 8(%rsp),%rdi\n"
"  call *(%rsp)\n"
"  mov %eax,%esi\n  mov 16(%rsp),%rdx\n  mov 24(%rsp),%rcx\n"
"  mov $60,%edi\n  mov xsc(%rip),%rax\n  push $0\n  call *%rax\n  ud2\n");

static int rpc_thr(void *arg) { *(volatile int *)arg = 42; return 7; }

static void test_v2(void)
{
    out("abi v2 / wine contract:\n");
    struct gctx2 { u32 sz, magic, abi, flags; u64 sysc, tlsoff; u64 log, g, a, i, budget; } *c = (void *)xctx;
    CHECK(sc1(0x10001, 0) == (long)c->g && c->g, "pseudo-syscall GET_GFX_OPS == ctx gfx table");
    CHECK(sc1(0x10002, 0) == (long)c->a && sc1(0x10003, 0) == (long)c->i, "pseudo-syscalls GET_AUDIO/INPUT_OPS");
    CHECK(sc1(0x10004, 0) == (long)c->log && c->log, "pseudo-syscall GET_LOG_FN");
    CHECK(sc1(0x10005, 0) == -38, "pseudo-syscall out of range -> -ENOSYS");

    /* thread pointer slot: SET_FS lands where patched `mov %fs:0` loads read */
    u64 mode = 0;
    CHECK(sc2(158, 0x5802, &mode) == 0 && (mode & 2), "TP slot active (arch_prctl XRL_GET_FS_MODE bit1)");
    struct tls *mt = mmap_anon(4096, 3); mt->self = mt;
    CHECK(sc2(158, 0x1002, mt) == 0, "arch_prctl(SET_FS) accepted");
    struct tls *rd;
    __asm__ volatile("mov %%fs:0, %0" : "=r"(rd));
    CHECK(rd == mt, "main thread reads its thread pointer back after SET_FS");

    long flags = 0x100 | 0x200 | 0x400 | 0x800 | 0x10000 | 0x40000 | 0x80000 | 0x200000 | 0x100000;
    u64 mlen = 256 * 1024;
    u8 *stk = mmap_anon(mlen, 3);
    struct tls *t = mmap_anon(4096, 3); t->self = t;
    u64 *s = (u64 *)(stk + mlen - 64);
    volatile int hit = 0, ctid = -1, ptid = 0;
    s[0] = (u64)rpc_thr; s[1] = (u64)&hit; s[2] = (u64)stk; s[3] = mlen;
    long tid = xclone_rpc(flags, s, (void *)&ptid, (void *)&ctid, t);
    CHECK(tid > 0 && ptid == tid, "clone with resume_pc starts the child (PARENT_SETTID)");
    if (tid <= 0) { out("  skip  resume_pc child checks (clone failed)\n"); return; }
    wait_eq(&ctid, 0);
    CHECK(hit == 42, "child ran fn(arg) from resume_pc");
    u8 vec[1];
    CHECK(sc3(27, stk, 4096, vec) == -12, "exit(code, map_base, map_size) unmapped the child's stack");
    CHECK(sc3(27, t, 4096, vec) == 0, "unrelated mapping (child TLS) left intact");
}

void cstart(u64 *sp)
{
    long argc = (long)sp[0];
    u64 *p = sp + 1 + argc + 1;
    while (*p) p++;           /* skip env */
    p++;
    for (; p[0]; p += 2) {
        if (p[0] == AT_XRL_SYSCALL) xsc = (xsc_t)p[1];
        else if (p[0] == AT_XRL_CTX) xctx = p[1];
        else if (p[0] == AT_RANDOM) at_random = p[1];
        else if (p[0] == 7) at_base = p[1];
        else if (p[0] == 5) at_phnum = p[1];
        else if (p[0] == 26) at_hwcap2 = p[1];
    }
    if (!xsc) { __asm__ volatile("ud2"); }
    for (long i = 1; i < argc; i++) {
        const char *a = (const char *)sp[1 + i], *f = "--fake-host";
        int k = 0; while (a[k] && a[k] == f[k]) k++;
        if (!a[k] && !f[k]) fake_host = 1;
    }
    out("xrl guest test start\n");
    outn("argc", argc);
    test_misc();
    test_mem();
    test_fs();
    test_threads();
    test_v2();
    outn("TOTAL FAILS", fails);
    sc1(231, fails);
    for (;;) {}
}
