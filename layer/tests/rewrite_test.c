/*
 * rewrite_test.c - host test for layer/src/rewrite_core.c (build and run in WSL/Linux:
 * layer/tests/rewrite_test.sh).
 *
 *   rewrite_test --diff         differential test on real hardware: hand-written snippets
 *                               with every %fs operand form and `syscall`, run unmodified
 *                               and rewritten; all registers, flags and the thread block
 *                               must end up identical.
 *   rewrite_test --elf FILE...  rewrite the executable segments of real ELF files and print
 *                               what was found (compare with objdump | grep -c syscall).
 */
#define _GNU_SOURCE
#include "../src/rewrite_core.h"
#include "../src/x86dec.h"
#include <elf.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

static void tlog(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap); fputc('\n', stderr);
}

/* ---------------------------------------------------------------- --elf */
static int do_elf(const char *path)
{
    int fd = open(path, O_RDONLY);
    struct stat st;
    if (fd < 0 || fstat(fd, &st)) { perror(path); return 1; }
    uint8_t *img = (uint8_t *)malloc((size_t)st.st_size);
    if (read(fd, img, (size_t)st.st_size) != st.st_size) { perror("read"); return 1; }
    close(fd);
    Elf64_Ehdr *eh = (Elf64_Ehdr *)img;
    Elf64_Phdr *ph = (Elf64_Phdr *)(img + eh->e_phoff);
    uint64_t *starts = NULL; size_t nst = 0;
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_GNU_EH_FRAME) continue;
        starts = xrw_eh_starts(img + ph[i].p_offset, ph[i].p_filesz, ph[i].p_vaddr, &nst);
    }
    xrw w; memset(&w, 0, sizeof w);
    w.tp_gs_off = 0x1678;
    w.log = tlog; w.verbose = getenv("XRW_TEST_VERBOSE") != NULL;
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD || !(ph[i].p_flags & PF_X)) continue;
        w.pool = NULL; w.pool_vaddr = ph[i].p_vaddr; w.pool_used = 0;
        xrw_rewrite_range(&w, img + ph[i].p_offset, ph[i].p_vaddr, ph[i].p_filesz, starts, nst);   /* dry run: pool size */
    }
    xrw d = w;
    size_t need = w.pool_used;
    printf("%s: %u insns, %u syscall, %u %%fs (%u in place, %u jmp, %u trap, %u moved), %u unsupported, %u decode-fail, %u legacy-sys, "
           "FDE starts %zu, pool %zu bytes\n", path, d.st.insns, d.st.syscalls, d.st.fs_sites, d.st.inplace, d.st.jumps, d.st.traps,
           d.st.moved, d.st.unsupported, d.st.decode_fail, d.st.legacy_sys, nst, need);
    /* second, real pass over a scratch copy to check the patched bytes decode cleanly */
    xrw r; memset(&r, 0, sizeof r);
    r.tp_gs_off = 0x1678; r.log = tlog;
    uint8_t *pool = (uint8_t *)malloc(need + 64);
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD || !(ph[i].p_flags & PF_X)) continue;
        r.pool = pool; r.pool_vaddr = ph[i].p_vaddr + ph[i].p_memsz; r.pool_cap = need + 64; r.pool_used = 0;
        if (xrw_rewrite_range(&r, img + ph[i].p_offset, ph[i].p_vaddr, ph[i].p_filesz, starts, nst)) { printf("  real pass: pool too small\n"); return 1; }
        /* every instruction boundary of the patched code must still decode and contain no syscall / %fs */
        size_t o = 0, n = ph[i].p_filesz;
        uint8_t *c = img + ph[i].p_offset;
        unsigned left_sys = 0, left_fs = 0;
        size_t si = 0;
        while (o < n) {
            while (si < nst && starts[si] < ph[i].p_vaddr + o) si++;
            x86i in;
            int l = x86_decode(c + o, n - o < 15 ? n - o : 15, &in);
            if (!l) { o++; continue; }
            if (in.map == 1 && in.opcode == 5 && !in.vex) left_sys++;
            if (in.seg == 0x64 && in.has_modrm && (in.modrm >> 6) != 3 && !(in.map == 0 && in.opcode == 0x8d)) left_fs++;
            o += (size_t)l;
        }
        if (left_sys || left_fs) printf("  segment %d: %u syscall and %u %%fs still present after rewriting (unsupported forms or desynced decode)\n", i, left_sys, left_fs);
    }
    printf("  real pass: %u syscall, %u %%fs, pool used %zu\n", r.st.syscalls, r.st.fs_sites, r.pool_used);
    free(starts); free(img); free(pool); free(r.traps);
    return 0;
}

/* --------------------------------------------------------------- --diff */
typedef struct regs { uint64_t r[16]; uint64_t flags; } regs;   /* rax rcx rdx rbx rsp rbp rsi rdi r8..r15 */

static uint8_t *g_tcb_block;        /* the thread block both runs use (same address: %fs:0 must match) */
static uint8_t g_snap[2][0x6000];  /* its contents after the original / rewritten run */
static uint8_t *g_gs_block;
#define TCB_OFF 0x2000
#define BLOCK_SIZE 0x6000

/* run(fn, in, out, fsbase, gsbase): loads every GPR from `in`, calls fn, stores GPRs and flags to `out`.
 * FS/GS bases are switched with arch_prctl only around the call (no libc code runs in between). */
extern void run_snippet(void *fn, const regs *in, regs *out, uint64_t fsbase, uint64_t gsbase);
__asm__(
    ".text\n.globl run_snippet\nrun_snippet:\n"
    "  push %rbp\n push %rbx\n push %r12\n push %r13\n push %r14\n push %r15\n"
    "  sub $0x40,%rsp\n"
    "  mov %rdi,0(%rsp)\n mov %rsi,8(%rsp)\n mov %rdx,16(%rsp)\n mov %rcx,24(%rsp)\n mov %r8,32(%rsp)\n"
    /* save the current FS base */
    "  lea 40(%rsp),%rsi\n mov $0x1003,%edi\n mov $158,%eax\n syscall\n"          /* ARCH_GET_FS */
    "  mov 24(%rsp),%rsi\n mov $0x1002,%edi\n mov $158,%eax\n syscall\n"          /* ARCH_SET_FS = fsbase */
    "  mov 32(%rsp),%rsi\n mov $0x1001,%edi\n mov $158,%eax\n syscall\n"          /* ARCH_SET_GS = gsbase */
    "  mov 8(%rsp),%r15\n"
    "  mov 0x00(%r15),%rax\n mov 0x08(%r15),%rcx\n mov 0x10(%r15),%rdx\n mov 0x18(%r15),%rbx\n mov 0x28(%r15),%rbp\n"
    "  mov 0x30(%r15),%rsi\n mov 0x38(%r15),%rdi\n mov 0x40(%r15),%r8\n mov 0x48(%r15),%r9\n mov 0x50(%r15),%r10\n"
    "  mov 0x58(%r15),%r11\n mov 0x60(%r15),%r12\n mov 0x68(%r15),%r13\n mov 0x70(%r15),%r14\n mov 0x78(%r15),%r15\n"
    "  call *0(%rsp)\n"
    "  pushfq\n"
    "  push %r15\n"
    "  mov 32(%rsp),%r15\n"             /* out: 16(%rsp) before pushfq + push */
    "  mov %rax,0x00(%r15)\n mov %rcx,0x08(%r15)\n mov %rdx,0x10(%r15)\n mov %rbx,0x18(%r15)\n mov %rbp,0x28(%r15)\n"
    "  mov %rsi,0x30(%r15)\n mov %rdi,0x38(%r15)\n mov %r8,0x40(%r15)\n mov %r9,0x48(%r15)\n mov %r10,0x50(%r15)\n"
    "  mov %r11,0x58(%r15)\n mov %r12,0x60(%r15)\n mov %r13,0x68(%r15)\n mov %r14,0x70(%r15)\n"
    "  pop %rax\n mov %rax,0x78(%r15)\n"
    "  pop %rax\n mov %rax,0x80(%r15)\n"
    /* restore the FS base */
    "  mov 40(%rsp),%rsi\n mov $0x1002,%edi\n mov $158,%eax\n syscall\n"
    "  add $0x40,%rsp\n pop %r15\n pop %r14\n pop %r13\n pop %r12\n pop %rbx\n pop %rbp\n ret\n"
);
/* what the stub's `call *slot` reaches in this test: the real syscall instruction (kernel register ABI) */
extern void test_rawsys(void);
__asm__(".text\n.globl test_rawsys\ntest_rawsys:\n syscall\n ret\n");

#define SNIP(name, body) \
    __asm__(".text\n.p2align 4\n.globl " #name "\n" #name ":\n" body "\n ret\n.globl " #name "_end\n" #name "_end:\n"); \
    extern char name[], name##_end[];

SNIP(s_tp0,       "mov %fs:0x0,%rax\n mov %fs:0x10,%rdx")
SNIP(s_canary,    "mov %fs:0x28,%rax\n mov %rax,%rbx\n sub %fs:0x28,%rbx\n xor %fs:0x28,%rdx")
SNIP(s_base,      "mov %fs:(%rax),%ecx\n mov %rdi,%rsi")
SNIP(s_base_dest, "mov %fs:(%rax),%rax\n add $1,%rax")
SNIP(s_base_r9,   "mov %fs:8(%rbx),%r9")
SNIP(s_idx,       "mov %fs:0x20(%rcx,%rdx,8),%rsi\n nop")
SNIP(s_idx_only,  "mov %fs:0x20(,%rcx,8),%rdx\n nop\n nop")
SNIP(s_r12,       "mov %fs:0x10(%r12),%r13\n mov %fs:(%r12),%r14")
SNIP(s_hi_regs,   "mov %fs:(%r10),%r11\n mov %fs:(%r8,%r9,2),%r10")
SNIP(s_zx,        "movzbl %fs:0x30,%eax\n movsbq %fs:0x31,%rbx\n movslq %fs:0x40,%rcx\n movzwl %fs:0x44,%edx")
SNIP(s_store,     "mov %rax,%fs:0x108\n movl $0x1234,%fs:0x110\n movq $-1,%fs:0x118\n add %rcx,%fs:0x100")
SNIP(s_cmp_flags, "cmpq $0,%fs:0x18\n sete %al\n pushfq\n pop %rdx")
SNIP(s_cmp_keep,  "cmp %rax,%rbx\n mov %fs:0x20,%rcx\n adc $0,%rdx")
SNIP(s_lock,      "lock incl %fs:(%rax)\n lock cmpxchg %rcx,%fs:(%rbx)\n xchg %rdx,%fs:0x40")
SNIP(s_sse,       "movdqu %fs:0x40,%xmm1\n movq %xmm1,%rdx\n movups %xmm1,%fs:0x60")
SNIP(s_byte,      "movb %fs:0x30,%ah\n movb %cl,%fs:0x31\n movb %dh,%fs:0x32\n mov %fs:0x30,%al")
SNIP(s_neg,       "mov %fs:-8,%rax\n mov %fs:-16(%rbx),%rcx")
SNIP(s_pair,      "mov %fs:0x30,%rax\n mov %fs:0x38,%rbx")
SNIP(s_getpid,    "mov $39,%eax\n syscall")
SNIP(s_gettid,    "mov $186,%eax\n syscall\n mov %rax,%rbx\n mov $39,%eax\n syscall\n cmp %rax,%rbx")
SNIP(s_sys_short, "xor %eax,%eax\n mov %fs:0x20,%rbx\n syscall")
SNIP(s_sys_args,  "mov $1,%eax\n mov $1,%edi\n lea 0(%rsp),%rsi\n xor %edx,%edx\n syscall\n mov %rdi,%r8\n mov %rsi,%r9\n mov %rdx,%r10")
SNIP(s_sys_nowin, "mov $39,%eax\n jmp 1f\n1: syscall")
SNIP(s_redzone,   "movq $0x11223344,-8(%rsp)\n mov $39,%eax\n syscall\n mov -8(%rsp),%rbx")
SNIP(s_target_before, "xor %ebx,%ebx\n test %rdi,%rdi\n jz 2f\n inc %ebx\n 2: mov $39,%eax\n syscall\n mov %ebx,%edx")

SNIP(s_rip_neighbor, "mov 1f(%rip),%rbx\n mov %fs:(%rax),%ecx\n jmp 2f\n1: .quad 0x1122334455667788\n2: nop")
SNIP(s_jcc_after, "cmpl $0x9,%fs:(%rax)\n jne 1f\n mov $1,%rbx\n1: mov $2,%rdx")
SNIP(s_jcc_taken, "cmpl $0,%fs:(%rax)\n je 1f\n mov $1,%rbx\n1: mov $2,%rdx")
SNIP(s_loop, "xor %ebx,%ebx\n1: mov %fs:(%rax),%ecx\n inc %ebx\n cmp $3,%ebx\n jne 1b")
/* a function entry reached only through a pointer must survive a window ending at the previous ret */
SNIP(s_sys_boundary, "lea 1f(%rip),%rbx\n call *%rbx\n mov %rax,%rdx\n jmp 9f\n1: xor %eax,%eax\n syscall\n ret\n2: mov $39,%eax\n syscall\n ret\n9: lea 2b(%rip),%rbx\n call *%rbx\n xor %ebx,%ebx")
SNIP(s_ret_in_window, "mov %fs:(%rax),%ecx\n ret")
/* the syscall of a sigreturn restorer is followed by another function (musl: __sigsetjmp): not a window to extend into */
SNIP(s_sigret_boundary, "lea 2f(%rip),%rbx\n call *%rbx\n mov %rax,%rdx\n jmp 9f\n1: mov $15,%eax\n syscall\n2: xor %eax,%eax\n inc %eax\n ret\n9: xor %ebx,%ebx")

typedef struct snip { const char *name; char *s, *e; } snip;
#define S(n) { #n, n, n##_end }
static snip g_snips[] = {
    S(s_tp0), S(s_canary), S(s_base), S(s_base_dest), S(s_base_r9), S(s_idx), S(s_idx_only), S(s_r12), S(s_hi_regs), S(s_zx),
    S(s_store), S(s_cmp_flags), S(s_cmp_keep), S(s_lock), S(s_sse), S(s_byte), S(s_neg), S(s_pair),
    S(s_getpid), S(s_gettid), S(s_sys_short), S(s_sys_args), S(s_sys_nowin), S(s_redzone), S(s_target_before),
    S(s_rip_neighbor), S(s_jcc_after), S(s_jcc_taken), S(s_loop), S(s_sys_boundary), S(s_ret_in_window), S(s_sigret_boundary),
};

static xrw g_w;
static void trap_handler(int sig, siginfo_t *si, void *uc_)
{
    (void)sig; (void)si;
    ucontext_t *uc = (ucontext_t *)uc_;
    uint64_t site = (uint64_t)uc->uc_mcontext.gregs[REG_RIP] - 1;
    for (size_t i = 0; i < g_w.ntraps; i++)
        if (g_w.traps[i].site == site) { uc->uc_mcontext.gregs[REG_RIP] = (greg_t)g_w.traps[i].stub; return; }
    static const char m[] = "unexpected int3\n";
    (void)!write(2, m, sizeof m - 1);
    _exit(3);
}

static int do_diff(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = trap_handler; sa.sa_flags = SA_SIGINFO;
    sigaction(SIGTRAP, &sa, NULL);
    g_tcb_block = (uint8_t *)mmap(NULL, BLOCK_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    g_gs_block = (uint8_t *)mmap(NULL, 0x4000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    uint8_t *code = (uint8_t *)mmap(NULL, 1 << 20, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    int fails = 0;
    for (size_t t = 0; t < sizeof g_snips / sizeof g_snips[0]; t++) {
        snip *sn = &g_snips[t];
        size_t len = (size_t)(sn->e - sn->s);
        uint8_t *buf = code + t * 4096;
        memcpy(buf, sn->s, len);
        memset(&g_w, 0, sizeof g_w);
        g_w.tp_gs_off = 0x1678; g_w.log = tlog;
        uint8_t *pool = buf + 1024;
        g_w.pool = pool; g_w.pool_vaddr = (uint64_t)pool; g_w.pool_cap = 3000;
        if (xrw_rewrite_range(&g_w, buf, (uint64_t)buf, len, NULL, 0)) { printf("FAIL %s: pool\n", sn->name); fails++; continue; }
        *(uint64_t *)pool = (uint64_t)test_rawsys;
        /* inputs: small offsets in the address registers, distinct values elsewhere */
        regs in; memset(&in, 0, sizeof in);
        uint64_t seed[16] = { 0x40, 0x48, 3, 0x50, 0, 0x58, 0x60, 0x68, 0x70, 5, 0x78, 0x80, 0x88, 0x90, 0x98, 0xa0 };
        memcpy(in.r, seed, sizeof seed);
        in.r[15] = 0xa0;
        uint8_t init[BLOCK_SIZE];
        for (int i = 0; i < BLOCK_SIZE; i++) init[i] = (uint8_t)(i * 37 + 11);
        *(uint64_t *)(init + TCB_OFF) = (uint64_t)(g_tcb_block + TCB_OFF);   /* the TCB self pointer at %fs:0, as in every ABI */
        regs out[2]; memset(out, 0, sizeof out);
        for (int k = 0; k < 2; k++) {
            memcpy(g_tcb_block, init, BLOCK_SIZE);
            memset(g_gs_block, 0, 0x4000);
            *(uint64_t *)(g_gs_block + 0x1678) = (uint64_t)(g_tcb_block + TCB_OFF);
            if (getenv("XRW_TEST_VERBOSE")) fprintf(stderr, "run %s (%s)\n", sn->name, k ? "rewritten" : "original");
            run_snippet(k == 0 ? (void *)sn->s : (void *)buf, &in, &out[k], (uint64_t)(g_tcb_block + TCB_OFF), (uint64_t)g_gs_block);
            memcpy(g_snap[k], g_tcb_block, BLOCK_SIZE);
        }
        int bad = 0;
        for (int i = 0; i < 16; i++) {
            if (i == 4) continue;                                   /* rsp */
            if ((i == 1 || i == 9) && strncmp(sn->name, "s_sys", 5) == 0) continue;   /* syscall clobbers rcx, r11 (either way) */
            if ((i == 1 || i == 9) && strncmp(sn->name, "s_get", 5) == 0) continue;
            if ((i == 1 || i == 9) && strncmp(sn->name, "s_redzone", 9) == 0) continue;
            if ((i == 1 || i == 9) && strncmp(sn->name, "s_target", 8) == 0) continue;
            if (out[0].r[i] != out[1].r[i]) { printf("  reg %d: %llx vs %llx\n", i, (unsigned long long)out[0].r[i], (unsigned long long)out[1].r[i]); bad = 1; }
        }
        if ((out[0].flags & 0x8d5) != (out[1].flags & 0x8d5) && strncmp(sn->name, "s_sys", 5) && strncmp(sn->name, "s_get", 5)) {
            printf("  flags %llx vs %llx\n", (unsigned long long)out[0].flags, (unsigned long long)out[1].flags); bad = 1;
        }
        /* the thread block: bytes identical (gs block pointer slot is the only difference by design) */
        if (memcmp(g_snap[0], g_snap[1], BLOCK_SIZE)) { printf("  thread block differs\n"); bad = 1; }
        printf("%s %-16s units=%u+%u jump=%u trap=%u inplace=%u moved=%u bytes=%zu\n", bad ? "FAIL" : "ok  ", sn->name,
               g_w.st.syscalls, g_w.st.fs_sites, g_w.st.jumps, g_w.st.traps, g_w.st.inplace, g_w.st.moved, len);
        fails += bad;
        free(g_w.traps);
    }
    printf("rewrite_test --diff: %d failure(s)\n", fails);
    return fails != 0;
}

int main(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "--diff")) return do_diff();
    if (argc >= 3 && !strcmp(argv[1], "--elf")) { int rc = 0; for (int i = 2; i < argc; i++) rc |= do_elf(argv[i]); return rc; }
    fprintf(stderr, "usage: rewrite_test --diff | --elf FILE...\n");
    return 2;
}
