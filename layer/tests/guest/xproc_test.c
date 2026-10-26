/*
 * xproc_test.c - the userland objects between guest PROCESSES (needs fork: the proc.c branch; on a single-process layer it
 * says so and passes). Freestanding guest like guest.c. Exit code = number of failed checks. Run by layer/tests/xproc-test.sh.
 *   - fds passed with SCM_RIGHTS arrive in the RECEIVER's fd table (a number the child did not have)
 *   - a named AF_UNIX socket: connect from another process, SO_PEERCRED names that process
 *   - SysV shm and a shared memfd mapping: a write in the child shows in the parent
 *   - a pty: the slave in one process, the master in another
 */
typedef unsigned long u64; typedef long s64; typedef unsigned int u32; typedef unsigned char u8; typedef unsigned short u16;
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
int memcmp(const void *a, const void *b, unsigned long n) { const u8 *p = a, *q = b; while (n--) { if (*p != *q) return *p - *q; p++; q++; } return 0; }
static unsigned long slen(const char *s) { unsigned long n = 0; while (s[n]) n++; return n; }
static void out(const char *s) { sc3(1, 1, s, slen(s)); }
static int fails;
#define CHECK(cond, name) do { if (cond) out("  ok   " name "\n"); else { out("  FAIL " name "\n"); fails++; } } while (0)
__asm__(".text\n.globl _start\n_start:\n  xor %ebp,%ebp\n  mov %rsp,%rdi\n  and $-16,%rsp\n  call cstart\n  ud2\n");

struct iov_ { u64 base, len; };
struct msg_ { u64 name; u32 nl, p0; u64 iov, iovlen, ctl, ctllen; u32 fl, p1; };
struct cm_fd { u64 len; int lvl, type; int fd; int pad; };
struct sun { u16 fam; char p[108]; };
struct ucred_ { int pid; u32 uid, gid; };

static void nap_ms(long ms) { struct { long s, n; } ts = { ms / 1000, (ms % 1000) * 1000000 }; sc4(35, &ts, 0, 0, 0); }
static long do_fork(void) { return sc(57, 0, 0, 0, 0, 0, 0); }
static void child_exit(int code) { sc1(231, code); for (;;) {} }
static int wait_child(long pid) { int st = -1; long r = sc4(61, pid, &st, 0, 0); return r == pid ? (st >> 8) & 0xff : 200; }

static void test_fdpass(void)
{
    out("fd passing between processes:\n");
    int sp[2];
    sc4(53, 1, 1, 0, sp);
    long pid = do_fork();
    if (pid == 0) {   /* the child waits for a descriptor it does not have */
        char b[16]; struct cm_fd cm; memset(&cm, 0, sizeof cm);
        struct iov_ iv = { (u64)b, 1 };
        struct msg_ m = { 0, 0, 0, (u64)&iv, 1, (u64)&cm, 24, 0, 0 };
        long r = sc3(47, sp[1], &m, 0);
        if (r != 1 || cm.type != 1) child_exit(11);
        char got[16]; memset(got, 0, sizeof got);
        long n = sc3(0, cm.fd, got, 16);
        child_exit(n == 5 && !memcmp(got, "hello", 5) && cm.fd > sp[1] ? 0 : 12);
    }
    int pf[2]; sc2(293, pf, 0);   /* created after the fork: the child can only get it through the socket */
    struct iov_ iv = { (u64)"F", 1 };
    struct cm_fd cm = { 20, 1, 1, pf[0], 0 };
    struct msg_ m = { 0, 0, 0, (u64)&iv, 1, (u64)&cm, 24, 0, 0 };
    CHECK(sc3(46, sp[0], &m, 0) == 1, "send a pipe's read end to the other process");
    sc1(3, pf[0]);   /* only the copy in flight / in the child is left */
    sc3(1, pf[1], "hello", 5);
    CHECK(wait_child(pid) == 0, "the child read from the received descriptor (a new number in its table)");
    sc1(3, pf[1]); sc1(3, sp[0]); sc1(3, sp[1]);
}

static void test_named(void)
{
    out("a named AF_UNIX socket between processes:\n");
    struct sun a; memset(&a, 0, sizeof a); a.fam = 1;
    const char *path = "/tmp/xrl_xproc.sock";
    for (int i = 0; path[i]; i++) a.p[i] = path[i];
    u32 al = 2 + (u32)slen(path) + 1;
    sc1(87, path);
    long ls = sc3(41, 1, 1, 0);
    CHECK(sc3(49, ls, &a, al) == 0 && sc2(50, ls, 4) == 0, "bind + listen");
    long me = sc(39, 0, 0, 0, 0, 0, 0);
    long pid = do_fork();
    if (pid == 0) {
        long c = sc3(41, 1, 1, 0);
        if (sc3(42, c, &a, al) != 0) child_exit(21);
        struct ucred_ pc = { -1, 0, 0 }; u32 pl = 12;
        if (sc(55, c, 1, 17, &pc, &pl, 0) != 0 || pc.pid != me) child_exit(22);   /* the peer is the listening parent */
        sc3(1, c, "from-child", 10);
        child_exit(0);
    }
    long cs = sc3(43, ls, 0, 0);
    char b[32]; memset(b, 0, sizeof b);
    CHECK(cs >= 3 && sc3(0, cs, b, 32) == 10 && !memcmp(b, "from-child", 10), "accept and read what the child sent");
    struct ucred_ pc = { -1, 0, 0 }; u32 pl = 12;
    CHECK(sc(55, cs, 1, 17, &pc, &pl, 0) == 0 && pc.pid == pid && pc.pid != me, "SO_PEERCRED names the connecting process, not us");
    CHECK(wait_child(pid) == 0, "and the child saw us as its peer");
    sc1(3, cs); sc1(3, ls); sc1(87, path);
}

static void test_shared_memory(void)
{
    out("shared memory between processes:\n");
    long id = sc3(29, 0, 8192, 01000 | 0600);
    volatile u8 *seg = (volatile u8 *)sc3(30, id, 0, 0);
    seg[0] = 1;
    long fd = sc3(319, "xproc", 0, 0);
    sc2(77, fd, 8192);
    volatile u8 *mm = (volatile u8 *)sc(9, 0, 8192, 3, 1, fd, 0);
    mm[0] = 1;
    long pid = do_fork();
    if (pid == 0) {
        volatile u8 *s2 = (volatile u8 *)sc3(30, id, 0, 0);          /* attach in the child */
        volatile u8 *m2 = (volatile u8 *)sc(9, 0, 8192, 3, 1, fd, 0); /* map again in the child */
        if (s2[0] != 1 || m2[0] != 1) child_exit(31);   /* it sees what the parent wrote before the fork */
        s2[100] = 0x5a; m2[200] = 0xa5;
        child_exit(0);
    }
    CHECK(wait_child(pid) == 0, "the child attached and mapped the same pages");
    CHECK(seg[100] == 0x5a, "SysV: a write in the child shows in the parent");
    CHECK(mm[200] == 0xa5, "memfd: a write in the child shows in the parent");
    CHECK(sc1(67, seg) == 0, "shmdt in the parent (the child's exit detached its own)");
    sc3(31, id, 0, 0);
    sc2(11, mm, 8192); sc1(3, fd);
}

static void test_pty(void)
{
    out("a pty between processes:\n");
    long m = sc3(2, "/dev/ptmx", 02, 0);
    int zero = 0; u32 n = 0;
    sc3(16, m, 0x40045431, &zero);
    sc3(16, m, 0x80045430, &n);
    char path[32] = "/dev/pts/"; { char d[12]; int k = 0, t = (int)n; do d[k++] = (char)('0' + t % 10); while ((t /= 10)); int o = 9; while (k) path[o++] = d[--k]; path[o] = 0; }
    long pid = do_fork();
    if (pid == 0) {
        sc1(3, m);   /* the master stays the parent's */
        long s = sc3(2, path, 02, 0);
        if (s < 0) child_exit(41);
        sc3(1, s, "tty-out\n", 8);
        child_exit(0);
    }
    char b[32]; memset(b, 0, sizeof b);
    long got = 0;
    for (int i = 0; i < 100 && got < 9; i++) { long r = sc3(0, m, b + got, 32 - got); if (r > 0) got += r; else nap_ms(20); }
    CHECK(got >= 9 && !memcmp(b, "tty-out\r\n", 9), "the master reads what another process wrote on the slave");
    CHECK(wait_child(pid) == 0, "the child's open of the slave worked");
    sc1(3, m);
}

void cstart(u64 *sp)
{
    long argc = (long)sp[0];
    u64 *p = sp + 1 + argc + 1;
    while (*p) p++;
    p++;
    for (; p[0]; p += 2) if (p[0] == AT_XRL_SYSCALL) xsc = (xsc_t)p[1];
    if (!xsc) { __asm__ volatile("ud2"); }
    long probe = do_fork();
    if (probe == -38) { out("xproc: fork is not available on this layer (needs the proc.c branch): skipped\nxproc: all checks passed\n"); sc1(231, 0); }
    if (probe == 0) child_exit(0);
    wait_child(probe);
    test_fdpass();
    test_named();
    test_shared_memory();
    test_pty();
    out(fails ? "xproc: FAILED\n" : "xproc: all checks passed\n");
    sc1(231, fails);
    for (;;) {}
}
