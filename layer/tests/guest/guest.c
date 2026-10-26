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
static int no_swap;     /* argv has --no-swap: skip the (slow) swap test */

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

/* /dev/ntsync (Linux ntsync uapi, Wine's fast sync path) */
#define NIOC(dir, nr, sz) (((unsigned long)(dir) << 30) | ((unsigned long)(sz) << 16) | ('N' << 8) | (nr))
#define N_CREATE_SEM   NIOC(1, 0x80, 8)
#define N_SEM_RELEASE  NIOC(3, 0x81, 4)
#define N_WAIT_ANY     NIOC(3, 0x82, 40)
#define N_WAIT_ALL     NIOC(3, 0x83, 40)
#define N_CREATE_MUTEX NIOC(1, 0x84, 8)
#define N_MUTEX_UNLOCK NIOC(3, 0x85, 8)
#define N_MUTEX_KILL   NIOC(1, 0x86, 4)
#define N_CREATE_EVENT NIOC(1, 0x87, 8)
#define N_EVENT_SET    NIOC(2, 0x88, 4)
#define N_EVENT_READ   NIOC(2, 0x8d, 8)
struct nwait { u64 timeout, objs; unsigned count, index, flags, owner, alert, pad; };
static volatile int ns_ev_fd, ns_go;

static int ns_setter(void *arg)
{
    (void)arg;
    struct { long s, n; } ts = { 0, 50000000 };
    sc4(35, &ts, 0, 0, 0);
    unsigned prev;
    sc3(16, ns_ev_fd, N_EVENT_SET, &prev);
    return 0;
}

static long ns_wait(int dev, int all, unsigned *fds, unsigned n, unsigned owner, u64 timeout, unsigned *index)
{
    struct nwait w = { timeout, (u64)(unsigned long)fds, n, ~0u, 0, owner, 0, 0 };
    /* pass the request sign-extended, as musl's ioctl(int fd, int req, ...) does */
    long r = sc3(16, dev, (long)(int)(all ? N_WAIT_ALL : N_WAIT_ANY), &w);
    if (index) *index = w.index;
    return r;
}

static void test_ntsync(void)
{
    out("ntsync:\n");
    int dev = (int)sc4(257, -100, "/dev/ntsync", 0x80000, 0);
    CHECK(dev >= 0, "open /dev/ntsync");
    if (dev < 0) return;
    unsigned a2[2];
    a2[0] = 0; a2[1] = 2;  /* sem count 0, max 2 */
    int sem = (int)sc3(16, dev, N_CREATE_SEM, a2);
    a2[0] = 0; a2[1] = 0;  /* manual 0, signaled 0 */
    int ev = (int)sc3(16, dev, N_CREATE_EVENT, a2);
    a2[0] = 0; a2[1] = 0;  /* unowned */
    int mtx = (int)sc3(16, dev, N_CREATE_MUTEX, a2);
    CHECK(sem >= 0 && ev >= 0 && mtx >= 0, "create semaphore, event, mutex");

    unsigned fds[2] = { (unsigned)sem, (unsigned)ev }, idx = 99;
    u64 now = mono_ns();
    CHECK(ns_wait(dev, 0, fds, 2, 7, now + 20000000ull, 0) == -110 && mono_ns() - now >= 18000000ull,
          "wait-any on unsignaled objects times out (-ETIMEDOUT)");
    unsigned v = 1;
    CHECK(sc3(16, sem, N_SEM_RELEASE, &v) == 0 && v == 0, "semaphore release returns previous count");
    CHECK(ns_wait(dev, 0, fds, 2, 7, ~0ull, &idx) == 0 && idx == 0, "wait-any takes the semaphore (index 0)");
    v = 3;
    CHECK(sc3(16, sem, N_SEM_RELEASE, &v) == -75, "release past max -> -EOVERFLOW");

    /* cross-thread wake: a thread sets the auto-reset event after 50 ms */
    ns_ev_fd = ev;
    long flags = 0x100 | 0x200 | 0x400 | 0x800 | 0x10000 | 0x40000 | 0x80000 | 0x200000 | 0x100000;
    u8 *stk = mmap_anon(256 * 1024, 3);
    struct tls *t = mmap_anon(4096, 3); t->self = t;
    volatile int c = -1;
    xclone(ns_setter, stk + 256 * 1024 - 64, flags, 0, 0, t, (void *)&c);
    now = mono_ns();
    long r = ns_wait(dev, 0, fds, 2, 7, ~0ull, &idx);
    u64 dt = mono_ns() - now;
    CHECK(r == 0 && idx == 1 && dt >= 40000000ull && dt < 2000000000ull, "event set by another thread wakes the waiter");
    wait_eq(&c, 0);
    CHECK(sc3(16, ev, N_EVENT_READ, a2) == 0 && a2[1] == 0, "auto-reset event is reset by the wait");

    /* mutex: recursion, owner checks, abandonment */
    unsigned m1[1] = { (unsigned)mtx };
    CHECK(ns_wait(dev, 0, m1, 1, 7, ~0ull, 0) == 0 && ns_wait(dev, 0, m1, 1, 7, ~0ull, 0) == 0, "mutex acquired twice by owner 7");
    CHECK(ns_wait(dev, 0, m1, 1, 8, mono_ns() + 1000000ull, 0) == -110, "owner 8 cannot take it");
    a2[0] = 8; a2[1] = 0;
    CHECK(sc3(16, mtx, N_MUTEX_UNLOCK, a2) == -1, "unlock by non-owner -> -EPERM");
    a2[0] = 7;
    CHECK(sc3(16, mtx, N_MUTEX_UNLOCK, a2) == 0 && a2[1] == 2, "unlock returns previous recursion count");
    v = 7;
    CHECK(sc3(16, mtx, N_MUTEX_KILL, &v) == 0, "kill by owner");
    CHECK(ns_wait(dev, 0, m1, 1, 8, ~0ull, 0) == -130, "next acquire reports abandonment (-EOWNERDEAD)");

    /* wait-all: needs every object at once */
    a2[0] = 1; a2[1] = 1;  /* manual, signaled */
    int ev2 = (int)sc3(16, dev, N_CREATE_EVENT, a2);
    unsigned all2[2] = { (unsigned)ev2, (unsigned)sem };
    CHECK(ns_wait(dev, 1, all2, 2, 7, mono_ns() + 1000000ull, 0) == -110, "wait-all blocks while one object is unsignaled");
    v = 1; sc3(16, sem, N_SEM_RELEASE, &v);
    CHECK(ns_wait(dev, 1, all2, 2, 7, ~0ull, 0) == 0, "wait-all succeeds once all are signaled");
    CHECK(sc3(16, ev2, N_EVENT_READ, a2) == 0 && a2[1] == 1, "manual event stays signaled");
    sc1(3, sem); sc1(3, ev); sc1(3, mtx); sc1(3, ev2); sc1(3, dev);
}

/* ---- signals: delivery at syscall return, async, restart, faults ------------ */
/* musl's patched __restore_rt (patches/musl): rt_sigreturn(&ucontext) via the gateway */
void sig_restorer(void);
__asm__(".text\n.globl sig_restorer\nsig_restorer:\n"
"  mov %rsp,%rsi\n  and $-16,%rsp\n  sub $16,%rsp\n  movq $0,(%rsp)\n  mov $15,%edi\n"
"  xor %edx,%edx\n  xor %ecx,%ecx\n  xor %r8d,%r8d\n  xor %r9d,%r9d\n  mov xsc(%rip),%rax\n  call *%rax\n  hlt\n");

/* Spin in pure guest code with known values in every register until *flag;
 * returns how many registers changed (an async signal lands in the loop). */
int spin_regs(volatile int *flag);
__asm__(".text\n.globl spin_regs\nspin_regs:\n"
"  push %rbx\n  push %rbp\n  push %r12\n  push %r13\n  push %r14\n  push %r15\n"
"  mov $0x1111,%rbx\n  mov $0x2222,%rbp\n  mov $0x3333,%r12\n  mov $0x4444,%r13\n  mov $0x5555,%r14\n  mov $0x6666,%r15\n"
"  mov $0x7777,%rsi\n  mov $0x8888,%r8\n  mov $0x9999,%r9\n  mov $0xaaaa,%r10\n  mov $0xbbbb,%r11\n  mov $0xcccc,%rdx\n"
"  mov $0xdddd,%rcx\n  movq %rbx,%xmm0\n  movq %rbp,%xmm7\n  movq %r12,%xmm15\n"
"1: pause\n  cmpl $0,(%rdi)\n  je 1b\n"
"  xor %eax,%eax\n"
"  cmp $0x1111,%rbx\n  je 2f\n  inc %eax\n2:\n  cmp $0x2222,%rbp\n  je 2f\n  inc %eax\n2:\n"
"  cmp $0x3333,%r12\n  je 2f\n  inc %eax\n2:\n  cmp $0x4444,%r13\n  je 2f\n  inc %eax\n2:\n"
"  cmp $0x5555,%r14\n  je 2f\n  inc %eax\n2:\n  cmp $0x6666,%r15\n  je 2f\n  inc %eax\n2:\n"
"  cmp $0x7777,%rsi\n  je 2f\n  inc %eax\n2:\n  cmp $0x8888,%r8\n  je 2f\n  inc %eax\n2:\n"
"  cmp $0x9999,%r9\n  je 2f\n  inc %eax\n2:\n  cmp $0xaaaa,%r10\n  je 2f\n  inc %eax\n2:\n"
"  cmp $0xbbbb,%r11\n  je 2f\n  inc %eax\n2:\n  cmp $0xcccc,%rdx\n  je 2f\n  inc %eax\n2:\n"
"  cmp $0xdddd,%rcx\n  je 2f\n  inc %eax\n2:\n"
"  movq %xmm0,%rcx\n  cmp $0x1111,%rcx\n  je 2f\n  inc %eax\n2:\n"
"  movq %xmm7,%rcx\n  cmp $0x2222,%rcx\n  je 2f\n  inc %eax\n2:\n"
"  movq %xmm15,%rcx\n  cmp $0x3333,%rcx\n  je 2f\n  inc %eax\n2:\n"
"  pop %r15\n  pop %r14\n  pop %r13\n  pop %r12\n  pop %rbp\n  pop %rbx\n  ret\n");

struct ksa { void *h; u64 flags; void *restorer; u64 mask; };
#define SA_SIGINFO 4ul
#define SA_ONSTACK 0x08000000ul
#define SA_RESTART 0x10000000ul
#define SA_RESTORER 0x04000000ul
static long sigact(int sig, void *h, u64 flags)
{
    struct ksa a = { h, flags | SA_SIGINFO | SA_RESTORER, (void *)sig_restorer, 0 };
    return sc4(13, sig, &a, 0, 8);
}
static u64 cur_mask(void) { u64 m = 0; sc4(14, 0, 0, &m, 8); return m; }
static long gettid(void) { return sc1(186, 0); }

/* ucontext offsets (Linux x86-64): mcontext at 40, rip = gregs[16], trapno, sigmask */
#define UC_RIP 168
#define UC_TRAPNO 200
#define UC_FPSTATE 224
#define UC_SIGMASK 296
static volatile int h_sig, h_code, h_pid, h_masked, h_tid, h_count, h_alt;
static volatile u64 h_rip, h_fpstate, segv_addr, segv_trap;
static volatile int segv_code;
static u8 *alt_lo, *alt_hi;
static void h_info(int sig, u8 *si, u8 *uc)
{
    h_sig = sig; h_code = *(int *)(si + 8); h_pid = *(int *)(si + 16);
    h_masked = (int)((cur_mask() >> (sig - 1)) & 1);
    h_rip = *(u64 *)(uc + UC_RIP); h_fpstate = *(u64 *)(uc + UC_FPSTATE);
    h_tid = (int)gettid();
    u8 here; h_alt = alt_lo && &here >= alt_lo && &here < alt_hi;
    __atomic_fetch_add(&h_count, 1, __ATOMIC_SEQ_CST);
}
static volatile int async_flag;
static void h_async(int sig, u8 *si, u8 *uc) { h_info(sig, si, uc); async_flag = 1; }
static void h_segv(int sig, u8 *si, u8 *uc)
{
    (void)sig;
    segv_addr = *(u64 *)(si + 16); segv_code = *(int *)(si + 8); segv_trap = *(u64 *)(uc + UC_TRAPNO);
    *(u64 *)(uc + UC_RIP) += 2;   /* skip the 2-byte faulting load */
}
static long fault_read(u64 addr)
{
    long v;
    __asm__ volatile("mov %1,%%rax\n  mov $77,%%edx\n  .byte 0x8b,0x10\n  mov %%rdx,%0"   /* mov (%rax),%edx */
                     : "=r"(v) : "r"(addr) : "rax", "rdx", "memory");
    return v;
}

static volatile int sg_ready, sg_done, sg_tid[4], sg_ctid[4];
static volatile long sg_ret;
static volatile int rw;
static int thr_spin(void *a) { (void)a; sg_ready = 1; futex(&sg_ready, 1, 1, 0, 0, 0); sg_ret = spin_regs(&async_flag); sg_done = 1; futex(&sg_done, 1, 1, 0, 0, 0); return 0; }
static int thr_futex(void *a)
{
    long ts[2] = { 3, 0 };
    sg_ready = 1; futex(&sg_ready, 1, 1, 0, 0, 0);
    sg_ret = futex(&rw, 0, 0, a ? ts : 0, 0, 0);   /* a: with a timeout, so a lost signal cannot hang the test */
    sg_done = 1; futex(&sg_done, 1, 1, 0, 0, 0);
    return 0;
}
static int thr_sleep(void *a)
{
    (void)a;
    long ts[2] = { 2, 0 }, rem[2] = { -1, -1 };
    sg_ready = 1; futex(&sg_ready, 1, 1, 0, 0, 0);
    sg_ret = sc2(35, ts, rem);
    sg_tid[3] = rem[0] == 1 || (rem[0] == 0 && rem[1] > 0);   /* about 2 s minus the time slept */
    sg_done = 1; futex(&sg_done, 1, 1, 0, 0, 0);
    return 0;
}
static long sg_start(int (*fn)(void *), void *arg, int slot)
{
    long flags = 0x100 | 0x200 | 0x400 | 0x800 | 0x10000 | 0x40000 | 0x80000 | 0x200000 | 0x100000;
    u8 *stk = mmap_anon(256 * 1024, 3);
    struct tls *t = mmap_anon(4096, 3);
    t->self = t;
    sg_ready = 0; sg_done = 0; sg_ctid[slot] = -1;
    long r = xclone(fn, stk + 256 * 1024 - 64, flags, arg, (void *)&sg_tid[slot], t, (void *)&sg_ctid[slot]);
    wait_ne(&sg_ready, 0);
    return r;
}
static void sleep_ms(long ms) { long ts[2] = { 0, ms * 1000000 }; sc2(35, ts, 0); }
static int wait_done(void)   /* <= 3 s */
{
    for (int i = 0; i < 300 && !sg_done; i++) sleep_ms(10);
    return sg_done;
}

static void test_signals(void)
{
    out("signals:\n");
    long pid = sc1(39, 0), me = gettid();
    sigact(10, h_info, 0);
    volatile u64 keep = 0x1234567890abcdefull;
    h_count = 0;
    long r = sc3(234, pid, me, 10);
    CHECK(r == 0 && h_count == 1 && h_sig == 10, "tgkill(self): the handler runs before the syscall returns");
    CHECK(h_code == -6 && h_pid == pid, "siginfo: SI_TKILL from our own pid");
    CHECK(h_masked, "the signal is blocked inside its handler");
    CHECK(h_fpstate != 0 && h_rip != 0, "ucontext has the interrupted rip and an fpstate");
    CHECK(!(cur_mask() & (1ul << 9)) && keep == 0x1234567890abcdefull, "rt_sigreturn restores the mask and the frame");
    sc4(14, 0, (u64[]){ 1ul << 9 }, 0, 8);          /* block SIGUSR1 */
    h_count = 0;
    sc3(234, pid, me, 10);
    CHECK(h_count == 0, "a blocked signal stays pending");
    sc4(14, 1, (u64[]){ 1ul << 9 }, 0, 8);          /* unblock: delivered as sigprocmask returns */
    CHECK(h_count == 1, "unblocking delivers the pending signal");

    /* async: the target spins in guest code, no syscalls */
    sigact(12, h_async, 0);
    async_flag = 0; h_count = 0;
    r = sg_start(thr_spin, 0, 0);
    sleep_ms(30);
    sc3(234, pid, sg_tid[0], 12);
    int ok = wait_done();
    CHECK(ok && h_count == 1 && h_tid == sg_tid[0], "async: the handler ran on the spinning thread");
    CHECK(ok && sg_ret == 0, "async: every register (GPRs, xmm0/7/15) intact after the handler");

    /* SA_RESTART: an interrupted futex wait carries on */
    sigact(10, h_info, SA_RESTART);
    rw = 0; h_count = 0;
    sg_start(thr_futex, 0, 1);
    sleep_ms(30);
    sc3(234, pid, sg_tid[1], 10);
    sleep_ms(50);
    CHECK(h_count == 1 && !sg_done, "SA_RESTART: the handler ran and the futex wait was restarted");
    rw = 1; futex(&rw, 1, 1, 0, 0, 0);
    CHECK(wait_done() && sg_ret == 0, "...and it returns 0 when woken");
    /* without SA_RESTART: -EINTR */
    sigact(10, h_info, 0);
    rw = 0; h_count = 0;
    sg_start(thr_futex, (void *)1, 2);
    sleep_ms(30);
    sc3(234, pid, sg_tid[2], 10);
    CHECK(wait_done() && h_count == 1 && sg_ret == -4, "no SA_RESTART: the futex wait returns -EINTR");

    /* nanosleep is never restarted: -EINTR and the time left */
    sigact(10, h_info, SA_RESTART);
    sg_start(thr_sleep, 0, 3);
    sleep_ms(30);
    sc3(234, pid, sg_tid[3], 10);
    CHECK(wait_done() && sg_ret == -4 && sg_tid[3], "nanosleep: -EINTR with the remaining time");

    /* a fault in guest code goes to the SIGSEGV handler */
    sigact(11, h_segv, 0);
    long v = fault_read(0x1000);
    CHECK(v == 77 && segv_addr == 0x1000 && segv_code == 1 && segv_trap == 14, "SIGSEGV: si_addr, SEGV_MAPERR, trap 14; handler skips the load");
    struct ksa dfl = { 0, 0, 0, 0 };
    sc4(13, 11, &dfl, 0, 8);

    /* sigaltstack + SA_ONSTACK */
    alt_lo = mmap_anon(64 * 1024, 3); alt_hi = alt_lo + 64 * 1024;
    struct { void *sp; int flags, pad; u64 size; } ss = { alt_lo, 0, 0, 64 * 1024 };
    sc2(131, &ss, 0);
    sigact(10, h_info, SA_ONSTACK);
    h_alt = 0;
    sc3(234, pid, me, 10);
    CHECK(h_alt, "SA_ONSTACK: the handler runs on the sigaltstack");
    ss.flags = 2;   /* SS_DISABLE */
    sc2(131, &ss, 0);
    alt_lo = 0;
    sc4(13, 10, &dfl, 0, 8);
    sc4(13, 12, &dfl, 0, 8);
}

/* ---- swap: touch more than the commit budget (the suite runs with 1 GiB) -- */
static void test_swap(void)
{
    out("swap:\n");
    const u64 len = 1280ull << 20, pages = len >> 12;
    u8 *m = mmap_anon(len, 3);
    CHECK((long)m > 0, "mmap 1.25 GiB (more than the 1 GiB budget)");
    if ((long)m <= 0) return;
    for (u64 i = 0; i < pages; i++) { *(u64 *)(m + (i << 12)) = i ^ 0x5157415055ull; *(u64 *)(m + (i << 12) + 4088) = ~i; }
    CHECK(1, "every page written (evictions made room)");
    u64 bad = 0;
    for (u64 i = 0; i < pages; i++)
        if (*(u64 *)(m + (i << 12)) != (i ^ 0x5157415055ull) || *(u64 *)(m + (i << 12) + 4088) != ~i) bad++;
    CHECK(bad == 0, "every page reads back its own data (swapped in on touch)");
    /* the kernel reads a buffer that was swapped out: write() it to a file */
    long fd = sc4(257, -100, "/tmp/xrl_swap_test", 0102 | 01000, 0644);   /* O_RDWR|O_CREAT|O_TRUNC */
    for (u64 i = 0; i < pages; i++) m[(i << 12) + 8] = (u8)i;   /* touch all again: the first MiB goes out */
    long w = fd >= 0 ? sc3(1, fd, m, 1 << 20) : -1;
    static u8 rb[1 << 20];
    long rr = fd >= 0 ? sc4(17, fd, rb, sizeof rb, 0) : -1;
    int same = rr == (1 << 20) && !memcmp(rb, m, 1 << 20);
    CHECK(w == (1 << 20) && same, "write() from swapped-out memory writes the right bytes");
    if (fd >= 0) { sc1(3, fd); sc1(87, "/tmp/xrl_swap_test"); }
    /* read back (clean), written again (dirty), evicted again: nothing lost */
    bad = 0;
    for (u64 i = 0; i < pages; i++)
        if (*(u64 *)(m + (i << 12)) != (i ^ 0x5157415055ull) || m[(i << 12) + 8] != (u8)i || *(u64 *)(m + (i << 12) + 4088) != ~i) bad++;
    CHECK(bad == 0, "pages written after a read-back keep the new data through another eviction");
    /* mprotect over partly swapped-out memory: read-only, then writable again */
    long pr = sc3(10, m, len, 1);
    bad = 0;
    for (u64 i = 0; i < pages; i += 7) if (*(u64 *)(m + (i << 12) + 4088) != ~i) bad++;
    long pw = sc3(10, m, len, 3);
    for (u64 i = 0; i < pages; i += 7) m[(i << 12) + 9] = 0x5a;
    for (u64 i = 0; i < pages; i += 7) if (m[(i << 12) + 9] != 0x5a || *(u64 *)(m + (i << 12)) != (i ^ 0x5157415055ull)) bad++;
    CHECK(pr == 0 && pw == 0 && bad == 0, "mprotect read-only and back over swapped memory keeps the data");
    /* unmapped while swapped out: a new mapping there is zero */
    sc2(11, m, len);
    u8 *m2 = (u8 *)sc(9, m, 1 << 20, 3, 0x22 | 0x100000 /*FIXED_NOREPLACE*/, -1, 0);
    CHECK(m2 == m && *(u64 *)m2 == 0 && *(u64 *)(m2 + 4096) == 0, "after munmap the address range is fresh zero memory");
    if ((long)m2 > 0) sc2(11, m2, 1 << 20);
}

/* /proc/cpuinfo must agree with what CPUID says about the CPU */
static const char *find(const char *h, const char *n)
{
    unsigned long l = slen(n);
    for (; *h; h++) if (!memcmp(h, n, l)) return h;
    return 0;
}
static int has_flag(const char *flags, const char *f)   /* whole word on the first flags line */
{
    char w[40]; unsigned long l = slen(f);
    w[0] = ' '; memcpy(w + 1, f, l); w[l + 1] = 0;
    for (const char *p = flags; *p && *p != '\n'; p++) {
        if (!memcmp(p, w, l + 1) && (p[l + 1] == ' ' || p[l + 1] == '\n')) return 1;
    }
    return 0;
}
static void cpuid(u32 leaf, u32 sub, u32 r[4])
{
    __asm__ volatile("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(leaf), "c"(sub));
}
static char cpubuf[1 << 16];
static void test_cpuinfo(void)
{
    long fd = sc3(2, "/proc/cpuinfo", 0, 0), n = 0, r;
    while (fd >= 0 && n < (long)sizeof cpubuf - 1 && (r = sc3(0, fd, cpubuf + n, sizeof cpubuf - 1 - n)) > 0) n += r;
    if (fd >= 0) sc1(3, fd);
    cpubuf[n > 0 ? n : 0] = 0;
    CHECK(n > 0, "read /proc/cpuinfo");
    u32 v[4], l1[4], l7[4];
    cpuid(0, 0, v);
    char vendor[13]; memcpy(vendor, &v[1], 4); memcpy(vendor + 4, &v[3], 4); memcpy(vendor + 8, &v[2], 4); vendor[12] = 0;
    const char *vl = find(cpubuf, "vendor_id\t: ");
    CHECK(vl && !memcmp(vl + 12, vendor, 12), "cpuinfo vendor_id = CPUID vendor");
    cpuid(1, 0, l1);
    cpuid(7, 0, l7);
    u32 fam = (l1[0] >> 8) & 15; if (fam == 15) fam += (l1[0] >> 20) & 255;
    const char *fl = find(cpubuf, "cpu family\t: ");
    long cf = 0; if (fl) for (const char *p = fl + 13; *p >= '0' && *p <= '9'; p++) cf = cf * 10 + (*p - '0');
    CHECK(fl && cf == (long)fam, "cpuinfo cpu family = CPUID family");
    const char *flags = find(cpubuf, "flags\t\t: ");
    CHECK(flags != 0, "cpuinfo has a flags line");
    if (!flags) return;
    flags += 8;   /* the space before the first flag stays: has_flag matches " name" */
    CHECK(has_flag(flags, "sse2") == (int)((l1[3] >> 26) & 1), "flag sse2 matches CPUID");
    CHECK(has_flag(flags, "fsgsbase") == (int)(l7[1] & 1), "flag fsgsbase matches CPUID (hidden on the console)");
    CHECK(has_flag(flags, "hypervisor") == (int)(l1[2] >> 31), "flag hypervisor matches CPUID");
    CHECK(has_flag(flags, "rdrand") == (int)((l1[2] >> 30) & 1), "flag rdrand matches CPUID");
    long procs = 0;
    for (const char *p = cpubuf; (p = find(p, "processor\t: ")); p++) procs++;
    u64 mask[16]; memset(mask, 0, sizeof mask);
    long ab = sc3(204, 0, sizeof mask, mask), bits = 0;   /* sched_getaffinity */
    for (long i = 0; ab > 0 && i < ab / 8; i++) for (u64 m = mask[i]; m; m &= m - 1) bits++;
    CHECK(procs > 0 && procs == bits, "one cpuinfo entry per CPU in the affinity mask");
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
    {   /* naming a thread through its comm file (glibc/musl pthread_setname_np; Wine's SetThreadDescription) */
        char cp[64] = "/proc/self/task/", nm[17];
        long tid = sc3(186, 0, 0, 0), t = tid, k = 16;
        char d[12]; int nd = 0;
        do d[nd++] = (char)('0' + t % 10); while ((t /= 10));
        while (nd) cp[k++] = d[--nd];
        memcpy(cp + k, "/comm", 6);
        long fc = sc3(2, cp, 1, 0);
        long w = fc >= 0 ? sc3(1, fc, "xrl-named\n", 10) : fc;
        memset(nm, 0, sizeof nm);
        long g = sc3(157, 16 /* PR_GET_NAME */, nm, 0);
        CHECK(w == 10 && g == 0 && !memcmp(nm, "xrl-named", 10), "write /proc/self/task/<tid>/comm names the thread");
    }
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

/* rz_probe(page): fills the x86-64 red zone (the 128 bytes below rsp, which a leaf function may use
 * without moving rsp) with a pattern, writes one byte to `page`, and returns how many of the 16
 * qwords changed. On Linux a page fault there (lazy commit, swap-in) is handled by the kernel and the
 * user stack is never written; on the layer the fault is a Windows exception on this thread. */
__asm__(".text\n.globl rz_probe\nrz_probe:\n"
        "  movabs $0x5a5a5a5a5a5a5a00, %rdx\n"
        "  mov $1, %rcx\n"
        "1:\n  lea (%rdx,%rcx), %rax\n  mov %rcx, %r8\n  neg %r8\n  mov %rax, (%rsp,%r8,8)\n"
        "  inc %rcx\n  cmp $16, %rcx\n  jbe 1b\n"
        "  movb $1, (%rdi)\n"
        "  xor %eax, %eax\n  mov $1, %rcx\n"
        "2:\n  lea (%rdx,%rcx), %r9\n  mov %rcx, %r8\n  neg %r8\n  cmp %r9, (%rsp,%r8,8)\n  setne %r10b\n"
        "  movzbl %r10b, %r10d\n  add %r10, %rax\n  inc %rcx\n  cmp $16, %rcx\n  jbe 2b\n"
        "  ret\n");
long rz_probe(volatile u8 *page);

static void test_redzone(void)
{
    /* large enough to be committed lazily, so each first touch is a fault the layer handles */
    u8 *lz = mmap_anon(8 << 20, 3);
    CHECK((long)lz > 0x10000, "red zone: an 8 MiB lazily committed mapping");
    if ((long)lz <= 0x10000) return;
    long bad = 0;
    for (int i = 0; i < 64; i++) bad += rz_probe(lz + (u64)i * 65536 + 123);
    CHECK(bad == 0, "the 128-byte red zone below rsp survives first-touch faults (64 faults, as on Linux)");
    if (bad) outn("  red-zone qwords changed", bad);
    sc2(11, lz, 8 << 20);
}

static void test_mem(void)
{
    out("memory:\n");
    test_redzone();
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
    /* commit is lazy (Linux overcommit): only MAP_POPULATE commits at mmap time */
    long big = sc(9, 0, budget + (1ull << 30), 3, 0x22 | 0x8000, -1, 0);
    CHECK(big == -12, "mmap(MAP_POPULATE) beyond guest_mem_budget -> -ENOMEM");
    long lz = sc(9, 0, 64ull << 20, 3, 0x22, -1, 0);
    CHECK(lz > 0, "64 MiB anonymous mmap (lazy)");
    if (lz > 0) {
        volatile char *pz = (volatile char *)lz;
        pz[0] = 1; pz[(64 << 20) - 1] = 2;
        CHECK(pz[0] == 1 && pz[(64 << 20) - 1] == 2 && pz[32 << 20] == 0, "lazy pages commit on first touch, zero-filled");
        CHECK(sc2(11, lz, 64ull << 20) == 0, "munmap lazy mapping");
    }
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
    {   /* GET_VCLOCK: a vDSO-style clock_gettime called directly (no syscall) */
        long f = sc1(0x10005, 0);
        long ts[2] = { -1, -1 };
        int r = f > 0 ? ((int (*)(int, long *))f)(1 /* CLOCK_MONOTONIC */, ts) : -1;
        CHECK(f > 0 && r == 0 && ts[0] >= 0 && ts[1] >= 0 && ts[1] < 1000000000, "pseudo-syscall GET_VCLOCK: monotonic clock without a syscall");
        CHECK(f > 0 && ((int (*)(int, long *))f)(2 /* CLOCK_PROCESS_CPUTIME_ID */, ts) == -38, "GET_VCLOCK leaves CPU-time clocks to the syscall (-ENOSYS)");
    }
    CHECK(sc1(0x10006, 0) == -38, "pseudo-syscall out of range -> -ENOSYS");

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

/* ---- shared TEB: guest view vs host view (docs/gs-teb.md) ------------------ */
static u64 gs_read(u64 off) { u64 v; __asm__ volatile("mov %%gs:(%1), %0" : "=r"(v) : "r"(off)); return v; }
static void gs_write(u64 off, u64 v) { __asm__ volatile("mov %0, %%gs:(%1)" : : "r"(v), "r"(off) : "memory"); }

static void test_teb(void)
{
    out("shared TEB (guest/host views):\n");
    /* Garbage in fields host code depends on: TLS array, PEB, LastError, FlsData.
     * Any host code (Win32, the CRT, the log path) that ran with these live would
     * crash, so passing = every transition swapped the host view in. */
    static const u64 offs[4] = { 0x58, 0x60, 0x68, 0x17c8 };
    u64 saved[4];
    for (int i = 0; i < 4; i++) saved[i] = gs_read(offs[i]);
    for (int i = 0; i < 4; i++) gs_write(offs[i], 0x5ee70000ull + i);

    long fd = sc4(257, -100, "/tmp/teb-test.txt", 0102, 0644);
    long w = fd >= 0 ? sc3(1, fd, "teb", 3) : -1;
    long l = fd >= 0 ? sc3(8, fd, 0, 0) : -1;
    char b[4] = {0};
    long rd = fd >= 0 ? sc3(0, fd, b, 3) : -1;
    if (fd >= 0) sc1(3, fd);
    CHECK(fd >= 0 && w == 3 && l == 0 && rd == 3 && !memcmp(b, "teb", 3), "file syscalls run host Win32 code under a foreign guest TEB");
    void *m = mmap_anon(1 << 20, 3);
    CHECK((long)m > 0, "mmap under a foreign guest TEB");
    struct gctx3 { u32 sz, magic, abi, flags; u64 sysc, tlsoff; void (*log)(int, const char *, const char *); } *c = (void *)xctx;
    c->log(1, "guest", "downcall (host CRT + logging) under a foreign guest TEB");
    out("  ok   downcall through the op-table stub under a foreign guest TEB\n");

    int same = 1;
    for (int i = 0; i < 4; i++) if (gs_read(offs[i]) != 0x5ee70000ull + i) same = 0;
    CHECK(same, "guest TEB values survive syscalls and downcalls");
    for (int i = 0; i < 4; i++) gs_write(offs[i], saved[i]);
}

/* ---- real sockets: AF_INET / AF_INET6 over Winsock (layer/src/net.c) ------- */
struct sa4 { u16 fam, port; u32 addr; u8 z[8]; };
struct sa6 { u16 fam, port; u32 flow; u8 addr[16]; u32 scope; };
struct pfd { int fd; short ev, rev; };
struct iov_ { u64 base, len; };
struct msg_ { u64 name; u32 nl, p0; u64 iov, iovlen, ctl, ctllen; u32 fl, p1; };

#define S_socket 41
#define S_connect 42
#define S_accept 43
#define S_sendto 44
#define S_recvfrom 45
#define S_sendmsg 46
#define S_recvmsg 47
#define S_shutdown 48
#define S_bind 49
#define S_listen 50
#define S_getsockname 51
#define S_getpeername 52
#define S_setsockopt 54
#define S_getsockopt 55
#define SOCK_NB 04000

static void sa4_loop(struct sa4 *s, u16 port_be) { memset(s, 0, sizeof *s); s->fam = 2; s->port = port_be; s->addr = 0x0100007f; }

/* a listening TCP socket on 127.0.0.1:<ephemeral>; returns fd, port (network order) in *port */
static long listen_loop(u16 *port, int type_flags)
{
    long ls = sc3(S_socket, 2, 1 | type_flags, 0);
    struct sa4 a; sa4_loop(&a, 0);
    if (ls < 0 || sc3(S_bind, ls, &a, 16) != 0 || sc2(S_listen, ls, 8) != 0) return -1;
    struct sa4 b; u32 bl = 16; memset(&b, 0, sizeof b);
    if (sc3(S_getsockname, ls, &b, &bl) != 0 || bl != 16 || b.fam != 2) return -1;
    *port = b.port;
    return ls;
}

static long wait_poll(long fd, short ev, int ms, short *rev)
{
    struct pfd p = { (int)fd, ev, 0 };
    long r = sc3(7, &p, 1, ms);
    if (rev) *rev = p.rev;
    return r;
}

static volatile int nt_done;
static u16 nt_port;
static volatile long nt_fd;

static long spawn_thread(int (*fn)(void *), void *arg, volatile int *ctid)
{
    long flags = 0x100 | 0x200 | 0x400 | 0x800 | 0x10000 | 0x40000 | 0x80000 | 0x200000 | 0x100000;
    u8 *stk = mmap_anon(256 * 1024, 3);
    struct tls *t = mmap_anon(4096, 3); t->self = t;
    *ctid = -1;
    return xclone(fn, stk + 256 * 1024 - 64, flags, arg, 0, t, (void *)ctid);
}
static void nap_ms(long ms) { struct { long s, n; } ts = { ms / 1000, (ms % 1000) * 1000000 }; sc4(35, &ts, 0, 0, 0); }

/* connects to 127.0.0.1:nt_port after a pause, then writes a greeting */
static int net_client_thr(void *arg)
{
    (void)arg;
    nap_ms(60);
    long c = sc3(S_socket, 2, 1, 0);
    struct sa4 a; sa4_loop(&a, nt_port);
    long r = sc3(S_connect, c, &a, 16);          /* blocking connect */
    if (r == 0) sc3(1, c, "late-hello", 10);
    nap_ms(60);
    sc1(3, c);
    nt_done = r == 0 ? 1 : -1;
    return 0;
}

/* streams NT_TOTAL bytes of a known pattern on nt_fd (blocking sends), then closes its side */
#define NT_TOTAL (3u << 20)
#define PAT(i) ((u8)((i) * 31 + ((i) >> 8)))
static int net_sender_thr(void *arg)
{
    (void)arg;
    static u8 buf[40000];
    u32 sent = 0;
    while (sent < NT_TOTAL) {
        u32 n = NT_TOTAL - sent < sizeof buf ? NT_TOTAL - sent : (u32)sizeof buf;
        for (u32 i = 0; i < n; i++) buf[i] = PAT(sent + i);
        long r = sc3(1, nt_fd, buf, n);
        if (r <= 0) { nt_done = -1; return 1; }
        sent += (u32)r;                          /* a short write: the next lap regenerates from `sent` */
    }
    sc2(S_shutdown, nt_fd, 1);
    nt_done = 1;
    return 0;
}

static void test_net(void)
{
    out("network (Winsock):\n");
    u16 port;
    long ls = listen_loop(&port, 0);
    CHECK(ls >= 3 && port != 0, "TCP: socket+bind(127.0.0.1:0)+listen+getsockname");
    if (ls < 3) { out("  skip  rest of the network tests\n"); return; }

    long cs = sc3(S_socket, 2, 1 | SOCK_NB, 0);
    struct sa4 a; sa4_loop(&a, port);
    long cr = sc3(S_connect, cs, &a, 16);
    CHECK(cr == -115 || cr == 0, "non-blocking connect -> -EINPROGRESS (or 0)");
    short rev = 0;
    CHECK(wait_poll(cs, 4 /*POLLOUT*/, 3000, &rev) == 1 && (rev & 4), "poll(POLLOUT) reports the connect complete");
    int soerr = -1; u32 sl = 4;
    CHECK(sc(S_getsockopt, cs, 1, 4 /*SO_ERROR*/, &soerr, &sl, 0) == 0 && soerr == 0, "getsockopt(SO_ERROR) == 0 after connect");
    struct sa4 peer; u32 pl = 16; memset(&peer, 0, sizeof peer);
    long as = sc3(S_accept, ls, &peer, &pl);
    CHECK(as >= 3 && pl == 16 && peer.fam == 2 && peer.addr == 0x0100007f && peer.port != 0, "accept returns the client's address");
    struct sa4 cn, pn; u32 l1 = 16, l2 = 16;
    CHECK(sc3(S_getsockname, cs, &cn, &l1) == 0 && sc3(S_getpeername, as, &pn, &l2) == 0 && cn.port == pn.port && cn.addr == pn.addr, "getsockname(client) == getpeername(server)");
    CHECK(sc3(1, cs, "hello", 5) == 5, "write() on a socket sends");
    char buf[64]; memset(buf, 0, sizeof buf);
    CHECK(sc3(0, as, buf, 64) == 5 && !memcmp(buf, "hello", 5), "read() on a socket (blocking) receives");
    CHECK(sc(S_sendto, as, "world!", 6, 0, 0, 0) == 6, "sendto(NULL addr) on a connected socket");
    CHECK(wait_poll(cs, 1 /*POLLIN*/, 3000, &rev) == 1 && (rev & 1), "poll(POLLIN) wakes when data arrives");
    memset(buf, 0, sizeof buf);
    CHECK(sc(S_recvfrom, cs, buf, 64, 0, 0, 0) == 6 && !memcmp(buf, "world!", 6), "recvfrom on the non-blocking client");
    CHECK(sc(S_recvfrom, cs, buf, 64, 0, 0, 0) == -11, "recv on an empty non-blocking socket -> -EAGAIN");
    u64 st[18]; memset(st, 0, sizeof st);
    CHECK(sc2(5, cs, st) == 0 && (st[3] & 0170000) == 0140000, "fstat: S_ISSOCK");

    /* vectored I/O on a socket */
    struct iov_ wv[2] = { { (u64)"ab", 2 }, { (u64)"cde", 3 } };
    CHECK(sc3(20, cs, wv, 2) == 5, "writev on a socket");
    char r1[2], r2[8]; struct iov_ rv[2] = { { (u64)r1, 2 }, { (u64)r2, 8 } };
    long rn = sc3(19, as, rv, 2);
    CHECK(rn == 5 && !memcmp(r1, "ab", 2) && !memcmp(r2, "cde", 3), "readv on a socket");

    /* half close */
    CHECK(sc2(S_shutdown, cs, 1) == 0, "shutdown(SHUT_WR)");
    CHECK(sc3(0, as, buf, 64) == 0, "peer reads EOF (0) after shutdown(SHUT_WR)");
    CHECK(wait_poll(as, 1, 1000, &rev) == 1 && (rev & 1), "poll(POLLIN) is true at EOF");
    CHECK(sc3(1, as, "still", 5) == 5 && wait_poll(cs, 1, 3000, 0) == 1 && sc(S_recvfrom, cs, buf, 64, 0, 0, 0) == 5, "the other direction still works");

    /* dup keeps the socket alive */
    long d = sc1(32, as);
    sc1(3, as);
    CHECK(d >= 3 && sc3(1, d, "x", 1) == 1, "dup'ed socket survives closing the original");
    sc1(3, d); sc1(3, cs);

    /* options */
    long os = sc3(S_socket, 2, 1, 0);
    int one = 1, got = 0; u32 gl = 4;
    CHECK(sc(S_setsockopt, os, 1, 2 /*SO_REUSEADDR*/, &one, 4, 0) == 0 && sc(S_getsockopt, os, 1, 2, &got, &gl, 0) == 0 && got == 1, "SO_REUSEADDR round trip");
    got = 0; gl = 4;
    CHECK(sc(S_setsockopt, os, 6 /*IPPROTO_TCP*/, 1 /*TCP_NODELAY*/, &one, 4, 0) == 0 && sc(S_getsockopt, os, 6, 1, &got, &gl, 0) == 0 && got != 0, "TCP_NODELAY round trip");
    got = 0; gl = 4;
    CHECK(sc(S_getsockopt, os, 1, 3 /*SO_TYPE*/, &got, &gl, 0) == 0 && got == 1, "SO_TYPE == SOCK_STREAM");
    got = 0; gl = 4;
    CHECK(sc(S_getsockopt, os, 1, 8 /*SO_RCVBUF*/, &got, &gl, 0) == 0 && got > 0, "SO_RCVBUF > 0");
    CHECK(sc(S_setsockopt, os, 1, 999, &one, 4, 0) == -92, "unknown option -> -ENOPROTOOPT");
    sc1(3, os);

    /* blocking accept + blocking connect/recv across threads */
    nt_port = port; nt_done = 0;
    volatile int ct = -1;
    long th = spawn_thread(net_client_thr, 0, &ct);
    CHECK(th > 0, "client thread started");
    u64 t0 = mono_ns();
    pl = 16;
    long as2 = sc3(S_accept, ls, &peer, &pl);          /* blocks ~60 ms until the thread connects */
    u64 dt = mono_ns() - t0;
    CHECK(as2 >= 3 && dt >= 40000000ull && dt < 4000000000ull, "blocking accept waits for the connection");
    memset(buf, 0, sizeof buf);
    long n2 = sc3(0, as2, buf, 64);                    /* blocks until the greeting */
    CHECK(n2 == 10 && !memcmp(buf, "late-hello", 10), "blocking read gets the greeting");
    CHECK(sc3(0, as2, buf, 64) == 0, "blocking read returns 0 when the peer closes");
    wait_eq(&ct, 0);
    CHECK(nt_done == 1, "client thread connected and exited");
    sc1(3, as2);

    /* poll timeout */
    long ps = sc3(S_socket, 2, 1 | SOCK_NB, 0);
    sa4_loop(&a, port);
    sc3(S_connect, ps, &a, 16);
    wait_poll(ps, 4, 3000, 0);
    long pa = sc3(S_accept, ls, 0, 0);
    t0 = mono_ns();
    long pr = wait_poll(ps, 1, 80, 0);
    dt = mono_ns() - t0;
    CHECK(pr == 0 && dt >= 60000000ull && dt < 2000000000ull, "poll(POLLIN, 80 ms) on an idle socket times out");
    sc1(3, ps); sc1(3, pa);

    /* epoll over listener + sockets */
    long ep = sc1(291, 0);
    struct { u32 e; u64 d; } __attribute__((packed)) ev = { 1, 0x7001 }, oev[4];
    CHECK(sc4(233, ep, 1, ls, &ev) == 0, "epoll_ctl(ADD, listening socket)");
    long ec = sc3(S_socket, 2, 1 | SOCK_NB, 0);
    sa4_loop(&a, port);
    sc3(S_connect, ec, &a, 16);
    CHECK(sc4(232, ep, oev, 4, 3000) == 1 && oev[0].d == 0x7001, "epoll_wait: the listener is readable when a connection is pending");
    long ea = sc3(S_accept, ls, 0, 0);
    ev.d = 0x7002; ev.e = 1;
    CHECK(sc4(233, ep, 1, ea, &ev) == 0, "epoll_ctl(ADD, accepted socket)");
    sc3(1, ec, "ping", 4);
    CHECK(sc4(232, ep, oev, 4, 3000) >= 1 && oev[0].d == 0x7002, "epoll_wait: the accepted socket is readable after the peer sends");
    sc1(3, ea); sc1(3, ec); sc1(3, ep);

    /* bulk transfer with back-pressure: one thread sends 3 MiB blocking, we read blocking */
    long bs = sc3(S_socket, 2, 1, 0);
    sa4_loop(&a, port);
    CHECK(sc3(S_connect, bs, &a, 16) == 0, "blocking connect");
    nt_fd = sc3(S_accept, ls, 0, 0);
    nt_done = 0;
    volatile int bt = -1;
    long bth = spawn_thread(net_sender_thr, 0, &bt);
    CHECK(bth > 0, "sender thread started");
    static u8 big[50000];
    u32 total = 0; int pat_ok = 1, eof = 0;
    for (;;) {
        long r = sc3(0, bs, big, sizeof big);
        if (r <= 0) { eof = r == 0; break; }
        for (long i = 0; i < r; i++) if (big[i] != PAT(total + i)) { pat_ok = 0; break; }
        total += (u32)r;
    }
    wait_eq(&bt, 0);
    outn("bulk bytes received", total);
    CHECK(eof && total == NT_TOTAL && pat_ok && nt_done == 1, "3 MiB arrived in order and ended with EOF through blocking send/recv");
    sc1(3, bs); sc1(3, nt_fd);

    /* connecting to 0.0.0.0 reaches this host, as on Linux */
    long zs = sc3(S_socket, 2, 1, 0);
    struct sa4 z; sa4_loop(&z, port); z.addr = 0;
    CHECK(sc3(S_connect, zs, &z, 16) == 0, "connect(0.0.0.0:port) reaches the loopback listener (Linux behaviour)");
    sc1(3, zs);
    sc1(3, sc3(S_accept, ls, 0, 0));

    /* connection refused */
    u16 dead;
    long tmp = listen_loop(&dead, 0);
    sc1(3, tmp);                                           /* nobody listens on `dead` now */
    long rs = sc3(S_socket, 2, 1 | SOCK_NB, 0);
    sa4_loop(&a, dead);
    t0 = mono_ns();
    long rr = sc3(S_connect, rs, &a, 16);
    if (rr == -115) {
        wait_poll(rs, 4, 6000, &rev);
        soerr = 0; sl = 4;
        sc(S_getsockopt, rs, 1, 4, &soerr, &sl, 0);
        rr = -soerr;
    }
    outn("connection-refused latency ms", (long)((mono_ns() - t0) / 1000000));
    CHECK(rr == -111, "connect to a closed port -> ECONNREFUSED (poll error + SO_ERROR)");
    sc1(3, rs);

    /* a blocking socket with SO_RCVTIMEO */
    long ts_ = sc3(S_socket, 2, 1, 0);
    sa4_loop(&a, port);
    sc3(S_connect, ts_, &a, 16);
    long tsa = sc3(S_accept, ls, 0, 0);
    long tv[2] = { 0, 70000 };
    CHECK(sc(S_setsockopt, ts_, 1, 20 /*SO_RCVTIMEO*/, tv, 16, 0) == 0, "setsockopt(SO_RCVTIMEO)");
    t0 = mono_ns();
    long tr = sc(S_recvfrom, ts_, buf, 8, 0, 0, 0);
    dt = mono_ns() - t0;
    CHECK(tr == -11 && dt >= 50000000ull && dt < 3000000000ull, "blocking recv honours SO_RCVTIMEO (-EAGAIN after ~70 ms)");
    sc1(3, ts_); sc1(3, tsa);

    /* UDP */
    long u1 = sc3(S_socket, 2, 2, 0), u2 = sc3(S_socket, 2, 2, 0);
    struct sa4 ua; sa4_loop(&ua, 0);
    CHECK(u1 >= 3 && sc3(S_bind, u1, &ua, 16) == 0, "UDP: socket+bind");
    struct sa4 ub; u32 ubl = 16;
    CHECK(sc3(S_getsockname, u1, &ub, &ubl) == 0 && ub.port != 0, "UDP: getsockname");
    CHECK(sc(S_sendto, u2, "dgram-1", 7, 0, &ub, 16) == 7, "UDP: sendto");
    struct sa4 from; u32 fl = 16; memset(&from, 0, sizeof from);
    memset(buf, 0, sizeof buf);
    CHECK(wait_poll(u1, 1, 3000, 0) == 1, "UDP: poll(POLLIN) after a datagram");
    CHECK(sc(S_recvfrom, u1, buf, 64, 2 /*MSG_PEEK*/, 0, 0) == 7, "UDP: MSG_PEEK leaves the datagram queued");
    long ur = sc(S_recvfrom, u1, buf, 64, 0, &from, &fl);
    struct sa4 u2n; u32 u2l = 16;
    sc3(S_getsockname, u2, &u2n, &u2l);
    CHECK(ur == 7 && !memcmp(buf, "dgram-1", 7) && fl == 16 && from.fam == 2 && from.port == u2n.port, "UDP: recvfrom returns the data and the sender's address");
    struct iov_ siov[2] = { { (u64)"two-", 4 }, { (u64)"parts", 5 } };
    struct msg_ sm = { (u64)&ub, 16, 0, (u64)siov, 2, 0, 0, 0, 0 };
    CHECK(sc3(S_sendmsg, u2, &sm, 0) == 9, "UDP: sendmsg gathers two iovecs into one datagram");
    wait_poll(u1, 1, 3000, 0);
    char small[4]; struct iov_ riov = { (u64)small, 4 };
    struct sa4 rn_; memset(&rn_, 0, sizeof rn_);
    struct msg_ rm = { (u64)&rn_, 16, 0, (u64)&riov, 1, 0, 0, 0, 0 };
    long mr = sc3(S_recvmsg, u1, &rm, 0);
    CHECK(mr == 4 && !memcmp(small, "two-", 4) && (rm.fl & 0x20 /*MSG_TRUNC*/) && rm.nl == 16 && rn_.port == u2n.port, "UDP: recvmsg truncates with MSG_TRUNC and reports the sender");
    CHECK(sc3(S_connect, u2, &ub, 16) == 0 && sc3(1, u2, "conn", 4) == 4, "UDP: connect + write");
    wait_poll(u1, 1, 3000, 0);
    CHECK(sc(S_recvfrom, u1, buf, 64, 0, 0, 0) == 4, "UDP: the connected sender's datagram arrives");
    got = 0; gl = 4;
    CHECK(sc(S_getsockopt, u1, 1, 3, &got, &gl, 0) == 0 && got == 2, "SO_TYPE == SOCK_DGRAM");
    sc1(3, u1); sc1(3, u2);

    /* IPv6 loopback, if the host has it */
    long s6 = sc3(S_socket, 10, 1, 0);
    if (s6 < 0) outn("  skip  IPv6 (socket(AF_INET6) failed with)", s6);
    else {
        struct sa6 a6; memset(&a6, 0, sizeof a6); a6.fam = 10; a6.addr[15] = 1;
        long br = sc3(S_bind, s6, &a6, 28);
        sc2(S_listen, s6, 4);
        struct sa6 b6; u32 b6l = 28; memset(&b6, 0, sizeof b6);
        long gn = sc3(S_getsockname, s6, &b6, &b6l);
        CHECK(br == 0 && gn == 0 && b6l == 28 && b6.fam == 10 && b6.port != 0 && b6.addr[15] == 1, "IPv6: bind(::1) + listen + getsockname");
        long c6 = sc3(S_socket, 10, 1, 0);
        a6.port = b6.port;
        CHECK(sc3(S_connect, c6, &a6, 28) == 0, "IPv6: connect(::1)");
        long a6s = sc3(S_accept, s6, 0, 0);
        CHECK(a6s >= 3 && sc3(1, c6, "v6", 2) == 2 && sc3(0, a6s, buf, 8) == 2, "IPv6: data both ways");
        sc1(3, c6); sc1(3, a6s); sc1(3, s6);
    }

    /* what is not a network socket */
    int pf2[2]; sc2(293, pf2, 0);
    CHECK(sc3(S_bind, pf2[0], &a, 16) == -88, "bind on a pipe -> -ENOTSOCK");
    CHECK(sc3(S_socket, 1, 7, 0) == -94 && sc3(S_socket, 1, 1, 5) == -93, "socket(AF_UNIX): bad type -> -ESOCKTNOSUPPORT, bad protocol -> -EPROTONOSUPPORT");
    CHECK(sc3(S_socket, 2, 3, 0) == -1, "socket(AF_INET, SOCK_RAW) -> -EPERM");
    sc1(3, ls);
}

/* ---- AF_UNIX sockets (layer/src/unix.c) ------------------------------------ */
struct sun { u16 fam; char p[108]; };
struct ucred_ { int pid; u32 uid, gid; };
struct cm_fd { u64 len; int lvl, type; int fd; int pad; };
struct cm_cred { u64 len; int lvl, type; struct ucred_ c; int pad; };

/* a path address (len counts the NUL) or, with a leading '@', an abstract name */
static u32 mkun(struct sun *a, const char *name)
{
    memset(a, 0, sizeof *a);
    a->fam = 1;
    u32 n = (u32)slen(name);
    if (name[0] == '@') { memcpy(a->p + 1, name + 1, n - 1); return 2 + n; }
    memcpy(a->p, name, n);
    return 2 + n + 1;
}

static volatile int un_cdone;
static struct sun un_addr; static u32 un_alen;

static int un_client_thr(void *arg)   /* blocking connect, then a greeting and the answer */
{
    (void)arg;
    nap_ms(60);
    long c = sc3(S_socket, 1, 1, 0);
    long r = sc3(S_connect, c, &un_addr, un_alen);
    char b[16]; memset(b, 0, sizeof b);
    if (r == 0) { sc3(1, c, "hello-unix", 10); r = sc3(0, c, b, 16) == 4 && !memcmp(b, "pong", 4) ? 0 : -1; }
    sc1(3, c);
    un_cdone = r == 0 ? 1 : -1;
    return 0;
}

#define UN_TOTAL (2u << 20)
static int un_sender_thr(void *arg)   /* UN_TOTAL bytes of a known pattern; the queue is far smaller, so this blocks */
{
    long fd = (long)arg;
    static u8 buf[30000];
    u32 sent = 0;
    while (sent < UN_TOTAL) {
        u32 n = UN_TOTAL - sent < sizeof buf ? UN_TOTAL - sent : (u32)sizeof buf;
        for (u32 i = 0; i < n; i++) buf[i] = PAT(sent + i);
        long r = sc3(1, fd, buf, n);
        if (r <= 0) { un_cdone = -1; return 1; }
        sent += (u32)r;
    }
    sc2(S_shutdown, fd, 1);
    un_cdone = 1;
    return 0;
}

static long sendm(long fd, struct msg_ *m, int fl) { return sc3(S_sendmsg, fd, m, fl); }
static long recvm(long fd, struct msg_ *m, int fl) { return sc3(S_recvmsg, fd, m, fl); }

static void test_unix(void)
{
    out("AF_UNIX sockets:\n");
    char buf[256]; long r;
    struct sun a, b; u32 al, bl;
    int sp[2];
    long me = sc(39, 0, 0, 0, 0, 0, 0);

    /* stream socketpair */
    CHECK(sc4(53, 1, 1, 0, sp) == 0, "socketpair(SOCK_STREAM)");
    CHECK(sc3(1, sp[0], "abc", 3) == 3 && sc3(1, sp[0], "def", 3) == 3, "stream: two writes");
    memset(buf, 0, sizeof buf);
    CHECK(sc3(0, sp[1], buf, 100) == 6 && !memcmp(buf, "abcdef", 6), "stream: one read returns both (bytes merge)");
    struct ucred_ pc = { -1, 0, 0 }; u32 pl = 12;
    CHECK(sc(S_getsockopt, sp[0], 1, 17 /*SO_PEERCRED*/, &pc, &pl, 0) == 0 && pl == 12 && pc.pid == me && pc.uid == 1000, "SO_PEERCRED: the creator (pid, uid)");
    int ty = 0; pl = 4;
    CHECK(sc(S_getsockopt, sp[0], 1, 3 /*SO_TYPE*/, &ty, &pl, 0) == 0 && ty == 1, "SO_TYPE == SOCK_STREAM");
    struct pfd pp = { sp[1], 1 | 4, 0 };
    CHECK(sc3(7, &pp, 1, 0) == 1 && pp.rev == 4, "poll: writable, nothing to read");
    u64 st[18]; memset(st, 0, sizeof st);
    CHECK(sc2(5, sp[0], st) == 0 && (st[3] & 0170000) == 0140000, "fstat: S_ISSOCK");
    sc1(3, sp[0]);
    pp.rev = 0;
    CHECK(sc3(7, &pp, 1, 0) == 1 && (pp.rev & 0x11) == 0x11, "poll after the peer closed: POLLIN|POLLHUP");
    CHECK(sc3(0, sp[1], buf, 10) == 0, "read after the peer closed: EOF");
    CHECK(sc3(1, sp[1], "x", 1) == -32, "write after the peer closed: -EPIPE");
    sc1(3, sp[1]);

    /* shutdown(SHUT_WR) leaves the other direction open */
    sc4(53, 1, 1, 0, sp);
    sc3(1, sp[0], "last", 4);
    CHECK(sc2(S_shutdown, sp[0], 1) == 0, "shutdown(SHUT_WR)");
    memset(buf, 0, sizeof buf);
    CHECK(sc3(0, sp[1], buf, 10) == 4 && sc3(0, sp[1], buf, 10) == 0, "peer reads the data, then EOF");
    CHECK(sc3(1, sp[1], "back", 4) == 4 && sc3(0, sp[0], buf, 10) == 4, "the other direction still works");
    sc1(3, sp[0]); sc1(3, sp[1]);

    /* datagram and seqpacket keep message boundaries */
    sc4(53, 1, 2, 0, sp);
    sc3(1, sp[0], "one", 3); sc3(1, sp[0], "second", 6);
    memset(buf, 0, sizeof buf);
    CHECK(sc3(0, sp[1], buf, 100) == 3 && !memcmp(buf, "one", 3), "dgram: a read returns one datagram");
    struct iov_ siv[2] = { { (u64)"ab", 2 }, { (u64)"cde", 3 } };
    sc3(0, sp[1], buf, 100);   /* drain "second" */
    CHECK(sc3(20, sp[0], siv, 2) == 5, "dgram: writev of two iovecs");
    struct iov_ riv[2] = { { (u64)buf, 2 }, { (u64)buf + 2, 8 } };
    memset(buf, 0, sizeof buf);
    CHECK(sc3(19, sp[1], riv, 2) == 5 && !memcmp(buf, "abcde", 5), "dgram: the writev was ONE datagram (readv gets all 5)");
    sc3(1, sp[0], "0123456789", 10);
    struct iov_ tiv = { (u64)buf, 4 };
    struct msg_ tm = { 0, 0, 0, (u64)&tiv, 1, 0, 0, 0, 0 };
    CHECK(recvm(sp[1], &tm, 0) == 4 && (tm.fl & 0x20), "dgram: a short buffer truncates and sets MSG_TRUNC");
    CHECK(recvm(sp[1], &tm, 0x40) == -11, "dgram: the truncated rest is gone (-EAGAIN with MSG_DONTWAIT)");
    sc3(1, sp[0], "keep", 4);
    CHECK(sc(S_recvfrom, sp[1], buf, 100, 2 /*MSG_PEEK*/, 0, 0) == 4 && sc(S_recvfrom, sp[1], buf, 100, 0, 0, 0) == 4, "MSG_PEEK leaves the datagram queued");
    CHECK(sc(S_recvfrom, sp[1], buf, 100, 0x40, 0, 0) == -11, "MSG_DONTWAIT on an empty queue -> -EAGAIN");
    sc1(3, sp[0]); sc1(3, sp[1]);
    CHECK(sc4(53, 1, 5, 0, sp) == 0, "socketpair(SOCK_SEQPACKET)");
    sc3(1, sp[0], "rec1", 4); sc3(1, sp[0], "record-two", 10);
    CHECK(sc3(0, sp[1], buf, 100) == 4 && sc3(0, sp[1], buf, 100) == 10, "seqpacket: records stay separate");
    sc1(3, sp[0]);
    CHECK(sc3(0, sp[1], buf, 100) == 0, "seqpacket: EOF after the peer closed");
    sc1(3, sp[1]);

    /* a named stream server: bind, listen, accept, with a client thread */
    long ls = sc3(S_socket, 1, 1, 0);
    al = mkun(&a, "/tmp/xrl_unix_test.sock");
    sc1(87, "/tmp/xrl_unix_test.sock");
    CHECK(sc3(S_bind, ls, &a, al) == 0, "bind(/tmp/xrl_unix_test.sock)");
    memset(st, 0, sizeof st);
    CHECK(sc2(4, "/tmp/xrl_unix_test.sock", st) == 0 && (st[3] & 0170000) == 0140000, "stat of the path: S_ISSOCK");
    CHECK(sc3(2, "/tmp/xrl_unix_test.sock", 0, 0) == -6, "open() of a socket file -> -ENXIO");
    long ls2 = sc3(S_socket, 1, 1, 0);
    CHECK(sc3(S_bind, ls2, &a, al) == -98, "bind to a taken path -> -EADDRINUSE");
    sc1(3, ls2);
    CHECK(sc3(S_bind, ls, &a, al) == -22, "bind twice -> -EINVAL");
    memset(&b, 0x55, sizeof b); bl = 110;
    CHECK(sc3(S_getsockname, ls, &b, &bl) == 0 && bl == al && !memcmp(&b, &a, al), "getsockname returns the bound path");
    CHECK(sc2(S_listen, ls, 5) == 0, "listen");
    {   /* readdir sees the socket */
        long dfd = sc3(2, "/tmp", 0200000 /*O_DIRECTORY*/, 0);
        static char db[8192]; int seen = 0;
        long n;
        while (dfd >= 0 && (n = sc3(217, dfd, db, sizeof db)) > 0)
            for (long o = 0; o < n;) { u16 rl = *(u16 *)(db + o + 16); u8 t = *(u8 *)(db + o + 18); if (!memcmp(db + o + 19, "xrl_unix_test.sock", 19) && t == 12) seen = 1; o += rl; }
        CHECK(seen, "getdents lists it as DT_SOCK");
        if (dfd >= 0) sc1(3, dfd);
    }
    un_addr = a; un_alen = al; un_cdone = 0;
    volatile int ct1; spawn_thread(un_client_thr, 0, &ct1);
    pp = (struct pfd){ (int)ls, 1, 0 };
    CHECK(sc3(7, &pp, 1, 3000) == 1 && (pp.rev & 1), "poll(POLLIN) on the listener wakes when a client connects");
    memset(&b, 0, sizeof b); bl = 110;
    long cs = sc3(S_accept, ls, &b, &bl);
    CHECK(cs >= 3 && bl == 2 && b.fam == 1, "accept: the client is unnamed (address length 2)");
    memset(buf, 0, sizeof buf);
    CHECK(sc3(0, cs, buf, 16) == 10 && !memcmp(buf, "hello-unix", 10), "blocking read gets what the client thread wrote");
    memset(&b, 0x55, sizeof b); bl = 110;
    CHECK(sc3(S_getsockname, cs, &b, &bl) == 0 && bl == al && !memcmp(&b, &a, al), "accepted socket's name is the listener's path");
    pc = (struct ucred_){ -1, 0, 0 }; pl = 12;
    CHECK(sc(S_getsockopt, cs, 1, 17, &pc, &pl, 0) == 0 && pc.pid == me && pc.uid == 1000 && pc.gid == 1000, "SO_PEERCRED on the accepted socket: the connector");
    CHECK(sc3(1, cs, "pong", 4) == 4, "write back");
    for (int i = 0; i < 100 && !un_cdone; i++) nap_ms(10);
    CHECK(un_cdone == 1, "the client thread read 'pong'");
    sc1(3, cs);
    long lsn = sc3(S_socket, 1, 1 | SOCK_NB, 0);
    struct sun a2; u32 al2 = mkun(&a2, "/tmp/xrl_unix_test2.sock");
    sc1(87, "/tmp/xrl_unix_test2.sock");
    sc3(S_bind, lsn, &a2, al2); sc2(S_listen, lsn, 0);
    CHECK(sc3(S_accept, lsn, 0, 0) == -11, "accept on a non-blocking listener with nothing pending -> -EAGAIN");
    long c1 = sc3(S_socket, 1, 1 | SOCK_NB, 0);
    CHECK(sc3(S_connect, c1, &a2, al2) == 0, "non-blocking connect to a listener completes at once");
    long c2 = sc3(S_socket, 1, 1 | SOCK_NB, 0);
    CHECK(sc3(S_connect, c2, &a2, al2) == -11, "backlog 0 queues one connection; a second one is refused for now (-EAGAIN)");
    CHECK(sc3(S_connect, c1, &a2, al2) == -106, "connect on a connected socket -> -EISCONN");
    long ac = sc3(S_accept, lsn, 0, 0);
    CHECK(ac >= 3, "accept the first");
    CHECK(sc3(S_connect, c2, &a2, al2) == 0, "now the second connects");
    long c3 = c2;
    sc1(3, ac); sc1(3, c1); sc1(3, c2); sc1(3, c3);
    sc1(3, lsn);
    CHECK(sc3(S_connect, sc3(S_socket, 1, 1, 0), &a2, al2) == -111, "connect to a socket file nobody listens on -> -ECONNREFUSED");
    CHECK(sc1(87, "/tmp/xrl_unix_test2.sock") == 0 && sc2(4, "/tmp/xrl_unix_test2.sock", st) == -2, "unlink removes the socket file");
    struct sun nx; u32 nxl = mkun(&nx, "/tmp/xrl_no_such.sock");
    CHECK(sc3(S_connect, sc3(S_socket, 1, 1, 0), &nx, nxl) == -2, "connect to a missing path -> -ENOENT");
    long rf = sc3(2, "/tmp/xrl_un_plain", 0102, 0644); sc1(3, rf);
    struct sun pf_; u32 pfl = mkun(&pf_, "/tmp/xrl_un_plain");
    CHECK(sc3(S_connect, sc3(S_socket, 1, 1, 0), &pf_, pfl) == -111, "connect to a regular file -> -ECONNREFUSED");
    CHECK(sc3(S_bind, sc3(S_socket, 1, 1, 0), &pf_, pfl) == -98, "bind onto a regular file -> -EADDRINUSE");
    sc1(87, "/tmp/xrl_un_plain");
    struct sun nd; u32 ndl = mkun(&nd, "/tmp/xrl_no_dir/x.sock");
    CHECK(sc3(S_bind, sc3(S_socket, 1, 1, 0), &nd, ndl) == -2, "bind in a missing directory -> -ENOENT");
    sc1(3, ls);
    CHECK(sc3(S_connect, sc3(S_socket, 1, 1, 0), &a, al) == -111, "connect after the listener closed -> -ECONNREFUSED (node still there)");
    sc1(87, "/tmp/xrl_unix_test.sock");

    /* abstract names, per socket type; autobind */
    long as_ = sc3(S_socket, 1, 1, 0);
    struct sun ab; u32 abl = mkun(&ab, "@xrl-abs-test");
    CHECK(sc3(S_bind, as_, &ab, abl) == 0 && sc2(S_listen, as_, 4) == 0, "bind+listen on an abstract name");
    long ad = sc3(S_socket, 1, 2, 0);
    CHECK(sc3(S_bind, ad, &ab, abl) == 0, "the same abstract name for a datagram socket is a different socket");
    long as2 = sc3(S_socket, 1, 1, 0);
    CHECK(sc3(S_bind, as2, &ab, abl) == -98, "same name, same type -> -EADDRINUSE");
    sc1(3, as2);
    long ac1 = sc3(S_socket, 1, 1, 0);
    CHECK(sc3(S_connect, ac1, &ab, abl) == 0, "connect to the abstract name");
    long acs = sc3(S_accept, as_, 0, 0);
    CHECK(acs >= 3 && sc3(1, ac1, "abs", 3) == 3 && sc3(0, acs, buf, 8) == 3, "data through an abstract socket");
    memset(&b, 0x55, sizeof b); bl = 110;
    CHECK(sc3(S_getpeername, ac1, &b, &bl) == 0 && bl == abl && !memcmp(&b, &ab, abl), "getpeername of the connector shows the abstract name");
    sc1(3, ac1); sc1(3, acs); sc1(3, as_);
    CHECK(sc3(S_connect, sc3(S_socket, 1, 1, 0), &ab, abl) == -111, "the name is gone once its socket closed");
    sc1(3, ad);
    long au = sc3(S_socket, 1, 2, 0);
    struct sun ua; memset(&ua, 0, sizeof ua); ua.fam = 1;
    CHECK(sc3(S_bind, au, &ua, 2) == 0, "bind with just the family autobinds");
    memset(&b, 0x55, sizeof b); bl = 110;
    CHECK(sc3(S_getsockname, au, &b, &bl) == 0 && bl == 8 && b.fam == 1 && b.p[0] == 0, "autobound name: abstract, 5 hex digits");

    /* datagrams to an address; the receiver learns the sender's name */
    long dr = sc3(S_socket, 1, 2, 0);
    struct sun da; u32 dal = mkun(&da, "/tmp/xrl_unix_dg.sock");
    sc1(87, "/tmp/xrl_unix_dg.sock");
    sc3(S_bind, dr, &da, dal);
    CHECK(sc(S_sendto, au, "dgram-to", 8, 0, &da, dal) == 8, "sendto(path) from an autobound datagram socket");
    memset(&b, 0, sizeof b); bl = 110; memset(buf, 0, sizeof buf);
    CHECK(sc(S_recvfrom, dr, buf, 64, 0, &b, &bl) == 8 && !memcmp(buf, "dgram-to", 8) && bl == 8, "recvfrom reports the sender's (autobound) address");
    long dc = sc3(S_socket, 1, 2, 0);
    CHECK(sc(S_sendto, dc, "x", 1, 0, 0, 0) == -107, "send without a destination -> -ENOTCONN");
    CHECK(sc3(S_connect, dc, &da, dal) == 0 && sc3(1, dc, "conn", 4) == 4, "connect() sets the default destination; write works");
    bl = 110;
    CHECK(sc(S_recvfrom, dr, buf, 64, 0, &b, &bl) == 4 && bl == 0, "an unbound sender has no address");
    struct sun un_ = { 0, {0} };
    CHECK(sc3(S_connect, dc, &un_, 110) == 0, "connect(AF_UNSPEC) dissolves the destination");
    CHECK(sc3(1, dc, "x", 1) == -107, "...so a write fails with -ENOTCONN again");
    sc1(3, dc); sc1(3, dr); sc1(3, au);
    sc1(87, "/tmp/xrl_unix_dg.sock");

    /* fds and credentials in ancillary data */
    int pf[2]; sc2(293, pf, 0);
    sc4(53, 1, 1, 0, sp);
    struct iov_ iv1 = { (u64)"F", 1 };
    struct cm_fd cm = { 20, 1, 1, pf[1], 0 };
    struct msg_ m1 = { 0, 0, 0, (u64)&iv1, 1, (u64)&cm, 24, 0, 0 };
    CHECK(sendm(sp[0], &m1, 0) == 1, "sendmsg(SCM_RIGHTS)");
    sc1(3, pf[1]);   /* the fd in flight keeps the pipe's write end alive */
    char rb[8]; struct iov_ riv1 = { (u64)rb, 4 };
    struct cm_fd rcm; memset(&rcm, 0, sizeof rcm);
    struct msg_ rm = { 0, 0, 0, (u64)&riv1, 1, (u64)&rcm, 24, 0, 0 };
    r = recvm(sp[1], &rm, 0);
    CHECK(r == 1 && rcm.type == 1 && rcm.fd >= 3 && rm.ctllen == 24, "recvmsg: the fd arrives");
    CHECK(sc3(1, rcm.fd, "via-unix", 8) == 8 && sc3(0, pf[0], buf, 16) == 8 && !memcmp(buf, "via-unix", 8), "the received fd is the pipe's write end");
    /* two sends carrying fds are never merged into one read */
    int pf2[2]; sc2(293, pf2, 0);
    struct cm_fd cmb = { 20, 1, 1, pf2[1], 0 };
    struct iov_ iv2 = { (u64)"G", 1 };
    struct msg_ m2 = { 0, 0, 0, (u64)&iv2, 1, (u64)&cm, 24, 0, 0 };
    cm.fd = pf2[0];
    sendm(sp[0], &m2, 0);
    m2.ctl = (u64)&cmb; sendm(sp[0], &m2, 0);
    struct iov_ riv2 = { (u64)rb, 8 };
    struct msg_ rm2 = { 0, 0, 0, (u64)&riv2, 1, (u64)&rcm, 24, 0, 0 };
    CHECK(recvm(sp[1], &rm2, 0) == 1 && recvm(sp[1], &rm2, 0) == 1, "a stream read stops at each chunk that carries fds");
    /* control buffer too small: MSG_CTRUNC and the fd is closed */
    struct msg_ m3 = { 0, 0, 0, (u64)&iv2, 1, (u64)&cm, 24, 0, 0 };
    sendm(sp[0], &m3, 0);
    struct msg_ rm3 = { 0, 0, 0, (u64)&riv2, 1, (u64)&rcm, 8, 0, 0 };
    CHECK(recvm(sp[1], &rm3, 0) == 1 && (rm3.fl & 8) && rm3.ctllen == 0, "no room for the control message: MSG_CTRUNC");
    struct cm_fd bad = { 20, 1, 1, 999, 0 };
    struct msg_ m4 = { 0, 0, 0, (u64)&iv2, 1, (u64)&bad, 24, 0, 0 };
    CHECK(sendm(sp[0], &m4, 0) == -9, "SCM_RIGHTS with a bad fd -> -EBADF");
    /* credentials */
    int one_ = 1;
    CHECK(sc(S_setsockopt, sp[1], 1, 16 /*SO_PASSCRED*/, &one_, 4, 0) == 0, "setsockopt(SO_PASSCRED)");
    sc3(1, sp[0], "c", 1);
    struct cm_cred ccm; memset(&ccm, 0, sizeof ccm);
    struct msg_ crm = { 0, 0, 0, (u64)&riv2, 1, (u64)&ccm, 32, 0, 0 };
    CHECK(recvm(sp[1], &crm, 0) == 1 && ccm.type == 2 && ccm.c.pid == me && ccm.c.uid == 1000, "SCM_CREDENTIALS: the sender's pid/uid/gid");
    struct cm_cred scm = { 28, 1, 2, { (int)me, 1000, 1000 }, 0 };
    struct msg_ m5 = { 0, 0, 0, (u64)&iv2, 1, (u64)&scm, 32, 0, 0 };
    CHECK(sendm(sp[0], &m5, 0) == 1, "sendmsg with our own credentials");
    scm.c.uid = 0;
    CHECK(sendm(sp[0], &m5, 0) == -1, "...but not somebody else's -> -EPERM");
    sc1(3, sp[0]); sc1(3, sp[1]); sc1(3, pf[0]); sc1(3, pf2[0]); sc1(3, pf2[1]);

    /* epoll on sockets; FIONREAD; flow control between two threads */
    sc4(53, 1, 1, 0, sp);
    long ep = sc1(291, 0);
    struct { u32 e; u64 d; } __attribute__((packed)) ev1 = { 1, 0xabc }, evo;
    sc4(233, ep, 1, sp[1], &ev1);
    CHECK(sc4(232, ep, &evo, 1, 0) == 0, "epoll: nothing readable yet");
    sc3(1, sp[0], "12345", 5);
    CHECK(sc4(232, ep, &evo, 1, 1000) == 1 && evo.d == 0xabc && (evo.e & 1), "epoll: EPOLLIN once data is queued");
    int nread = 0;
    CHECK(sc3(16, sp[1], 0x541B, &nread) == 0 && nread == 5, "ioctl(FIONREAD)");
    sc3(0, sp[1], buf, 16);
    un_cdone = 0;
    volatile int ct2; spawn_thread(un_sender_thr, (void *)(long)sp[0], &ct2);
    u32 got_ = 0, bad_ = 0;
    static u8 rbuf[50000];
    for (;;) {
        long n = sc3(0, sp[1], rbuf, sizeof rbuf);
        if (n <= 0) break;
        for (long i = 0; i < n; i++) if (rbuf[i] != PAT(got_ + (u32)i)) bad_++;
        got_ += (u32)n;
    }
    CHECK(got_ == UN_TOTAL && bad_ == 0, "2 MiB streamed between two threads (the sender blocks on a full queue), every byte intact");
    for (int i = 0; i < 100 && !un_cdone; i++) nap_ms(10);
    CHECK(un_cdone == 1, "...and shutdown(SHUT_WR) gave the reader its EOF");
    sc1(3, sp[0]); sc1(3, sp[1]); sc1(3, ep);

    /* what is not a socket */
    int pf3[2]; sc2(293, pf3, 0);
    CHECK(sc3(S_connect, pf3[0], &a, al) == -88 && sc2(S_listen, pf3[0], 1) == -88, "connect/listen on a pipe -> -ENOTSOCK");
}

/* ---- shared memory: SysV segments, memfd / /dev/shm mappings (layer/src/shm.c) ------- */
static void test_shm(void)
{
    out("shared memory:\n");
    long id = sc3(29 /*shmget*/, 0 /*IPC_PRIVATE*/, 100000, 01000 | 0600);
    CHECK(id >= 0, "shmget(IPC_PRIVATE, 100000)");
    u8 ds[112]; memset(ds, 0xff, sizeof ds);
    CHECK(sc3(31 /*shmctl*/, id, 2 /*IPC_STAT*/, ds) == 0 && *(u64 *)(ds + 48) == 100000 && *(u64 *)(ds + 88) == 0 && (*(u32 *)(ds + 20) & 0777) == 0600, "IPC_STAT: size as asked, no attaches, mode 0600");
    u8 *a = (u8 *)sc3(30 /*shmat*/, id, 0, 0);
    CHECK((long)a > 0 && !((u64)a & 4095), "shmat(NULL) -> a page-aligned address");
    CHECK(a[0] == 0 && a[99999] == 0, "a new segment reads as zeros");
    for (int i = 0; i < 100000; i += 4096) a[i] = (u8)(i >> 12) + 1;
    u8 *b = (u8 *)sc3(30, id, 0, 0);
    CHECK(b == a && b[4096] == 2 && b[8192] == 3, "a second attach shows the same pages");
    sc3(31, id, 2, ds);
    CHECK(*(u64 *)(ds + 88) == 2 && *(int *)(ds + 84) == (int)sc(39, 0, 0, 0, 0, 0, 0), "nattch == 2, lpid is us");
    CHECK(sc1(67 /*shmdt*/, a) == 0, "shmdt");
    sc3(31, id, 2, ds);
    CHECK(*(u64 *)(ds + 88) == 1, "nattch == 1");
    CHECK(sc3(31, id, 0 /*IPC_RMID*/, 0) == 0, "IPC_RMID with one attach left");
    CHECK(a[4096] == 2, "the segment lives on while attached");
    CHECK(sc1(67, a) == 0, "last shmdt destroys it");
    CHECK(sc3(30, id, 0, 0) == -22, "shmat of a destroyed id -> -EINVAL");
    CHECK(sc1(67, 0x1000) == -22, "shmdt of a non-attach -> -EINVAL");
    CHECK(sc3(30, 12345, 0, 0) == -22, "shmat of an id that never existed -> -EINVAL");

    long k1 = sc3(29, 0x58524c31, 8192, 01000 | 0666);
    CHECK(k1 >= 0 && sc3(29, 0x58524c31, 4096, 0) == k1, "shmget with a key: the same segment again");
    CHECK(sc3(29, 0x58524c31, 4096, 01000 | 02000) == -17, "IPC_CREAT|IPC_EXCL on an existing key -> -EEXIST");
    CHECK(sc3(29, 0x58524c31, 16384, 0) == -22, "asking for a bigger size than it has -> -EINVAL");
    CHECK(sc3(29, 0x58524c32, 4096, 0) == -2, "an unknown key without IPC_CREAT -> -ENOENT");
    CHECK(sc3(29, 0x58524c33, 0, 01000) == -22, "creating with size 0 -> -EINVAL");
    sc3(31, k1, 0, 0);
    CHECK(sc3(29, 0x58524c31, 4096, 0) == -2, "after IPC_RMID the key is free");

    /* memfd: every shared mapping shows the same pages, whatever the offset */
    long fd = sc3(319 /*memfd_create*/, "xrl-shm", 3 /*CLOEXEC|ALLOW_SEALING*/, 0);
    CHECK(fd >= 0 && sc2(77 /*ftruncate*/, fd, 16384) == 0, "memfd_create + ftruncate(16 KiB)");
    u8 *m1 = (u8 *)sc(9, 0, 16384, 3, 1 /*MAP_SHARED*/, fd, 0);
    CHECK((long)m1 > 0, "mmap(MAP_SHARED) of the memfd");
    m1[0] = 0x11; m1[5000] = 0x22;
    u8 *m2 = (u8 *)sc(9, 0, 8192, 3, 1, fd, 4096);
    CHECK(m2 == m1 + 4096 && m2[904] == 0x22, "a mapping at offset 4096 is the same pages (sees the write at 5000)");
    m2[0] = 0x33;
    CHECK(m1[4096] == 0x33, "and a write through it shows in the first mapping");
    sc1(3, fd);
    m1[100] = 0x44;
    CHECK(m1[100] == 0x44, "close(fd) leaves the mappings usable");
    CHECK(sc2(11, m2, 8192) == 0 && m1[4096] == 0x33, "munmap of the second mapping keeps the first");
    CHECK(sc2(11, m1, 16384) == 0, "munmap of the first");

    /* the content survives 'unmap everything, map again' while the fd is open; seals */
    long f2 = sc3(319, "xrl-keep", 2, 0);
    sc2(77, f2, 8192);
    u8 *k = (u8 *)sc(9, 0, 8192, 3, 1, f2, 0);
    k[10] = 0x5a; k[8000] = 0x6b;
    sc2(11, k, 8192);
    u8 *k2 = (u8 *)sc(9, 0, 8192, 3, 1, f2, 0);
    CHECK((long)k2 > 0 && k2[10] == 0x5a && k2[8000] == 0x6b, "unmap and map again: the data is still there (fd open)");
    CHECK(sc3(72, f2, 1034 /*F_GET_SEALS*/, 0) == 0, "no seals yet");
    CHECK(sc3(72, f2, 1033 /*F_ADD_SEALS*/, 2 | 4) == 0 && sc3(72, f2, 1034, 0) == 6, "F_ADD_SEALS(SHRINK|GROW)");
    CHECK(sc2(77, f2, 4096) == -1 && sc2(77, f2, 16384) == -1, "ftruncate smaller or bigger -> -EPERM");
    CHECK(sc2(77, f2, 8192) == 0, "ftruncate to the same size is fine");
    CHECK(sc3(72, f2, 1033, 8) == 0 && sc3(1, f2, "x", 1) == -1, "F_SEAL_WRITE: write() -> -EPERM");
    CHECK(sc3(72, f2, 1033, 1) == 0 && sc3(72, f2, 1033, 8) == -1, "F_SEAL_SEAL: no more seals -> -EPERM");
    long f3 = sc3(319, "xrl-nosealing", 0, 0);
    CHECK(sc3(72, f3, 1033, 2) == -1 && sc3(72, f3, 1034, 0) == 1, "a memfd without MFD_ALLOW_SEALING: adding -> -EPERM, GET shows F_SEAL_SEAL");
    CHECK(sc3(319, "x", 0x1000, 0) == -22, "memfd_create with an unknown flag -> -EINVAL");
    long pf = sc3(2, "/tmp/xrl_not_a_memfd", 0102, 0600);
    CHECK(sc3(72, pf, 1034, 0) == -22, "F_GET_SEALS on a regular file -> -EINVAL");
    sc1(3, pf); sc1(87, "/tmp/xrl_not_a_memfd");
    sc2(11, k2, 8192); sc1(3, f2); sc1(3, f3);

    /* a /dev/shm file: shared while mapped, written back when the last mapping goes */
    long sf = sc3(2, "/dev/shm/xrl_shm_test", 0102, 0600);
    sc2(77, sf, 8192);
    u8 *s1 = (u8 *)sc(9, 0, 8192, 3, 1, sf, 0);
    u8 *s2 = (u8 *)sc(9, 0, 4096, 3, 1, sf, 0);
    s1[7] = 0x77;
    CHECK(s2 == s1 && s2[7] == 0x77, "/dev/shm file: two mappings, one set of pages");
    sc2(11, s1, 8192);
    char rb[4]; rb[0] = 0;
    sc2(11, s2, 4096);
    CHECK(sc4(17, sf, rb, 1, 7) == 1 && (u8)rb[0] == 0x77, "after the last munmap the file holds what was written");
    sc1(3, sf); sc1(87, "/dev/shm/xrl_shm_test");
    /* a file elsewhere is not shared memory: the ordinary private-copy mapping */
    long of = sc3(2, "/tmp/xrl_plain_map", 0102, 0600);
    sc2(77, of, 4096);
    u8 *p1 = (u8 *)sc(9, 0, 4096, 3, 1, of, 0);
    u8 *p2 = (u8 *)sc(9, 0, 4096, 3, 1, of, 0);
    CHECK((long)p1 > 0 && (long)p2 > 0 && p1 != p2, "a regular file's shared mapping is its own copy (not shm.c's)");
    sc2(11, p1, 4096); sc2(11, p2, 4096); sc1(3, of); sc1(87, "/tmp/xrl_plain_map");
}

/* ---- pseudo-terminals (layer/src/pty.c) ---------------------------------------- */
struct ktio { u32 iflag, oflag, cflag, lflag; u8 line; u8 cc[19]; };
struct wsz_ { u16 row, col, xp, yp; };

static long rd_all(long fd, char *b, long cap)   /* what is readable now (non-blocking read loop) */
{
    long n = 0, r;
    while (n < cap && (r = sc3(0, fd, b + n, cap - n)) > 0) n += r;
    return n;
}

static void test_pty(void)
{
    out("pseudo-terminals:\n");
    char buf[256], path[32] = "/dev/pts/";
    long m = sc3(2, "/dev/ptmx", 02 | 04000 /* O_RDWR|O_NONBLOCK */, 0);
    CHECK(m >= 3, "open /dev/ptmx");
    u32 n = 99;
    CHECK(sc3(16, m, 0x80045430 /*TIOCGPTN*/, &n) == 0 && n < 4096, "TIOCGPTN");
    { char d[12]; int k = 0, t = (int)n; do d[k++] = (char)('0' + t % 10); while ((t /= 10)); int o = 9; while (k) path[o++] = d[--k]; path[o] = 0; }
    CHECK(sc3(2, path, 02, 0) == -5, "opening a locked slave -> -EIO");
    int zero = 0;
    CHECK(sc3(16, m, 0x40045431 /*TIOCSPTLCK*/, &zero) == 0, "TIOCSPTLCK(0): unlockpt");
    u64 st[18]; memset(st, 0, sizeof st);
    CHECK(sc2(4, path, st) == 0 && (st[3] & 0170000) == 0020000 && (u32)(st[5] >> 8) == 136, "stat /dev/pts/N: a character device, major 136");
    long s = sc3(2, path, 02 | 04000, 0);
    CHECK(s >= 3, "open the slave");
    CHECK(sc3(16, s, 0x5413 /*TIOCGWINSZ*/, buf) == 0, "TIOCGWINSZ on the slave (isatty)");
    struct wsz_ w = { 40, 132, 0, 0 };
    CHECK(sc3(16, m, 0x5414 /*TIOCSWINSZ*/, &w) == 0, "TIOCSWINSZ on the master");
    struct wsz_ g; memset(&g, 0, sizeof g);
    CHECK(sc3(16, s, 0x5413, &g) == 0 && g.row == 40 && g.col == 132, "the slave sees the size");
    struct ktio t;
    CHECK(sc3(16, s, 0x5401 /*TCGETS*/, &t) == 0 && (t.lflag & 2 /*ICANON*/) && (t.lflag & 8 /*ECHO*/) && (t.oflag & 4 /*ONLCR*/) && t.cc[0] == 3 && t.cc[4] == 4, "default termios: ICANON|ECHO, ONLCR, ^C ^D");

    /* canonical mode: a line becomes readable at the newline, and is echoed */
    CHECK(sc3(1, m, "hel", 3) == 3, "the master types 'hel'");
    CHECK(sc3(0, s, buf, 100) == -11, "the slave sees nothing yet (no newline): -EAGAIN");
    memset(buf, 0, sizeof buf);
    long e = rd_all(m, buf, 100);
    CHECK(e == 3 && !memcmp(buf, "hel", 3), "echo of the typed characters");
    sc3(1, m, "lo\r", 3);
    memset(buf, 0, sizeof buf);
    CHECK(sc3(0, s, buf, 100) == 6 && !memcmp(buf, "hello\n", 6), "'\\r' became '\\n' (ICRNL); the whole line is readable now");
    memset(buf, 0, sizeof buf);
    e = rd_all(m, buf, 100);
    CHECK(e == 4 && !memcmp(buf, "lo\r\n", 4), "echo of 'lo' and the newline as CR LF");
    sc3(1, m, "abc\x7f" "d\n", 6);   /* backspace erases 'c' */
    memset(buf, 0, sizeof buf);
    CHECK(sc3(0, s, buf, 100) == 4 && !memcmp(buf, "abd\n", 4), "VERASE edits the line");
    rd_all(m, buf, 100);
    sc3(1, m, "x\ny\n", 4);
    CHECK(sc3(0, s, buf, 100) == 2 && sc3(0, s, buf, 100) == 2, "two lines are two reads");
    rd_all(m, buf, 100);
    sc3(1, m, "\x04", 1);   /* ^D on an empty line */
    CHECK(sc3(0, s, buf, 100) == 0, "^D on an empty line: the slave reads 0 (EOF)");
    CHECK(sc3(0, s, buf, 100) == -11, "...once");
    sc3(1, m, "par", 3); sc3(1, m, "\x04", 1);
    CHECK(sc3(0, s, buf, 100) == 3, "^D after text delivers the text without a newline");
    rd_all(m, buf, 100);

    /* output side */
    CHECK(sc3(1, s, "out\n", 4) == 4, "the slave writes");
    memset(buf, 0, sizeof buf);
    CHECK(rd_all(m, buf, 100) == 5 && !memcmp(buf, "out\r\n", 5), "the master reads it with ONLCR (CR LF)");

    /* raw mode */
    struct ktio r = t;
    r.lflag &= ~(2u | 8u | 1u);   /* ~ICANON & ~ECHO & ~ISIG */
    r.iflag &= ~0400u;            /* ~ICRNL */
    r.oflag &= ~1u;               /* ~OPOST */
    r.cc[6] = 1; r.cc[5] = 0;
    CHECK(sc3(16, s, 0x5402 /*TCSETS*/, &r) == 0, "TCSETS: raw");
    sc3(1, m, "q\r\x03", 3);
    memset(buf, 0, sizeof buf);
    CHECK(sc3(0, s, buf, 100) == 3 && buf[0] == 'q' && buf[1] == '\r' && buf[2] == 3, "raw: bytes arrive at once, untranslated, ^C is a byte");
    CHECK(rd_all(m, buf, 100) == 0, "raw without ECHO: no echo");
    sc3(1, s, "a\n", 2);
    memset(buf, 0, sizeof buf);
    CHECK(rd_all(m, buf, 100) == 2 && buf[1] == '\n', "raw without OPOST: no CR added");
    struct pfd pp = { (int)s, 1, 0 };
    CHECK(sc3(7, &pp, 1, 0) == 0, "poll: slave has nothing to read");
    sc3(1, m, "z", 1);
    pp.rev = 0;
    CHECK(sc3(7, &pp, 1, 0) == 1 && (pp.rev & 1), "poll: POLLIN once a byte arrives");
    int nb = 0;
    CHECK(sc3(16, s, 0x541B /*FIONREAD*/, &nb) == 0 && nb == 1, "FIONREAD on the slave");
    sc3(0, s, buf, 10);
    r.cc[6] = 4;   /* VMIN = 4 */
    sc3(16, s, 0x5402, &r);
    sc3(1, m, "12", 2);
    CHECK(sc3(0, s, buf, 100) == -11, "VMIN=4 with 2 bytes queued: a non-blocking read -> -EAGAIN");
    sc3(1, m, "34", 2);
    CHECK(sc3(0, s, buf, 100) == 4, "...and 4 bytes are delivered together");

    /* signals: ISIG on, ^C flushes and signals the foreground group (no process model here: logged, dropped) */
    r.lflag |= 1u | 2u | 8u; r.cc[6] = 1;
    sc3(16, s, 0x5402, &r);
    sc3(1, m, "abc", 3);
    sc3(1, m, "\x03", 1);
    memset(buf, 0, sizeof buf);
    e = rd_all(m, buf, 100);
    CHECK(e == 5 && !memcmp(buf, "abc^C", 5), "^C is echoed as ^C and the typed-ahead line is discarded");
    sc3(1, m, "\n", 1);
    CHECK(sc3(0, s, buf, 100) == 1, "(the line discipline is clean after it)");

    /* peer going away */
    sc1(3, s);
    CHECK(sc3(0, m, buf, 100) >= 0, "(echo of the last newline is still readable)");
    rd_all(m, buf, 100);
    CHECK(sc3(0, m, buf, 100) == -5, "master read after the last slave closed -> -EIO");
    pp = (struct pfd){ (int)m, 1, 0 };
    CHECK(sc3(7, &pp, 1, 0) == 1 && (pp.rev & 0x10), "poll(master): POLLHUP");
    long m2 = sc3(2, "/dev/ptmx", 02, 0);
    u32 n2 = 99; sc3(16, m2, 0x80045430, &n2);
    CHECK(n2 != n, "a second pty gets another number");
    sc3(16, m2, 0x40045431, &zero);
    { char p2[32] = "/dev/pts/"; char d[12]; int k = 0, tt = (int)n2; do d[k++] = (char)('0' + tt % 10); while ((tt /= 10)); int o = 9; while (k) p2[o++] = d[--k]; p2[o] = 0;
      long s2 = sc3(2, p2, 02, 0);
      sc1(3, m2);
      CHECK(sc3(0, s2, buf, 10) == 0, "slave read after the master closed: EOF");
      CHECK(sc3(1, s2, "x", 1) == -5, "slave write after the master closed: -EIO");
      sc1(3, s2); }
    sc1(3, m);
    CHECK(sc3(2, path, 02, 0) == -2, "after both closed the pty is gone: open -> -ENOENT");
}

/* ---- timerfd, itimers, inotify, copy_file_range/splice (layer/src/timers.c, syscall.c) --- */
static void test_timers(void)
{
    out("timers and file copying:\n");
    char buf[64];
    long tf = sc3(283 /*timerfd_create*/, 1 /*CLOCK_MONOTONIC*/, 04000, 0);
    CHECK(tf >= 3, "timerfd_create(MONOTONIC, NONBLOCK)");
    u64 cnt = 0;
    CHECK(sc3(0, tf, &cnt, 8) == -11, "read of an unarmed timerfd -> -EAGAIN");
    long its[4] = { 0, 0, 0, 50000000 };   /* one shot, 50 ms */
    u64 t0 = mono_ns();
    CHECK(sc4(286 /*timerfd_settime*/, tf, 0, its, 0) == 0, "timerfd_settime(50 ms one-shot)");
    long cur[4]; memset(cur, 0xff, sizeof cur);
    CHECK(sc2(287 /*timerfd_gettime*/, tf, cur) == 0 && cur[2] == 0 && cur[3] > 0 && cur[3] <= 50000000 && cur[0] == 0 && cur[1] == 0, "timerfd_gettime: the time left, no interval");
    struct pfd pp = { (int)tf, 1, 0 };
    CHECK(sc3(7, &pp, 1, 0) == 0, "poll: not yet");
    CHECK(sc3(7, &pp, 1, 2000) == 1 && (pp.rev & 1), "poll(POLLIN) wakes when it expires");
    u64 dt = mono_ns() - t0;
    CHECK(dt >= 45000000ull && dt < 1000000000ull, "...after about 50 ms");
    CHECK(sc3(0, tf, &cnt, 8) == 8 && cnt == 1, "read -> 1 expiration");
    CHECK(sc3(0, tf, &cnt, 8) == -11, "...and it is consumed (a one-shot does not repeat)");
    long iv[4] = { 0, 20000000, 0, 20000000 };   /* every 20 ms */
    sc4(286, tf, 0, iv, 0);
    nap_ms(130);
    CHECK(sc3(0, tf, &cnt, 8) == 8 && cnt >= 4 && cnt <= 8, "an interval timer counts the expirations it missed (130 ms / 20 ms)");
    long ep = sc1(291, 0);
    struct { u32 e; u64 d; } __attribute__((packed)) ev = { 1, 0x7f }, evo;
    sc4(233, ep, 1, tf, &ev);
    CHECK(sc4(232, ep, &evo, 1, 1000) == 1 && evo.d == 0x7f, "epoll_wait wakes on the timerfd");
    long off[4] = { 0, 0, 0, 0 };
    CHECK(sc4(286, tf, 0, off, cur) == 0 && cur[1] == 20000000, "settime(0) disarms and returns the old setting");
    sc3(0, tf, &cnt, 8);
    nap_ms(60);
    CHECK(sc3(0, tf, &cnt, 8) == -11, "a disarmed timer stays quiet");
    long abs_[4] = { 0, 0, 0, 0 };
    { long ts[2]; sc2(228, 1, ts); u64 nowns = (u64)ts[0] * 1000000000ull + (u64)ts[1] + 30000000ull; abs_[2] = (long)(nowns / 1000000000ull); abs_[3] = (long)(nowns % 1000000000ull); }
    sc4(286, tf, 1 /*TIMER_ABSTIME*/, abs_, 0);
    pp.rev = 0;
    CHECK(sc3(7, &pp, 1, 2000) == 1 && (pp.rev & 1), "TFD_TIMER_ABSTIME on the monotonic clock");
    CHECK(sc2(287, 99, cur) == -9 && sc4(286, 0, 0, off, 0) == -22, "timerfd calls on a bad fd / a non-timerfd");
    sc1(3, tf); sc1(3, ep);

    /* setitimer/alarm: SIGALRM runs a handler */
    sigact(14, h_info, 0);
    h_count = 0; h_sig = 0;
    long itv[4] = { 0, 0, 0, 30000 };   /* 30 ms */
    CHECK(sc3(38 /*setitimer*/, 0, itv, 0) == 0, "setitimer(ITIMER_REAL, 30 ms)");
    long cv[4]; memset(cv, 0xff, sizeof cv);
    CHECK(sc2(36 /*getitimer*/, 0, cv) == 0 && cv[0] == 0 && cv[3] > 0 && cv[3] <= 30000, "getitimer: time left");
    for (int i = 0; i < 200 && !h_count; i++) nap_ms(10);
    CHECK(h_count == 1 && h_sig == 14, "SIGALRM ran the handler once (one-shot)");
    CHECK(sc1(37 /*alarm*/, 5) == 0, "alarm(5): nothing was pending");
    CHECK(sc1(37, 0) == 5, "alarm(0) returns the 5 seconds that were left (rounded up)");

    /* inotify: descriptors that never report */
    long in_ = sc1(294 /*inotify_init1*/, 04000);
    CHECK(in_ >= 3, "inotify_init1");
    CHECK(sc3(254 /*add_watch*/, in_, "/tmp", 0x2 | 0x100) >= 1, "inotify_add_watch(/tmp) -> a watch number");
    CHECK(sc3(254, in_, "/tmp/xrl_no_such_dir", 0x2) == -2, "...of a missing path -> -ENOENT");
    CHECK(sc3(0, in_, buf, 64) == -11, "read: no events -> -EAGAIN");
    pp = (struct pfd){ (int)in_, 1, 0 };
    CHECK(sc3(7, &pp, 1, 20) == 0, "poll: never readable");
    CHECK(sc2(255 /*rm_watch*/, in_, 1) == 0, "inotify_rm_watch");
    sc1(3, in_);

    /* copy_file_range / splice / sendfile with offsets */
    long a = sc3(2, "/tmp/xrl_cfr_a", 0102, 0600);
    sc3(1, a, "0123456789abcdef", 16);
    long b = sc3(2, "/tmp/xrl_cfr_b", 0102, 0600);
    long oi = 4, oo = 2;
    CHECK(sc(326 /*copy_file_range*/, a, &oi, b, &oo, 6, 0) == 6 && oi == 10 && oo == 8, "copy_file_range with both offsets: moves them");
    memset(buf, 0, sizeof buf);
    CHECK(sc4(17, b, buf, 8, 0) == 8 && !memcmp(buf + 2, "456789", 6), "...and the bytes landed at offset 2");
    sc3(8, a, 0, 0);
    sc3(8, b, 0, 0);
    CHECK(sc(326, a, 0, b, 0, 4, 0) == 4 && sc3(8, a, 0, 1) == 4 && sc3(8, b, 0, 1) == 4, "copy_file_range without offsets moves the file positions");
    long so = 0;
    CHECK(sc(40 /*sendfile*/, b, a, &so, 16, 0, 0) == 16 && so == 16, "sendfile with an offset pointer");
    int p1[2], p2[2]; sc2(293, p1, 0); sc2(293, p2, 0);
    sc3(1, p1[1], "pipeline", 8);
    CHECK(sc(275 /*splice*/, p1[0], 0, p2[1], 0, 8, 0) == 8 && sc3(0, p2[0], buf, 16) == 8 && !memcmp(buf, "pipeline", 8), "splice from a pipe to a pipe");
    CHECK(sc(275, a, &oi, p2[1], &oo, 4, 0) == -29, "splice with an offset on a pipe end -> -ESPIPE");
    sc1(3, a); sc1(3, b); sc1(87, "/tmp/xrl_cfr_a"); sc1(87, "/tmp/xrl_cfr_b");
    sc1(3, p1[0]); sc1(3, p1[1]); sc1(3, p2[0]); sc1(3, p2[1]);
}

/* ---- EPOLLET: new data is an edge even when the fd was ready before and was drained in between --- */
static void test_epoll_et(void)
{
    out("edge-triggered epoll:\n");
    struct { u32 e; u64 d; } __attribute__((packed)) ev = { 1 | (1u << 31), 0x11 }, evo;
    char b[16];
    int sp[2], pf[2];
    long ep = sc1(291, 0);
    sc4(53, 1, 1, 0, sp);
    sc4(233, ep, 1, sp[1], &ev);
    CHECK(sc4(232, ep, &evo, 1, 0) == 0, "ET: a socket with nothing to read is quiet");
    sc3(1, sp[0], "a", 1);
    CHECK(sc4(232, ep, &evo, 1, 500) == 1 && evo.d == 0x11, "ET: data arrives -> one event");
    CHECK(sc4(232, ep, &evo, 1, 0) == 0, "ET: not reported again while nothing new came");
    CHECK(sc3(0, sp[1], b, 16) == 1, "(drain it)");
    sc3(1, sp[0], "b", 1);
    CHECK(sc4(232, ep, &evo, 1, 500) == 1, "ET: drained and refilled between two waits is a new edge (the lost-wake-up an X server hit)");
    sc3(0, sp[1], b, 16);
    sc3(1, sp[0], "c", 1);
    sc3(1, sp[0], "d", 1);
    CHECK(sc4(232, ep, &evo, 1, 500) == 1 && sc4(232, ep, &evo, 1, 0) == 0, "ET: two more writes before the wait are still one report");
    sc1(3, ep);
    /* a pipe */
    ep = sc1(291, 0);
    sc2(293, pf, 04000);
    sc4(233, ep, 1, pf[0], &ev);
    sc3(1, pf[1], "x", 1);
    CHECK(sc4(232, ep, &evo, 1, 500) == 1, "ET pipe: data -> event");
    sc3(0, pf[0], b, 16);
    sc3(1, pf[1], "y", 1);
    CHECK(sc4(232, ep, &evo, 1, 500) == 1, "ET pipe: drained and refilled -> event");
    sc1(3, ep); sc1(3, sp[0]); sc1(3, sp[1]); sc1(3, pf[0]); sc1(3, pf[1]);
}

/* ---- regressions found by review: control buffer sizes, WAITALL + bad iovec, shm sizes, pty mode switch, memfd seals --- */
static void test_review_fixes(void)
{
    out("review regressions:\n");
    char b[64];
    int sp[2];
    sc4(53, 1, 1, 0, sp);
    int pf[2]; sc2(293, pf, 0);

    /* a control buffer of exactly CMSG_LEN(4) = 20 bytes: nothing is written past it, controllen is 20 */
    struct { u8 buf[32]; } ctl;
    memset(&ctl, 0x7e, sizeof ctl);
    struct cm_fd cm1 = { 20, 1, 1, pf[1], 0 };
    struct iov_ iv1 = { (u64)"F", 1 };
    struct msg_ m1 = { 0, 0, 0, (u64)&iv1, 1, (u64)&cm1, 24, 0, 0 };
    sc3(S_sendmsg, sp[0], &m1, 0);
    struct iov_ riv = { (u64)b, 4 };
    struct msg_ rm = { 0, 0, 0, (u64)&riv, 1, (u64)ctl.buf, 20, 0, 0 };
    long r = sc3(S_recvmsg, sp[1], &rm, 0);
    CHECK(r == 1 && rm.ctllen == 20 && ctl.buf[20] == 0x7e && ctl.buf[23] == 0x7e && *(int *)(ctl.buf + 16) >= 3, "recvmsg with a 20-byte control buffer: fd delivered, controllen 20, no write past it");
    sc1(3, *(int *)(ctl.buf + 16));

    /* MSG_WAITALL with a bad second iovec returns EFAULT instead of spinning */
    sc3(1, sp[0], "abc", 3);
    struct iov_ biv[2] = { { (u64)b, 2 }, { 1, 8 } };
    struct msg_ bm = { 0, 0, 0, (u64)biv, 2, 0, 0, 0, 0 };
    CHECK(sc3(S_recvmsg, sp[1], &bm, 0x100 /*MSG_WAITALL*/) == -14, "MSG_WAITALL with an unwritable iovec -> -EFAULT (no spin)");
    { struct msg_ dm = { 0, 0, 0, (u64)&riv, 1, 0, 0, 0, 0 }; sc3(S_recvmsg, sp[1], &dm, 0x40); }

    /* shmget compares with the size asked for, not the page-rounded one */
    long id = sc3(29, 0x58524c40, 100, 01000 | 0600);
    CHECK(id >= 0 && sc3(29, 0x58524c40, 4000, 0) == -22 && sc3(29, 0x58524c40, 100, 0) == id, "shmget(existing, bigger than asked but within the page) -> -EINVAL");
    sc3(31, id, 0, 0);

    /* raw -> canonical: what was typed ahead becomes one complete line */
    long m = sc3(2, "/dev/ptmx", 02 | 04000, 0);
    int zero = 0; sc3(16, m, 0x40045431, &zero);
    u32 n = 0; sc3(16, m, 0x80045430, &n);
    char path[32] = "/dev/pts/"; { char d[12]; int k = 0, t = (int)n; do d[k++] = (char)('0' + t % 10); while ((t /= 10)); int o = 9; while (k) path[o++] = d[--k]; path[o] = 0; }
    long s = sc3(2, path, 02 | 04000, 0);
    struct ktio t0, t1;
    sc3(16, s, 0x5401, &t0);
    t1 = t0; t1.lflag &= ~(2u | 8u); t1.cc[6] = 1;
    sc3(16, s, 0x5402, &t1);
    sc3(1, m, "ab", 2);
    sc3(16, s, 0x5402, &t0);   /* ICANON back on */
    CHECK(sc3(0, s, b, 16) == 2 && b[0] == 'a' && b[1] == 'b', "raw -> canonical: typed-ahead bytes are readable at once as one line");
    sc1(3, s); sc1(3, m);

    /* memfd seals as the kernel's shmem: GROW stops a write past the end, WRITE/FUTURE_WRITE any write, truncation checks only SHRINK/GROW */
    long f = sc3(319, "xrl-seals", 2, 0);
    sc2(77, f, 4096);
    sc3(72, f, 1033, 4 /*F_SEAL_GROW*/);
    CHECK(sc4(18 /*pwrite64*/, f, "x", 1, 4095) == 1 && sc4(18, f, "xy", 2, 4095) == -1, "F_SEAL_GROW: a write inside the file works, one past its end -> -EPERM");
    sc3(72, f, 1033, 8 /*F_SEAL_WRITE*/);
    CHECK(sc2(77, f, 4096) == 0, "F_SEAL_WRITE does not forbid a same-size ftruncate");
    sc1(3, f);
    long g = sc3(319, "xrl-fw", 2, 0);
    sc2(77, g, 4096);
    sc3(72, g, 1033, 0x10 /*F_SEAL_FUTURE_WRITE*/);
    CHECK(sc3(1, g, "x", 1) == -1, "F_SEAL_FUTURE_WRITE: write() -> -EPERM");
    sc1(3, g);
    sc1(3, sp[0]); sc1(3, sp[1]); sc1(3, pf[0]);
}

/* ---- second review round: MSG_DONTWAIT on send, shm file growth/offsets/identity, MAP_FIXED over a region, read-only slave --- */
static void test_review_fixes2(void)
{
    out("review regressions (2):\n");
    static u8 big[65536];
    int sp[2];
    sc4(53, 1, 1, 0, sp);
    long total = 0, r = 0;
    for (int i = 0; i < 64 && r >= 0; i++) { r = sc4(S_sendto, sp[0], big, sizeof big, 0x40 /*MSG_DONTWAIT*/); if (r > 0) total += r; }
    CHECK(r == -11 && total > 65536, "send(MSG_DONTWAIT) on a full blocking stream socket -> -EAGAIN instead of blocking");
    sc1(3, sp[0]); sc1(3, sp[1]);

    long fd = sc3(319, "xrl-grow", 0, 0);
    sc2(77, fd, 4096);
    u8 *a = (u8 *)sc(9, 0, 4096, 3, 1, fd, 0);
    a[10] = 0x42;
    CHECK(sc(9, 0, 4096, 3, 1, fd, -4096) == -22, "mmap(MAP_SHARED, memfd, negative offset) -> -EINVAL");
    sc2(77, fd, 1 << 20);   /* the file grows after it was mapped */
    u8 *b = (u8 *)sc(9, 0, 1 << 20, 3, 1, fd, 0);
    CHECK((long)b > 0 && b == a && b[10] == 0x42 && (b[900000] = 7) == 7, "growing the memfd and mapping it larger works (same pages, headroom)");
    long fx = sc(9, a, 4096, 3, 0x11 /*MAP_SHARED|MAP_FIXED*/, fd, 0);
    CHECK(fx == -22, "MAP_FIXED over a shared region -> -EINVAL (the pages are shared with others)");
    sc2(11, a, 4096); sc2(11, b, 1 << 20); sc1(3, fd);

    long s1 = sc3(2, "/dev/shm/xrl_ident", 0102, 0600);
    sc2(77, s1, 4096);
    u8 *p1 = (u8 *)sc(9, 0, 4096, 3, 1, s1, 0);
    p1[0] = 0x11;
    sc1(87, "/dev/shm/xrl_ident");
    long s2 = sc3(2, "/dev/shm/xrl_ident", 0102, 0600);
    sc2(77, s2, 4096);
    u8 *p2 = (u8 *)sc(9, 0, 4096, 3, 1, s2, 0);
    CHECK((long)p2 > 0 && p2 != p1 && p2[0] == 0, "a /dev/shm file unlinked and recreated is a new object, not the old pages");
    sc2(11, p1, 4096); sc2(11, p2, 4096); sc1(3, s1); sc1(3, s2); sc1(87, "/dev/shm/xrl_ident");

    long m = sc3(2, "/dev/ptmx", 02, 0);
    int zero = 0; u32 n = 0; sc3(16, m, 0x40045431, &zero); sc3(16, m, 0x80045430, &n);
    char path[32] = "/dev/pts/"; { char d[12]; int k = 0, t = (int)n; do d[k++] = (char)('0' + t % 10); while ((t /= 10)); int o = 9; while (k) path[o++] = d[--k]; path[o] = 0; }
    long ro = sc3(2, path, 0 /*O_RDONLY*/, 0);
    CHECK(ro >= 0 && (sc3(72, ro, 3 /*F_GETFL*/, 0) & 3) == 0, "open(/dev/pts/N, O_RDONLY) stays read-only (F_GETFL)");
    sc1(3, ro); sc1(3, m);
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
        const char *a = (const char *)sp[1 + i], *f = "--fake-host", *g = "--no-swap";
        int k = 0; while (a[k] && a[k] == f[k]) k++;
        if (!a[k] && !f[k]) fake_host = 1;
        k = 0; while (a[k] && a[k] == g[k]) k++;
        if (!a[k] && !g[k]) no_swap = 1;
    }
    out("xrl guest test start\n");
    outn("argc", argc);
    test_misc();
    test_mem();
    test_fs();
    test_cpuinfo();
    test_threads();
    test_net();
    test_unix();
    test_epoll_et();
    test_shm();
    test_pty();
    test_timers();
    test_review_fixes();
    test_review_fixes2();
    test_ntsync();
    test_signals();
    if (!no_swap) test_swap();
    test_net();
    test_v2();
    test_teb();
    outn("TOTAL FAILS", fails);
    sc1(231, fails);
    for (;;) {}
}
