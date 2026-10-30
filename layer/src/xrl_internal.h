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
#define XRL_UTS_VERSION "#1 SMP PREEMPT_DYNAMIC Debian 6.1.90-1 (2024-05-03)"   /* uname -v and /proc/version: the build of 6.1.0-21-amd64 */

/* ------------------------------------------------------------------ log */
void xrl_logf(int level, const char *tag, const char *fmt, ...);
void xrl_heapcheck(const char *where);   /* XRL_HEAPCHECK=1: HeapValidate the process heap, log the first place it is not */
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
/* Full guest register state for signal delivery and rt_sigreturn (signal.c).
 * xrl_sig_jump (thunks.asm) loads it: offsets are asm-visible, and the first
 * 18 qwords are in Linux sigcontext order. */
typedef struct xrl_sigregs {
    u64 r8, r9, r10, r11, r12, r13, r14, r15;     /* 0x00 */
    u64 rdi, rsi, rbp, rbx, rdx, rax, rcx, rsp;   /* 0x40 */
    u64 rip, eflags;                              /* 0x80 */
    u64 xmask;                                    /* 0x90 0: fxrstor xs, else xrstor xs with this mask */
    u64 pad[5];
    __declspec(align(64)) unsigned char xs[1024]; /* 0xC0 XSAVE image, standard format */
} xrl_sigregs;

struct xrl_task;
typedef struct xrl_thread xrl_thread;
typedef struct xrl_proc xrl_proc;
struct xrl_thread {
    /* --- asm-visible block (offsets frozen, see thread.c static asserts) --- */
    u64 host_rsp;          /* 0x00 16-aligned top of host stack used by thunks   */
    u64 guest_tp;          /* 0x08 guest FS base (arch_prctl / CLONE_SETTLS)     */
    u64 frame;             /* 0x10 SysV frame of the syscall in flight           */
    u64 self;              /* 0x18 == (u64)this                                  */
    u64 resume[8];         /* 0x20 clone child: r15 r14 r13 r12 rbx rbp rip rsp  */
    u64 child_rsp;         /* 0x60 clone child: initial guest rsp                */
    u64 fsb_enabled;       /* 0x68 per-thread copy of global FSGSBASE flag       */
    int teb_guest_live;    /* 0x70 the guest TEB view is live (teb.c; com tramp) */
    int pad0;
    u64 sig_jump;          /* 0x78 xrl_sigregs* to load on the way out of xrl_syscall
                            *      (signal delivery / rt_sigreturn), 0 = plain return */
    /* --- C-only --- */
    int  tid;
    int  in_sys;           /* >0 while executing layer code on behalf of guest   */
    u64  nsys;             /* syscalls made (perf.c); written by this thread only */
    u32 *sysn;             /* XRL_PROFILE: syscalls by number since perf.c last read them */
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
    HANDLE sig_ev;         /* auto-reset: set when a signal is queued (interruptible waits) */
    struct xrl_sigregs *sig_regs;   /* per-thread buffer behind sig_jump (signal.c) */
    u64 sig_from_kill;     /* pending signals sent by kill() rather than tgkill (si_code) */
    volatile LONG exec_kill; /* execve is replacing the image: this thread ends at its next syscall return (exec.c) */
    int  daemon;           /* survives an execve that keeps the old image (the wineserver thread) */
    u64  host_teb;         /* this thread's host TEB (key of its xrl_ttab entry) */
    xrl_proc *proc;        /* the guest process this thread belongs to (proc.c) */
    volatile LONG suspended; /* SuspendThread'ed by xrl_proc_freeze while in guest code */
    int  leader;           /* first thread of a forked process (signals to the process prefer it) */
    int  sig_sender;       /* pid of the last process that signalled this thread (si_pid) */
    int  restore_sigmask;  /* rt_sigsuspend: sigmask is the temporary one; saved_sigmask comes back after the handler (or at once) */
    u64  saved_sigmask;
    /* shared-TEB views (teb.c): the copy NOT currently live in the TEB */
    u64 teb_host[XRL_TEB_SWAP_QWORDS];
    u64 teb_guest[XRL_TEB_SWAP_QWORDS];
    struct xrl_task *task; /* non-NULL: the body of a fork()ed task (task.c, XRL_FORK_TASKS=1) */
    int  proc_pid;         /* non-zero: getpid() for this thread (an in-process exec'd guest; task.c). Inherited by thread clones. */
    int  proc_ppid;        /* non-zero: getppid() for such a thread (its launcher) */
    void *fdt;             /* its fd table (fd.c); NULL: the process's */
    void *tr_park;         /* tracee.c: this thread's copy of its CONTEXT while a tracer has it stopped */
};

/* teb.c: guest/host views of the shared TEB (docs/gs-teb.md) */
void xrl_teb_init(void);
void xrl_teb_thread_init(xrl_thread *t);
int  xrl_teb_enter_host(xrl_thread *t);       /* returns 1 if the guest view was live */
void xrl_teb_enter_guest(xrl_thread *t);
void xrl_teb_restore(xrl_thread *t, int was_guest);
void xrl_teb_stack_here(u64 saved[2]);        /* host code on a guest stack: limits around rsp */
void xrl_teb_stack_back(const u64 saved[2]);

/* com.c: host COM objects called directly by the guest (docs/gs-teb.md Stage 2) */
int  xrl_com_scan(const u64 *v, int n);   /* 1: hooked a new vtable */
void xrl_com_after(const u64 *regs, const u64 *stack_args, u64 ret, u64 fn);   /* from xrl_com_tramp */
void xrl_com_hot(void (*emit)(const char *line), int max);   /* busiest methods (hang dump) */
void xrl_perf_start(void);   /* perf.c: XRL_PERF=N numbers every N s (default 10) */
extern void xrl_com_tramp(void);                                        /* Win64, r11 = target */

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
slong xrl_do_clone(u64 flags, u64 child_stack, u64 ptid, u64 ctid, u64 tls, u64 resume_pc);
slong xrl_do_exit(int code, int group);
/* task.c: fork() and ptrace() for children that alternate with their parent (experiment) */
int  xrl_fork_tasks_enabled(void);
slong xrl_do_fork(void);
int  xrl_task_pid(xrl_thread *t, int *ppid);             /* the task's pid (0: not a task) and its parent's */
slong xrl_task_wait(int pid, u64 ustatus, int options);
int  xrl_task_kill(int pid, int sig, slong *res);
slong xrl_task_ptrace(int req, int pid, u64 addr, u64 data);
void xrl_task_syscall_stop(xrl_thread *t, slong nr, const slong *a, slong ret, int enter);
int  xrl_task_signal(xrl_thread *t, int sig);
slong xrl_task_exec(xrl_thread *t, const char *path, u64 uargv, u64 uenvp);
void xrl_task_on_exit(xrl_thread *t, int code);
void xrl_task_wake_parent(xrl_thread *t);
void xrl_task_signal_death(xrl_thread *t, int sig);
xrl_thread *xrl_task_thread_new(xrl_thread *p, u64 rsp, struct xrl_task *k);   /* thread.c */
void xrl_task_thread_start(xrl_thread *t);
size_t xrl_fd_inherit_list(char *out, size_t cap);
void xrl_fd_inherit_boot(void);
void *xrl_fd_table_clone(void);                          /* fd.c */
void xrl_fd_table_free(void *tab);
/* tracee.c: a traced layer process, controlled from its tracer's process over a named pipe */
int  xrl_tr_send(HANDLE h, u32 op, u64 a, u64 b, const void *p, u32 len);
int  xrl_tr_recv(HANDLE h, u32 *op, u64 *a, u64 *b, void **p, u32 *len);
int  xrl_tracee_active(void);
void xrl_systrace_arm(void);
void xrl_tracee_boot(u64 *entry, u64 *sp);
void xrl_tracee_stop(CONTEXT *c, xrl_thread *t, int sig);
slong xrl_do_exit_sig(int sig);            /* the calling process dies of signal `sig` (wait status = sig) */
xrl_thread *xrl_thread_new_in(xrl_proc *p);
int  xrl_thread_clone_prepare(xrl_thread *p, xrl_proc *np, u64 flags, u64 child_stack, u64 tls, u64 resume_pc, xrl_thread **out);
void xrl_thread_start(xrl_thread *t);
int  xrl_thread_listed(xrl_thread *t);
void xrl_wake_ctid(xrl_thread *t);
int  xrl_threads_start_main(u64 entry, u64 guest_rsp, xrl_thread **out);
int  xrl_threads_start_main_fdt(u64 entry, u64 guest_rsp, void *fdt, int proc_pid, int proc_ppid, xrl_thread **out);   /* own fd table + getpid identity (task.c in-process game) */
void xrl_threads_terminate_all(void);
void xrl_thread_reap(xrl_thread *t);       /* drop the record of a thread that was terminated from outside */
void xrl_thread_die(xrl_thread *t);        /* the calling thread ends quietly (execve, exec kill); no return */
/* exec.c */
slong xrl_sys_execve(u64 path, u64 argv, u64 envp);
slong xrl_exec_prctl(int op, u64 a2, u64 a3);
void xrl_fd_close_on_exec(void);           /* fd.c: close every fd with FD_CLOEXEC */

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
int xrl_host_protect(void *p, size_t n, DWORD prot, DWORD *old);   /* VirtualProtect on host memory */
void *xrl_host_alloc(size_t n);   /* VirtualAlloc'd committed read-write host memory (the UWP-safe call; VirtualFree frees it) */
slong xrl_mem_populate(u64 addr, u64 len);    /* commit lazy pages before Win32 I/O on guest memory */
slong xrl_mem_populate_containing(u64 addr);  /* commit the whole lazy mapping holding addr */
slong xrl_mem_pin_stack(u64 addr, u64 len);    /* commit and never swap (a sigaltstack) */
int xrl_mem_lazy_fault(u64 addr, int access); /* VEH: 1 = committed, retry */
int  xrl_fd_list(int *out, int cap);
slong xrl_fd_close_range(unsigned first, unsigned last, unsigned flags);
slong xrl_memfd_create(const char *name, unsigned flags);
void xrl_futex_interrupt(xrl_thread *t);

/* ------------------------------------------------------------ processes */
/*
 * Guest processes (proc.c, docs/decisions.md "Guest processes"). Many Linux processes live in the one Windows
 * process and its one address space. A process owns threads, an fd table, a view of the memory map (its mm),
 * signal dispositions, ids and a wait status. Everything the rest of the layer used to keep in G (fd table,
 * cwd, umask, handlers, argv, pid) is per process and found through the calling thread (xrl_cur()->proc).
 */
typedef struct xrl_files xrl_files;               /* fd.c: a refcounted fd table */
struct swapgrp;
typedef struct xrl_mm { u32 id; volatile LONG refs; struct swapgrp *grp; } xrl_mm;   /* owner tag of the VMAs it maps (mem.c); grp: fork group (proc.c) */
typedef struct xrl_msnap xrl_msnap;               /* mem.c: snapshot of an mm's writable private memory */

#define XRL_CWD_MAX 1024
enum { XP_RUNNING, XP_STOPPED, XP_ZOMBIE };

struct xrl_proc {
    int pid, ppid, pgid, sid;
    xrl_proc *parent, *next;        /* parent (NULL for init), global list */
    int state;
    int wstatus;                    /* zombie: the wait4 status (exit<<8 | sig) */
    int stop_sig, stop_reported, cont_reported;
    int exit_signal;                /* sent to the parent when this process ends (SIGCHLD) */
    int is_init;                    /* the process boot started: its exit ends the layer */
    int pdeathsig;                  /* PR_SET_PDEATHSIG: sent to this process when its parent ends */
    int subreaper;                  /* PR_SET_CHILD_SUBREAPER: orphaned descendants are reparented here, not to init */
    volatile LONG refs;             /* the process table (until reaped) + one per thread record */
    volatile LONG nthreads, ndaemons;
    volatile LONG dying;            /* the process is being torn down: its threads end at their next syscall return */
    volatile LONG frozen;           /* xrl_proc_freeze: threads park at syscall return */
    HANDLE thaw_ev;                 /* manual-reset: set while not frozen */
    xrl_files *files;
    xrl_mm *mm;
    char cwd[XRL_CWD_MAX];
    u32 umask;
    u32 uid, euid, suid, gid, egid, sgid;
    lx_rlimit rlim[16];
    lx_ksigaction act[LX_NSIG + 1];
    char **argv, **envp;
    int argc, envc;
    char *exec_path;
    u64 start_ns;
    /* vfork / fork: the parent waits (suspended in its fork syscall) until this child execs or exits */
    HANDLE vfork_ev;                /* manual-reset event, NULL when no parent waits */
    xrl_thread *vfork_parent;
    u8 vfork_frame[2048];           /* the parent's thunk (and rawsys) frames below the child's rsp: the child overwrites them */
    size_t vfork_len;
    xrl_msnap *snap;                /* fork(): put the parent's memory back when the child leaves it */
    u32 snap_mm;
    struct swapgrp *grp;            /* the fork group this process shares its address space with (NULL: none) */
    xrl_msnap *saved;               /* its memory while another member's image is in the address space */
};

void xrl_proc_init(void);                       /* boot: create the init process (pid = Windows pid) */
xrl_proc *xrl_curproc(void);                    /* the calling thread's process (init for host threads) */
xrl_proc *xrl_proc_by_pid(int pid);             /* NULL if none (zombies included); takes a reference: xrl_proc_put */
void  xrl_proc_put(xrl_proc *p);
void  xrl_proc_ref(xrl_proc *p);
int   xrl_proc_stop_signal(xrl_proc *p, int sig);   /* a stop signal with the default action: stop p (1 if it was one) */
slong xrl_sig_send_thread(xrl_thread *t, int sig, int from_kill);   /* signal.c: queue + deliver */
xrl_proc *xrl_proc_init_get(void);
slong xrl_proc_clone(u64 flags, u64 child_stack, u64 ptid, u64 ctid, u64 tls, u64 resume_pc);
void  xrl_proc_vfork_release(xrl_proc *p);      /* the child left the parent's memory (exec / exit) */
void  xrl_proc_thread_gone(xrl_thread *t);      /* a thread was unlinked: the last one ends the process */
void  xrl_proc_exit(xrl_proc *p, int wstatus, xrl_thread *self);   /* end process p (zombie, SIGCHLD, ...) */
void  xrl_proc_kill_threads(xrl_proc *p, xrl_thread *self, int keep_daemons);   /* stop every other thread of p */
void  xrl_proc_freeze(xrl_proc *p, xrl_thread *self);
void  xrl_proc_thaw(xrl_proc *p);
void  xrl_proc_park(xrl_thread *t);             /* syscall return: a frozen or stopped process waits here */
int   xrl_swap_block(void);                     /* about to block in the layer: let another fork-group member run (1: released) */
void  xrl_swap_resume(void);                    /* the wait is over: get the address space back */
void  xrl_proc_exec_done(xrl_proc *p);          /* execve loaded the new image: release a vfork parent */
void  xrl_proc_exec_mm(xrl_proc *p);            /* execve: p gets a mm of its own (in place when it is the only user) */
slong xrl_proc_fatal_signal(xrl_proc *p, int sig, xrl_thread *self);   /* default action: terminate */
slong xrl_proc_signal(xrl_proc *p, int sig, int from_kill);
slong xrl_sys_kill(int pid, int sig);
slong xrl_sys_wait4(int pid, u64 ustatus, int options, u64 urusage);
slong xrl_sys_waitid(int idtype, int id, u64 uinfo, int options, u64 urusage);
slong xrl_sys_setpgid(int pid, int pgid);
slong xrl_sys_getpgid(int pid);
slong xrl_sys_setsid(void);
slong xrl_sys_getsid(int pid);
int   xrl_proc_list(int *out, int cap);         /* pids of every live process (procfs) */
int   xrl_proc_family_stopped(xrl_proc *p);
int   xrl_tty_pgrp(void);
int   xrl_tty_set_pgrp(int pgrp);
void  xrl_proc_comm(xrl_proc *p, char out[16]);   /* the main thread's name (the program's basename after exec) */
/* fd.c */
xrl_files *xrl_files_new(void);
xrl_files *xrl_files_clone(xrl_files *src);     /* fork: same xfile objects, own table */
void  xrl_files_ref(xrl_files *f);
void  xrl_files_put(xrl_files *f);              /* drop a reference; the last closes every fd */
int   xrl_files_list(xrl_files *f, int *out, int cap);
/* mem.c */
u32   xrl_mm_new(void);
void  xrl_mm_release(u32 mm);                   /* unmap every VMA the mm owns, drop its brk window */
size_t xrl_mem_format_maps_mm(u32 mm, char *buf, size_t cap);
u64   xrl_mem_mm_size(u32 mm);                  /* bytes mapped (VmSize) */
xrl_msnap *xrl_mem_snapshot(u32 mm, u64 cap_bytes);   /* NULL: too big or out of memory */
void  xrl_mem_snap_restore(xrl_msnap *s);
void  xrl_mem_snap_free(xrl_msnap *s);
int   xrl_mem_range_foreign(u64 s, u64 e, u32 mm, u32 *owner_out);   /* [s,e) overlaps a mapping of another mm */

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
    int   tp_slot;                 /* 1 if TlsSlots[XRL_TP_TLS_INDEX] is ours (gs:[XRL_TP_GS_OFFSET]) */
    int   trace_syscalls;
    SRWLOCK thread_lock;
    xrl_thread *threads;
    volatile LONG nthreads;
    volatile LONG ndaemons;        /* of nthreads: threads that do not count as "the last one" */
    u64   boot_qpc, qpc_freq;
    u64   boot_time_100ns;         /* FILETIME at boot                   */
    SRWLOCK cwd_lock;              /* guards every process's cwd         */
    void *guest_ctx;               /* xrl_guest_ctx visible via auxv     */
    void *wineserver_hook;         /* optional: guest fn started as thread */
    xrl_proc *init_proc;           /* the process boot started (proc.c) */
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
void  xrl_mem_rename_range(u64 lo, u64 hi, const char *new_name);
void  xrl_boot_apply_dynfix(void);
int   xrl_mem_sync_shared_page(u64 addr, u64 len);
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
int  xrl_mem_page_writable(u64 addr);
int  xrl_mem_writewatch_allow(u64 addr);   /* task.c: cap the fork stack snapshot at the writable stack end */
void xrl_mem_interp_rw_reset(void);          /* RELRO-safe: keep the interpreter's RW PT_LOAD writable */
void xrl_mem_interp_rw_add(u64 lo, u64 hi);

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
    u32  (*edge)(xfile *f);   /* optional: a counter that changes whenever new data/state arrives, for EPOLLET (see ipc.c) */
    /* A MAP_SHARED, PROT_WRITE mapping of the file now lives at [addr, addr+len) / is about to go (g_lock
     * held: must not call back into the memory manager). For devices whose mapping is live memory (fbdev.c). */
    void (*mapped)(xfile *f, u64 addr, u64 len, s64 off);
    void (*unmapped)(xfile *f, u64 addr, u64 len);
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
    u32    seals;          /* memfd F_ADD_SEALS bitmask                        */
    void  *priv;           /* type-specific                                    */
};

xfile *xfile_new(const xfile_ops *ops, int flags);
void   xfile_ref(xfile *f);
void   xfile_unref(xfile *f);

int    xrl_fd_install(xfile *f, int cloexec, int minfd);   /* returns fd or -errno */
xfile *xrl_fd_get(int fd);                                  /* takes a ref; NULL if bad */
xfile *xrl_files_get(xrl_files *f, int fd);                 /* the same in a given table (procfs: another process's fd) */
slong   xrl_fd_close(int fd);
slong   xrl_fd_dup(int oldfd, int newfd, int flags, int use_min);
int    xrl_fd_getfd(int fd);                                /* cloexec flag or -EBADF */
slong   xrl_fd_setfd(int fd, int cloexec);
void   xrl_fd_init(void);
xfile *xrl_dev_ntsync(int flags);                           /* ntsync.c: /dev/ntsync */
xfile *xrl_dev_evdev_open(const char *gabs, int flags, int *err);   /* evdev.c: /dev/input/eventN, NULL if not one */
int    xrl_dev_evdev_stat(const char *gabs, lx_stat *st);           /* 1 if handled */
xfile *xrl_dev_fb_open(const char *gabs, int flags, int *err);      /* fbdev.c: /dev/fb0, NULL if not it */
int    xrl_dev_fb_stat(const char *gabs, lx_stat *st);              /* 1 if handled */

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
slong xrl_vfs_chmod(int dirfd, const char *path, u32 mode, int flags);
slong xrl_vfs_fchmod(int fd, u32 mode);
slong xrl_vfs_chown(int dirfd, const char *path, u32 uid, u32 gid, int flags);   /* uid/gid (u32)-1: leave */
slong xrl_vfs_fchown(int fd, u32 uid, u32 gid);
u32 xrl_cur_uid(void);   /* util.c: the caller's effective ids (XRL_UID/XRL_GID, default 1000) */
u32 xrl_cur_gid(void);
u32 xrl_env_uid(void);
u32 xrl_env_gid(void);
int xrl_cur_pid(void);   /* the calling process (G.pid until proc.c) */
void xrl_signal_pid(int pid, int sig);   /* a signal for a process (a process that is gone: dropped) */
u32 xrl_cur_umask(void);   /* the caller's umask (G.umask until proc.c holds one per process) */
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
slong xrl_epoll_create(int flags);
slong xrl_epoll_ctl(int epfd, int op, int fd, u64 ev);
slong xrl_epoll_wait(int epfd, u64 events, int maxevents, int timeout_ms, int *intr);
slong xrl_poll(u64 fds, u64 nfds, s64 timeout_ns, u64 sigmask);
extern volatile LONG xrl_io_epoch;
void xrl_io_notify(void);
int  xrl_io_wait(LONG seen, s64 timeout_ns);   /* 1 woken, 0 timeout, -1 interrupted */

/* net.c: AF_INET/AF_INET6 TCP+UDP over Winsock. Each returns XRL_NOT_NET when `fd` (or the
 * domain) is not one of ours, so the caller can fall back to the AF_UNIX socketpair code. */
#define XRL_NOT_NET ((slong)0x7ffffffffffffff0ll)
slong xrl_net_socket(int domain, int type, int proto);
slong xrl_net_bind(int fd, u64 addr, u64 len);
slong xrl_net_listen(int fd, int backlog);
slong xrl_net_accept(int fd, u64 addr, u64 lenp, int flags);
slong xrl_net_connect(int fd, u64 addr, u64 len);
slong xrl_net_getsockname(int fd, u64 addr, u64 lenp);
slong xrl_net_getpeername(int fd, u64 addr, u64 lenp);
slong xrl_net_shutdown(int fd, int how);
slong xrl_net_sendto(int fd, u64 buf, u64 len, int flags, u64 addr, u64 addrlen);
slong xrl_net_recvfrom(int fd, u64 buf, u64 len, int flags, u64 addr, u64 lenp);
slong xrl_net_sendmsg(int fd, u64 msg, int flags);
slong xrl_net_recvmsg(int fd, u64 msg, int flags);
slong xrl_net_sendmmsg(int fd, u64 vec, u32 vlen, int flags);
slong xrl_net_recvmmsg(int fd, u64 vec, u32 vlen, int flags);
slong xrl_net_setsockopt(int fd, int level, int opt, u64 val, u64 len);
slong xrl_net_getsockopt(int fd, int level, int opt, u64 val, u64 lenp);

/* unix.c: AF_UNIX sockets (stream, dgram, seqpacket) as in-memory objects. Same convention as
 * net.c: XRL_NOT_NET when `fd` (or the domain) is not an AF_UNIX socket. */
slong xrl_unix_socket(int domain, int type, int proto);
slong xrl_unix_socketpair(int type, int flags, int fds[2]);
slong xrl_unix_bind(int fd, u64 addr, u64 len);
slong xrl_unix_listen(int fd, int backlog);
slong xrl_unix_accept(int fd, u64 addr, u64 lenp, int flags);
slong xrl_unix_connect(int fd, u64 addr, u64 len);
slong xrl_unix_getsockname(int fd, u64 addr, u64 lenp);
slong xrl_unix_getpeername(int fd, u64 addr, u64 lenp);
slong xrl_unix_shutdown(int fd, int how);
slong xrl_unix_sendto(int fd, u64 buf, u64 len, int flags, u64 addr, u64 addrlen);
slong xrl_unix_recvfrom(int fd, u64 buf, u64 len, int flags, u64 addr, u64 lenp);
slong xrl_unix_sendmsg(int fd, u64 msg, int flags);
slong xrl_unix_recvmsg(int fd, u64 msg, int flags);
slong xrl_unix_sendmmsg(int fd, u64 vec, u32 vlen, int flags);
slong xrl_unix_recvmmsg(int fd, u64 vec, u32 vlen, int flags);
slong xrl_unix_setsockopt(int fd, int level, int opt, u64 val, u64 len);
slong xrl_unix_getsockopt(int fd, int level, int opt, u64 val, u64 lenp);
slong xrl_unix_iov(int fd, u64 iov, u64 cnt, int is_write);   /* readv/writev: one message on a datagram socket */
/* vfs.c: nodes that exist only in the layer's tables (no host file): the socket files bind() makes.
 * `ref`/`unref` keep `obj` alive for as long as the node is listed. */
slong xrl_vfs_vnode_add(int dirfd, const char *path, u32 type, u32 mode, void *obj, void (*ref)(void *), void (*unref)(void *));
slong xrl_vfs_vnode_lookup(int dirfd, const char *path, u32 *type, void **obj);   /* ENOENT, or 0 with type 0 for an ordinary file */

/* timers.c: timerfd, setitimer/alarm, inotify */
slong xrl_timerfd_create(int clock, int flags);
slong xrl_timerfd_settime(int fd, int flags, u64 unew, u64 uold);
slong xrl_timerfd_gettime(int fd, u64 ucur);
slong xrl_setitimer(int which, u64 unew, u64 uold);
slong xrl_getitimer(int which, u64 ucur);
slong xrl_alarm(unsigned seconds);
slong xrl_inotify_init(int flags);
slong xrl_inotify_add_watch(int fd, const char *path, u32 mask);
slong xrl_inotify_rm_watch(int fd, int wd);

/* pty.c: /dev/ptmx and /dev/pts/N. xrl_tty_signal is proc.c's (signal a process group); pty.c links a logging default. */
xfile *xrl_pty_open_master(int flags, int *err);              /* NULL + positive errno */
xfile *xrl_pty_open_slave(int index, int flags, int *err);
int    xrl_pty_stat(const char *guest_abs, lx_stat *st);      /* 1 if handled; st_nlink 0 = no such pty */
int    xrl_tty_signal(int pgrp, int sig);

/* shm.c: SysV shared memory, and one set of pages behind every shared mapping of a memfd or /dev/shm file.
 * The mem.c hooks answer XRL_NOT_NET when the file / range is not shm.c's. */
slong xrl_shm_map_file(xfile *f, u64 len, s64 off);       /* the address to return for mmap(MAP_SHARED, f, off), or -errno */
slong xrl_shm_munmap(u64 addr, u64 len);
int   xrl_shm_overlaps(u64 addr, u64 len);                /* mem.c: a MAP_FIXED range touching a shared region is refused */
slong xrl_shmget(int key, u64 size, int flags);
slong xrl_shmat(int id, u64 addr, int flags);
slong xrl_shmdt(u64 addr);
slong xrl_shmctl(int id, int cmd, u64 buf);
slong xrl_memfd_seals(xfile *f, int op, u32 seals);       /* fcntl F_ADD_SEALS (op 1) / F_GET_SEALS (op 0) on a memfd; -EINVAL if f is not one */
int   xrl_memfd_sealed(const xfile *f, u32 seal);         /* a write/truncate must be refused: the seal is set on this memfd */
void  xrl_shm_proc_gone(int pid);                       /* proc.c: process pid ended or exec'd (its attaches and mappings go) */
void  xrl_shm_memfd_gone(u32 n);                      /* vfs.c: the last description of memfd n was closed */
u64   xrl_mem_shared_alloc(u64 len, const char *name);   /* mem.c: backing pages for shm.c */
void  xrl_mem_shared_free(u64 addr, u64 len);

/* -------------------------------------------------------------- signals */
void xrl_sig_init(void);
slong xrl_sys_rt_sigaction(int sig, u64 act, u64 oact, u64 sigsetsize);
slong xrl_sys_rt_sigprocmask(int how, u64 set, u64 oset, u64 sigsetsize);
slong xrl_sys_sigaltstack(u64 ss, u64 oss);
slong xrl_sys_tgkill(int tgid, int tid, int sig);
slong xrl_sys_rt_sigreturn(void);
slong xrl_sys_rt_sigsuspend(u64 mask, u64 size);
void xrl_sig_after_syscall(xrl_thread *t);
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
const char *xrl_hostname(void);
int   xrl_spinlog_on(void);   /* DIAG: XRL_SPINLOG=1 enables throttled read/write/poll fd logging */
void  xrl_memfd_trace_arm(int n);   /* DIAG: trace the current thread's next n syscalls */
/* signal.c: delivery (see the "signal delivery" comment there) */
void  xrl_sig_at_return(xrl_thread *t, slong nr, const slong *args, const u64 *gframe, slong res, xrl_sigregs *from);
slong xrl_sig_return(xrl_thread *t, u64 uc_addr);
slong xrl_sig_kill_process(int sig);
/* mem.c, for signal delivery: rip in executable and rsp in writable guest
 * memory (1), not (0), or the memory lock is held (-1, retry) */
int   xrl_mem_guest_ctx(u64 rip, u64 rsp);
void  xrl_mem_swap_stats(u64 *now, u64 *out, u64 *in);
int   xrl_mem_name_of(u64 a, char *name, size_t n, u64 *off);
int  xrl_mem_vma_of(u64 a, char *name, size_t n, u64 *off);   /* mem.c: the mapping containing a (sample.c) */
void xrl_sample_start(void);   /* sample.c: XRL_SAMPLE=<hz> sampling profiler */
void xrl_mem_breakdown(void);  /* mem.c: committed memory by guest mapping vs the partition, to the log */
int   xrl_mem_is_exec(u64 a);
int   xrl_mem_lock_held(void);
void  xrl_mem_clock_stats(char *b, size_t n);
void  xrl_sig_stats(char *b, size_t n);
void  xrl_hang_watch_start(void);   /* hangdump.c, XRL_HANG_DUMP */   /* bytes in the swap file, written, read */
/* cpuinfo.c: the real CPU (CPUID) and topology */
char *xrl_cpuinfo_text(size_t *len);
int  xrl_cpu_core_id(int cpu);
int  xrl_cpu_ncores(void);
void xrl_cpu_siblings_list(int cpu, char *out, size_t cap);
void xrl_cpu_mask_str(u64 mask, char *out, size_t cap);
typedef struct xrl_cache { int level, type /* 0 data, 1 instruction, 2 unified */, id; u32 size, line, ways; u64 mask; } xrl_cache;
int  xrl_cpu_caches(int cpu, xrl_cache *out, int cap);

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
    u64 interp_lo, interp_hi;         /* interpreter span (0 if none)   */
    u64 exports_xrl_wineserver_main;  /* dynsym lookup, 0 if absent */
} xrl_elf_image;
int xrl_elf_load(const char *guest_path, xrl_elf_image *out);
int xrl_elf_fixed_range(const char *guest_path, u64 *lo, u64 *hi);   /* 1: a non-PIE image and the range it must map */
u64 xrl_elf_build_stack(const xrl_elf_image *img, u64 *out_stack_lo, u64 *out_stack_hi);
void xrl_boot_move_libc_to_game(const xrl_elf_image *img);   /* Plan C: give EAC the game's musl, not the launcher's */

/* -------------------------------------------------------------- rewrite */
/* rewrite.c: load-time rewriting of foreign ELF images (`syscall`, `%fs:`). Called for every
 * file-backed mapping that is executable. e_flags of an image verified clean at build time
 * (tools/xrl-tpfix.py --stamp) carries this value and is skipped. */
#define XRL_ELF_E_FLAGS_CLEAN 0x58524c01u
void xrl_rewrite_mapped(xfile *f, u64 addr, u64 len, s64 off, int prot);
void xrl_rewrite_fd_mapped(int fd, u64 addr, u64 len, s64 off, int prot);

/* -------------------------------------------------------------- dbt */
/* dbt.c: run an image byte-identical (translation into a code cache), selected by XRL_DBT */
#define XRL_SYS_DBT_ENTER 0x10010      /* layer-private: dbt.asm asks for the translation of rdi */
int   xrl_dbt_wanted(const char *path);
int   xrl_dbt_wanted_file(const char *gpath);   /* ... or code from a memfd, in a program the policy selects */
int   xrl_dbt_attach(const char *name, u64 addr, u64 len);
void  xrl_dbt_attach_pe(const char *name, u64 addr, u64 len);
int   xrl_dbt_exec_hint(u64 addr, u64 len);   /* a range just became executable (mprotect); 1: it is part of a Windows image */
void  xrl_dbt_generated(u64 addr, u64 len);   /* a writable+executable anonymous range (generated code) */
void  xrl_dbt_attach_exec(u64 addr, u64 len); /* a read-only+executable non-PE range (anti-cheat's unpacked module code) */
void  xrl_dbt_dump_faults(void);              /* print faults the DBT let escape (call from a safe context, e.g. boot exit) */
void  xrl_mem_dump_faults(void);              /* print lazy-commit faults the handler could not back, + the full guest map */
void  xrl_dbt_unmapped(u64 addr, u64 len);    /* a range was unmapped or remapped */
u64   xrl_dbt_translate(u64 pc);
u64   xrl_dbt_reverse(u64 ca);            /* cache address → original (for the tracee) */
slong xrl_dbt_enter_sys(u64 pc);
void  xrl_dbt_stats(u64 *faults, u64 *translated);

/* -------------------------------------------------------------- syscall */
int  xrl_rlimit_get(int res, lx_rlimit *out);          /* misc.c: the current limit, for /proc/self/limits */
void xrl_sig_proc_masks(u64 *ign, u64 *cgt);          /* signal.c: SigIgn / SigCgt of /proc/self/status */
const char *xrl_sysname(slong nr);
void xrl_systrace(slong nr, const slong *a, slong r, int tid);   /* XRL_TRACE=2 (systrace.c) */
void xrl_syscall_table_init(void);

#endif /* XRL_INTERNAL_H */
