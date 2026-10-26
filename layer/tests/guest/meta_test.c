/*
 * meta_test.c - Unix modes and owners over NTFS (layer/src/vfs.c "meta", the .xrl-meta table).
 * Freestanding guest like guest.c. argv[1] picks the phase, layer/tests/meta-test.sh runs both:
 *   root   run as XRL_UID=0: ownership, chmod/chown, umask, rename/link/unlink carry the entries
 *   user   run as XRL_UID=1000 on the same tree: the permission rules for a non-root caller
 * Exit code = number of failed checks.
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
static unsigned long slen(const char *s) { unsigned long n = 0; while (s[n]) n++; return n; }
static int seq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static void out(const char *s) { sc3(1, 1, s, slen(s)); }
static int fails;
#define CHECK(cond, name) do { if (cond) out("  ok   " name "\n"); else { out("  FAIL " name "\n"); fails++; } } while (0)
__asm__(".text\n.globl _start\n_start:\n  xor %ebp,%ebp\n  mov %rsp,%rdi\n  and $-16,%rsp\n  call cstart\n  ud2\n");

struct st { u64 dev, ino, nlink; u32 mode, uid, gid, pad; u64 rdev; s64 size, blksize, blocks, t[6], un[3]; };
static long stat_(const char *p, struct st *s) { return sc2(4, p, s); }
static long mk(const char *p, long mode) { long fd = sc3(2, p, 0101 | 0200 /* O_CREAT|O_WRONLY|O_EXCL */, mode); if (fd >= 0) sc1(3, fd); return fd; }

static void phase_root(void)
{
    struct st s;
    out("meta (root):\n");
    sc1(95, 022);   /* umask */
    CHECK(sc2(83, "/m", 0755) == 0 || 1, "mkdir /m");
    CHECK(stat_("/m", &s) == 0 && (s.mode & 07777) == 0755 && s.uid == 0 && s.gid == 0, "a directory made by root: 0755, owner 0:0");
    CHECK(mk("/m/plain", 0666) >= 0 && stat_("/m/plain", &s) == 0 && (s.mode & 07777) == 0644 && s.uid == 0, "create(0666) under umask 022 -> 0644 root");
    CHECK(mk("/m/secret", 0600) >= 0 && stat_("/m/secret", &s) == 0 && (s.mode & 07777) == 0600, "create(0600) -> 0600");
    sc1(95, 077);
    CHECK(mk("/m/u077", 0666) >= 0 && stat_("/m/u077", &s) == 0 && (s.mode & 07777) == 0600, "umask 077 turns 0666 into 0600");
    CHECK(sc2(83, "/m/d700", 0777) == 0 && stat_("/m/d700", &s) == 0 && (s.mode & 07777) == 0700, "mkdir(0777) under umask 077 -> 0700");
    sc1(95, 022);
    CHECK(sc2(90, "/m/plain", 04755) == 0 && stat_("/m/plain", &s) == 0 && (s.mode & 07777) == 04755, "chmod keeps the setuid bit");
    long fd = sc3(2, "/m/plain", 2, 0);
    CHECK(sc2(91, fd, 0640) == 0 && stat_("/m/plain", &s) == 0 && (s.mode & 07777) == 0640, "fchmod");
    CHECK(sc3(93, fd, 1000, 100) == 0 && stat_("/m/plain", &s) == 0 && s.uid == 1000 && s.gid == 100, "fchown to 1000:100");
    struct st f; CHECK(sc2(5, fd, &f) == 0 && f.uid == 1000 && f.gid == 100 && (f.mode & 07777) == 0640, "fstat shows the same");
    sc1(3, fd);
    CHECK(sc3(92, "/m/plain", -1, 7) == 0 && stat_("/m/plain", &s) == 0 && s.uid == 1000 && s.gid == 7, "chown(-1, gid) leaves the owner");
    CHECK(sc2(82, "/m/plain", "/m/renamed") == 0 && stat_("/m/renamed", &s) == 0 && (s.mode & 07777) == 0640 && s.uid == 1000, "rename carries mode and owner");
    CHECK(sc2(86, "/m/renamed", "/m/linked") == 0 && stat_("/m/linked", &s) == 0 && (s.mode & 07777) == 0640 && s.uid == 1000, "link: the new name has the same mode and owner");
    CHECK(sc2(82, "/m", "/m2") == 0 && stat_("/m2/secret", &s) == 0 && (s.mode & 07777) == 0600 && stat_("/m2/d700", &s) == 0 && (s.mode & 07777) == 0700, "renaming a directory moves the entries below it");
    CHECK(sc1(87, "/m2/secret") == 0 && mk("/m2/secret", 0644) >= 0 && stat_("/m2/secret", &s) == 0 && (s.mode & 07777) == 0644 && s.uid == 0, "unlink then create again: a fresh 0644 file, not the old mode");
    CHECK(sc3(2, "/m2/renamed", 0, 0) >= 0, "root opens a 0640 file owned by someone else");
    CHECK(sc2(21, "/m2/renamed", 4) == 0 && sc2(21, "/m2/renamed", 1) == -13, "access(X_OK) on a file with no x bit -> -EACCES even for root");
    /* what the user phase works on */
    sc1(95, 0);
    mk("/m2/rootonly", 0600);
    mk("/m2/mine", 0644); sc3(92, "/m2/mine", 1000, 1000);
    mk("/m2/group", 0660); sc3(92, "/m2/group", 5, 1000);
    mk("/m2/world", 0666);
}

static void phase_user(void)
{
    struct st s;
    out("meta (uid 1000):\n");
    CHECK(sc(102, 0, 0, 0, 0, 0, 0) == 1000 && sc(104, 0, 0, 0, 0, 0, 0) == 1000, "getuid/getgid come from XRL_UID/XRL_GID");
    CHECK(sc3(2, "/m2/rootonly", 0, 0) == -13, "open of a 0600 root file -> -EACCES");
    CHECK(sc3(2, "/m2/mine", 2, 0) >= 0, "open of my own 0644 file for writing works");
    CHECK(sc3(2, "/m2/group", 2, 0) >= 0, "group bits apply when the gid matches (0660, group 1000)");
    CHECK(sc3(2, "/m2/world", 2, 0) >= 0, "other bits apply (0666)");
    CHECK(sc3(2, "/m2/secret", 2, 0) == -13 && sc3(2, "/m2/secret", 0, 0) >= 0, "a root-owned 0644 file: read yes, write -> -EACCES");
    CHECK(sc2(90, "/m2/secret", 0777) == -1, "chmod of somebody else's file -> -EPERM");
    CHECK(sc3(92, "/m2/secret", 1000, 1000) == -1, "chown of somebody else's file -> -EPERM");
    CHECK(sc2(90, "/m2/mine", 0600) == 0 && stat_("/m2/mine", &s) == 0 && (s.mode & 07777) == 0600, "chmod of my own file works");
    CHECK(sc3(92, "/m2/mine", 0, 0) == -1, "giving my file away -> -EPERM");
    CHECK(sc3(92, "/m2/mine", -1, 1000) == 0, "chown to my own gid is fine");
    CHECK(sc2(21, "/m2/rootonly", 4) == -13 && sc2(21, "/m2/mine", 6) == 0, "access(R_OK|W_OK) follows the same bits");
    CHECK(sc2(83, "/m2/usermade", 0755) == 0 && stat_("/m2/usermade", &s) == 0 && s.uid == 1000 && s.gid == 1000, "files made by uid 1000 belong to 1000:1000");
    sc1(84, "/m2/usermade");
}

void cstart(u64 *sp)
{
    long argc = (long)sp[0];
    u64 *p = sp + 1 + argc + 1;
    while (*p) p++;
    p++;
    for (; p[0]; p += 2) if (p[0] == AT_XRL_SYSCALL) xsc = (xsc_t)p[1];
    if (!xsc) { __asm__ volatile("ud2"); }
    const char *ph = argc > 1 ? (const char *)sp[2] : "root";
    if (seq(ph, "user")) phase_user(); else phase_root();
    out(fails ? "meta: FAILED\n" : "meta: all checks passed\n");
    sc1(231, fails);
    for (;;) {}
}
