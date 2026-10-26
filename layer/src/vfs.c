/*
 * vfs.c - the guest's view of the filesystem, mapped onto the writable
 * LocalState tree (xrl_boot_context.vfs_root).
 *
 *   guest "/"            -> <vfs_root>\
 *   guest "/a/b"         -> <vfs_root>\a\b          (via \\?\ extended paths)
 *   /dev/{null,zero,urandom,random,tty,...}, /proc, /sys   -> synthesized
 *
 * Details that matter:
 *   - Symlinks. LocalState (AppContainer) cannot create NTFS symlinks, so
 *     symlinks live in a table persisted at <vfs_root>\.xrl-symlinks
 *     (one "guest_path<TAB>target" per line). Resolution walks components and
 *     substitutes; getdents() injects them as DT_LNK; readlink()/lstat() see
 *     them. The prefix baker (wine/ tooling) writes this file for the Wine
 *     prefix's dosdevices links; symlink(2) from the guest appends to it.
 *   - Names Windows cannot represent (':' '*' '?' '"' '<' '>' '|' '\' control
 *     chars, trailing space/dot) are stored as U+F0xx private-use characters
 *     (Cygwin-style "POSIX mode") and decoded again by getdents().
 *   - The host file system is case-insensitive; the guest sees exact-case names
 *     as stored. Wine tolerates this (it does its own case-insensitive lookup).
 *   - Only LocalState + TEMP are writable on the console; everything lives
 *     under vfs_root, so nothing here ever touches C:\.
 *   - st_ino is derived from the NTFS FileId; getdents d_ino is a path hash
 *     (documented imperfection - Find* does not return file ids).
 */
#include "xrl_internal.h"

/* ============================================================== symlinks */
typedef struct sym { char *path; char *target; } sym;
static sym *g_sym;
static size_t g_nsym, g_capsym;
static SRWLOCK g_sym_lock = SRWLOCK_INIT;
static wchar_t g_symfile_w[1024];

static int sym_find(const char *path)   /* caller holds lock */
{
    for (size_t i = 0; i < g_nsym; i++) if (!strcmp(g_sym[i].path, path)) return (int)i;
    return -1;
}

static char *sym_lookup(const char *path)
{
    char *r = NULL;
    AcquireSRWLockShared(&g_sym_lock);
    int i = sym_find(path);
    if (i >= 0) r = _strdup(g_sym[i].target);
    ReleaseSRWLockShared(&g_sym_lock);
    return r;
}

static void sym_persist(void)   /* caller holds exclusive lock */
{
    if (!g_symfile_w[0]) return;
    HANDLE h = CreateFile2(g_symfile_w, GENERIC_WRITE, 0, CREATE_ALWAYS, NULL);
    if (h == INVALID_HANDLE_VALUE) { LOGW("vfs: cannot persist symlink table (err %lu)", GetLastError()); return; }
    for (size_t i = 0; i < g_nsym; i++) {
        char line[2200];
        int n = snprintf(line, sizeof line, "%s\t%s\n", g_sym[i].path, g_sym[i].target);
        DWORD w; WriteFile(h, line, (DWORD)n, &w, NULL);
    }
    CloseHandle(h);
}

static int sym_add(const char *path, const char *target, int persist)
{
    AcquireSRWLockExclusive(&g_sym_lock);
    if (sym_find(path) >= 0) { ReleaseSRWLockExclusive(&g_sym_lock); return -LX_EEXIST; }
    if (g_nsym == g_capsym) {
        size_t nc = g_capsym ? g_capsym * 2 : 64;
        sym *n = (sym *)realloc(g_sym, nc * sizeof(sym));
        if (!n) { ReleaseSRWLockExclusive(&g_sym_lock); return -LX_ENOMEM; }
        g_sym = n; g_capsym = nc;
    }
    g_sym[g_nsym].path = _strdup(path);
    g_sym[g_nsym].target = _strdup(target);
    g_nsym++;
    if (persist) sym_persist();
    ReleaseSRWLockExclusive(&g_sym_lock);
    return 0;
}

static int sym_remove(const char *path)
{
    int r = -LX_ENOENT;
    AcquireSRWLockExclusive(&g_sym_lock);
    int i = sym_find(path);
    if (i >= 0) {
        free(g_sym[i].path); free(g_sym[i].target);
        g_sym[i] = g_sym[--g_nsym];
        sym_persist();
        r = 0;
    }
    ReleaseSRWLockExclusive(&g_sym_lock);
    return r;
}

static void sym_load(const char *file_utf8_unused)
{
    (void)file_utf8_unused;
    HANDLE h = CreateFile2(g_symfile_w, GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart > (16 << 20)) { CloseHandle(h); return; }
    char *buf = (char *)malloc((size_t)sz.QuadPart + 1);
    DWORD got = 0;
    if (buf && ReadFile(h, buf, (DWORD)sz.QuadPart, &got, NULL)) {
        buf[got] = 0;
        char *p = buf;
        while (*p) {
            char *nl = strchr(p, '\n');
            if (nl) *nl = 0;
            char *tab = strchr(p, '\t');
            size_t ln = strlen(p);
            if (ln && p[ln - 1] == '\r') p[ln - 1] = 0;
            if (tab && p[0] == '/') { *tab = 0; sym_add(p, tab + 1, 0); }
            if (!nl) break;
            p = nl + 1;
        }
        LOGI("vfs: loaded %zu symlinks from .xrl-symlinks", g_nsym);
    }
    free(buf);
    CloseHandle(h);
}

/* ======================================================== name encoding */
static int enc_needs(wchar_t c)
{
    return c < 0x20 || c == L'"' || c == L'*' || c == L':' || c == L'<' || c == L'>' || c == L'?' || c == L'|' || c == L'\\';
}

/* UTF-8 component -> host UTF-16 with private-use escapes. Returns wchar count or -1. */
static int comp_to_host(const char *s, size_t len, wchar_t *out, size_t cap)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, (int)len, out, (int)cap);
    if (n <= 0 && len) return -1;
    for (int i = 0; i < n; i++) if (enc_needs(out[i])) out[i] = (wchar_t)(0xF000 + out[i]);
    if (n > 0 && (out[n - 1] == L' ' || out[n - 1] == L'.')) out[n - 1] = (wchar_t)(0xF000 + out[n - 1]);
    return n;
}

static int comp_from_host(const wchar_t *w, int wlen, char *out, size_t cap)
{
    wchar_t tmp[512];
    if (wlen > 511) return -1;
    for (int i = 0; i < wlen; i++) {
        wchar_t c = w[i];
        if ((c & 0xFF00) == 0xF000) {
            wchar_t d = (wchar_t)(c & 0xFF);
            if (enc_needs(d) || d == L' ' || d == L'.') c = d;
        }
        tmp[i] = c;
    }
    int n = WideCharToMultiByte(CP_UTF8, 0, tmp, wlen, out, (int)cap - 1, NULL, NULL);
    if (n <= 0 && wlen) return -1;
    out[n] = 0;
    return n;
}

/* ============================================================== init */
slong xrl_vfs_init(const char *root)
{
    size_t rl = strlen(root);
    G.vfs_root = _strdup(root);
    while (rl > 3 && (G.vfs_root[rl - 1] == '\\' || G.vfs_root[rl - 1] == '/')) G.vfs_root[--rl] = 0;
    wchar_t tmp[1024];
    int n = MultiByteToWideChar(CP_UTF8, 0, G.vfs_root, -1, tmp, 1000);
    if (n <= 0) return -LX_EINVAL;
    for (int i = 0; i < n; i++) if (tmp[i] == L'/') tmp[i] = L'\\';
    const wchar_t *pfx = L"\\\\?\\";
    size_t wl = (size_t)n + 8;
    G.vfs_root_w = (wchar_t *)calloc(wl, sizeof(wchar_t));
    if (wcsncmp(tmp, L"\\\\?\\", 4) == 0 || wcsncmp(tmp, L"\\\\.\\", 4) == 0) wcscpy_s(G.vfs_root_w, wl, tmp);
    else { wcscpy_s(G.vfs_root_w, wl, pfx); wcscat_s(G.vfs_root_w, wl, tmp); }

    DWORD a = GetFileAttributesW(G.vfs_root_w);
    if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) {
        if (!CreateDirectoryW(G.vfs_root_w, NULL)) {
            LOGE("vfs: vfs_root '%s' does not exist and cannot be created (err %lu)", G.vfs_root, GetLastError());
            return -LX_ENOENT;
        }
    }
    static const wchar_t *dirs[] = { L"\\tmp", L"\\dev", L"\\dev\\shm", L"\\var", L"\\var\\tmp", L"\\run", L"\\home", L"\\etc" };
    for (size_t i = 0; i < ARRAY_LEN(dirs); i++) {
        wchar_t p[1100]; wcscpy_s(p, 1100, G.vfs_root_w); wcscat_s(p, 1100, dirs[i]);
        CreateDirectoryW(p, NULL);
    }
    wcscpy_s(g_symfile_w, 1024, G.vfs_root_w); wcscat_s(g_symfile_w, 1024, L"\\.xrl-symlinks");
    sym_load(NULL);
    /* conventional device symlinks */
    sym_add("/dev/stdin", "/proc/self/fd/0", 0);
    sym_add("/dev/stdout", "/proc/self/fd/1", 0);
    sym_add("/dev/stderr", "/proc/self/fd/2", 0);
    sym_add("/dev/fd", "/proc/self/fd", 0);
    strcpy_s(G.cwd, sizeof G.cwd, "/");
    LOGI("vfs: root=%s (%zu symlinks)", G.vfs_root, g_nsym);
    return 0;
}

int xrl_vfs_host_path(const char *gabs, wchar_t *out, size_t cap)
{
    size_t rl = wcslen(G.vfs_root_w);
    if (rl + 2 >= cap) return -LX_ENAMETOOLONG;
    wmemcpy(out, G.vfs_root_w, rl);
    size_t o = rl;
    const char *p = gabs;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *e = p;
        while (*e && *e != '/') e++;
        if (o + 2 >= cap) return -LX_ENAMETOOLONG;
        out[o++] = L'\\';
        int n = comp_to_host(p, (size_t)(e - p), out + o, cap - o - 1);
        if (n < 0) return -LX_ENAMETOOLONG;
        o += (size_t)n;
        p = e;
    }
    out[o] = 0;
    return 0;
}

/* ============================================================== resolve */
#define PATHBUF 4096

static slong dirfd_path(int dirfd, char *out, size_t cap)
{
    if (dirfd == LX_AT_FDCWD) {
        AcquireSRWLockShared(&G.cwd_lock);
        strcpy_s(out, cap, G.cwd);
        ReleaseSRWLockShared(&G.cwd_lock);
        return 0;
    }
    xfile *f = xrl_fd_get(dirfd);
    if (!f) return -LX_EBADF;
    slong r = 0;
    if (!f->gpath) r = -LX_ENOTDIR;
    else strcpy_s(out, cap, f->gpath);
    xfile_unref(f);
    return r;
}

/* Normalize + substitute symlinks. out receives an absolute guest path. */
slong xrl_vfs_resolve(int dirfd, const char *path, int follow, char *out, size_t cap)
{
    char cur[PATHBUF], rest[PATHBUF * 2];
    int depth = 0;
    if (!path[0]) return -LX_ENOENT;
    if (strlen(path) >= PATHBUF) return -LX_ENAMETOOLONG;
    if (path[0] == '/') { strcpy_s(cur, sizeof cur, "/"); }
    else { slong r = dirfd_path(dirfd, cur, sizeof cur); if (r) return r; }
    strcpy_s(rest, sizeof rest, path);
    char *rp = rest;
    while (*rp) {
        while (*rp == '/') rp++;
        if (!*rp) break;
        char *e = rp;
        while (*e && *e != '/') e++;
        char comp[256];
        size_t cl = (size_t)(e - rp);
        if (cl >= sizeof comp) return -LX_ENAMETOOLONG;
        memcpy(comp, rp, cl); comp[cl] = 0;
        int more = 0;
        { char *q = e; while (*q == '/') q++; more = *q != 0; }
        rp = e;
        if (!strcmp(comp, ".")) continue;
        if (!strcmp(comp, "..")) {
            char *sl = strrchr(cur, '/');
            if (sl && sl != cur) *sl = 0; else strcpy_s(cur, sizeof cur, "/");
            continue;
        }
        char cand[PATHBUF];
        if (strlen(cur) + cl + 2 >= PATHBUF) return -LX_ENAMETOOLONG;
        if (!strcmp(cur, "/")) snprintf(cand, sizeof cand, "/%s", comp);
        else snprintf(cand, sizeof cand, "%s/%s", cur, comp);
        char *tgt = (follow || more) ? sym_lookup(cand) : NULL;
        if (tgt) {
            if (++depth > 40) { free(tgt); return -LX_ELOOP; }
            char nrest[PATHBUF * 2];
            snprintf(nrest, sizeof nrest, "%s/%s", tgt, rp);
            if (tgt[0] == '/') strcpy_s(cur, sizeof cur, "/");
            free(tgt);
            memmove(rest, nrest, strlen(nrest) + 1);
            rp = rest;
            continue;
        }
        strcpy_s(cur, sizeof cur, cand);
    }
    if (strlen(cur) >= cap) return -LX_ENAMETOOLONG;
    strcpy_s(out, cap, cur);
    return 0;
}

/* ================================================== host file helpers */
#define SHARE_ALL (FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE)

static HANDLE hopen(const wchar_t *hp, DWORD access, DWORD disp, DWORD fileflags)
{
    CREATEFILE2_EXTENDED_PARAMETERS p;
    memset(&p, 0, sizeof p);
    p.dwSize = sizeof p;
    p.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
    p.dwFileFlags = fileflags;
    return CreateFile2(hp, access, SHARE_ALL, disp, &p);
}

static void ft_to_ts(FILETIME ft, lx_timespec *ts)
{
    u64 t = ((u64)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    if (t < 116444736000000000ull) { ts->tv_sec = 0; ts->tv_nsec = 0; return; }
    t -= 116444736000000000ull;
    ts->tv_sec = (s64)(t / 10000000ull);
    ts->tv_nsec = (s64)(t % 10000000ull) * 100;
}

static u64 hash_str(const char *s)
{
    u64 h = 1469598103934665603ull;
    for (; *s; s++) { h ^= (u8)*s; h *= 1099511628211ull; }
    return h;
}

static int is_exec_name(const char *p)
{
    const char *dot = strrchr(p, '.');
    if (!dot) return 0;
    static const char *ex[] = { ".exe", ".dll", ".so", ".sh", ".drv", ".sys", ".ocx", ".cpl", ".scr", ".com", ".bat", ".cmd", ".acm", ".ax", ".vxd", ".tlb" };
    for (size_t i = 0; i < ARRAY_LEN(ex); i++) if (!_stricmp(dot, ex[i])) return 1;
    return 0;
}

static slong fstat_handle(HANDLE h, const char *gpath, lx_stat *st)
{
    FILE_BASIC_INFO bi; FILE_STANDARD_INFO si;
    if (!GetFileInformationByHandleEx(h, FileBasicInfo, &bi, sizeof bi)) return xrl_lasterr();
    if (!GetFileInformationByHandleEx(h, FileStandardInfo, &si, sizeof si)) return xrl_lasterr();
    memset(st, 0, sizeof *st);
    int isdir = (bi.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    u32 perm = isdir ? 0755 : ((gpath && is_exec_name(gpath)) ? 0755 : 0644);
    if (bi.FileAttributes & FILE_ATTRIBUTE_READONLY) perm &= ~0222u;
    st->st_mode = (isdir ? LX_S_IFDIR : LX_S_IFREG) | perm;
    st->st_nlink = isdir ? 2 : (si.NumberOfLinks ? si.NumberOfLinks : 1);
    st->st_size = isdir ? 4096 : si.EndOfFile.QuadPart;
    st->st_blksize = 4096;
    st->st_blocks = (si.AllocationSize.QuadPart + 511) / 512;
    st->st_uid = 1000; st->st_gid = 1000;
    FILETIME ft;
    ft.dwLowDateTime = bi.LastAccessTime.LowPart; ft.dwHighDateTime = bi.LastAccessTime.HighPart; ft_to_ts(ft, &st->st_atim);
    ft.dwLowDateTime = bi.LastWriteTime.LowPart;  ft.dwHighDateTime = bi.LastWriteTime.HighPart;  ft_to_ts(ft, &st->st_mtim);
    ft.dwLowDateTime = bi.ChangeTime.LowPart;     ft.dwHighDateTime = bi.ChangeTime.HighPart;     ft_to_ts(ft, &st->st_ctim);
    FILE_ID_INFO ii;
    if (GetFileInformationByHandleEx(h, FileIdInfo, &ii, sizeof ii)) {
        u64 lo, hi; memcpy(&lo, ii.FileId.Identifier, 8); memcpy(&hi, ii.FileId.Identifier + 8, 8);
        st->st_ino = lo ^ (hi * 0x9E3779B97F4A7C15ull);
        st->st_dev = (u64)(u32)ii.VolumeSerialNumber;
    } else {
        st->st_ino = gpath ? hash_str(gpath) : 1;
        st->st_dev = 1;
    }
    if (!st->st_ino) st->st_ino = 1;
    return 0;
}

/* ============================================================ disk files */
typedef struct dirstate {
    HANDLE find;
    WIN32_FIND_DATAW fd;
    int have_fd;           /* fd holds an unconsumed entry */
    int phase;             /* 0 ".", 1 "..", 2 find, 3 virtual symlinks, 4 done */
    size_t vidx;
    s64 cookie;
    wchar_t *pattern;
    int exhausted;
} dirstate;

static slong dfile_read(xfile *f, void *buf, u64 n, s64 off)
{
    if (f->h == INVALID_HANDLE_VALUE || !(f->mode == 0)) return -LX_EISDIR;
    if ((f->flags & LX_O_ACCMODE) == LX_O_WRONLY) return -LX_EBADF;
    slong pr = xrl_mem_populate((u64)(uintptr_t)buf, n);   /* the kernel writes buf: no lazy pages */
    if (pr) return pr;
    int use_pos = off < 0;
    if (use_pos) { AcquireSRWLockExclusive(&f->lock); off = f->pos; }
    u64 total = 0;
    slong err = 0;
    int retries = 0;
    while (total < n) {
        DWORD chunk = (DWORD)((n - total) > 0x40000000ull ? 0x40000000ull : (n - total));
        OVERLAPPED ov; memset(&ov, 0, sizeof ov);
        ov.Offset = (DWORD)((u64)off + total); ov.OffsetHigh = (DWORD)(((u64)off + total) >> 32);
        DWORD got = 0;
        if (!ReadFile(f->h, (char *)buf + total, chunk, &got, &ov)) {
            DWORD e = GetLastError();
            if (e == ERROR_HANDLE_EOF) break;
            /* the swap clock probed the buffer after the populate: again */
            if (e == ERROR_NOACCESS && retries++ < 4 && !xrl_mem_populate((u64)(uintptr_t)buf + total, chunk)) continue;
            err = -(slong)xrl_win_to_errno(e);
            break;
        }
        total += got;
        if (got < chunk) break;
    }
    if (use_pos) { if (!err || total) f->pos = off + (s64)total; ReleaseSRWLockExclusive(&f->lock); }
    if (err && !total) return err;
    return (slong)total;
}

static slong dfile_write(xfile *f, const void *buf, u64 n, s64 off)
{
    if (f->h == INVALID_HANDLE_VALUE || f->mode != 0) return -LX_EISDIR;
    if ((f->flags & LX_O_ACCMODE) == LX_O_RDONLY) return -LX_EBADF;
    slong pr = xrl_mem_populate((u64)(uintptr_t)buf, n);   /* the kernel reads buf: no lazy pages */
    if (pr) return pr;
    int use_pos = off < 0;
    if (use_pos) {
        AcquireSRWLockExclusive(&f->lock);
        off = f->pos;
        if (f->flags & LX_O_APPEND) { LARGE_INTEGER sz; if (GetFileSizeEx(f->h, &sz)) off = sz.QuadPart; }
    }
    u64 total = 0;
    slong err = 0;
    int retries = 0;
    while (total < n) {
        DWORD chunk = (DWORD)((n - total) > 0x40000000ull ? 0x40000000ull : (n - total));
        OVERLAPPED ov; memset(&ov, 0, sizeof ov);
        ov.Offset = (DWORD)((u64)off + total); ov.OffsetHigh = (DWORD)(((u64)off + total) >> 32);
        DWORD got = 0;
        if (!WriteFile(f->h, (const char *)buf + total, chunk, &got, &ov)) {
            DWORD e = GetLastError();
            if (e == ERROR_NOACCESS && retries++ < 4 && !xrl_mem_populate((u64)(uintptr_t)buf + total, chunk)) continue;
            err = -(slong)xrl_win_to_errno(e);
            break;
        }
        total += got;
        if (got < chunk) break;
    }
    if (use_pos) { if (total) f->pos = off + (s64)total; ReleaseSRWLockExclusive(&f->lock); }
    if (err && !total) return err;
    return (slong)total;
}

static slong dfile_lseek(xfile *f, s64 off, int wh)
{
    s64 np;
    AcquireSRWLockExclusive(&f->lock);
    if (f->mode == 1) {   /* directory */
        slong r = 0;
        if (wh == 0 && off == 0) {
            dirstate *d = (dirstate *)f->priv;
            if (d) { if (d->find != INVALID_HANDLE_VALUE) FindClose(d->find); d->find = INVALID_HANDLE_VALUE; d->have_fd = 0; d->phase = 0; d->vidx = 0; d->cookie = 0; d->exhausted = 0; }
            f->pos = 0;
        } else r = (wh == 1 && off == 0) ? f->pos : -LX_EINVAL;
        ReleaseSRWLockExclusive(&f->lock);
        return r;
    }
    switch (wh) {
    case 0: np = off; break;
    case 1: np = f->pos + off; break;
    case 2: { LARGE_INTEGER sz; if (!GetFileSizeEx(f->h, &sz)) { ReleaseSRWLockExclusive(&f->lock); return xrl_lasterr(); } np = sz.QuadPart + off; break; }
    default: ReleaseSRWLockExclusive(&f->lock); return -LX_EINVAL;
    }
    if (np < 0) { ReleaseSRWLockExclusive(&f->lock); return -LX_EINVAL; }
    f->pos = np;
    ReleaseSRWLockExclusive(&f->lock);
    return (slong)np;
}

static slong dfile_fstat(xfile *f, lx_stat *st) { return fstat_handle(f->h, f->gpath, st); }

static slong dfile_ftruncate(xfile *f, s64 len)
{
    if (f->mode == 1) return -LX_EISDIR;
    if ((f->flags & LX_O_ACCMODE) == LX_O_RDONLY) return -LX_EINVAL;
    FILE_END_OF_FILE_INFO e; e.EndOfFile.QuadPart = len;
    if (!SetFileInformationByHandle(f->h, FileEndOfFileInfo, &e, sizeof e)) return xrl_lasterr();
    return 0;
}

static slong dfile_fsync(xfile *f) { return FlushFileBuffers(f->h) ? 0 : xrl_lasterr(); }
static u32 dfile_poll(xfile *f, u32 ev) { (void)f; return ev & (LX_POLLIN | LX_POLLOUT); }

static slong dfile_ioctl(xfile *f, u64 req, u64 arg)
{
    (void)f;
    if (req == LX_FIOCLEX || req == LX_FIONCLEX || req == LX_FIONBIO) return 0;
    if (req == LX_FIONREAD) {
        LARGE_INTEGER sz;
        s64 avail = 0;
        if (GetFileSizeEx(f->h, &sz)) avail = sz.QuadPart - f->pos;
        if (avail < 0) avail = 0;
        int a = (int)avail;
        return xrl_ctu(arg, &a, 4);
    }
    return -LX_ENOTTY;
}

static void dir_close(xfile *f)
{
    dirstate *d = (dirstate *)f->priv;
    if (d) { if (d->find != INVALID_HANDLE_VALUE) FindClose(d->find); free(d->pattern); free(d); }
}

static int put_dirent(char *buf, u64 cap, u64 *used, u64 ino, s64 off, int type, const char *name)
{
    size_t nl = strlen(name);
    u64 rl = (19 + nl + 1 + 7) & ~7ull;
    if (*used + rl > cap) return 0;
    lx_dirent64 *d = (lx_dirent64 *)(buf + *used);
    memset(d, 0, (size_t)rl);
    d->d_ino = ino; d->d_off = off; d->d_reclen = (u16)rl; d->d_type = (u8)type;
    memcpy(d->d_name, name, nl + 1);
    *used += rl;
    return 1;
}

static slong dir_getdents(xfile *f, void *ubuf, u64 cap)
{
    dirstate *d = (dirstate *)f->priv;
    if (!d) return -LX_ENOTDIR;
    char *kbuf = (char *)malloc((size_t)(cap > 65536 ? 65536 : cap));
    if (!kbuf) return -LX_ENOMEM;
    if (cap > 65536) cap = 65536;
    u64 used = 0;
    AcquireSRWLockExclusive(&f->lock);
    for (;;) {
        char name[1024];
        int type = LX_DT_REG;
        u64 ino;
        if (d->phase == 0) { if (!put_dirent(kbuf, cap, &used, hash_str(f->gpath) ^ 1, d->cookie + 1, LX_DT_DIR, ".")) break; d->cookie++; d->phase = 1; continue; }
        if (d->phase == 1) { if (!put_dirent(kbuf, cap, &used, hash_str(f->gpath) ^ 2, d->cookie + 1, LX_DT_DIR, "..")) break; d->cookie++; d->phase = 2; continue; }
        if (d->phase == 2) {
            if (!d->have_fd) {
                if (d->find == INVALID_HANDLE_VALUE && !d->exhausted) {
                    d->find = FindFirstFileExW(d->pattern, FindExInfoBasic, &d->fd, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
                    if (d->find == INVALID_HANDLE_VALUE) { d->phase = 3; continue; }
                    d->have_fd = 1;
                } else if (d->find != INVALID_HANDLE_VALUE) {
                    if (!FindNextFileW(d->find, &d->fd)) { d->phase = 3; continue; }
                    d->have_fd = 1;
                } else { d->phase = 3; continue; }
            }
            const wchar_t *wn = d->fd.cFileName;
            if (!wcscmp(wn, L".") || !wcscmp(wn, L"..")) { d->have_fd = 0; continue; }
            if (comp_from_host(wn, (int)wcslen(wn), name, sizeof name) < 0) { d->have_fd = 0; continue; }
            type = (d->fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? LX_DT_DIR : LX_DT_REG;
            char full[PATHBUF];
            snprintf(full, sizeof full, "%s/%s", strcmp(f->gpath, "/") ? f->gpath : "", name);
            ino = hash_str(full);
            if (!put_dirent(kbuf, cap, &used, ino, d->cookie + 1, type, name)) break;
            d->cookie++; d->have_fd = 0;
            continue;
        }
        if (d->phase == 3) {
            /* inject symlinks living directly in this directory */
            int emitted = 0, stop = 0;
            AcquireSRWLockShared(&g_sym_lock);
            for (; d->vidx < g_nsym; d->vidx++) {
                const char *sp = g_sym[d->vidx].path;
                const char *sl = strrchr(sp, '/');
                if (!sl) continue;
                size_t plen = (size_t)(sl - sp);
                const char *me = f->gpath;
                size_t ml = strcmp(me, "/") ? strlen(me) : 0;
                if (plen != ml || strncmp(sp, me, ml)) continue;
                if (!put_dirent(kbuf, cap, &used, hash_str(sp), d->cookie + 1, LX_DT_LNK, sl + 1)) { stop = 1; break; }
                d->cookie++; emitted++;
            }
            ReleaseSRWLockShared(&g_sym_lock);
            (void)emitted;
            if (stop) break;
            d->phase = 4;
        }
        break;
    }
    ReleaseSRWLockExclusive(&f->lock);
    slong r;
    if (used == 0) r = 0;
    else r = xrl_ctu((u64)ubuf, kbuf, (size_t)used) ? -LX_EFAULT : (slong)used;
    free(kbuf);
    return r;
}

static const xfile_ops g_file_ops = { "file", dfile_read, dfile_write, dfile_lseek, dfile_fstat, NULL, dfile_ioctl, dfile_ftruncate, dfile_fsync, dfile_poll, NULL, dfile_read };
static const xfile_ops g_dir_ops = { "dir", NULL, NULL, dfile_lseek, dfile_fstat, dir_getdents, dfile_ioctl, NULL, NULL, dfile_poll, dir_close, NULL };

static xfile *wrap_handle(HANDLE h, int flags, const char *gpath, int isdir, const wchar_t *hostpath)
{
    xfile *f = xfile_new(isdir ? &g_dir_ops : &g_file_ops, flags);
    if (!f) return NULL;
    f->h = h;
    f->gpath = _strdup(gpath);
    f->mode = isdir ? 1 : 0;
    if (isdir) {
        dirstate *d = (dirstate *)calloc(1, sizeof *d);
        size_t pl = wcslen(hostpath) + 3;
        d->pattern = (wchar_t *)malloc(pl * sizeof(wchar_t));
        wcscpy_s(d->pattern, pl, hostpath);
        wcscat_s(d->pattern, pl, L"\\*");
        d->find = INVALID_HANDLE_VALUE;
        f->priv = d;
    }
    return f;
}

/* ================================================================= open */
static slong open_special(const char *gabs, int flags, int *handled)
{
    *handled = 1;
    xfile *f = NULL;
    if (!strcmp(gabs, "/dev/null")) f = xrl_dev_null(flags);
    else if (!strcmp(gabs, "/dev/zero")) f = xrl_dev_zero(flags);
    else if (!strcmp(gabs, "/dev/urandom") || !strcmp(gabs, "/dev/random")) f = xrl_dev_urandom(flags);
    else if (!strcmp(gabs, "/dev/tty") || !strcmp(gabs, "/dev/console") || !strcmp(gabs, "/dev/ptmx")) f = xrl_dev_tty(flags);
    else if (!strcmp(gabs, "/dev/full")) f = xrl_dev_null(flags);
    else if (!strcmp(gabs, "/dev/ntsync")) f = xrl_dev_ntsync(flags);
    if (f) {
        f->gpath = _strdup(gabs);
        int fd = xrl_fd_install(f, (flags & LX_O_CLOEXEC) != 0, 0);
        xfile_unref(f);
        return fd;
    }
    if (!strncmp(gabs, "/proc", 5) || !strncmp(gabs, "/sys", 4)) {
        int err = 0;
        f = xrl_proc_open(gabs, flags, &err);
        if (!f) return err ? -err : -LX_ENOENT;
        int fd = xrl_fd_install(f, (flags & LX_O_CLOEXEC) != 0, 0);
        xfile_unref(f);
        return fd;
    }
    *handled = 0;
    return 0;
}

slong xrl_vfs_open(int dirfd, const char *path, int flags, int mode)
{
    (void)mode;
    char gabs[PATHBUF];
    int follow = !(flags & LX_O_NOFOLLOW);
    slong r = xrl_vfs_resolve(dirfd, path, follow, gabs, sizeof gabs);
    if (r) return r;
    if ((flags & LX_O_TMPFILE) == LX_O_TMPFILE) return -LX_EOPNOTSUPP;

    /* /proc symlinks (exe, cwd, fd/N) open their target */
    if (!strncmp(gabs, "/proc/", 6)) {
        char tgt[PATHBUF];
        slong n = xrl_proc_readlink(gabs, tgt, sizeof tgt - 1);
        if (n > 0 && follow) { tgt[n] = 0; r = xrl_vfs_resolve(LX_AT_FDCWD, tgt, 1, gabs, sizeof gabs); if (r) return r; }
    }

    int handled;
    r = open_special(gabs, flags, &handled);
    if (handled) return r;

    wchar_t hp[PATHBUF];
    r = xrl_vfs_host_path(gabs, hp, ARRAY_LEN(hp));
    if (r) return r;

    int acc = flags & LX_O_ACCMODE;
    DWORD access = acc == LX_O_RDONLY ? GENERIC_READ : acc == LX_O_WRONLY ? GENERIC_WRITE : (GENERIC_READ | GENERIC_WRITE);
    DWORD disp;
    if (flags & LX_O_CREAT) {
        if (flags & LX_O_EXCL) disp = CREATE_NEW;
        else if (flags & LX_O_TRUNC) disp = CREATE_ALWAYS;
        else disp = OPEN_ALWAYS;
    } else if (flags & LX_O_TRUNC) disp = TRUNCATE_EXISTING;
    else disp = OPEN_EXISTING;

    /* is it a directory? */
    DWORD attr = GetFileAttributesW(hp);
    int isdir = attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
    if (isdir) {
        if (acc != LX_O_RDONLY || (flags & (LX_O_TRUNC)) ) return -LX_EISDIR;
        if ((flags & LX_O_CREAT) && (flags & LX_O_EXCL)) return -LX_EEXIST;
        HANDLE h = hopen(hp, FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY | SYNCHRONIZE, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS);
        if (h == INVALID_HANDLE_VALUE) return xrl_lasterr();
        xfile *f = wrap_handle(h, flags, gabs, 1, hp);
        int fd = xrl_fd_install(f, (flags & LX_O_CLOEXEC) != 0, 0);
        xfile_unref(f);
        return fd;
    }
    if (flags & LX_O_DIRECTORY) return (attr == INVALID_FILE_ATTRIBUTES) ? -LX_ENOENT : -LX_ENOTDIR;

    HANDLE h = hopen(hp, access, disp, FILE_ATTRIBUTE_NORMAL);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        if (e == ERROR_ACCESS_DENIED && (flags & LX_O_CREAT)) {
            DWORD a2 = GetFileAttributesW(hp);
            if (a2 != INVALID_FILE_ATTRIBUTES && (a2 & FILE_ATTRIBUTE_DIRECTORY)) return -LX_EISDIR;
        }
        return -(slong)xrl_win_to_errno(e);
    }
    xfile *f = wrap_handle(h, flags & ~(LX_O_CREAT | LX_O_EXCL | LX_O_TRUNC), gabs, 0, hp);
    if (!f) { CloseHandle(h); return -LX_ENOMEM; }
    int fd = xrl_fd_install(f, (flags & LX_O_CLOEXEC) != 0, 0);
    xfile_unref(f);
    return fd;
}

slong xrl_memfd_create(const char *name, unsigned flags)
{
    (void)name;
    static volatile LONG ctr;
    char g[128];
    snprintf(g, sizeof g, "/tmp/.memfd-%d-%d", G.pid, (int)InterlockedIncrement(&ctr));
    wchar_t hp[PATHBUF];
    slong r = xrl_vfs_host_path(g, hp, ARRAY_LEN(hp));
    if (r) return r;
    HANDLE h = hopen(hp, GENERIC_READ | GENERIC_WRITE | DELETE, CREATE_NEW, FILE_FLAG_DELETE_ON_CLOSE);
    if (h == INVALID_HANDLE_VALUE) return xrl_lasterr();
    xfile *f = wrap_handle(h, LX_O_RDWR, g, 0, hp);
    int fd = xrl_fd_install(f, (flags & 1) != 0, 0);
    xfile_unref(f);
    return fd;
}

/* ================================================================ stat */
slong xrl_vfs_stat(int dirfd, const char *path, int atflags, lx_stat *st)
{
    char gabs[PATHBUF];
    if ((atflags & LX_AT_EMPTY_PATH) && !path[0]) {
        xfile *f = xrl_fd_get(dirfd);
        if (!f) return -LX_EBADF;
        slong r = f->ops->fstat ? f->ops->fstat(f, st) : -LX_EBADF;
        xfile_unref(f);
        return r;
    }
    int follow = !(atflags & LX_AT_SYMLINK_NOFOLLOW);
    slong r = xrl_vfs_resolve(dirfd, path, follow, gabs, sizeof gabs);
    if (r) return r;

    /* symlink entry (lstat) */
    if (!follow) {
        char *t = sym_lookup(gabs);
        if (t) {
            xrl_stat_synth(st, LX_S_IFLNK | 0777, hash_str(gabs), (s64)strlen(t));
            free(t);
            return 0;
        }
    }
    if (xrl_proc_stat(gabs, st)) return st->st_nlink ? 0 : -LX_ENOENT;
    if (!strcmp(gabs, "/dev/null")) { xrl_stat_synth(st, LX_S_IFCHR | 0666, 3, 0); st->st_rdev = (1u << 8) | 3; return 0; }
    if (!strcmp(gabs, "/dev/zero")) { xrl_stat_synth(st, LX_S_IFCHR | 0666, 5, 0); st->st_rdev = (1u << 8) | 5; return 0; }
    if (!strcmp(gabs, "/dev/urandom") || !strcmp(gabs, "/dev/random")) { xrl_stat_synth(st, LX_S_IFCHR | 0666, 9, 0); st->st_rdev = (1u << 8) | 9; return 0; }
    if (!strcmp(gabs, "/dev/tty") || !strcmp(gabs, "/dev/console")) { xrl_stat_synth(st, LX_S_IFCHR | 0666, 11, 0); st->st_rdev = (5u << 8); return 0; }

    wchar_t hp[PATHBUF];
    r = xrl_vfs_host_path(gabs, hp, ARRAY_LEN(hp));
    if (r) return r;
    HANDLE h = hopen(hp, FILE_READ_ATTRIBUTES, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS);
    if (h == INVALID_HANDLE_VALUE) return xrl_lasterr();
    r = fstat_handle(h, gabs, st);
    CloseHandle(h);
    return r;
}

slong xrl_vfs_access(int dirfd, const char *path, int mode)
{
    lx_stat st;
    slong r = xrl_vfs_stat(dirfd, path, 0, &st);
    if (r) return r;
    if ((mode & 2) && !(st.st_mode & 0222)) return -LX_EACCES;
    return 0;
}

slong xrl_vfs_readlink(int dirfd, const char *path, char *buf, u64 cap)
{
    char gabs[PATHBUF];
    slong r = xrl_vfs_resolve(dirfd, path, 0, gabs, sizeof gabs);
    if (r) return r;
    char *t = sym_lookup(gabs);
    if (t) {
        size_t n = strlen(t);
        if (n > cap) n = (size_t)cap;
        memcpy(buf, t, n);
        free(t);
        return (slong)n;
    }
    slong n = xrl_proc_readlink(gabs, buf, cap);
    if (n != -LX_ENOENT) return n;
    lx_stat st;
    r = xrl_vfs_stat(LX_AT_FDCWD, gabs, 0, &st);
    return r ? r : -LX_EINVAL;
}

/* ============================================================== mutation */
slong xrl_vfs_mkdir(int dirfd, const char *path, int mode)
{
    (void)mode;
    char gabs[PATHBUF]; wchar_t hp[PATHBUF];
    slong r = xrl_vfs_resolve(dirfd, path, 0, gabs, sizeof gabs);
    if (r) return r;
    r = xrl_vfs_host_path(gabs, hp, ARRAY_LEN(hp));
    if (r) return r;
    if (!CreateDirectoryW(hp, NULL)) return xrl_lasterr();
    return 0;
}

slong xrl_vfs_unlink(int dirfd, const char *path, int flags)
{
    char gabs[PATHBUF]; wchar_t hp[PATHBUF];
    slong r = xrl_vfs_resolve(dirfd, path, 0, gabs, sizeof gabs);
    if (r) return r;
    if (!(flags & LX_AT_REMOVEDIR) && sym_remove(gabs) == 0) return 0;
    r = xrl_vfs_host_path(gabs, hp, ARRAY_LEN(hp));
    if (r) return r;
    DWORD a = GetFileAttributesW(hp);
    if (a == INVALID_FILE_ATTRIBUTES) return xrl_lasterr();
    if (a & FILE_ATTRIBUTE_DIRECTORY) {
        if (!(flags & LX_AT_REMOVEDIR)) return -LX_EISDIR;
        return RemoveDirectoryW(hp) ? 0 : xrl_lasterr();
    }
    if (flags & LX_AT_REMOVEDIR) return -LX_ENOTDIR;
    if (a & FILE_ATTRIBUTE_READONLY) SetFileAttributesW(hp, a & ~FILE_ATTRIBUTE_READONLY);
    return DeleteFileW(hp) ? 0 : xrl_lasterr();
}

slong xrl_vfs_rename(int olddirfd, const char *oldp, int newdirfd, const char *newp, int flags)
{
    char go[PATHBUF], gn[PATHBUF]; wchar_t ho[PATHBUF], hn[PATHBUF];
    slong r = xrl_vfs_resolve(olddirfd, oldp, 0, go, sizeof go);
    if (r) return r;
    r = xrl_vfs_resolve(newdirfd, newp, 0, gn, sizeof gn);
    if (r) return r;
    char *t = sym_lookup(go);
    if (t) { /* renaming a symlink entry */
        sym_remove(go); sym_add(gn, t, 1); free(t); return 0;
    }
    if ((r = xrl_vfs_host_path(go, ho, ARRAY_LEN(ho))) || (r = xrl_vfs_host_path(gn, hn, ARRAY_LEN(hn)))) return r;
    DWORD an = GetFileAttributesW(hn);
    if ((flags & 1 /*RENAME_NOREPLACE*/) && an != INVALID_FILE_ATTRIBUTES) return -LX_EEXIST;
    DWORD ao = GetFileAttributesW(ho);
    if (ao == INVALID_FILE_ATTRIBUTES) return -LX_ENOENT;
    if (an != INVALID_FILE_ATTRIBUTES && (an & FILE_ATTRIBUTE_DIRECTORY)) {
        if (!(ao & FILE_ATTRIBUTE_DIRECTORY)) return -LX_EISDIR;
        if (!RemoveDirectoryW(hn)) return xrl_lasterr();   /* ENOTEMPTY if not empty */
    } else if (an != INVALID_FILE_ATTRIBUTES && (ao & FILE_ATTRIBUTE_DIRECTORY)) return -LX_ENOTDIR;
    if (!MoveFileExW(ho, hn, MOVEFILE_REPLACE_EXISTING)) return xrl_lasterr();
    return 0;
}

slong xrl_vfs_symlink(const char *target, int dirfd, const char *linkpath)
{
    char gabs[PATHBUF];
    slong r = xrl_vfs_resolve(dirfd, linkpath, 0, gabs, sizeof gabs);
    if (r) return r;
    lx_stat st;
    if (xrl_vfs_stat(LX_AT_FDCWD, gabs, LX_AT_SYMLINK_NOFOLLOW, &st) == 0) return -LX_EEXIST;
    return sym_add(gabs, target, 1);
}

slong xrl_vfs_link(int olddirfd, const char *oldp, int newdirfd, const char *newp)
{
    char go[PATHBUF], gn[PATHBUF]; wchar_t ho[PATHBUF], hn[PATHBUF];
    slong r = xrl_vfs_resolve(olddirfd, oldp, 1, go, sizeof go);
    if (r) return r;
    r = xrl_vfs_resolve(newdirfd, newp, 0, gn, sizeof gn);
    if (r) return r;
    if ((r = xrl_vfs_host_path(go, ho, ARRAY_LEN(ho))) || (r = xrl_vfs_host_path(gn, hn, ARRAY_LEN(hn)))) return r;
    return CreateHardLinkW(hn, ho, NULL) ? 0 : xrl_lasterr();
}

slong xrl_vfs_chdir(const char *path)
{
    char gabs[PATHBUF];
    slong r = xrl_vfs_resolve(LX_AT_FDCWD, path, 1, gabs, sizeof gabs);
    if (r) return r;
    lx_stat st;
    r = xrl_vfs_stat(LX_AT_FDCWD, gabs, 0, &st);
    if (r) return r;
    if ((st.st_mode & LX_S_IFMT) != LX_S_IFDIR) return -LX_ENOTDIR;
    AcquireSRWLockExclusive(&G.cwd_lock);
    strcpy_s(G.cwd, sizeof G.cwd, gabs);
    ReleaseSRWLockExclusive(&G.cwd_lock);
    return 0;
}

slong xrl_vfs_getcwd(char *buf, u64 cap)
{
    AcquireSRWLockShared(&G.cwd_lock);
    size_t n = strlen(G.cwd) + 1;
    slong r = n > cap ? -LX_ERANGE : (slong)n;
    if (r > 0) memcpy(buf, G.cwd, n);
    ReleaseSRWLockShared(&G.cwd_lock);
    return r;
}

slong xrl_vfs_truncate(const char *path, s64 len)
{
    char gabs[PATHBUF]; wchar_t hp[PATHBUF];
    slong r = xrl_vfs_resolve(LX_AT_FDCWD, path, 1, gabs, sizeof gabs);
    if (r) return r;
    if ((r = xrl_vfs_host_path(gabs, hp, ARRAY_LEN(hp)))) return r;
    HANDLE h = hopen(hp, GENERIC_WRITE, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
    if (h == INVALID_HANDLE_VALUE) return xrl_lasterr();
    FILE_END_OF_FILE_INFO e; e.EndOfFile.QuadPart = len;
    r = SetFileInformationByHandle(h, FileEndOfFileInfo, &e, sizeof e) ? 0 : xrl_lasterr();
    CloseHandle(h);
    return r;
}

slong xrl_vfs_utimens(int dirfd, const char *path, const lx_timespec *ts, int flags)
{
    char gabs[PATHBUF]; wchar_t hp[PATHBUF];
    slong r = xrl_vfs_resolve(dirfd, path, !(flags & LX_AT_SYMLINK_NOFOLLOW), gabs, sizeof gabs);
    if (r) return r;
    if ((r = xrl_vfs_host_path(gabs, hp, ARRAY_LEN(hp)))) return r;
    HANDLE h = hopen(hp, FILE_WRITE_ATTRIBUTES, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS);
    if (h == INVALID_HANDLE_VALUE) return xrl_lasterr();
    FILETIME fa, fm, *pa = NULL, *pm = NULL;
    u64 now = xrl_real_ns();
    for (int i = 0; i < 2; i++) {
        s64 sec = ts ? ts[i].tv_sec : (s64)(now / 1000000000ull);
        s64 nsec = ts ? ts[i].tv_nsec : (s64)(now % 1000000000ull);
        if (ts && nsec == ((1 << 30) - 2)) continue;      /* UTIME_OMIT */
        if (ts && nsec == ((1 << 30) - 1)) { sec = (s64)(now / 1000000000ull); nsec = (s64)(now % 1000000000ull); }
        u64 t = (u64)sec * 10000000ull + (u64)nsec / 100 + 116444736000000000ull;
        FILETIME ft; ft.dwLowDateTime = (DWORD)t; ft.dwHighDateTime = (DWORD)(t >> 32);
        if (i == 0) { fa = ft; pa = &fa; } else { fm = ft; pm = &fm; }
    }
    r = SetFileTime(h, NULL, pa, pm) ? 0 : xrl_lasterr();
    CloseHandle(h);
    return r;
}

slong xrl_vfs_statfs(const char *path, lx_statfs *st)
{
    (void)path;
    memset(st, 0, sizeof *st);
    ULARGE_INTEGER avail, total, freeb;
    st->f_type = 0xEF53;
    st->f_bsize = 4096; st->f_frsize = 4096;
    st->f_namelen = 255;
    if (GetDiskFreeSpaceExW(G.vfs_root_w, &avail, &total, &freeb)) {
        st->f_blocks = (s64)(total.QuadPart / 4096);
        st->f_bfree = (s64)(freeb.QuadPart / 4096);
        st->f_bavail = (s64)(avail.QuadPart / 4096);
    } else { st->f_blocks = 1 << 20; st->f_bfree = st->f_bavail = 1 << 19; }
    st->f_files = 1 << 20; st->f_ffree = 1 << 19;
    return 0;
}
