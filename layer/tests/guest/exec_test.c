/*
 * exec_test.c - execve in the layer, plain Linux semantics (freestanding guest, like guest.c).
 *
 *   exec_test.elf            "parent": sets up state, starts threads that must die, calls
 *                            execve() on this same file with argv[1] = "child"
 *   exec_test.elf child ...  the new program: checks what an exec must and must not keep
 *
 * Exit code = number of failed checks. Run by layer/tests/exec-test.sh.
 */
typedef unsigned long u64; typedef long s64; typedef unsigned int u32; typedef unsigned char u8;
typedef long (*xsc_t)(long, long, long, long, long, long, long);
xsc_t xsc;
#define AT_XRL_SYSCALL 0x5801
#define sc(n, a, b, c, d, e, f) xsc((n), (long)(a), (long)(b), (long)(c), (long)(d), (long)(e), (long)(f))
#define sc1(n, a) sc(n, a, 0, 0, 0, 0, 0)
#define sc2(n, a, b) sc(n, a, b, 0, 0, 0, 0)
#define sc3(n, a, b, c) sc(n, a, b, c, 0, 0, 0)

void *memset(void *d, int c, unsigned long n) { u8 *p = d; while (n--) *p++ = (u8)c; return d; }
void *memcpy(void *d, const void *s, unsigned long n) { u8 *p = d; const u8 *q = s; while (n--) *p++ = *q++; return d; }
static unsigned long slen(const char *s) { unsigned long n = 0; while (s[n]) n++; return n; }
static int seq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
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
static void utoa_(u64 v, char *o) { ltoa_((long)v, o); }
static int fails;
#define CHECK(cond, name) do { if (cond) out("  ok   " name "\n"); else { out("  FAIL " name "\n"); fails++; } } while (0)

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
__asm__(".text\n.globl _start\n_start:\n  xor %ebp,%ebp\n  mov %rsp,%rdi\n  and $-16,%rsp\n  call cstart\n  ud2\n");

static long futex(volatile int *a, int op, int val, const void *ts) { return sc(202, a, op, val, ts, 0, 0); }
static void *mmap_anon(u64 len) { return (void *)sc(9, 0, len, 3, 0x22, -1, 0); }

/* ---- the threads that must not survive the exec ---------------------------------------- */
struct tls { struct tls *self; u64 pad[7]; };
static volatile int spin_flag, futex_word, ctid[3];
static int thr_spin(void *a) { (void)a; for (;;) spin_flag++; }                       /* guest code, never in a syscall */
static int thr_futex(void *a) { (void)a; for (;;) futex(&futex_word, 0, 0, 0); }       /* blocked in the layer */
static int thr_sleep(void *a) { (void)a; for (;;) { long ts[2] = { 30, 0 }; sc2(35, ts, 0); } }  /* nanosleep */

struct ksig { u64 handler, flags, restorer, mask; };
static void on_usr1(int s) { (void)s; }

static long sig_get(int sig) { struct ksig o; sc(13, sig, 0, &o, 8, 0, 0); return (long)o.handler; }

static void parent(char **argv, char **envp)
{
    out("exec_test parent\n");
    (void)argv; (void)envp;
    long pid = sc(39, 0, 0, 0, 0, 0, 0);

    /* things the exec must (not) keep */
    int cl[2], keep[2];
    sc3(293, cl, 02000000, 0);      /* pipe2(O_CLOEXEC) */
    sc3(293, keep, 0, 0);           /* pipe2(0) */
    struct ksig h = { (u64)on_usr1, 0x04000000 /* SA_RESTORER, unused */, 0, 0 };
    sc(13, 10, &h, 0, 8, 0, 0);     /* SIGUSR1: a handler: must reset */
    struct ksig ign = { 1, 0, 0, 0 };
    sc(13, 12, &ign, 0, 8, 0, 0);   /* SIGUSR2: SIG_IGN: must stay */
    u8 *marker = mmap_anon(8192);
    marker[0] = 0x5a;

    long flags = 0x100 | 0x200 | 0x400 | 0x800 | 0x10000 | 0x40000 | 0x80000 | 0x200000 | 0x100000;
    int (*fns[3])(void *) = { thr_spin, thr_futex, thr_sleep };
    for (int i = 0; i < 3; i++) {
        u8 *stk = mmap_anon(256 * 1024);
        struct tls *t = mmap_anon(4096); t->self = t;
        ctid[i] = -1;
        long r = xclone(fns[i], stk + 256 * 1024 - 64, flags, 0, (void *)&ctid[i], t, (void *)&ctid[i]);
        if (r <= 0) { outn("clone failed", r); sc1(231, 99); }
    }
    { struct { long s, n; } ts = { 0, 300000000 }; sc2(35, &ts, 0); }   /* let them settle in their loops */
    CHECK(spin_flag > 0, "a spinning thread, a futex waiter and a sleeper are running");

    /* failures return an errno and leave everything alone */
    {
        char *a0[] = { "x", 0 };
        CHECK(sc(59, "/bin/does-not-exist", a0, envp, 0, 0, 0) == -2, "execve of a missing file -> -ENOENT, we are still here");
        long fd = sc3(2, "/notelf", 0101 | 0200 /* O_CREAT|O_WRONLY */, 0755);
        const char txt[] = "This is a text file, not an ELF image, but long enough to be read as a header ..........";
        sc3(1, fd, txt, sizeof txt); sc1(3, fd);
        CHECK(sc(59, "/notelf", a0, envp, 0, 0, 0) == -8, "execve of a non-ELF file -> -ENOEXEC");
    }

    char pids[24], ma[24], k0[24], t0[24], t1[24], t2[24];
    ltoa_(pid, pids); utoa_((u64)marker, ma); ltoa_(keep[0], k0);
    ltoa_(ctid[0], t0); ltoa_(ctid[1], t1); ltoa_(ctid[2], t2);
    char kc[24]; ltoa_(cl[0], kc);
    char kw[24]; ltoa_(keep[1], kw);
    char *av[] = { "exec_test.elf", "child", pids, ma, k0, kw, kc, t0, t1, t2, "a b", 0 };
    char *ev[] = { "XRL_EXEC_TEST=1", "PATH=/bin", 0 };
    long r = sc(59, "/bin/exec_test.elf", av, ev, 0, 0, 0);
    outn("execve returned", r);
    fails++;
    sc1(231, 100 + fails);
}

static void child(long argc, char **argv, char **envp)
{
    out("exec_test child (the new program)\n");
    CHECK(argc == 11, "argc");
    CHECK(seq(argv[0], "exec_test.elf") && seq(argv[1], "child") && seq(argv[10], "a b"), "argv carried over (an argument with a space)");
    CHECK(envp[0] && seq(envp[0], "XRL_EXEC_TEST=1") && envp[1] && seq(envp[1], "PATH=/bin") && !envp[2], "envp is exactly what execve was given");
    CHECK(sc(39, 0, 0, 0, 0, 0, 0) == atol_(argv[2]), "getpid() unchanged");
    long marker = (long)atol_(argv[3]);
    char v;
    (void)v;
    /* unmapped, or (the allocator may hand the address out again) fresh zero pages: never the old data */
    CHECK(sc3(10, marker, 4096, 3) == -12 || *(volatile u8 *)marker != 0x5a, "the old image's anonymous memory is gone");
    long keep_r = atol_(argv[4]), keep_w = atol_(argv[5]), cl_r = atol_(argv[6]);
    CHECK(sc(72, cl_r, 1, 0, 0, 0, 0) == -9, "a CLOEXEC descriptor was closed");
    CHECK(sc(72, keep_r, 1, 0, 0, 0, 0) == 0, "a plain descriptor was inherited");
    CHECK(sc3(1, keep_w, "x", 1) == 1, "...and works");
    { char c = 0; CHECK(sc3(0, keep_r, &c, 1) == 1 && c == 'x', "...reads back what was written"); }
    CHECK(sig_get(10) == 0, "a signal handler was reset to the default");
    CHECK(sig_get(12) == 1, "SIG_IGN stays ignored");
    for (int i = 0; i < 3; i++) {
        long tid = atol_(argv[7 + i]);
        CHECK(sc(234, sc(39, 0, 0, 0, 0, 0, 0), tid, 0, 0, 0, 0) == -3, "the old thread is gone (tgkill -> -ESRCH)");
    }
    outn("TOTAL FAILS", fails);
    sc1(231, fails);
}

void cstart(u64 *sp)
{
    long argc = (long)sp[0];
    char **argv = (char **)(sp + 1), **envp = argv + argc + 1;
    u64 *p = (u64 *)envp;
    while (*p) p++;
    p++;
    for (; p[0]; p += 2) if (p[0] == AT_XRL_SYSCALL) xsc = (xsc_t)p[1];
    if (!xsc) { __asm__ volatile("ud2"); }
    if (argc > 1 && seq(argv[1], "child")) child(argc, argv, envp);
    else parent(argv, envp);
    for (;;) {}
}
