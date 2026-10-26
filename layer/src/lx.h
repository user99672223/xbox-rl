/*
 * lx.h - Linux x86-64 ABI constants and struct layouts the layer emulates.
 *
 * The guest speaks the Linux x86-64 kernel ABI (via musl). We deliberately do
 * not include any host <errno.h>/<fcntl.h>: every value here is the *Linux*
 * value, because it crosses the xrl_syscall seam verbatim.
 */
#ifndef XRL_LX_H
#define XRL_LX_H

#include <stdint.h>
#include <stddef.h>

typedef int64_t  s64;
typedef uint64_t u64;
typedef int32_t  s32;
typedef uint32_t u32;
typedef uint16_t u16;
typedef int16_t  s16;
typedef uint8_t  u8;
typedef int64_t  slong;   /* the Linux 'long' (MSVC's long is 32-bit!) */

/* ---- syscall numbers (x86-64) ------------------------------------------ */
#define XRL_NR_MAX 470

#define NR_read 0
#define NR_write 1
#define NR_open 2
#define NR_close 3
#define NR_stat 4
#define NR_fstat 5
#define NR_lstat 6
#define NR_poll 7
#define NR_lseek 8
#define NR_mmap 9
#define NR_mprotect 10
#define NR_munmap 11
#define NR_brk 12
#define NR_rt_sigaction 13
#define NR_rt_sigprocmask 14
#define NR_rt_sigreturn 15
#define NR_ioctl 16
#define NR_pread64 17
#define NR_pwrite64 18
#define NR_readv 19
#define NR_writev 20
#define NR_access 21
#define NR_pipe 22
#define NR_select 23
#define NR_sched_yield 24
#define NR_mremap 25
#define NR_msync 26
#define NR_mincore 27
#define NR_madvise 28
#define NR_dup 32
#define NR_dup2 33
#define NR_pause 34
#define NR_nanosleep 35
#define NR_getpid 39
#define NR_sendfile 40
#define NR_socket 41
#define NR_connect 42
#define NR_accept 43
#define NR_sendto 44
#define NR_recvfrom 45
#define NR_sendmsg 46
#define NR_recvmsg 47
#define NR_shutdown 48
#define NR_bind 49
#define NR_listen 50
#define NR_getsockname 51
#define NR_getpeername 52
#define NR_socketpair 53
#define NR_setsockopt 54
#define NR_getsockopt 55
#define NR_clone 56
#define NR_fork 57
#define NR_vfork 58
#define NR_execve 59
#define NR_exit 60
#define NR_wait4 61
#define NR_kill 62
#define NR_uname 63
#define NR_fcntl 72
#define NR_flock 73
#define NR_fsync 74
#define NR_fdatasync 75
#define NR_truncate 76
#define NR_ftruncate 77
#define NR_getdents 78
#define NR_getcwd 79
#define NR_chdir 80
#define NR_fchdir 81
#define NR_rename 82
#define NR_mkdir 83
#define NR_rmdir 84
#define NR_creat 85
#define NR_link 86
#define NR_unlink 87
#define NR_symlink 88
#define NR_readlink 89
#define NR_chmod 90
#define NR_fchmod 91
#define NR_chown 92
#define NR_fchown 93
#define NR_lchown 94
#define NR_umask 95
#define NR_gettimeofday 96
#define NR_getrlimit 97
#define NR_getrusage 98
#define NR_sysinfo 99
#define NR_times 100
#define NR_getuid 102
#define NR_getgid 104
#define NR_setuid 105
#define NR_setgid 106
#define NR_geteuid 107
#define NR_getegid 108
#define NR_setpgid 109
#define NR_getppid 110
#define NR_getpgrp 111
#define NR_setsid 112
#define NR_getgroups 115
#define NR_setresuid 117
#define NR_getresuid 118
#define NR_setresgid 119
#define NR_getresgid 120
#define NR_getpgid 121
#define NR_getsid 124
#define NR_rt_sigpending 127
#define NR_rt_sigtimedwait 128
#define NR_rt_sigsuspend 130
#define NR_sigaltstack 131
#define NR_utime 132
#define NR_statfs 137
#define NR_fstatfs 138
#define NR_getpriority 140
#define NR_setpriority 141
#define NR_sched_setparam 142
#define NR_sched_getparam 143
#define NR_sched_setscheduler 144
#define NR_sched_getscheduler 145
#define NR_sched_get_priority_max 146
#define NR_sched_get_priority_min 147
#define NR_mlock 149
#define NR_munlock 150
#define NR_mlockall 151
#define NR_munlockall 152
#define NR_prctl 157
#define NR_arch_prctl 158
#define NR_setrlimit 160
#define NR_gettid 186
#define NR_tkill 200
#define NR_time 201
#define NR_futex 202
#define NR_sched_setaffinity 203
#define NR_sched_getaffinity 204
#define NR_set_thread_area 205
#define NR_get_thread_area 211
#define NR_epoll_create 213
#define NR_getdents64 217
#define NR_set_tid_address 218
#define NR_restart_syscall 219
#define NR_fadvise64 221
#define NR_clock_settime 227
#define NR_clock_gettime 228
#define NR_clock_getres 229
#define NR_clock_nanosleep 230
#define NR_exit_group 231
#define NR_eventfd 284
#define NR_personality 135
#define NR_epoll_wait 232
#define NR_epoll_ctl 233
#define NR_tgkill 234
#define NR_utimes 235
#define NR_waitid 247
#define NR_openat 257
#define NR_mkdirat 258
#define NR_newfstatat 262
#define NR_unlinkat 263
#define NR_renameat 264
#define NR_linkat 265
#define NR_symlinkat 266
#define NR_readlinkat 267
#define NR_fchmodat 268
#define NR_fchownat 260
#define NR_fchmodat2 452
#define NR_faccessat 269
#define NR_pselect6 270
#define NR_ppoll 271
#define NR_set_robust_list 273
#define NR_get_robust_list 274
#define NR_utimensat 280
#define NR_epoll_pwait 281
#define NR_fallocate 285
#define NR_accept4 288
#define NR_recvmmsg 299
#define NR_sendmmsg 307
#define NR_eventfd2 290
#define NR_epoll_create1 291
#define NR_dup3 292
#define NR_pipe2 293
#define NR_preadv 295
#define NR_pwritev 296
#define NR_preadv2 327
#define NR_setxattr 188
#define NR_lsetxattr 189
#define NR_fsetxattr 190
#define NR_getxattr 191
#define NR_lgetxattr 192
#define NR_fgetxattr 193
#define NR_listxattr 194
#define NR_llistxattr 195
#define NR_flistxattr 196
#define NR_removexattr 197
#define NR_lremovexattr 198
#define NR_fremovexattr 199
#define NR_pwritev2 328
#define NR_prlimit64 302
#define NR_getcpu 309
#define NR_renameat2 316
#define NR_getrandom 318
#define NR_membarrier 324
#define NR_memfd_create 319
#define NR_statx 332
#define NR_rseq 334
#define NR_close_range 436
#define NR_clone3 435
#define NR_faccessat2 439
#define NR_epoll_pwait2 441
#define NR_futex_waitv 449

/* ---- errno (Linux values) ---------------------------------------------- */
#define LX_EPERM 1
#define LX_ENOENT 2
#define LX_ESRCH 3
#define LX_EINTR 4
#define LX_EIO 5
#define LX_ENXIO 6
#define LX_E2BIG 7
#define LX_ENOEXEC 8
#define LX_EBADF 9
#define LX_ECHILD 10
#define LX_EAGAIN 11
#define LX_ENOMEM 12
#define LX_EACCES 13
#define LX_EFAULT 14
#define LX_EBUSY 16
#define LX_EEXIST 17
#define LX_EXDEV 18
#define LX_ENODEV 19
#define LX_ENOTDIR 20
#define LX_EISDIR 21
#define LX_EINVAL 22
#define LX_ENFILE 23
#define LX_EMFILE 24
#define LX_ENOTTY 25
#define LX_ETXTBSY 26
#define LX_EFBIG 27
#define LX_ENOSPC 28
#define LX_ESPIPE 29
#define LX_EROFS 30
#define LX_EMLINK 31
#define LX_EPIPE 32
#define LX_EDOM 33
#define LX_ERANGE 34
#define LX_EDEADLK 35
#define LX_ENAMETOOLONG 36
#define LX_ENOSYS 38
#define LX_ENOTEMPTY 39
#define LX_ELOOP 40
#define LX_EOVERFLOW 75
#define LX_ENOTSOCK 88
#define LX_EDESTADDRREQ 89
#define LX_EMSGSIZE 90
#define LX_EPROTOTYPE 91
#define LX_ENOPROTOOPT 92
#define LX_EPROTONOSUPPORT 93
#define LX_ESOCKTNOSUPPORT 94
#define LX_EADDRINUSE 98
#define LX_ECONNRESET 104
#define LX_ENOBUFS 105
#define LX_EISCONN 106
#define LX_EOPNOTSUPP 95
#define LX_ENODATA 61
#define LX_ENOTSUP LX_EOPNOTSUPP
#define LX_EAFNOSUPPORT 97
#define LX_ENOTCONN 107
#define LX_ETIMEDOUT 110
#define LX_EOWNERDEAD 130
#define LX_ECONNREFUSED 111
#define LX_EINPROGRESS 115

/* ---- open flags -------------------------------------------------------- */
#define LX_O_ACCMODE 03
#define LX_O_RDONLY 00
#define LX_O_WRONLY 01
#define LX_O_RDWR 02
#define LX_O_CREAT 0100
#define LX_O_EXCL 0200
#define LX_O_NOCTTY 0400
#define LX_O_TRUNC 01000
#define LX_O_APPEND 02000
#define LX_O_NONBLOCK 04000
#define LX_O_DSYNC 010000
#define LX_O_DIRECT 040000
#define LX_O_LARGEFILE 0100000
#define LX_O_DIRECTORY 0200000
#define LX_O_NOFOLLOW 0400000
#define LX_O_NOATIME 01000000
#define LX_O_CLOEXEC 02000000
#define LX_O_SYNC 04010000
#define LX_O_PATH 010000000
#define LX_O_TMPFILE 020200000

#define LX_AT_FDCWD (-100)
#define LX_AT_SYMLINK_NOFOLLOW 0x100
#define LX_AT_REMOVEDIR 0x200
#define LX_AT_SYMLINK_FOLLOW 0x400
#define LX_AT_EMPTY_PATH 0x1000

/* ---- stat -------------------------------------------------------------- */
#define LX_S_IFMT 0170000
#define LX_S_IFSOCK 0140000
#define LX_S_IFLNK 0120000
#define LX_S_IFREG 0100000
#define LX_S_IFBLK 0060000
#define LX_S_IFDIR 0040000
#define LX_S_IFCHR 0020000
#define LX_S_IFIFO 0010000

#define LX_DT_UNKNOWN 0
#define LX_DT_FIFO 1
#define LX_DT_CHR 2
#define LX_DT_DIR 4
#define LX_DT_REG 8
#define LX_DT_LNK 10
#define LX_DT_SOCK 12

typedef struct lx_timespec { s64 tv_sec; s64 tv_nsec; } lx_timespec;
typedef struct lx_timeval { s64 tv_sec; s64 tv_usec; } lx_timeval;

typedef struct lx_stat {
    u64 st_dev;
    u64 st_ino;
    u64 st_nlink;
    u32 st_mode;
    u32 st_uid;
    u32 st_gid;
    u32 __pad0;
    u64 st_rdev;
    s64 st_size;
    s64 st_blksize;
    s64 st_blocks;
    lx_timespec st_atim;
    lx_timespec st_mtim;
    lx_timespec st_ctim;
    s64 __unused[3];
} lx_stat;

typedef struct lx_statx_ts { s64 tv_sec; u32 tv_nsec; s32 __rsv; } lx_statx_ts;
typedef struct lx_statx {
    u32 stx_mask; u32 stx_blksize; u64 stx_attributes;
    u32 stx_nlink; u32 stx_uid; u32 stx_gid; u16 stx_mode; u16 __spare0;
    u64 stx_ino; u64 stx_size; u64 stx_blocks; u64 stx_attributes_mask;
    lx_statx_ts stx_atime, stx_btime, stx_ctime, stx_mtime;
    u32 stx_rdev_major; u32 stx_rdev_minor; u32 stx_dev_major; u32 stx_dev_minor;
    u64 __spare2[14];
} lx_statx;

typedef struct lx_statfs {
    s64 f_type, f_bsize, f_blocks, f_bfree, f_bavail, f_files, f_ffree;
    s32 f_fsid[2];
    s64 f_namelen, f_frsize, f_flags, f_spare[4];
} lx_statfs;

#pragma pack(push, 1)
typedef struct lx_dirent64 {
    u64 d_ino;
    s64 d_off;
    u16 d_reclen;
    u8  d_type;
    char d_name[1];
} lx_dirent64;
#pragma pack(pop)

typedef struct lx_pollfd { s32 fd; s16 events; s16 revents; } lx_pollfd;
#define LX_POLLIN 0x001
#define LX_POLLPRI 0x002
#define LX_POLLOUT 0x004
#define LX_POLLERR 0x008
#define LX_POLLHUP 0x010
#define LX_POLLNVAL 0x020
#define LX_POLLRDNORM 0x040
#define LX_POLLWRNORM 0x100
#define LX_POLLRDHUP 0x2000

#pragma pack(push, 1)
typedef struct lx_epoll_event { u32 events; u64 data; } lx_epoll_event;
#pragma pack(pop)
#define LX_EPOLLIN 0x001
#define LX_EPOLLOUT 0x004
#define LX_EPOLLERR 0x008
#define LX_EPOLLHUP 0x010
#define LX_EPOLLRDHUP 0x2000
#define LX_EPOLLONESHOT (1u << 30)
#define LX_EPOLLET (1u << 31)
#define LX_EPOLL_CTL_ADD 1
#define LX_EPOLL_CTL_DEL 2
#define LX_EPOLL_CTL_MOD 3

typedef struct lx_iovec { u64 iov_base; u64 iov_len; } lx_iovec;

typedef struct lx_utsname {
    char sysname[65], nodename[65], release[65], version[65], machine[65], domainname[65];
} lx_utsname;

typedef struct lx_sysinfo {
    s64 uptime; u64 loads[3];
    u64 totalram, freeram, sharedram, bufferram, totalswap, freeswap;
    u16 procs; u16 pad; u32 pad2;
    u64 totalhigh, freehigh;
    u32 mem_unit; char _f[4];
} lx_sysinfo;

typedef struct lx_rlimit { u64 rlim_cur, rlim_max; } lx_rlimit;
#define LX_RLIM_INFINITY (~0ull)
#define LX_RLIMIT_CPU 0
#define LX_RLIMIT_FSIZE 1
#define LX_RLIMIT_DATA 2
#define LX_RLIMIT_STACK 3
#define LX_RLIMIT_CORE 4
#define LX_RLIMIT_RSS 5
#define LX_RLIMIT_NPROC 6
#define LX_RLIMIT_NOFILE 7
#define LX_RLIMIT_MEMLOCK 8
#define LX_RLIMIT_AS 9

typedef struct lx_rusage {
    lx_timeval ru_utime, ru_stime;
    s64 ru_maxrss, ru_ixrss, ru_idrss, ru_isrss, ru_minflt, ru_majflt, ru_nswap,
        ru_inblock, ru_oublock, ru_msgsnd, ru_msgrcv, ru_nsignals, ru_nvcsw, ru_nivcsw;
} lx_rusage;

typedef struct lx_tms { s64 tms_utime, tms_stime, tms_cutime, tms_cstime; } lx_tms;

/* ---- signals ----------------------------------------------------------- */
#define LX_NSIG 64
#define LX_SIGHUP 1
#define LX_SIGINT 2
#define LX_SIGQUIT 3
#define LX_SIGILL 4
#define LX_SIGTRAP 5
#define LX_SIGABRT 6
#define LX_SIGBUS 7
#define LX_SIGFPE 8
#define LX_SIGKILL 9
#define LX_SIGUSR1 10
#define LX_SIGSEGV 11
#define LX_SIGUSR2 12
#define LX_SIGPIPE 13
#define LX_SIGALRM 14
#define LX_SIGTERM 15
#define LX_SIGCHLD 17
#define LX_SIGCONT 18
#define LX_SIGSTOP 19

#define LX_SA_NOCLDSTOP 1u
#define LX_SA_SIGINFO 4u
#define LX_SA_ONSTACK 0x08000000u
#define LX_SA_RESTART 0x10000000u
#define LX_SA_NODEFER 0x40000000u
#define LX_SA_RESETHAND 0x80000000u
#define LX_SA_RESTORER 0x04000000u
#define LX_SIG_DFL 0ull
#define LX_SIG_IGN 1ull

typedef struct lx_ksigaction { u64 handler; u64 flags; u64 restorer; u64 mask; } lx_ksigaction;
typedef struct lx_stack_t { u64 ss_sp; s32 ss_flags; s32 pad; u64 ss_size; } lx_stack_t;
#define LX_SS_ONSTACK 1
#define LX_SS_DISABLE 2

#define LX_SIG_BLOCK 0
#define LX_SIG_UNBLOCK 1
#define LX_SIG_SETMASK 2

/* ---- mmap -------------------------------------------------------------- */
#define LX_PROT_NONE 0
#define LX_PROT_READ 1
#define LX_PROT_WRITE 2
#define LX_PROT_EXEC 4
#define LX_MAP_SHARED 0x01
#define LX_MAP_PRIVATE 0x02
#define LX_MAP_SHARED_VALIDATE 0x03
#define LX_MAP_FIXED 0x10
#define LX_MAP_ANONYMOUS 0x20
#define LX_MAP_GROWSDOWN 0x100
#define LX_MAP_DENYWRITE 0x800
#define LX_MAP_NORESERVE 0x4000
#define LX_MAP_POPULATE 0x8000
#define LX_MAP_NONBLOCK 0x10000
#define LX_MAP_STACK 0x20000
#define LX_MAP_HUGETLB 0x40000
#define LX_MAP_FIXED_NOREPLACE 0x100000
#define LX_MREMAP_MAYMOVE 1
#define LX_MREMAP_FIXED 2
#define LX_MREMAP_DONTUNMAP 4
#define LX_MADV_NORMAL 0
#define LX_MADV_DONTNEED 4
#define LX_MADV_FREE 8
#define LX_MADV_WILLNEED 3
#define LX_MADV_POPULATE_READ 22
#define LX_MADV_POPULATE_WRITE 23
#define LX_MS_ASYNC 1
#define LX_MS_INVALIDATE 2
#define LX_MS_SYNC 4

/* ---- clone ------------------------------------------------------------- */
#define LX_CLONE_VM 0x00000100
#define LX_CLONE_FS 0x00000200
#define LX_CLONE_FILES 0x00000400
#define LX_CLONE_SIGHAND 0x00000800
#define LX_CLONE_PARENT 0x00008000
#define LX_CLONE_THREAD 0x00010000
#define LX_CLONE_SYSVSEM 0x00040000
#define LX_CLONE_SETTLS 0x00080000
#define LX_CLONE_PARENT_SETTID 0x00100000
#define LX_CLONE_CHILD_CLEARTID 0x00200000
#define LX_CLONE_DETACHED 0x00400000
#define LX_CLONE_CHILD_SETTID 0x01000000

/* ---- futex ------------------------------------------------------------- */
#define LX_FUTEX_WAIT 0
#define LX_FUTEX_WAKE 1
#define LX_FUTEX_FD 2
#define LX_FUTEX_REQUEUE 3
#define LX_FUTEX_CMP_REQUEUE 4
#define LX_FUTEX_WAKE_OP 5
#define LX_FUTEX_LOCK_PI 6
#define LX_FUTEX_UNLOCK_PI 7
#define LX_FUTEX_TRYLOCK_PI 8
#define LX_FUTEX_WAIT_BITSET 9
#define LX_FUTEX_WAKE_BITSET 10
#define LX_FUTEX_WAIT_REQUEUE_PI 11
#define LX_FUTEX_CMP_REQUEUE_PI 12
#define LX_FUTEX_LOCK_PI2 13
#define LX_FUTEX_PRIVATE_FLAG 128
#define LX_FUTEX_CLOCK_REALTIME 256
#define LX_FUTEX_CMD_MASK (~(LX_FUTEX_PRIVATE_FLAG | LX_FUTEX_CLOCK_REALTIME))
#define LX_FUTEX_BITSET_MATCH_ANY 0xffffffffu

/* ---- arch_prctl / prctl ------------------------------------------------ */
#define LX_ARCH_SET_GS 0x1001
#define LX_ARCH_SET_FS 0x1002
#define LX_ARCH_GET_FS 0x1003
#define LX_ARCH_GET_GS 0x1004
#define LX_ARCH_GET_CPUID 0x1011
#define LX_ARCH_SET_CPUID 0x1012
#define LX_PR_SET_PDEATHSIG 1
#define LX_PR_GET_DUMPABLE 3
#define LX_PR_SET_DUMPABLE 4
#define LX_PR_SET_NAME 15
#define LX_PR_GET_NAME 16
#define LX_PR_SET_NO_NEW_PRIVS 38
#define LX_PR_GET_NO_NEW_PRIVS 39
#define LX_PR_SET_VMA 0x53564d41
#define LX_PR_SET_TIMERSLACK 29
#define LX_PR_GET_TIMERSLACK 30

/* ---- clocks ------------------------------------------------------------ */
#define LX_CLOCK_REALTIME 0
#define LX_CLOCK_MONOTONIC 1
#define LX_CLOCK_PROCESS_CPUTIME_ID 2
#define LX_CLOCK_THREAD_CPUTIME_ID 3
#define LX_CLOCK_MONOTONIC_RAW 4
#define LX_CLOCK_REALTIME_COARSE 5
#define LX_CLOCK_MONOTONIC_COARSE 6
#define LX_CLOCK_BOOTTIME 7
#define LX_TIMER_ABSTIME 1

/* ---- fcntl / ioctl ----------------------------------------------------- */
#define LX_F_DUPFD 0
#define LX_F_GETFD 1
#define LX_F_SETFD 2
#define LX_F_GETFL 3
#define LX_F_SETFL 4
#define LX_F_GETLK 5
#define LX_F_SETLK 6
#define LX_F_SETLKW 7
#define LX_F_SETOWN 8
#define LX_F_GETOWN 9
#define LX_F_DUPFD_CLOEXEC 1030
#define LX_F_ADD_SEALS 1033
#define LX_F_GET_SEALS 1034
#define LX_FD_CLOEXEC 1
#define LX_F_RDLCK 0
#define LX_F_WRLCK 1
#define LX_F_UNLCK 2
#define LX_LOCK_SH 1
#define LX_LOCK_EX 2
#define LX_LOCK_NB 4
#define LX_LOCK_UN 8

#define LX_TCGETS 0x5401
#define LX_TIOCGWINSZ 0x5413
#define LX_TIOCGPGRP 0x540F
#define LX_FIONREAD 0x541B
#define LX_FIONBIO 0x5421
#define LX_FIOCLEX 0x5451
#define LX_FIONCLEX 0x5450

/* ---- sockets ----------------------------------------------------------- */
#define LX_AF_UNIX 1
#define LX_AF_INET 2
#define LX_SOCK_STREAM 1
#define LX_SOCK_DGRAM 2
#define LX_SOCK_SEQPACKET 5
#define LX_SOCK_NONBLOCK 04000
#define LX_SOCK_CLOEXEC 02000000
#define LX_SOL_SOCKET 1
#define LX_SCM_RIGHTS 1
#define LX_SCM_CREDENTIALS 2
#define LX_AF_UNSPEC 0
#define LX_MSG_PEEK 2
#define LX_MSG_DONTWAIT 0x40
#define LX_MSG_NOSIGNAL 0x4000
#define LX_MSG_CTRUNC 8
#define LX_SHUT_RD 0
#define LX_SHUT_WR 1
#define LX_SHUT_RDWR 2

typedef struct lx_msghdr {
    u64 msg_name; u32 msg_namelen; u32 pad0;
    u64 msg_iov; u64 msg_iovlen;
    u64 msg_control; u64 msg_controllen;
    u32 msg_flags; u32 pad1;
} lx_msghdr;
typedef struct lx_cmsghdr { u64 cmsg_len; s32 cmsg_level; s32 cmsg_type; } lx_cmsghdr;

#define LX_EFD_SEMAPHORE 1
#define LX_EFD_NONBLOCK 04000
#define LX_EFD_CLOEXEC 02000000

#define LX_GRND_NONBLOCK 1
#define LX_GRND_RANDOM 2

#define LX_PAGE 4096ull

/* Linux mcontext/ucontext for rt_sigframe construction (x86-64). */
typedef struct lx_sigcontext {
    u64 r8, r9, r10, r11, r12, r13, r14, r15;
    u64 rdi, rsi, rbp, rbx, rdx, rax, rcx, rsp, rip, eflags;
    u16 cs, gs, fs, ss;
    u64 err, trapno, oldmask, cr2;
    u64 fpstate;
    u64 reserved1[8];
} lx_sigcontext;

typedef struct lx_ucontext {
    u64 uc_flags;
    u64 uc_link;
    lx_stack_t uc_stack;
    lx_sigcontext uc_mcontext;
    u64 uc_sigmask;
} lx_ucontext;

typedef struct lx_siginfo {
    s32 si_signo, si_errno, si_code, pad0;
    union {
        struct { u64 si_addr; } fault;
        struct { s32 si_pid; u32 si_uid; } kill;
        u8 pad[112];
    } u;
} lx_siginfo;

/* Linux fxsave-layout fpstate */
typedef struct lx_fpstate {
    u16 cwd, swd, twd, fop;
    u64 rip, rdp;
    u32 mxcsr, mxcr_mask;
    u32 st_space[32];
    u32 xmm_space[64];
    u32 reserved[24];
} lx_fpstate;

#endif /* XRL_LX_H */
