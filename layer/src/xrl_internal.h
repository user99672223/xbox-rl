/*
 * xrl_internal.h - layer-private declarations. Not part of the ABI.
 *
 * Calling-convention note (read this before touching anything):
 *   - The guest (Wine/musl, built with gcc/clang for x86-64 Linux) uses the
 *     System V ABI. The layer is Windows code and uses the Win64 ABI.
 *   - The *exported* `xrl_syscall` symbol is a SysV-ABI assembly thunk
 *     (asm/thunks.asm). It saves the guest's callee-saved registers, switches
 *     from the guest stack to the thread's host (Windows) stack, and calls
 *     xrl_syscall_dispatch() below with the Win64 ABI. Nothing in C may call
 *     the exported xrl_syscall with the Win64 convention.
 *   - Every guest->host transition (syscalls, host op-table calls) runs on the
 *     host stack, so Windows APIs / SEH / D3D always see a TEB-consistent
 *     stack. The guest stack is just memory.
 */
#ifndef XRL_INTERNAL_H
#define XRL_INTERNAL_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <stdlib.h>

#include "xrl_abi.h"

#if defined(XRL_UWP)
/* Exported by WindowsApp.lib but hidden by the WINAPI_FAMILY_APP headers. */
typedef LONG (NTAPI *XRL_VEH_FN)(PEXCEPTION_POINTERS);
PVOID WINAPI AddVectoredExceptionHandler(ULONG First, XRL_VEH_FN Handler);
BOOL WINAPI TerminateThread(HANDLE hThread, DWORD dwExitCode);
#endif
#include "lx.h"

#define XRL_LAYER_VERSION "0.1.0"

/* ------------------------------------------------------------------ log */
void xrl_logf(int level, const char *tag, const char *fmt, ...);
#define LOGT(...) xrl_logf(0, "layer", __VA_ARGS__)
#define LOGI(...) xrl_logf(1, "layer", __VA_ARGS__)
#define LOGW(...) xrl_logf(2, "layer", __VA_ARGS__)
#define LOGE(...) xrl_logf(3, "layer", __VA_ARGS__)
/* trace logging is compiled in but gated at runtime (g.trace_syscalls) */

#define NEGERR(e) (-(slong)(e))
#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))
#define ALIGN_UP(x, a) (((x) + ((a) - 1)) & ~((u64)(a) - 1))
#define ALIGN_DOWN(x, a) ((x) & ~((u64)(a) - 1))

/* ------------------------------------------------------------- threads */
/*
 * Per-guest-thread record. The first fields are read by assembly; their
 * offsets are asserted in thread.c. Reachable from any layer code via
 * xrl_cur() (hash of the host TEB Self pointer) and from the asm thunks.
 */
typedef struct xrl_thread xrl_thread;
struct xrl_thread {
    /* --- asm-visible block (offsets frozen, see thread.c static asserts) --- */
    u64 host_rsp;          /* 0x00 16-aligned top of host stack used by thunks   */
    u64 guest_tp;          /* 0x08 guest FS base (arch_prctl / CLONE_SETTLS)     */
    u64 frame;             /* 0x10 SysV frame of the syscall in flight           */
    u64 self;              /* 0x18 == (u64)this                                  */
    u64 resume[8];         /* 0x20 clone child: r15 r14 r13 r12 rbx rbp rip rsp  */
    u64 child_rsp;         /* 0x60 clone child: initial guest rsp                */
    u64 fsb_enabled;       /* 0x68 per-thread copy of global FSGSBASE flag       */
    /* --- C-only --- */
    int  tid;
    int  in_sys;           /* >0 while executing layer code on behalf of guest   */
    HANDLE handle;
    DWORD  win_tid;
    u64  guest_gs;
    u64  clear_child_tid;  /* CLONE_CHILD_CLEARTID / set_tid_address             */
    u64  robust_list;
    u64  robust_len;
    u64  sigmask;
    u64  sigpending;
    lx_stack_t altstack;
    char name[16];
    u64  guest_stack_lo, guest_stack_hi;  /* mapping we own (0 = not ours)       */
    HANDLE timer;          /* cached high-res waitable timer for sleeps          */
    volatile LONG exiting;
    int  exit_code;
    void *futex_node;      /* current futex waiter node, for interruption        */
    xrl_thread *next, *prev; /* all-threads list                                  */
    /* clone() arguments carried to the new thread */
    u64  clone_ctid, clone_ptid;
    u32  clone_flags;
    int  is_main;
    u64 defer_unmap_addr, defer_unmap_len;
    u32 sig_in_handler;
};

/* Thread lookup: hash of the host TEB Self pointer (gs:[30h]) -> xrl_thread*.
 * Table is read lock-free by the asm thunks. See thunks.asm. */
typedef struct xrl_ttab_entry { volatile u64 key; volatile u64 t; } xrl_ttab_entry;
#define XRL_TTAB_SIZE 4096
extern xrl_ttab_entry xrl_ttab[XRL_TTAB_SIZE];
xrl_thread *xrl_cur(void);
void xrl_thread_bind(xrl_thread *t);       /* register current OS thread -> t */
void xrl_thread_unbind(void);
xrl_thread *xrl_thread_new(void);
xrl_thread *xrl_thread_by_tid(int tid);
int  xrl_thread_count(void);
slong xrl_do_clone(u64 flags, u64 child_stack, u64 ptid, u64 ctid, u64 tls);
slong xrl_do_exit(int code, int group);
void xrl_wake_ctid(xrl_thread *t);
int  xrl_threads_start_main(u64 entry, u64 guest_rsp, xrl_thread **out);
void xrl_threads_terminate_all(void);

/* asm entry points (thunks.asm) */
void xrl_enter_guest(u64 entry, u64 guest_rsp, xrl_thread *t);        /* Win64 */
void xrl_clone_resume(xrl_thread *t);                                  /* Win64 */
extern void xrl_downcall_tramp(void);                                  /* SysV  */
slong xrl_syscall_dispatch(slong nr, slong a1, slong a2, slong a3, slong a4,
                          slong a5, slong a6, void *frame);              /* Win64 */
extern u64 xrl_asm_fsb_flag;

void xrl_threads_init(void);
int  xrl_threads_start_fn(u64 fn, const char *name);
u64  xrl_exit_stub_addr(void);
void *xrl_exec_alloc(size_t n);
int  xrl_fd_list(int *out, int cap);
slong xrl_fd_close_range(unsigned first, unsigned last, unsigned flags);
slong xrl_memfd_create(const char *name, unsigned flags);
void xrl_futex_interrupt(xrl_thread *t);

/* -------------------------------------------------------------- global */
typedef struct xrl_global {
    xrl_boot_context ctx;          /* deep-copied: strings owned below */
    char  *vfs_root;               /* UTF-8 host path, no trailing slash */
    wchar_t *vfs_root_w;           /* \\?\-prefixed wide path           */
    char  *prefix_path, *game_path;
    char  *exec_path;              /* guest path of the ELF (argv[0])    */
    char **argv, **envp;           /* owned copies                       */
    int   argc, envc;
    u64   mem_budget;
    HANDLE exit_event;
    volatile LONG exiting;
    volatile LONG exit_code;
    volatile LONG next_tid;
    int   pid;
    int   fsgsbase;                /* 1 if wrfsbase works on this host   */
    int   trace_syscalls;
    SRWLOCK thread_lock;
    xrl_thread *threads;
    volatile LONG nthreads;
    u64   boot_qpc, qpc_freq;
    u64   boot_time_100ns;         /* FILETIME at boot                   */
    char  cwd[1024];               /* guest cwd                          */
    SRWLOCK cwd_lock;
    u32   umask;
    void *guest_ctx;               /* xrl_guest_ctx visible via auxv     */
    void *wineserver_hook;         /* optional: guest fn started as thread */
} xrl_global;
extern xrl_global G;

/* ----------------------------------------------------- user-copy helpers */
/* All return 0 on success or -EFAULT. Safe against bad guest pointers. */
int  xrl_cfu(void *dst, u64 usrc, size_t n);          /* copy from user */
int  xrl_ctu(u64 udst, const void *src, size_t n);    /* copy to user   */
int  xrl_strfu(char *dst, u64 usrc, size_t cap);      /* path string, -ENAMETOOLONG/-EFAULT */
int  xrl_zerou(u64 udst, size_t n);

/* ------------------------------------------------------------- errno map */
int  xrl_win_to_errno(DWORD err);          /* Win32 error -> Linux errno (positive) */
slong xrl_lasterr(void);                    /* -errno from GetLastError()            */

/* --------------------------------------------------------------- random */
void xrl_rng_init(void);
void xrl_rng_fill(void *buf, size_t n);

/* --------------------------------------------------------------- memory */
int  xrl_mem_init(u64 budget);
slong xrl_mem_mmap(u64 addr, u64 len, int prot, int flags, int fd, s64 off);
slong xrl_mem_munmap(u64 addr, u64 len);
slong xrl_mem_mprotect(u64 addr, u64 len, int prot);
slong xrl_mem_mremap(u64 old_addr, u64 old_len, u64 new_len, int flags, u64 new_addr);
slong xrl_mem_brk(u64 addr);
slong xrl_mem_madvise(u64 addr, u64 len, int advice);
slong xrl_mem_msync(u64 addr, u64 len, int flags);
slong xrl_mem_mincore(u64 addr, u64 len, u64 vec);
u64  xrl_mem_stack_alloc(u64 size);        /* anon RW + guard; returns base or 0 */
void xrl_mem_stack_free(u64 base, u64 size);
size_t xrl_mem_format_maps(char *buf, size_t cap);   /* /proc/self/maps text */
u64  xrl_mem_committed(void);
u64  xrl_mem_arena_base(void);
u64  xrl_mem_arena_size(void);
int  xrl_mem_is_mapped(u64 addr);

/* --------------------------------------------------------------- futex */
void xrl_futex_init(void);
slong xrl_sys_futex(u64 uaddr, int op, u32 val, u64 timeout_or_val2, u64 uaddr2, u32 val3);
slong xrl_futex_wake_addr(u64 uaddr, int n);       /* internal wake (ctid clear) */
slong xrl_futex_waitv(u64 waiters, u32 nr, u32 flags, u64 timeout, int clockid);

/* ----------------------------------------------------------------- time */
void xrl_time_init(void);
u64  xrl_mono_ns(void);
u64  xrl_real_ns(void);
slong xrl_sleep_ns(u64 ns, int *interrupted);
u64  xrl_ts_to_ns(const lx_timespec *ts);
/* wait on a Win32 address for up to `ns` (u64 ~0 = infinite). returns 1 if
 * the address changed / was woken, 0 on timeout. */
int  xrl_wait_addr(volatile void *addr, void *cmp, size_t sz, u64 ns);

/* ------------------------------------------------------- files / vfs */
typedef struct xfile xfile;
typedef struct xfile_ops {
    const char *name;
    slong (*read)(xfile *f, void *buf, u64 n, s64 off);      /* off<0: use f->pos  */
    slong (*write)(xfile *f, const void *buf, u64 n, s64 off);
    slong (*lseek)(xfile *f, s64 off, int whence);
    slong (*fstat)(xfile *f, lx_stat *st);
    slong (*getdents)(xfile *f, void *buf, u64 n);
    slong (*ioctl)(xfile *f, u64 req, u64 arg);
    slong (*ftruncate)(xfile *f, s64 len);
    slong (*fsync)(xfile *f);
    u32  (*poll)(xfile *f, u32 events);                      /* ready mask */
    void (*close)(xfile *f);
    slong (*mmap_read)(xfile *f, void *dst, u64 n, s64 off);  /* for file mmap */
} xfile_ops;

struct xfile {
    volatile LONG refs;
    const xfile_ops *ops;
    int    flags;          /* O_* status flags (NONBLOCK, APPEND, access mode) */
    SRWLOCK lock;
    s64    pos;
    HANDLE h;
    char  *gpath;          /* guest path (for /proc/self/fd, fstat, mmap)      */
    u32    mode;           /* st_mode type bits for synthesized objects        */
    void  *priv;           /* type-specific                                    */
};

xfile *xfile_new(const xfile_ops *ops, int flags);
void   xfile_ref(xfile *f);
void   xfile_unref(xfile *f);

int    xrl_fd_install(xfile *f, int cloexec, int minfd);   /* returns fd or -errno */
xfile *xrl_fd_get(int fd);                                  /* takes a ref; NULL if bad */
slong   xrl_fd_close(int fd);
slong   xrl_fd_dup(int oldfd, int newfd, int flags, int use_min);
int    xrl_fd_getfd(int fd);                                /* cloexec flag or -EBADF */
slong   xrl_fd_setfd(int fd, int cloexec);
void   xrl_fd_init(void);

slong xrl_vfs_init(const char *root);
slong xrl_vfs_resolve(int dirfd, const char *path, int follow, char *out_guest, size_t cap);
slong xrl_vfs_open(int dirfd, const char *path, int flags, int mode);
slong xrl_vfs_stat(int dirfd, const char *path, int flags, lx_stat *st);
slong xrl_vfs_access(int dirfd, const char *path, int mode);
slong xrl_vfs_readlink(int dirfd, const char *path, char *buf, u64 cap);
slong xrl_vfs_mkdir(int dirfd, const char *path, int mode);
slong xrl_vfs_unlink(int dirfd, const char *path, int flags);
slong xrl_vfs_rename(int olddirfd, const char *oldp, int newdirfd, const char *newp, int flags);
slong xrl_vfs_symlink(const char *target, int dirfd, const char *linkpath);
slong xrl_vfs_link(int olddirfd, const char *oldp, int newdirfd, const char *newp);
slong xrl_vfs_chdir(const char *path);
slong xrl_vfs_getcwd(char *buf, u64 cap);
slong xrl_vfs_truncate(const char *path, s64 len);
slong xrl_vfs_utimens(int dirfd, const char *path, const lx_timespec *ts, int flags);
slong xrl_vfs_statfs(const char *path, lx_statfs *st);
int  xrl_vfs_host_path(const char *guest_abs, wchar_t *out, size_t cap);   /* -> \\?\ wide */
void xrl_stat_synth(lx_stat *st, u32 mode, u64 ino, s64 size);

/* special-file factories */
xfile *xrl_dev_null(int flags);
xfile *xrl_dev_zero(int flags);
xfile *xrl_dev_urandom(int flags);
xfile *xrl_dev_log(int level, const char *tag);      /* stdout/stderr -> host log */
xfile *xrl_dev_tty(int flags);
xfile *xrl_proc_open(const char *guest_abs, int flags, int *err);   /* NULL+err if not proc */
slong   xrl_proc_readlink(const char *guest_abs, char *buf, u64 cap);  /* -ENOENT if not virtual */
int    xrl_proc_stat(const char *guest_abs, lx_stat *st);             /* 1 if handled */
int    xrl_proc_is_virtual(const char *guest_abs);

/* ipc fds */
slong xrl_pipe_create(int flags, int fds[2]);
slong xrl_eventfd_create(u32 initval, int flags);
slong xrl_socketpair_create(int type, int flags, int fds[2]);
slong xrl_sendmsg(int fd, u64 msg, int flags);
slong xrl_recvmsg(int fd, u64 msg, int flags);
slong xrl_shutdown(int fd, int how);
slong xrl_epoll_create(int flags);
slong xrl_epoll_ctl(int epfd, int op, int fd, u64 ev);
slong xrl_epoll_wait(int epfd, u64 events, int maxevents, int timeout_ms, int *intr);
slong xrl_poll(u64 fds, u64 nfds, s64 timeout_ns, u64 sigmask);
extern volatile LONG xrl_io_epoch;
void xrl_io_notify(void);
int  xrl_io_wait(LONG seen, s64 timeout_ns);   /* 1 woken, 0 timeout, -1 interrupted */

/* -------------------------------------------------------------- signals */
void xrl_sig_init(void);
slong xrl_sys_rt_sigaction(int sig, u64 act, u64 oact, u64 sigsetsize);
slong xrl_sys_rt_sigprocmask(int how, u64 set, u64 oset, u64 sigsetsize);
slong xrl_sys_sigaltstack(u64 ss, u64 oss);
slong xrl_sys_tgkill(int tgid, int tid, int sig);
slong xrl_sys_rt_sigreturn(void);
slong xrl_sys_rt_sigsuspend(u64 mask, u64 size);
slong xrl_sys_rt_sigtimedwait(u64 set, u64 info, u64 ts, u64 size);
int  xrl_sig_pending_for(xrl_thread *t);       /* unblocked pending bitmask nonzero? */
void xrl_sig_install_veh(void);

/* --------------------------------------------------------------- misc */
slong xrl_sys_uname(u64 buf);
slong xrl_sys_sysinfo(u64 buf);
slong xrl_sys_getrandom(u64 buf, u64 len, u32 flags);
slong xrl_sys_arch_prctl(int code, u64 addr);
slong xrl_sys_prctl(int op, u64 a2, u64 a3, u64 a4, u64 a5);
slong xrl_sys_prlimit(int pid, int res, u64 newp, u64 oldp);
slong xrl_sys_sched_getaffinity(int pid, u64 size, u64 mask);
int  xrl_nprocs(void);

/* ----------------------------------------------------------- guest ctx */
/*
 * What the guest finds via AT_XRL_CTX. Same shape as xrl_boot_context's
 * service pointers, but every function pointer is a SysV-callable stub.
 * Layout is part of the B<->C contract (documented in notes-C.md).
 */
#define AT_XRL_SYSCALL 0x5801
#define AT_XRL_CTX     0x5802
#define AT_XRL_ABI     0x5803
#define XRL_GUEST_CTX_MAGIC 0x58524c47u /* 'XRLG' */
typedef struct xrl_guest_ctx {
    u32 struct_size;
    u32 magic;
    u32 abi_version;
    u32 flags;                 /* bit0: FS base is hardware (wrfsbase), bit1: GS TLS slot valid */
    u64 syscall;               /* SysV-callable: long (*)(nr,a1..a6)             */
    u64 tls_slot_gs_off;       /* gs:[tls_slot_gs_off] == current thread's FS base shadow (Tier 3) */
    xrl_log_fn log;            /* SysV stub                                       */
    const xrl_gfx_ops   *gfx;  /* each member fn is a SysV stub                   */
    const xrl_audio_ops *audio;
    const xrl_input_ops *input;
    u64 guest_mem_budget;
} xrl_guest_ctx;
int  xrl_guest_ctx_build(void);
u64  xrl_stub_for(void *host_fn);       /* allocate a SysV->Win64 downcall stub */

/* ------------------------------------------------------------------ elf */
typedef struct xrl_elf_image {
    u64 entry;          /* first instruction to run (interp entry if any)    */
    u64 prog_entry;     /* AT_ENTRY                                           */
    u64 phdr;           /* AT_PHDR                                            */
    u64 phent, phnum;
    u64 base;           /* AT_BASE (interpreter load bias, 0 if none)         */
    u64 load_bias;
    u64 brk_start;
    int is_dyn;
    char interp[256];
    u64 exports_xrl_wineserver_main;  /* dynsym lookup, 0 if absent */
} xrl_elf_image;
int xrl_elf_load(const char *guest_path, xrl_elf_image *out);
u64 xrl_elf_build_stack(const xrl_elf_image *img, u64 *out_stack_lo, u64 *out_stack_hi);

/* -------------------------------------------------------------- syscall */
const char *xrl_sysname(slong nr);
void xrl_syscall_table_init(void);

#endif /* XRL_INTERNAL_H */
