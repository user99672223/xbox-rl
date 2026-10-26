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

int main(void)
{
    size_t n = (size_t)((char *)rawsys_stub_end - (char *)rawsys_stub);
    printf("rawsys: stub is %u bytes (the bootstrapper's is 21)\n", (unsigned)n);

    run_case("image", (sys3_fn)(void *)rawsys_stub);

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
    }

    printf("rawsys: %s (%d failed)\n", g_fail ? "FAILED" : "RAWSYS-OK", g_fail);
    fflush(stdout);
    return g_fail ? 1 : 0;
}
