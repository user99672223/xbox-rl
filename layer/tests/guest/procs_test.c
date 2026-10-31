/*
 * procs_test.c - guest processes in the layer: fork/vfork/exec/wait, pipes between processes, signals between
 * processes, exit codes, CLOEXEC, process groups, sessions, orphans, /proc/<pid>. Freestanding like guest.c.
 *
 *   procs_test.elf           the test driver (a static PIE, so every process gets its own load address)
 *   procs_test.elf <role> .. a child program the driver starts with fork+execve (see run_role)
 *   procs_nopie.elf          the same program linked at 0x400000: exec of a second copy while the first lives
 *                            must fail with -ENOMEM
 *
 * `syscall` instructions are left in on purpose (fork_, vfork_, wait4_ ...): the loader rewrites them like it
 * does for Alpine's binaries, so these are the paths musl's fork()/vfork()/posix_spawn take.
 * Exit code = number of failed checks. Run by layer/tests/procs-test.sh.
 */
typedef unsigned long u64; typedef long s64; typedef unsigned int u32; typedef unsigned char u8;
typedef long (*xsc_t)(long, long, long, long, long, long, long);
xsc_t xsc;
#define AT_XRL_SYSCALL 0x5801
#define sc(n, a, b, c, d, e, f) xsc((n), (long)(a), (long)(b), (long)(c), (long)(d), (long)(e), (long)(f))
#define sc1(n, a) sc(n, a, 0, 0, 0, 0, 0)
#define sc2(n, a, b) sc(n, a, b, 0, 0, 0, 0)
#define sc3(n, a, b, c) sc(n, a, b, c, 0, 0, 0)
#define sc4(n, a, b, c, d) sc(n, a, b, c, d, 0, 0)

void *memset(void *d, int c, unsigned long n) { u8 *p = d; while (n--) *p++ = (u8)c; return d; }
void *memcpy(void *d, const void *s, unsigned long n) { u8 *p = d; const u8 *q = s; while (n--) *p++ = *q++; return d; }
static unsigned long slen(const char *s) { unsigned long n = 0; while (s[n]) n++; return n; }
static int seq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static const char *sfind(const char *h, const char *n)
{
    unsigned long nl = slen(n);
    for (; *h; h++) { unsigned long i = 0; while (i < nl && h[i] == n[i]) i++; if (i == nl) return h; }
    return 0;
}
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
static long atol_(const char *s) { long v = 0; while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0'); return v; }
static void ltoa_(long v, char *o) { char t[24]; int k = 0; if (!v) t[k++] = '0'; while (v) { t[k++] = '0' + v % 10; v /= 10; } int n = 0; while (k) o[n++] = t[--k]; o[n] = 0; }
static int fails;
#define CHECK(cond, name) do { if (cond) out("  ok   " name "\n"); else { out("  FAIL " name "\n"); fails++; } } while (0)
#define CHECKV(cond, name, v) do { if (cond) out("  ok   " name "\n"); else { out("  FAIL " name "\n"); outn("       value", (long)(v)); fails++; } } while (0)

/* ---------------------------------------------------------------- raw syscalls (rewritten by the loader) */
long fork_(void);
__asm__(".text\n.globl fork_\nfork_:\n  mov $57,%eax\n  syscall\n  ret\n");
long vfork_(void);   /* musl's vfork: the return address lives in a register across the syscall */
__asm__(".text\n.globl vfork_\nvfork_:\n  pop %rdx\n  mov $58,%eax\n  syscall\n  push %rdx\n  ret\n");
long wait4_(long pid, int *st, long opt, void *ru);
__asm__(".text\n.globl wait4_\nwait4_:\n  mov %rcx,%r10\n  mov $61,%eax\n  syscall\n  ret\n");
long kill_(long pid, long sig);
__asm__(".text\n.globl kill_\nkill_:\n  mov $62,%eax\n  syscall\n  ret\n");
__attribute__((noreturn)) void exit_(long code);
__asm__(".text\n.globl exit_\nexit_:\n  mov $231,%eax\n  syscall\n  hlt\n");

static long getpid_(void) { return sc(39, 0, 0, 0, 0, 0, 0); }
static long getppid_(void) { return sc(110, 0, 0, 0, 0, 0, 0); }
static long execve_(const char *p, char **av, char **ev) { return sc3(59, p, av, ev); }
static long pipe2_(int *f, long fl) { return sc2(293, f, fl); }
static long close_(long fd) { return sc1(3, fd); }
static long write_(long fd, const void *b, u64 n) { return sc3(1, fd, b, n); }
static long read_(long fd, void *b, u64 n) { return sc3(0, fd, b, n); }
static long dup2_(long a, long b) { return sc2(33, a, b); }
static void *mmap_anon(u64 len) { return (void *)sc(9, 0, len, 3, 0x22, -1, 0); }
static void msleep(long ms) { struct { long s, n; } ts = { ms / 1000, (ms % 1000) * 1000000 }; sc2(35, &ts, 0); }
static long futex(volatile int *a, int op, int val, const void *ts) { return sc(202, a, op, val, ts, 0, 0); }

/* wait for a child (any pid when pid == -1) with a timeout: wait4(WNOHANG) polling */
static long wait_for(long pid, int *st, int ms)
{
    for (int i = 0; i < ms / 5 + 1; i++) {
        long r = wait4_(pid, st, 1 /* WNOHANG */, 0);
        if (r != 0) return r;
        msleep(5);
    }
    return 0;
}

/* ---------------------------------------------------------------- clone for threads (as exec_test.c) */
long xclone(int (*fn)(void *), void *stack, long flags, void *arg, void *ptid, void *tls, void *ctid);
__asm__(
".text\n.globl xclone\nxclone:\n"
"  push %r12\n  push %r13\n"
"  mov %rdi,%r12\n  mov %rcx,%r13\n"
"  mov 24(%rsp),%rax\n"
"  and $-16,%rsi\n"
"  mov %rsi,%r10\n  mov %rdx,%r11\n"
"  mov $56,%edi\n  mov %r11,%rsi\n  mov %r10,%rdx\n  mov %r8,%rcx\n  mov %rax,%r8\n"
"  mov xsc(%rip),%rax\n"
"  push $0\n  push $0\n"
"  call *%rax\n"
"  add $16,%rsp\n"
"  test %eax,%eax\n  jnz 1f\n"
"  mov %r13,%rdi\n  xor %ebp,%ebp\n  call *%r12\n"
"  mov %eax,%esi\n  mov $60,%edi\n  mov xsc(%rip),%rax\n  push $0\n  push $0\n  call *%rax\n  ud2\n"
"1: pop %r13\n  pop %r12\n  ret\n");

struct tls { struct tls *self; u64 pad[7]; };
static volatile int spin_flag, futex_word, ctid_[3];
static int thr_spin(void *a) { (void)a; for (;;) spin_flag++; }
static int thr_futex(void *a) { (void)a; for (;;) futex(&futex_word, 0, 0, 0); }

/* ---------------------------------------------------------------- signals */
struct ksa { void *h; u64 flags; void *restorer; u64 mask; };
void sig_restorer(void);
__asm__(".text\n.globl sig_restorer\nsig_restorer:\n"
"  mov %rsp,%rsi\n  and $-16,%rsp\n  sub $16,%rsp\n  movq $0,(%rsp)\n  mov $15,%edi\n"
"  xor %edx,%edx\n  xor %ecx,%ecx\n  xor %r8d,%r8d\n  xor %r9d,%r9d\n  mov xsc(%rip),%rax\n  call *%rax\n  hlt\n");
static void sigaction_(int sig, void (*h)(int))
{
    struct ksa a = { (void *)h, 0x4 | 0x04000000 | 0x10000000, (void *)sig_restorer, 0 };   /* SA_SIGINFO|SA_RESTORER|SA_RESTART */
    sc4(13, sig, &a, 0, 8);
}
static volatile int got_usr1, got_chld;
static void on_usr1(int s) { (void)s; got_usr1++; }
static void on_chld(int s) { (void)s; got_chld++; }

/* ---------------------------------------------------------------- the children */
static char *g_self;
static char **g_envp;

static long spawn(char **av)   /* fork + execve of this program; the child's execve failure is its exit code 127 */
{
    long pid = fork_();
    if (pid == 0) { long r = execve_(g_self, av, g_envp); exit_(r == -12 ? 42 : 127); }
    return pid;
}

static volatile long g_val;   /* defined with its value below */
static int role_ok(int cond) { return cond ? 0 : 1; }

static void run_role(int argc, char **argv)
{
    const char *role = argv[1];
    if (seq(role, "exit")) exit_(atol_(argv[2]));
    if (seq(role, "sleep")) { msleep(20000); exit_(99); }
    if (seq(role, "writer")) { long fd = atol_(argv[2]); write_(fd, "from-exec-child", 15); exit_(0); }
    if (seq(role, "ids")) {   /* argv[2] = the parent's pid, argv[3] = the pid fork returned (== our pid) */
        long pg = sc1(121, 0), sid = sc1(124, 0);
        exit_(role_ok(getppid_() == atol_(argv[2]) && getpid_() == atol_(argv[3]) && getpid_() != getppid_() && pg > 0 && sid > 0));
    }
    if (seq(role, "chkfd")) {   /* argv[2] = a CLOEXEC fd (must be gone), argv[3] = a plain fd (must work) */
        long cl = sc(72, atol_(argv[2]), 1, 0, 0, 0, 0);
        long pl = write_(atol_(argv[3]), "k", 1);
        exit_(cl == -9 ? (pl == 1 ? 0 : 2) : 1);
    }
    if (seq(role, "stage")) {   /* a pipeline stage: fd 0 -> fd 1 */
        char b[64];
        if (seq(argv[2], "gen")) { write_(1, "abc\n", 4); exit_(0); }
        if (seq(argv[2], "upper")) {
            long n = read_(0, b, sizeof b);
            for (long i = 0; i < n; i++) if (b[i] >= 'a' && b[i] <= 'z') b[i] -= 32;
            write_(1, b, n > 0 ? n : 0);
            exit_(0);
        }
        long n = read_(0, b, sizeof b);   /* collect: the last stage compares */
        exit_(role_ok(n == 4 && b[0] == 'A' && b[1] == 'B' && b[2] == 'C' && b[3] == '\n'));
    }
    if (seq(role, "sigusr1")) {   /* waits for SIGUSR1 sent by another process */
        sigaction_(10, on_usr1);
        for (int i = 0; i < 400 && !got_usr1; i++) msleep(10);
        exit_(got_usr1 == 1 ? 0 : 5);
    }
    if (seq(role, "orphan")) {   /* forks a grandchild that reports its parent's pid later, then exits at once */
        long pid = fork_();
        if (pid == 0) {
            char fd[24]; ltoa_(atol_(argv[2]), fd);
            char *av[] = { g_self, "late-ppid", fd, 0 };
            execve_(g_self, av, g_envp); exit_(127);
        }
        exit_(0);
    }
    if (seq(role, "late-ppid")) {   /* after the real parent is gone: write our ppid to the pipe */
        msleep(400);
        long pp = getppid_();
        write_(atol_(argv[2]), &pp, 8);
        exit_(0);
    }
    if (seq(role, "setpgrp")) {   /* argv[2]: the group to join (0 = a new one), then sleep: the driver kills the group */
        sc2(109, 0, atol_(argv[2]));
        msleep(20000);
        exit_(99);
    }
    if (seq(role, "setsid")) {
        long r = sc(112, 0, 0, 0, 0, 0, 0);
        exit_(role_ok(r == getpid_() && sc1(124, 0) == r && sc1(121, 0) == r && sc(112, 0, 0, 0, 0, 0, 0) < 0));
    }
    if (seq(role, "mt")) {   /* a process with threads that calls exit_group */
        long flags = 0x100 | 0x200 | 0x400 | 0x800 | 0x10000 | 0x40000 | 0x80000 | 0x200000 | 0x100000;
        int (*fns[2])(void *) = { thr_spin, thr_futex };
        for (int i = 0; i < 2; i++) {
            u8 *stk = mmap_anon(256 * 1024);
            struct tls *t = mmap_anon(4096); t->self = t;
            ctid_[i] = -1;
            long r = xclone(fns[i], stk + 256 * 1024 - 64, flags, 0, (void *)&ctid_[i], t, (void *)&ctid_[i]);
            if (r <= 0) exit_(98);
        }
        msleep(100);
        exit_(9);
    }
    if (seq(role, "stop")) { msleep(20000); exit_(99); }
    if (seq(role, "mtfork")) {   /* a multi-threaded process forks twice (no exec, then exec): its threads keep running */
        long flags = 0x100 | 0x200 | 0x400 | 0x800 | 0x10000 | 0x40000 | 0x80000 | 0x200000 | 0x100000;
        int (*fns[2])(void *) = { thr_spin, thr_futex };
        for (int i = 0; i < 2; i++) {
            u8 *stk = mmap_anon(256 * 1024);
            struct tls *t = mmap_anon(4096); t->self = t;
            ctid_[i] = -1;
            if (xclone(fns[i], stk + 256 * 1024 - 64, flags, 0, (void *)&ctid_[i], t, (void *)&ctid_[i]) <= 0) exit_(98);
        }
        msleep(100);
        g_val = 1;
        int st;
        long pid = fork_();
        if (pid == 0) { g_val = 5; exit_(role_ok(g_val == 5)); }
        long w = wait4_(pid, &st, 0, 0);
        if (w != pid || st != 0 || g_val != 1) exit_(1);
        int before = spin_flag;
        msleep(100);
        if (spin_flag == before) exit_(2);   /* the spinning thread was not resumed */
        char *av[] = { g_self, "exit", "4", 0 };
        pid = fork_();
        if (pid == 0) { execve_(g_self, av, g_envp); exit_(127); }
        w = wait4_(pid, &st, 0, 0);
        exit_(w == pid && st == (4 << 8) ? 0 : 3);
    }
    if (seq(role, "daemon")) {   /* a daemon start: fork; the child starts a thread at once and runs on; the parent only exits */
        long pid = fork_();
        if (pid == 0) {
            u8 *stk = mmap_anon(256 * 1024);
            struct tls *t = mmap_anon(4096); t->self = t;
            long flags = 0x100 | 0x200 | 0x400 | 0x800 | 0x10000 | 0x40000 | 0x80000 | 0x200000 | 0x100000;
            ctid_[2] = -1;
            if (xclone(thr_futex, stk + 256 * 1024 - 64, flags, 0, (void *)&ctid_[2], t, (void *)&ctid_[2]) <= 0) exit_(98);
            msleep(300);
            exit_(0);
        }
        exit_(pid > 0 ? 0 : 1);
    }
    if (seq(role, "interleave")) {   /* a forked child that blocks lets the parent run; each keeps its own memory */
        int p[2]; pipe2_(p, 0);
        int st;
        g_val = 1;
        long pid = fork_();
        if (pid == 0) {
            msleep(300);
            write_(p[1], "C", 1);
            exit_(role_ok(g_val == 1));   /* the parent's later write must not show here */
        }
        g_val = 7;                        /* runs while the child sleeps */
        write_(p[1], "P", 1);
        long w = wait4_(pid, &st, 0, 0);
        char b[4] = { 0, 0, 0, 0 };
        long n = read_(p[0], b, 2);
        exit_(w == pid && st == 0 && g_val == 7 && n == 2 && b[0] == 'P' && b[1] == 'C' ? 0 : 1);
    }
    if (seq(role, "sigpipe")) {   /* write to a pipe nobody reads: SIGPIPE kills us (status 13) */
        int p[2]; pipe2_(p, 0);
        close_(p[0]);
        write_(p[1], "x", 1);
        exit_(99);
    }
    if (seq(role, "epipe")) {     /* ... and with SIGPIPE ignored: -EPIPE */
        struct ksa ign = { (void *)1, 0, 0, 0 };
        sc4(13, 13, &ign, 0, 8);
        int p[2]; pipe2_(p, 0);
        close_(p[0]);
        exit_(role_ok(write_(p[1], "x", 1) == -32));
    }
    if (seq(role, "pdparent")) {   /* starts a child that asked for SIGUSR1 when its parent ends, reports its pid, ends */
        long pid = fork_();
        if (pid == 0) { char *av[] = { g_self, "pdchild", 0 }; execve_(g_self, av, g_envp); exit_(127); }
        write_(atol_(argv[2]), &pid, 8);
        msleep(300);
        exit_(0);
    }
    if (seq(role, "pdchild")) { sc(157, 1 /* PR_SET_PDEATHSIG */, 10, 0, 0, 0, 0); msleep(20000); exit_(99); }
    (void)argc;
    exit_(120);
}

/* read a whole /proc file into buf (NUL terminated) */
static long slurp(const char *path, char *buf, u64 cap)
{
    long fd = sc3(2, path, 0, 0);
    if (fd < 0) return fd;
    long n = 0, r;
    while (n < (long)cap - 1 && (r = read_(fd, buf + n, cap - 1 - n)) > 0) n += r;
    buf[n] = 0;
    close_(fd);
    return n;
}
static void pidpath(char *o, long pid, const char *leaf)
{
    const char *p = "/proc/";
    int n = 0;
    while (*p) o[n++] = *p++;
    char t[24]; ltoa_(pid, t);
    for (char *q = t; *q; q++) o[n++] = *q;
    o[n++] = '/';
    while (*leaf) o[n++] = *leaf++;
    o[n] = 0;
}
static u64 first_map(const char *maps, const char *needle)   /* start address of the first line mentioning needle */
{
    const char *p = maps;
    while (*p) {
        const char *e = p; while (*e && *e != '\n') e++;
        int hit = 0;
        for (const char *q = p; q + slen(needle) <= e; q++) { unsigned long i = 0; while (needle[i] && q[i] == needle[i]) i++; if (!needle[i]) { hit = 1; break; } }
        if (hit) { u64 v = 0; for (; (*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f'); p++) v = v * 16 + (*p <= '9' ? *p - '0' : *p - 'a' + 10); return v; }
        p = *e ? e + 1 : e;
    }
    return 0;
}

/* data the fork tests look at (.data/.bss, the heap-like mmap, the stack) */
static volatile long g_val = 1;
static volatile long vflag;

static void driver(void)
{
    out("procs_test driver\n");
    long me = getpid_();
    int st;

    /* ---- fork + exec + wait, exit codes ------------------------------------------------- */
    out("fork/exec/wait:\n");
    {
        char *av[] = { g_self, "exit", "7", 0 };
        long pid = spawn(av);
        CHECK(pid > 0 && pid != me, "fork returns the child's pid in the parent");
        long w = wait4_(pid, &st, 0, 0);
        CHECKV(w == pid && st == (7 << 8), "wait4 returns that pid with exit status 7", st);
        CHECK(wait4_(-1, &st, 0, 0) == -10, "wait4 with no children left -> -ECHILD");
    }
    {
        long codes[3] = { 0, 1, 200 };
        for (int i = 0; i < 3; i++) {
            char c[8]; ltoa_(codes[i], c);
            char *av[] = { g_self, "exit", c, 0 };
            long pid = spawn(av);
            long w = wait4_(pid, &st, 0, 0);
            CHECKV(w == pid && st == (int)(codes[i] << 8), "exit codes 0, 1, 200 come back in the wait status", st);
        }
    }
    {
        char *av[] = { g_self, "exit", "3", 0 };
        long pid = spawn(av);
        u8 info[128];
        memset(info, 0xff, sizeof info);
        long r = sc(247, 1 /* P_PID */, pid, info, 4 | 0x01000000 /* WEXITED|WNOWAIT */, 0, 0);
        CHECKV(r == 0 && *(int *)(info + 16) == pid && *(int *)(info + 8) == 1 /* CLD_EXITED */ && *(int *)(info + 24) == 3,
               "waitid(WNOWAIT) reports the exit and leaves the zombie", *(int *)(info + 24));
        char path[64], buf[2048];
        pidpath(path, pid, "status");
        slurp(path, buf, sizeof buf);
        CHECK(sfind(buf, "State:\tZ") != 0, "/proc/<pid>/status of the zombie says State Z");
        long w = wait4_(pid, &st, 0, 0);
        CHECKV(w == pid && st == (3 << 8), "the zombie is reaped by wait4", st);
    }
    {
        char *av[] = { g_self, "ids", 0, 0, 0 };
        char a2[24], a3[24];
        ltoa_(me, a2);
        av[2] = a2; av[3] = a3;
        /* the child puts its own pid in argv (a3) and the program checks that against getpid() after the exec */
        long pid = fork_();
        if (pid == 0) {
            ltoa_(getpid_(), a3);
            execve_(g_self, av, g_envp);
            exit_(127);
        }
        long w = wait4_(pid, &st, 0, 0);
        CHECKV(w == pid && st == 0, "getpid/getppid/getpgid/getsid in the child are the right ones", st);
    }

    /* ---- fork without exec: the parent's memory is its own ------------------------------ */
    out("fork without exec (memory isolation):\n");
    {
        volatile char *heap = mmap_anon(64 * 1024), *gone = mmap_anon(64 * 1024);
        heap[0] = 1; gone[0] = 77;
        volatile long local = 1;
        g_val = 1;
        long pid = fork_();
        if (pid == 0) {
            g_val = 2; heap[0] = 9; local = 3; gone[0] = 55;
            volatile char *fresh = mmap_anon(64 * 1024); fresh[0] = 1;
            sc2(11, (long)gone, 64 * 1024);                       /* the child unmaps one of the parent's mappings */
            exit_(role_ok(g_val == 2 && heap[0] == 9 && local == 3));
        }
        long w = wait4_(pid, &st, 0, 0);
        CHECKV(w == pid && st == 0, "the child saw its own writes", st);
        CHECKV(g_val == 1, "the parent's .data is untouched", g_val);
        CHECKV(heap[0] == 1, "the parent's mmap memory is untouched", heap[0]);
        CHECKV(local == 1, "the parent's stack frame is untouched", local);
        CHECKV(gone[0] == 77, "a mapping the child unmapped is back with its contents", gone[0]);
    }
    {   /* fork without exec in a loop: nothing leaks into the parent (shell-like) */
        int ok = 1;
        for (int i = 0; i < 20; i++) {
            long pid = fork_();
            if (pid == 0) { g_val = 100 + i; exit_(i & 63); }
            long w = wait4_(pid, &st, 0, 0);
            if (w != pid || st != ((i & 63) << 8) || g_val != 1) ok = 0;
        }
        CHECK(ok, "20 forked children that only exit: statuses right, parent memory untouched");
    }

    /* ---- vfork: the memory is shared ----------------------------------------------------- */
    out("vfork:\n");
    {
        vflag = 0;
        long pid = vfork_();
        if (pid == 0) { vflag = 5; exit_(3); }
        long w = wait4_(pid, &st, 0, 0);
        CHECKV(w == pid && st == (3 << 8), "vfork child exits with 3", st);
        CHECKV(vflag == 5, "what a vfork child writes the parent sees", vflag);
    }
    {
        char *av[] = { g_self, "exit", "4", 0 };
        long pid = vfork_();
        if (pid == 0) { execve_(g_self, av, g_envp); exit_(127); }
        long w = wait4_(pid, &st, 0, 0);
        CHECKV(w == pid && st == (4 << 8), "vfork + execve", st);
    }

    /* ---- pipes between processes --------------------------------------------------------- */
    out("pipes:\n");
    {
        int p[2]; pipe2_(p, 0);
        long pid = fork_();
        if (pid == 0) { close_(p[0]); write_(p[1], "hello", 5); exit_(0); }
        close_(p[1]);
        char b[16]; memset(b, 0, sizeof b);
        long n = read_(p[0], b, sizeof b);
        CHECKV(n == 5 && seq(b, "hello"), "the forked child's write arrives", n);
        long z = read_(p[0], b, sizeof b);
        CHECKV(z == 0, "EOF once the child has exited and the parent closed its write end", z);
        wait4_(pid, &st, 0, 0);
        close_(p[0]);
    }
    {
        int p[2]; pipe2_(p, 0);
        char fd[24]; ltoa_(p[1], fd);
        char *av[] = { g_self, "writer", fd, 0 };
        long pid = spawn(av);
        close_(p[1]);
        char b[32]; memset(b, 0, sizeof b);
        long n = read_(p[0], b, sizeof b);
        CHECKV(n == 15 && seq(b, "from-exec-child"), "an fd inherited across fork+exec works", n);
        wait4_(pid, &st, 0, 0);
        close_(p[0]);
    }
    {   /* a pipeline of three processes: gen | upper | collect, wired with dup2 */
        int a[2], b[2];
        pipe2_(a, 0); pipe2_(b, 0);
        long pids[3];
        for (int i = 0; i < 3; i++) {
            const char *name = i == 0 ? "gen" : i == 1 ? "upper" : "collect";
            char *av[] = { g_self, "stage", (char *)name, 0 };
            long pid = fork_();
            if (pid == 0) {
                if (i > 0) dup2_(i == 1 ? a[0] : b[0], 0);
                if (i < 2) dup2_(i == 0 ? a[1] : b[1], 1);
                close_(a[0]); close_(a[1]); close_(b[0]); close_(b[1]);
                execve_(g_self, av, g_envp);
                exit_(127);
            }
            pids[i] = pid;
        }
        close_(a[0]); close_(a[1]); close_(b[0]); close_(b[1]);
        int good = 1;
        for (int i = 0; i < 3; i++) { long w = wait4_(pids[i], &st, 0, 0); if (w != pids[i] || st != 0) good = 0; }
        CHECK(good, "pipeline of 3 processes (dup2 + exec): gen | upper | collect, all exit 0");
    }

    /* ---- CLOEXEC ------------------------------------------------------------------------- */
    out("CLOEXEC:\n");
    {
        int cl[2], pl[2];
        pipe2_(cl, 02000000);
        pipe2_(pl, 0);
        char a2[24], a3[24];
        ltoa_(cl[1], a2); ltoa_(pl[1], a3);
        char *av[] = { g_self, "chkfd", a2, a3, 0 };
        long pid = spawn(av);
        long w = wait4_(pid, &st, 0, 0);
        CHECKV(w == pid && st == 0, "O_CLOEXEC fd closed at execve, plain fd kept", st);
        char c = 0;
        CHECK(read_(pl[0], &c, 1) == 1 && c == 'k', "...and the plain fd's pipe carried the child's byte");
        CHECK(sc(72, cl[1], 1, 0, 0, 0, 0) == 1, "the parent's O_CLOEXEC flag is still set in the parent");
        close_(cl[0]); close_(cl[1]); close_(pl[0]); close_(pl[1]);
    }

    /* ---- signals between processes -------------------------------------------------------- */
    out("signals:\n");
    {
        char *av[] = { g_self, "sleep", 0 };
        long pid = spawn(av);
        msleep(300);
        CHECK(wait4_(pid, &st, 1, 0) == 0, "wait4(WNOHANG) on a running child -> 0");
        char path[64], buf[4096];
        pidpath(path, pid, "cmdline");
        long cl = slurp(path, buf, sizeof buf);
        for (long i = 0; i < cl; i++) if (!buf[i]) buf[i] = ' ';   /* arguments are NUL separated */
        CHECK(sfind(buf, "procs_test.elf sleep") != 0, "/proc/<pid>/cmdline of the child");
        pidpath(path, pid, "status");
        slurp(path, buf, sizeof buf);
        CHECK(sfind(buf, "State:\tS") != 0, "/proc/<pid>/status says it is sleeping");
        CHECK(kill_(pid, 0) == 0, "kill(pid, 0) finds it");
        CHECK(kill_(pid, 15) == 0, "kill(pid, SIGTERM)");
        long w = wait_for(pid, &st, 3000);
        CHECKV(w == pid && st == 15, "the child died of SIGTERM (status 15)", st);
        CHECK(kill_(pid, 0) == -3, "kill of a reaped pid -> -ESRCH");
    }
    {
        char *av[] = { g_self, "sigusr1", 0 };
        long pid = spawn(av);
        msleep(400);
        kill_(pid, 10);
        long w = wait_for(pid, &st, 3000);
        CHECKV(w == pid && st == 0, "SIGUSR1 sent by another process runs the child's handler", st);
    }
    {
        got_chld = 0;
        sigaction_(17, on_chld);
        char *av[] = { g_self, "exit", "0", 0 };
        long pid = spawn(av);
        for (int i = 0; i < 300 && !got_chld; i++) msleep(10);
        CHECKV(got_chld >= 1, "SIGCHLD reaches the parent's handler when a child ends", got_chld);
        wait4_(pid, &st, 0, 0);
        struct ksa dfl = { 0, 0, 0, 0 };
        sc4(13, 17, &dfl, 0, 8);
    }
    {   /* a stop and a continue */
        char *av[] = { g_self, "stop", 0 };
        long pid = spawn(av);
        msleep(300);
        kill_(pid, 19);   /* SIGSTOP */
        long w = wait4_(pid, &st, 2 /* WUNTRACED */, 0);
        CHECKV(w == pid && (st & 0xff) == 0x7f && ((st >> 8) & 0xff) == 19, "SIGSTOP: wait4(WUNTRACED) reports the stop", st);
        kill_(pid, 18);   /* SIGCONT */
        w = wait4_(pid, &st, 8 /* WCONTINUED */, 0);
        CHECKV(w == pid && st == 0xffff, "SIGCONT: wait4(WCONTINUED) reports the continue", st);
        kill_(pid, 9);
        w = wait4_(pid, &st, 0, 0);
        CHECKV(w == pid && st == 9, "SIGKILL ends it (status 9)", st);
    }

    /* ---- process groups and sessions ------------------------------------------------------ */
    out("groups and sessions:\n");
    {
        long pids[2];
        for (int i = 0; i < 2; i++) {
            char pg[24]; ltoa_(i == 0 ? 0 : pids[0], pg);   /* the second joins the first's group */
            char *av[] = { g_self, "setpgrp", pg, 0 };
            long pid = fork_();
            if (pid == 0) {
                execve_(g_self, av, g_envp);
                exit_(127);
            }
            pids[i] = pid;
            if (i == 0) msleep(200);
        }
        msleep(200);
        CHECKV(sc1(121, pids[0]) == pids[0], "setpgid(0, 0) in the child: getpgid(child) == child", sc1(121, pids[0]));
        long r = kill_(-pids[0], 15);
        CHECKV(r == 0, "kill(-pgid, SIGTERM)", r);
        int good = 0;
        for (int i = 0; i < 2; i++) { long w = wait_for(pids[i], &st, 3000); if (w == pids[i] && st == 15) good++; }
        CHECKV(good == 2, "both members of the group died of SIGTERM", good);
    }
    {
        char *av[] = { g_self, "setsid", 0 };
        long pid = spawn(av);
        long w = wait4_(pid, &st, 0, 0);
        CHECKV(w == pid && st == 0, "setsid: new session, leader of its group, a second setsid fails", st);
    }

    /* ---- orphans, threads, address spaces -------------------------------------------------- */
    out("orphans, threads, address spaces:\n");
    {
        int p[2]; pipe2_(p, 0);
        char fd[24]; ltoa_(p[1], fd);
        char *av[] = { g_self, "orphan", fd, 0 };
        long pid = spawn(av);
        close_(p[1]);
        wait4_(pid, &st, 0, 0);
        long pp = 0;
        long n = read_(p[0], &pp, 8);
        CHECKV(n == 8 && pp == me, "an orphan is reparented to the init process (its getppid is the init's pid)", pp);
        close_(p[0]);
        long g = wait_for(-1, &st, 3000);   /* the grandchild is now our child: it can be waited for */
        CHECKV(g > 0 && st == 0, "...which can wait4 for it", g);
    }
    {
        char *av[] = { g_self, "mt", 0 };
        long pid = spawn(av);
        long w = wait_for(pid, &st, 5000);
        CHECKV(w == pid && st == (9 << 8), "exit_group of a process with several threads ends it with the status", st);
    }
    {
        char *a1[] = { g_self, "sleep", 0 }, *a2[] = { g_self, "sleep", 0 };
        long p1 = spawn(a1), p2 = spawn(a2);
        msleep(500);
        char path[64], m1[8192], m2[8192], mine[8192];
        pidpath(path, p1, "maps"); slurp(path, m1, sizeof m1);
        pidpath(path, p2, "maps"); slurp(path, m2, sizeof m2);
        slurp("/proc/self/maps", mine, sizeof mine);
        u64 b1 = first_map(m1, "procs_test"), b2 = first_map(m2, "procs_test"), b0 = first_map(mine, "procs_test");
        CHECK(b1 && b2 && b0 && b1 != b2 && b1 != b0 && b2 != b0, "three processes of the same PIE program load at three different bases");
        CHECK(sfind(m1, "[stack]") != 0 && !sfind(m1, "[heap]x"), "/proc/<pid>/maps lists that process's own mappings");
        kill_(p1, 9); kill_(p2, 9);
        wait_for(p1, &st, 3000); wait_for(p2, &st, 3000);
    }
    {   /* a non-PIE image whose fixed range is taken */
        char *av[] = { "/bin/procs_nopie.elf", "sleep", 0 };
        long a = fork_();
        if (a == 0) { long r = execve_("/bin/procs_nopie.elf", av, g_envp); exit_(r == 0 ? 0 : 127); }
        msleep(500);
        long b = fork_();
        if (b == 0) { long r = execve_("/bin/procs_nopie.elf", av, g_envp); exit_(r == -12 ? 42 : 127); }
        long w = wait_for(b, &st, 3000);
        CHECKV(w == b && st == (42 << 8), "a second copy of a non-PIE program while the first lives: execve -> -ENOMEM", st);
        kill_(a, 9);
        wait_for(a, &st, 3000);
        long c = fork_();
        if (c == 0) { char *av2[] = { "/bin/procs_nopie.elf", "exit", "6", 0 }; execve_("/bin/procs_nopie.elf", av2, g_envp); exit_(127); }
        w = wait_for(c, &st, 3000);
        CHECKV(w == c && st == (6 << 8), "...and it runs again once the first is gone", st);
    }
    out("pipes, SIGPIPE, parent-death signal:\n");
    {
        char *av[] = { g_self, "sigpipe", 0 };
        long pid = spawn(av);
        long w = wait_for(pid, &st, 3000);
        CHECKV(w == pid && st == 13, "write to a pipe with no reader kills the writer with SIGPIPE", st);
        char *av2[] = { g_self, "epipe", 0 };
        pid = spawn(av2);
        w = wait_for(pid, &st, 3000);
        CHECKV(w == pid && st == 0, "...and with SIGPIPE ignored the write returns -EPIPE", st);
    }
    {
        int p[2]; pipe2_(p, 0);
        char fd[24]; ltoa_(p[1], fd);
        char *av[] = { g_self, "pdparent", fd, 0 };
        long pid = spawn(av);
        close_(p[1]);
        long child = 0;
        long n = read_(p[0], &child, 8);
        close_(p[0]);
        long w = wait_for(pid, &st, 3000);
        CHECKV(n == 8 && w == pid && st == 0, "the parent ends normally", st);
        long w2 = wait_for(child, &st, 3000);
        CHECKV(w2 == child && st == 10, "PR_SET_PDEATHSIG: the orphan got SIGUSR1 when its parent ended (and is the init's child now)", st);
    }
    {
        long r = sc(56, 0x20000000 | 17 /* CLONE_NEWPID|SIGCHLD */, 0, 0, 0, 0, 0);
        CHECKV(r == -1, "clone with namespace flags -> -EPERM", r);
    }
    out("fork from a threaded process, interleaved parent and child:\n");
    {
        char *av[] = { g_self, "mtfork", 0 };
        long pid = spawn(av);
        long w = wait_for(pid, &st, 15000);
        CHECKV(w == pid && st == 0, "a multi-threaded process forks (no exec, exec) and its threads keep running", st);
        char *av2[] = { g_self, "interleave", 0 };
        pid = spawn(av2);
        w = wait_for(pid, &st, 15000);
        CHECKV(w == pid && st == 0, "forked child blocks, parent runs and writes first, each keeps its memory", st);
        /* Firefox's crashhelper: the parent of a child that went multi-threaded right after the fork still gets to exit */
        char *av3[] = { g_self, "daemon", 0 };
        pid = spawn(av3);
        w = wait_for(pid, &st, 5000);
        CHECKV(w == pid && st == 0, "daemon start: the child starts a thread at once, its parent still exits", st);
        wait_for(-1, &st, 3000);   /* the orphan, reparented to us */
    }
    CHECK(wait4_(-1, &st, 1, 0) == -10, "no children left at the end");
    outn("TOTAL FAILS", fails);
    exit_(fails);
}

/* ---------------------------------------------------------------- start-up (static PIE: relocate ourselves) */
#ifdef NOPIE
static void relocate(void) {}   /* linked at 0x400000: nothing to do */
#else
static void relocate(void)
{
    u64 *got, dynamic;
    __asm__("lea _GLOBAL_OFFSET_TABLE_(%%rip),%0" : "=r"(got));
    __asm__("lea _DYNAMIC(%%rip),%0" : "=r"(dynamic));   /* the run-time address, no GOT slot (not relocated yet) */
    u64 base = dynamic - got[0];
    struct dyn { long tag; u64 val; } *d = (struct dyn *)dynamic;
    u64 rela = 0, relasz = 0;
    for (; d->tag; d++) { if (d->tag == 7) rela = d->val; else if (d->tag == 8) relasz = d->val; }
    struct rel { u64 off, info; long addend; } *r = (struct rel *)(rela + base);
    for (u64 i = 0; i < relasz / sizeof *r; i++)
        if ((r[i].info & 0xffffffffu) == 8) *(u64 *)(base + r[i].off) = base + r[i].addend;
}
#endif

__asm__(".text\n.globl _start\n_start:\n  xor %ebp,%ebp\n  mov %rsp,%rdi\n  and $-16,%rsp\n  call cstart\n  ud2\n");
void cstart(u64 *sp)
{
    relocate();
    long argc = (long)sp[0];
    char **argv = (char **)(sp + 1), **envp = argv + argc + 1;
    u64 *p = (u64 *)envp;
    while (*p) p++;
    p++;
    for (; p[0]; p += 2) if (p[0] == AT_XRL_SYSCALL) xsc = (xsc_t)p[1];
    if (!xsc) { __asm__ volatile("ud2"); }
    g_self = argv[0];
    g_envp = envp;
    if (argc > 1 && !seq(argv[1], "--fake-host")) run_role((int)argc, argv);
    else driver();
    for (;;) {}
}
