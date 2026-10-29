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

static void meta_load(const wchar_t *file_w);

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

/* ============================================================== mounts
 * XRL_MOUNTS="/game=S:\path\to\dir;/other=..." (host environment): a guest
 * directory served from a host directory outside vfs_root. The console's
 * LocalState is a small data area, so a big read-only tree (the game) stays in
 * the package's install folder and is mounted at /game. The mount point also
 * exists as an empty directory under vfs_root, so "/" lists it. */
typedef struct vmount { char *guest; size_t glen; wchar_t *host; } vmount;
static vmount g_mnt[8];
static size_t g_nmnt;

static void mounts_load(void)
{
    char env[4096];
    DWORD n = GetEnvironmentVariableA("XRL_MOUNTS", env, sizeof env);
    if (!n || n >= sizeof env) return;
    for (char *tok = strtok(env, ";"); tok && g_nmnt < ARRAY_LEN(g_mnt); tok = strtok(NULL, ";")) {
        char *eq = strchr(tok, '=');
        if (!eq || tok[0] != '/' || eq == tok + 1) { LOGW("vfs: XRL_MOUNTS: ignoring '%s'", tok); continue; }
        *eq = 0;
        size_t gl = strlen(tok);
        while (gl > 1 && tok[gl - 1] == '/') tok[--gl] = 0;
        wchar_t h[1024];
        int hn = MultiByteToWideChar(CP_UTF8, 0, eq + 1, -1, h, 1000);
        if (hn <= 1) continue;
        for (int i = 0; i < hn; i++) if (h[i] == L'/') h[i] = L'\\';
        while (hn > 2 && h[hn - 2] == L'\\') h[--hn - 1] = 0;
        size_t wl = (size_t)hn + 8;
        wchar_t *w = (wchar_t *)calloc(wl, sizeof(wchar_t));
        /* extended-length form: \\?\C:\dir, or \\?\UNC\server\share\dir for a network path */
        if (wcsncmp(h, L"\\\\?\\", 4) == 0) wcscpy_s(w, wl, h);
        else if (h[0] == L'\\' && h[1] == L'\\') { wcscpy_s(w, wl, L"\\\\?\\UNC\\"); wcscat_s(w, wl, h + 2); }
        else { wcscpy_s(w, wl, L"\\\\?\\"); wcscat_s(w, wl, h); }
        DWORD a = GetFileAttributesW(w);
        g_mnt[g_nmnt].guest = _strdup(tok); g_mnt[g_nmnt].glen = gl; g_mnt[g_nmnt].host = w;
        g_nmnt++;
        wchar_t mp[1100]; int mn;   /* the mount point under vfs_root (parents first) */
        wcscpy_s(mp, 1100, G.vfs_root_w); mn = (int)wcslen(mp);
        MultiByteToWideChar(CP_UTF8, 0, tok, -1, mp + mn, 1100 - mn);
        for (wchar_t *c = mp + mn; *c; c++) if (*c == L'/') { *c = 0; CreateDirectoryW(mp, NULL); *c = L'\\'; }
        CreateDirectoryW(mp, NULL);
        LOGI("vfs: mount %s -> %s%s", tok, eq + 1, a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) ? "" : " (not a directory yet)");
    }
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
    static const wchar_t *dirs[] = { L"\\tmp", L"\\dev", L"\\dev\\shm", L"\\dev\\pts", L"\\var", L"\\var\\tmp", L"\\run", L"\\home", L"\\etc" };
    for (size_t i = 0; i < ARRAY_LEN(dirs); i++) {
        wchar_t p[1100]; wcscpy_s(p, 1100, G.vfs_root_w); wcscat_s(p, 1100, dirs[i]);
        CreateDirectoryW(p, NULL);
    }
    wcscpy_s(g_symfile_w, 1024, G.vfs_root_w); wcscat_s(g_symfile_w, 1024, L"\\.xrl-symlinks");
    sym_load(NULL);
    { wchar_t mf[1024]; wcscpy_s(mf, 1024, G.vfs_root_w); wcscat_s(mf, 1024, L"\\.xrl-meta"); meta_load(mf); }
    /* conventional device symlinks */
    sym_add("/dev/stdin", "/proc/self/fd/0", 0);
    sym_add("/dev/stdout", "/proc/self/fd/1", 0);
    sym_add("/dev/stderr", "/proc/self/fd/2", 0);
    sym_add("/dev/fd", "/proc/self/fd", 0);
    mounts_load();
    LOGI("vfs: root=%s (%zu symlinks)", G.vfs_root, g_nsym);
    return 0;
}

int xrl_vfs_host_path(const char *gabs, wchar_t *out, size_t cap)
{
    const wchar_t *base = G.vfs_root_w;
    const char *p = gabs;
    for (size_t i = 0; i < g_nmnt; i++) {   /* a mount: its host directory, then the rest of the path */
        const vmount *m = &g_mnt[i];
        if (!strncmp(gabs, m->guest, m->glen) && (gabs[m->glen] == 0 || gabs[m->glen] == '/')) {
            base = m->host; p = gabs + m->glen;
            break;
        }
    }
    size_t rl = wcslen(base);
    if (rl + 2 >= cap) return -LX_ENAMETOOLONG;
    wmemcpy(out, base, rl);
    size_t o = rl;
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
        strcpy_s(out, cap, xrl_curproc()->cwd);
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

static int g_meta_on;
static int memfd_sealed(const xfile *f, u32 seal);
static void meta_apply(const char *gpath, lx_stat *st);

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
    if (g_meta_on && gpath) meta_apply(gpath, st);
    return 0;
}

/* ================================================================ vnodes
 * Nodes that exist only in the layer: the socket files bind(2) makes (unix.c). They are not host files:
 * stat says S_IFSOCK, getdents lists them as DT_SOCK, unlink/rename move them, open fails with ENXIO,
 * and connect looks them up here. Each holds a reference on its object (ref/unref callbacks). */
typedef struct vnode { char *path; u32 type, mode; void *obj; void (*ref)(void *); void (*unref)(void *); } vnode;
static vnode *g_vn;
static size_t g_nvn, g_capvn;
static SRWLOCK g_vn_lock = SRWLOCK_INIT;

static int vn_find(const char *path)   /* caller holds lock */
{
    for (size_t i = 0; i < g_nvn; i++) if (!strcmp(g_vn[i].path, path)) return (int)i;
    return -1;
}

static int vn_exists(const char *path)
{
    AcquireSRWLockShared(&g_vn_lock);
    int r = vn_find(path) >= 0;
    ReleaseSRWLockShared(&g_vn_lock);
    return r;
}

static int vn_stat(const char *path, lx_stat *st)
{
    int r = 0;
    AcquireSRWLockShared(&g_vn_lock);
    int i = vn_find(path);
    if (i >= 0) { xrl_stat_synth(st, g_vn[i].type | g_vn[i].mode, hash_str(path), 0); r = 1; }
    ReleaseSRWLockShared(&g_vn_lock);
    return r;
}

static int vn_remove(const char *path)
{
    void (*unref)(void *) = NULL; void *obj = NULL;
    int r = -LX_ENOENT;
    AcquireSRWLockExclusive(&g_vn_lock);
    int i = vn_find(path);
    if (i >= 0) { unref = g_vn[i].unref; obj = g_vn[i].obj; free(g_vn[i].path); g_vn[i] = g_vn[--g_nvn]; r = 0; }
    ReleaseSRWLockExclusive(&g_vn_lock);
    if (unref) unref(obj);
    return r;
}

static int vn_rename(const char *from, const char *to)
{
    char *np = _strdup(to);
    int r = -LX_ENOENT;
    void (*unref)(void *) = NULL; void *obj = NULL;
    AcquireSRWLockExclusive(&g_vn_lock);
    int i = vn_find(from);
    if (i >= 0) {
        int j = vn_find(to);
        if (j >= 0 && j != i) { unref = g_vn[j].unref; obj = g_vn[j].obj; free(g_vn[j].path); g_vn[j] = g_vn[--g_nvn]; if (i == (int)g_nvn) i = j; }
        free(g_vn[i].path); g_vn[i].path = np; np = NULL; r = 0;
    }
    ReleaseSRWLockExclusive(&g_vn_lock);
    free(np);
    if (unref) unref(obj);
    return r;
}

slong xrl_vfs_vnode_add(int dirfd, const char *path, u32 type, u32 mode, void *obj, void (*ref)(void *), void (*unref)(void *))
{
    char gabs[PATHBUF], par[PATHBUF];
    slong r = xrl_vfs_resolve(dirfd, path, 0, gabs, sizeof gabs);
    if (r) return r;
    lx_stat st;
    r = xrl_vfs_stat(LX_AT_FDCWD, gabs, LX_AT_SYMLINK_NOFOLLOW, &st);
    if (r == 0) return -LX_EEXIST;
    if (r != -LX_ENOENT) return r;
    strcpy_s(par, sizeof par, gabs);
    char *sl = strrchr(par, '/');
    if (!sl || !sl[1]) return -LX_EINVAL;
    if (sl == par) par[1] = 0; else *sl = 0;
    r = xrl_vfs_stat(LX_AT_FDCWD, par, 0, &st);
    if (r) return r;
    if ((st.st_mode & LX_S_IFMT) != LX_S_IFDIR) return -LX_ENOTDIR;
    AcquireSRWLockExclusive(&g_vn_lock);
    if (vn_find(gabs) >= 0) r = -LX_EEXIST;
    else {
        if (g_nvn == g_capvn) {
            size_t nc = g_capvn ? g_capvn * 2 : 16;
            vnode *n = (vnode *)realloc(g_vn, nc * sizeof(vnode));
            if (!n) { ReleaseSRWLockExclusive(&g_vn_lock); return -LX_ENOMEM; }
            g_vn = n; g_capvn = nc;
        }
        g_vn[g_nvn] = (vnode){ _strdup(gabs), type, mode, obj, ref, unref };
        g_nvn++;
        ref(obj);
        r = 0;
    }
    ReleaseSRWLockExclusive(&g_vn_lock);
    return r;
}

slong xrl_vfs_vnode_lookup(int dirfd, const char *path, u32 *type, void **obj)
{
    char gabs[PATHBUF];
    slong r = xrl_vfs_resolve(dirfd, path, 1, gabs, sizeof gabs);
    if (r) return r;
    *type = 0; *obj = NULL;
    AcquireSRWLockShared(&g_vn_lock);
    int i = vn_find(gabs);
    if (i >= 0) { *type = g_vn[i].type; *obj = g_vn[i].obj; g_vn[i].ref(*obj); }
    ReleaseSRWLockShared(&g_vn_lock);
    if (i >= 0) return 0;
    lx_stat st;
    return xrl_vfs_stat(LX_AT_FDCWD, gabs, 0, &st);   /* an ordinary file: type 0 */
}

/* ================================================================= meta
 * Unix modes and owners. NTFS has neither, so a table keeps them: <vfs_root>\.xrl-meta, one
 * "guest_path<TAB>mode(octal)<TAB>uid<TAB>gid" per file whose mode/owner differs from the default (0644 file,
 * 0755 directory or executable-looking name, owner = the "#default<TAB>uid<TAB>gid" line that
 * tools/xrl-rootfs-unpack.py writes for a rootfs whose files belong to root). A change is appended to the file
 * as a line of the same form ("-<TAB>path" forgets a path); a later line wins, so the file is a journal that
 * load replays. The mechanism is ON only when .xrl-meta exists: the Wine prefix and every tree baked before
 * this keep today's behaviour (everything 1000:1000, chmod/chown accepted and ignored). With it on: stat and
 * fstat report the table, chmod/chown change it (with Linux's permission rules), new files and directories get
 * (mode & ~umask) owned by the caller, unlink/rename/link carry the entries, open() and access() check the
 * permission bits for a non-root caller. Directory permissions along the path are not checked. */
typedef struct mentry { struct mentry *next; char *path; u32 mode, uid, gid; u64 h; } mentry;
static mentry **g_mb;
static size_t g_mbn, g_mcount;
static int g_meta_on;
static u32 g_mdef_uid, g_mdef_gid;
static SRWLOCK g_meta_lock = SRWLOCK_INIT;
static HANDLE g_meta_h = INVALID_HANDLE_VALUE;

static mentry *meta_find(const char *path, u64 h)   /* lock held */
{
    if (!g_mbn) return NULL;
    for (mentry *e = g_mb[h % g_mbn]; e; e = e->next) if (e->h == h && !strcmp(e->path, path)) return e;
    return NULL;
}

static void meta_unlink_entry(mentry *e)   /* lock held */
{
    mentry **pp = &g_mb[e->h % g_mbn];
    while (*pp != e) pp = &(*pp)->next;
    *pp = e->next;
    free(e->path); free(e);
    g_mcount--;
}

static void meta_set_mem(const char *path, u32 mode, u32 uid, u32 gid)   /* lock held */
{
    u64 h = hash_str(path);
    mentry *e = meta_find(path, h);
    if (!e) {
        if (g_mcount >= g_mbn * 2) {   /* grow */
            size_t nb = g_mbn ? g_mbn * 4 : 1024;
            mentry **n = (mentry **)calloc(nb, sizeof *n);
            if (!n) return;
            for (size_t i = 0; i < g_mbn; i++) for (mentry *x = g_mb[i], *nx; x; x = nx) { nx = x->next; x->next = n[x->h % nb]; n[x->h % nb] = x; }
            free(g_mb); g_mb = n; g_mbn = nb;
        }
        e = (mentry *)calloc(1, sizeof *e);
        if (!e) return;
        e->path = _strdup(path); e->h = h;
        e->next = g_mb[h % g_mbn]; g_mb[h % g_mbn] = e;
        g_mcount++;
    }
    e->mode = mode; e->uid = uid; e->gid = gid;
}

static void meta_journal_set(const char *path, u32 mode, u32 uid, u32 gid)   /* lock held */
{
    if (g_meta_h == INVALID_HANDLE_VALUE || strpbrk(path, "\t\n")) return;
    char line[PATHBUF + 64];
    int n = snprintf(line, sizeof line, "%s\t%o\t%u\t%u\n", path, mode, uid, gid);
    DWORD w;
    if (n > 0 && n < (int)sizeof line) WriteFile(g_meta_h, line, (DWORD)n, &w, NULL);
}

static void meta_journal_del(const char *path)   /* lock held */
{
    if (g_meta_h == INVALID_HANDLE_VALUE || strpbrk(path, "\t\n")) return;
    char line[PATHBUF + 8];
    int n = snprintf(line, sizeof line, "-\t%s\n", path);
    DWORD w;
    if (n > 0 && n < (int)sizeof line) WriteFile(g_meta_h, line, (DWORD)n, &w, NULL);
}

static int meta_lookup(const char *path, u32 *mode, u32 *uid, u32 *gid)
{
    int r = 0;
    AcquireSRWLockShared(&g_meta_lock);
    mentry *e = meta_find(path, hash_str(path));
    if (e) { *mode = e->mode; *uid = e->uid; *gid = e->gid; r = 1; }
    ReleaseSRWLockShared(&g_meta_lock);
    return r;
}

static u32 default_perm(const char *path, int isdir) { return isdir ? 0755 : is_exec_name(path) ? 0755 : 0644; }

static void meta_put(const char *path, u32 mode, u32 uid, u32 gid, int isdir)
{
    AcquireSRWLockExclusive(&g_meta_lock);
    if (mode == default_perm(path, isdir) && uid == g_mdef_uid && gid == g_mdef_gid) {   /* the default: no entry */
        mentry *e = meta_find(path, hash_str(path));
        if (e) { meta_unlink_entry(e); meta_journal_del(path); }
    } else { meta_set_mem(path, mode, uid, gid); meta_journal_set(path, mode, uid, gid); }
    ReleaseSRWLockExclusive(&g_meta_lock);
}

static void meta_drop(const char *path)
{
    if (!g_meta_on) return;
    AcquireSRWLockExclusive(&g_meta_lock);
    mentry *e = meta_find(path, hash_str(path));
    if (e) { meta_unlink_entry(e); meta_journal_del(path); }
    ReleaseSRWLockExclusive(&g_meta_lock);
}

/* rename: the entry of `from` and, for a directory, everything below it, now live under `to` */
static void meta_move(const char *from, const char *to, int isdir)
{
    if (!g_meta_on || !strcmp(from, to)) return;
    size_t fl = strlen(from);
    AcquireSRWLockExclusive(&g_meta_lock);
    mentry *e = meta_find(to, hash_str(to));
    if (e) { meta_unlink_entry(e); meta_journal_del(to); }
    size_t nmv = 0, cap = 0;
    mentry **list = NULL;   /* collected first: moving changes the table under the walk */
    for (size_t i = 0; i < g_mbn; i++)
        for (mentry *x = g_mb[i]; x; x = x->next)
            if (!strncmp(x->path, from, fl) && (x->path[fl] == 0 || (isdir && x->path[fl] == '/'))) {
                if (nmv == cap) { cap = cap ? cap * 2 : 16; list = (mentry **)realloc(list, cap * sizeof *list); }
                list[nmv++] = x;
            }
    for (size_t i = 0; i < nmv; i++) {
        char np[PATHBUF], op[PATHBUF];
        strcpy_s(op, sizeof op, list[i]->path);
        snprintf(np, sizeof np, "%s%s", to, op + fl);
        u32 m = list[i]->mode, u = list[i]->uid, g = list[i]->gid;
        meta_unlink_entry(list[i]); meta_journal_del(op);
        meta_set_mem(np, m, u, g); meta_journal_set(np, m, u, g);
    }
    free(list);
    ReleaseSRWLockExclusive(&g_meta_lock);
}

/* hard link: the new name starts with the mode and owner of the old one */
static void meta_copy(const char *from, const char *to)
{
    u32 m, u, g;
    if (g_meta_on && meta_lookup(from, &m, &u, &g)) {
        AcquireSRWLockExclusive(&g_meta_lock);
        meta_set_mem(to, m, u, g); meta_journal_set(to, m, u, g);
        ReleaseSRWLockExclusive(&g_meta_lock);
    }
}

static void meta_apply(const char *gpath, lx_stat *st)
{
    u32 mode, uid, gid;
    if (meta_lookup(gpath, &mode, &uid, &gid)) { st->st_mode = (st->st_mode & LX_S_IFMT) | (mode & 07777); st->st_uid = uid; st->st_gid = gid; }
    else { st->st_uid = g_mdef_uid; st->st_gid = g_mdef_gid; }
}

/* a file or directory was just created: (requested mode & ~umask), owned by the caller */
static void meta_created(const char *gabs, int isdir, u32 mode)
{
    if (!g_meta_on) return;
    meta_put(gabs, mode & ~xrl_cur_umask() & 07777, xrl_cur_uid(), xrl_cur_gid(), isdir);
}

static void meta_load(const wchar_t *file_w)
{
    g_mdef_uid = xrl_cur_uid(); g_mdef_gid = xrl_cur_gid();
    HANDLE h = CreateFile2(file_w, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, OPEN_EXISTING, NULL);
    if (h == INVALID_HANDLE_VALUE) return;   /* no table: meta stays off */
    g_meta_on = 1;
    LARGE_INTEGER sz;
    if (GetFileSizeEx(h, &sz) && sz.QuadPart < (64ll << 20)) {
        char *buf = (char *)malloc((size_t)sz.QuadPart + 1);
        DWORD got = 0;
        if (buf && ReadFile(h, buf, (DWORD)sz.QuadPart, &got, NULL)) {
            buf[got] = 0;
            AcquireSRWLockExclusive(&g_meta_lock);
            for (char *p = buf; *p;) {
                char *nl = strchr(p, '\n');
                if (nl) *nl = 0;
                size_t ln = strlen(p);
                if (ln && p[ln - 1] == '\r') p[ln - 1] = 0;
                if (!strncmp(p, "#default\t", 9)) { unsigned u, g; if (sscanf(p + 9, "%u\t%u", &u, &g) == 2) { g_mdef_uid = u; g_mdef_gid = g; } }
                else if (!strncmp(p, "-\t/", 3)) { mentry *e = meta_find(p + 2, hash_str(p + 2)); if (e) meta_unlink_entry(e); }
                else if (p[0] == '/') {
                    char *t1 = strchr(p, '\t');
                    unsigned m, u, g;
                    if (t1 && sscanf(t1 + 1, "%o\t%u\t%u", &m, &u, &g) == 3) { *t1 = 0; meta_set_mem(p, m, u, g); }
                }
                if (!nl) break;
                p = nl + 1;
            }
            ReleaseSRWLockExclusive(&g_meta_lock);
        }
        free(buf);
    }
    CloseHandle(h);
    g_meta_h = CreateFile2(file_w, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, OPEN_EXISTING, NULL);
    LOGI("vfs: modes and owners on: %zu entries from .xrl-meta (default owner %u:%u)", g_mcount, g_mdef_uid, g_mdef_gid);
}

/* permission bits against the caller: want = 4 read, 2 write, 1 execute */
static int perm_ok(const lx_stat *st, int want)
{
    u32 euid = xrl_cur_uid(), egid = xrl_cur_gid();
    u32 m = st->st_mode & 0777;
    if (euid == 0) return !(want & 1) || (st->st_mode & LX_S_IFMT) == LX_S_IFDIR || (m & 0111);
    u32 cls = euid == st->st_uid ? (m >> 6) & 7 : egid == st->st_gid ? (m >> 3) & 7 : m & 7;
    return (cls & (u32)want) == (u32)want;
}

static slong chmod_gabs(const char *gabs, u32 mode)
{
    lx_stat st;
    slong r = xrl_vfs_stat(LX_AT_FDCWD, gabs, 0, &st);
    if (r) return r;
    if (!g_meta_on) return 0;
    u32 euid = xrl_cur_uid();
    if (euid != 0 && euid != st.st_uid) return -LX_EPERM;
    meta_put(gabs, mode & 07777, st.st_uid, st.st_gid, (st.st_mode & LX_S_IFMT) == LX_S_IFDIR);
    AcquireSRWLockExclusive(&g_vn_lock);
    int i = vn_find(gabs);
    if (i >= 0) g_vn[i].mode = mode & 07777;
    ReleaseSRWLockExclusive(&g_vn_lock);
    return 0;
}

static slong chown_gabs(const char *gabs, u32 uid, u32 gid)   /* (u32)-1 = leave */
{
    lx_stat st;
    slong r = xrl_vfs_stat(LX_AT_FDCWD, gabs, 0, &st);
    if (r) return r;
    if (!g_meta_on) return 0;
    u32 euid = xrl_cur_uid();
    u32 nu = uid == (u32)-1 ? st.st_uid : uid, ng = gid == (u32)-1 ? st.st_gid : gid;
    if (euid != 0 && (euid != st.st_uid || nu != st.st_uid || (ng != st.st_gid && ng != xrl_cur_gid()))) return -LX_EPERM;
    u32 mode = st.st_mode & 07777;
    if ((nu != st.st_uid || ng != st.st_gid) && (st.st_mode & LX_S_IFMT) != LX_S_IFDIR) mode &= ~06000u;   /* an ownership change drops setuid/setgid */
    meta_put(gabs, mode, nu, ng, (st.st_mode & LX_S_IFMT) == LX_S_IFDIR);
    return 0;
}

static int gpath_is_file(const xfile *f) { return f->gpath && f->gpath[0] == '/' && strncmp(f->gpath, "/memfd:", 7) != 0; }

slong xrl_vfs_chmod(int dirfd, const char *path, u32 mode, int flags)
{
    char gabs[PATHBUF];
    if (flags & ~(LX_AT_SYMLINK_NOFOLLOW | LX_AT_EMPTY_PATH)) return -LX_EINVAL;
    if ((flags & LX_AT_EMPTY_PATH) && !path[0]) return xrl_vfs_fchmod(dirfd, mode);
    slong r = xrl_vfs_resolve(dirfd, path, 1, gabs, sizeof gabs);
    if (r) return r;
    return chmod_gabs(gabs, mode);
}

slong xrl_vfs_fchmod(int fd, u32 mode)
{
    xfile *f = xrl_fd_get(fd);
    if (!f) return -LX_EBADF;
    slong r = gpath_is_file(f) ? chmod_gabs(f->gpath, mode) : 0;
    xfile_unref(f);
    return r;
}

slong xrl_vfs_chown(int dirfd, const char *path, u32 uid, u32 gid, int flags)
{
    char gabs[PATHBUF];
    if ((flags & LX_AT_EMPTY_PATH) && !path[0]) return xrl_vfs_fchown(dirfd, uid, gid);
    slong r = xrl_vfs_resolve(dirfd, path, !(flags & LX_AT_SYMLINK_NOFOLLOW), gabs, sizeof gabs);
    if (r) return r;
    return chown_gabs(gabs, uid, gid);
}

slong xrl_vfs_fchown(int fd, u32 uid, u32 gid)
{
    xfile *f = xrl_fd_get(fd);
    if (!f) return -LX_EBADF;
    slong r = gpath_is_file(f) ? chown_gabs(f->gpath, uid, gid) : 0;
    xfile_unref(f);
    return r;
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
    if (memfd_sealed(f, 8 /*F_SEAL_WRITE*/ | 0x10 /*F_SEAL_FUTURE_WRITE*/)) return -LX_EPERM;
    if (memfd_sealed(f, 4 /*F_SEAL_GROW*/)) {   /* a write may not extend the file */
        LARGE_INTEGER cur;
        s64 at = off >= 0 ? off : (f->flags & LX_O_APPEND) ? (GetFileSizeEx(f->h, &cur) ? cur.QuadPart : 0) : f->pos;
        if (GetFileSizeEx(f->h, &cur) && at + (s64)n > cur.QuadPart) return -LX_EPERM;
    }
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
    if (f->gpath && !strncmp(f->gpath, "/memfd:", 7)) {   /* a sealed memfd keeps its size (and its contents) */
        LARGE_INTEGER cur;
        if (GetFileSizeEx(f->h, &cur) && ((len < cur.QuadPart && memfd_sealed(f, 2 /*F_SEAL_SHRINK*/)) || (len > cur.QuadPart && memfd_sealed(f, 4 /*F_SEAL_GROW*/)))) return -LX_EPERM;   /* only SHRINK and GROW count for a truncation */
    }
    FILE_END_OF_FILE_INFO e; e.EndOfFile.QuadPart = len;
    if (!SetFileInformationByHandle(f->h, FileEndOfFileInfo, &e, sizeof e)) return xrl_lasterr();
    return 0;
}

static slong dfile_fsync(xfile *f) { return FlushFileBuffers(f->h) ? 0 : xrl_lasterr(); }
static u32 dfile_poll(xfile *f, u32 ev) { (void)f; return ev & (LX_POLLIN | LX_POLLOUT); }

static const xfile_ops g_dir_ops;

/* FS_IOC_GETFLAGS (EXT2_IOC_GETFLAGS): FS_CASEFOLD_FL on a case-insensitive directory, as on a
 * casefolded ext4 one. Wine asks this (get_dir_case_sensitivity) before it looks for a name it did
 * not find by reading the whole directory and comparing case-insensitively: NTFS already matches
 * names that way, so a miss is a miss, and a game probing many absent files in a large directory
 * no longer lists that directory each time. A directory with Windows' per-directory case
 * sensitivity on (WSL can set it) says so and gets no flag. */
#define LX_FS_IOC_GETFLAGS 0x80086601u
#define LX_FS_CASEFOLD_FL 0x40000000
static slong dir_getflags(xfile *f, u64 arg)
{
    FILE_CASE_SENSITIVE_INFO cs = {0};
    int flags = LX_FS_CASEFOLD_FL;
    if (GetFileInformationByHandleEx(f->h, FileCaseSensitiveInfo, &cs, sizeof cs) &&
        (cs.Flags & FILE_CS_FLAG_CASE_SENSITIVE_DIR)) flags = 0;
    return xrl_ctu(arg, &flags, 4);
}

static slong dfile_ioctl(xfile *f, u64 req, u64 arg)
{
    if (req == LX_FIOCLEX || req == LX_FIONCLEX || req == LX_FIONBIO) return 0;
    if (req == LX_FS_IOC_GETFLAGS && f->ops == &g_dir_ops) {   /* XRL_NO_CASEFOLD=1: ENOTTY as before */
        static int off = -1;
        if (off < 0) { char v[4]; off = GetEnvironmentVariableA("XRL_NO_CASEFOLD", v, sizeof v) && v[0] == '1'; }
        if (!off) return dir_getflags(f, arg);
    }
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
            d->phase = 4; d->vidx = 0;
        }
        if (d->phase == 4) {
            /* ... and the layer's socket nodes */
            int stop = 0;
            AcquireSRWLockShared(&g_vn_lock);
            for (; d->vidx < g_nvn; d->vidx++) {
                const char *sp = g_vn[d->vidx].path;
                const char *sl = strrchr(sp, '/');
                if (!sl) continue;
                size_t plen = (size_t)(sl - sp);
                const char *me = f->gpath;
                size_t ml = strcmp(me, "/") ? strlen(me) : 0;
                if (plen != ml || strncmp(sp, me, ml)) continue;
                if (!put_dirent(kbuf, cap, &used, hash_str(sp), d->cookie + 1, LX_DT_SOCK, sl + 1)) { stop = 1; break; }
                d->cookie++;
            }
            ReleaseSRWLockShared(&g_vn_lock);
            if (stop) break;
            d->phase = 5;
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
static void memfd_close(xfile *f);
static const xfile_ops g_memfd_ops = { "memfd", dfile_read, dfile_write, dfile_lseek, dfile_fstat, NULL, dfile_ioctl, dfile_ftruncate, dfile_fsync, dfile_poll, memfd_close, dfile_read };
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
    else if (!strcmp(gabs, "/dev/tty") || !strcmp(gabs, "/dev/console")) f = xrl_dev_tty(flags);
    else if (!strcmp(gabs, "/dev/ptmx")) { int err = 0; f = xrl_pty_open_master(flags, &err); if (!f) return -err; }
    else if (!strncmp(gabs, "/dev/pts/", 9)) { int err = 0; char *e; long n = strtol(gabs + 9, &e, 10); if (e == gabs + 9 || *e) { *handled = 0; return 0; } f = xrl_pty_open_slave((int)n, flags, &err); if (!f) return -err; }
    else if (!strcmp(gabs, "/dev/full")) f = xrl_dev_null(flags);
    else if (!strcmp(gabs, "/dev/ntsync")) f = xrl_dev_ntsync(flags);
    else if (!strncmp(gabs, "/dev/input/event", 16) || !strcmp(gabs, "/dev/fb0")) {   /* evdev.c, fbdev.c */
        int err = 0;
        f = gabs[5] == 'f' ? xrl_dev_fb_open(gabs, flags, &err) : xrl_dev_evdev_open(gabs, flags, &err);
        if (!f) return err ? -err : -LX_ENOENT;
    }
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

static slong memfd_reopen(const char *gabs, int flags);

static slong vfs_open_impl(int dirfd, const char *path, int flags, int mode)
{
    char gabs[PATHBUF];
    int follow = !(flags & LX_O_NOFOLLOW);
    slong r = xrl_vfs_resolve(dirfd, path, follow, gabs, sizeof gabs);
    if (r) return r;
    if ((flags & LX_O_TMPFILE) == LX_O_TMPFILE) return -LX_EOPNOTSUPP;

    /* /proc symlinks (exe, cwd, fd/N) open their target */
    if (!strncmp(gabs, "/proc/", 6) && follow) { slong mfd = memfd_reopen(gabs, flags); if (mfd != -1) return mfd; }
    if (!strncmp(gabs, "/proc/", 6)) {
        char tgt[PATHBUF];
        slong n = xrl_proc_readlink(gabs, tgt, sizeof tgt - 1);
        if (n > 0 && follow) { tgt[n] = 0; r = xrl_vfs_resolve(LX_AT_FDCWD, tgt, 1, gabs, sizeof gabs); if (r) return r; }
    }

    if (vn_exists(gabs)) return -LX_ENXIO;   /* a socket file cannot be opened */
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

    if (g_meta_on) {   /* permission bits of an existing file (a root caller passes) */
        lx_stat ps;
        if (xrl_vfs_stat(LX_AT_FDCWD, gabs, 0, &ps) == 0) {
            int want = acc == LX_O_RDONLY ? 4 : acc == LX_O_WRONLY ? 2 : 6;
            if (flags & LX_O_TRUNC) want |= 2;
            if (!perm_ok(&ps, want)) return -LX_EACCES;
        }
    }

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

    SetLastError(0);
    HANDLE h = hopen(hp, access, disp, FILE_ATTRIBUTE_NORMAL);
    DWORD open_err = GetLastError();
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = open_err;
        if (e == ERROR_ACCESS_DENIED && (flags & LX_O_CREAT)) {
            DWORD a2 = GetFileAttributesW(hp);
            if (a2 != INVALID_FILE_ATTRIBUTES && (a2 & FILE_ATTRIBUTE_DIRECTORY)) return -LX_EISDIR;
        }
        return -(slong)xrl_win_to_errno(e);
    }
    if (disp == CREATE_NEW || ((disp == OPEN_ALWAYS || disp == CREATE_ALWAYS) && open_err != ERROR_ALREADY_EXISTS)) meta_created(gabs, 0, (u32)mode);
    xfile *f = wrap_handle(h, flags & ~(LX_O_CREAT | LX_O_EXCL | LX_O_TRUNC), gabs, 0, hp);
    if (!f) { CloseHandle(h); return -LX_ENOMEM; }
    int fd = xrl_fd_install(f, (flags & LX_O_CLOEXEC) != 0, 0);
    xfile_unref(f);
    return fd;
}

/* XRL_TRACE=1 also logs every path the guest opens (the syscall trace only has pointers) */
slong xrl_vfs_open(int dirfd, const char *path, int flags, int mode)
{
    slong r = vfs_open_impl(dirfd, path, flags, mode);
    if (G.trace_syscalls == 1) LOGT("vfs: open(%s, 0x%x) = %lld", path, flags, r);
    return r;
}

/* memfd: a host temp file (deleted on close) that shows up as Linux shows one: /proc/self/fd/N and
 * /proc/self/maps say "/memfd:NAME (deleted)". f->priv holds its number; opening /proc/self/fd/N reopens it
 * (the new description shares the contents, like on Linux). */
static slong memfd_host_path(u64 key, char *g, size_t cap, wchar_t *hp, size_t hpcap)
{
    snprintf(g, cap, "/tmp/.memfd-%u-%u", (unsigned)(key >> 32), (unsigned)key);   /* key: creator's pid, number */
    return xrl_vfs_host_path(g, hp, hpcap);
}

/* Per memfd: how many open file descriptions refer to it (the pages behind a shared mapping, shm.c, live until the
 * last one is closed and the last mapping is gone) and its seals. */
typedef struct mfd_state { u32 n; int opens; u32 seals; int sealable; } mfd_state;
static mfd_state *g_mfd;
static size_t g_nmfd, g_capmfd;
static SRWLOCK g_mfd_lock = SRWLOCK_INIT;

static mfd_state *mfd_find(u32 n)   /* lock held */
{
    for (size_t i = 0; i < g_nmfd; i++) if (g_mfd[i].n == n) return &g_mfd[i];
    return NULL;
}

static void mfd_open(u32 n, int sealable)
{
    AcquireSRWLockExclusive(&g_mfd_lock);
    mfd_state *m = mfd_find(n);
    if (!m) {
        if (g_nmfd == g_capmfd) { g_capmfd = g_capmfd ? g_capmfd * 2 : 16; g_mfd = (mfd_state *)realloc(g_mfd, g_capmfd * sizeof *g_mfd); }
        m = &g_mfd[g_nmfd++];
        *m = (mfd_state){ n, 0, 0, sealable };
    }
    m->opens++;
    ReleaseSRWLockExclusive(&g_mfd_lock);
}

static void memfd_close(xfile *f)
{
    u32 n = (u32)(uintptr_t)f->priv;
    int gone = 0;
    AcquireSRWLockExclusive(&g_mfd_lock);
    mfd_state *m = mfd_find(n);
    if (m && --m->opens <= 0) { *m = g_mfd[--g_nmfd]; gone = 1; }
    ReleaseSRWLockExclusive(&g_mfd_lock);
    if (gone) xrl_shm_memfd_gone(n);
}

static int memfd_sealed(const xfile *f, u32 seal)
{
    if (f->ops != &g_memfd_ops) return 0;
    AcquireSRWLockShared(&g_mfd_lock);
    mfd_state *m = mfd_find((u32)(uintptr_t)f->priv);
    int r = m && (m->seals & seal);
    ReleaseSRWLockShared(&g_mfd_lock);
    return r;
}

/* fcntl(F_ADD_SEALS = op 1, F_GET_SEALS = op 0) */
slong xrl_memfd_seals(xfile *f, int op, u32 seals)
{
    if (f->ops != &g_memfd_ops) return -LX_EINVAL;
    slong r = 0;
    AcquireSRWLockExclusive(&g_mfd_lock);
    mfd_state *m = mfd_find((u32)(uintptr_t)f->priv);
    if (!m) r = -LX_EINVAL;
    else if (op == 0) r = m->sealable ? (slong)m->seals : 0x1 /*F_SEAL_SEAL: no sealing allowed*/;
    else if (!m->sealable || (m->seals & 1)) r = -LX_EPERM;
    else if (seals & ~0x1fu) r = -LX_EINVAL;
    else m->seals |= seals;
    ReleaseSRWLockExclusive(&g_mfd_lock);
    return r;
}

slong xrl_memfd_create(const char *name, unsigned flags)
{
    static volatile LONG ctr;
    if (flags & ~(1u | 2u | 4u | (0x3fu << 26))) return -LX_EINVAL;   /* MFD_CLOEXEC, MFD_ALLOW_SEALING, MFD_HUGETLB (+ size bits, ignored) */
    unsigned n = (unsigned)InterlockedIncrement(&ctr);
    char g[128], disp[300];
    wchar_t hp[PATHBUF];
    u64 key = ((u64)(unsigned)G.pid << 32) | n;
    slong r = memfd_host_path(key, g, sizeof g, hp, ARRAY_LEN(hp));
    if (r) return r;
    HANDLE h = hopen(hp, GENERIC_READ | GENERIC_WRITE | DELETE, CREATE_NEW, FILE_FLAG_DELETE_ON_CLOSE);
    if (h == INVALID_HANDLE_VALUE) return xrl_lasterr();
    snprintf(disp, sizeof disp, "/memfd:%.249s (deleted)", name ? name : "");
    xfile *f = wrap_handle(h, LX_O_RDWR, disp, 0, hp);
    f->ops = &g_memfd_ops;
    f->priv = (void *)(uintptr_t)key;   /* the seal state is keyed by n, the low 32 bits (mfd_find) */
    mfd_open(n, (flags & 2) != 0);
    int fd = xrl_fd_install(f, (flags & 1) != 0, 0);
    xfile_unref(f);
    return fd;
}

/* A process started by exec (task.c) inherits the memfds its parent had open without close-on-exec:
 * XRL_INHERIT_FDS="fd|key|name;..." opens the same host file at the same descriptor number. */
void xrl_fd_inherit_boot(void)
{
    char v[4096];
    if (!GetEnvironmentVariableA("XRL_INHERIT_FDS", v, sizeof v)) return;
    for (char *p = v; *p; ) {
        char *e = strchr(p, ';'); if (e) *e = 0;
        int fd = atoi(p);
        char *k = strchr(p, '|'), *nm = k ? strchr(k + 1, '|') : NULL;
        if (k && nm) {
            u64 key = strtoull(k + 1, NULL, 10);
            char g[128], disp[300]; wchar_t hp[PATHBUF];
            if (!memfd_host_path(key, g, sizeof g, hp, ARRAY_LEN(hp))) {
                HANDLE h = hopen(hp, GENERIC_READ | GENERIC_WRITE, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
                if (h != INVALID_HANDLE_VALUE) {
                    snprintf(disp, sizeof disp, "/memfd:%.249s (deleted)", nm + 1);
                    xfile *f = wrap_handle(h, LX_O_RDWR, disp, 0, hp);
                    f->priv = (void *)(uintptr_t)key;
                    int got = xrl_fd_install(f, 0, fd);
                    xfile_unref(f);
                    LOGW("exec: inherited memfd %s as fd %d (wanted %d)", disp, got, fd);
                } else LOGW("exec: cannot reopen an inherited memfd (%lu)", GetLastError());
            }
        }
        if (!e) break;
        p = e + 1;
    }
}

/* open("/proc/self/fd/N") where N is a memfd: a new open file description of the same memfd, or <0 if it is not one */
static slong memfd_reopen(const char *gabs, int flags)
{
    int fd = -1;
    char pre[40];
    snprintf(pre, sizeof pre, "/proc/%d/fd/", G.pid);
    if (!strncmp(gabs, "/proc/self/fd/", 14)) fd = atoi(gabs + 14);
    else if (!strncmp(gabs, pre, strlen(pre))) fd = atoi(gabs + strlen(pre));
    if (fd < 0) return -1;
    xfile *of = xrl_fd_get(fd);
    if (!of) { LOGW("memfd_reopen(%s): fd %d not found", gabs, fd); return -1; }
    if ((of->ops != &g_memfd_ops && of->ops != &g_file_ops) || !of->gpath || strncmp(of->gpath, "/memfd:", 7)) {
        LOGW("memfd_reopen(%s): fd %d not a memfd (gpath=%s)", gabs, fd, of->gpath ? of->gpath : "(null)");
        xfile_unref(of); return -1;
    }
    char g[128];
    wchar_t hp[PATHBUF];
    slong r = memfd_host_path((u64)(uintptr_t)of->priv, g, sizeof g, hp, ARRAY_LEN(hp));
    if (r) { xfile_unref(of); return r; }
    int acc = flags & LX_O_ACCMODE;
    HANDLE h = hopen(hp, acc == LX_O_RDONLY ? GENERIC_READ : acc == LX_O_WRONLY ? GENERIC_WRITE : (GENERIC_READ | GENERIC_WRITE), OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
    if (h == INVALID_HANDLE_VALUE) { xfile_unref(of); return xrl_lasterr(); }
    xfile *f = wrap_handle(h, flags & ~(LX_O_CREAT | LX_O_EXCL | LX_O_TRUNC), of->gpath, 0, hp);
    f->ops = &g_memfd_ops;
    f->priv = of->priv;
    mfd_open((u32)(uintptr_t)of->priv, 0);
    xfile_unref(of);
    int nfd = xrl_fd_install(f, (flags & LX_O_CLOEXEC) != 0, 0);
    xfile_unref(f);
    LOGW("memfd_reopen(%s): ok -> fd %d", gabs, nfd);
    return nfd;
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
    if (vn_stat(gabs, st)) return 0;
    if (xrl_proc_stat(gabs, st)) return st->st_nlink ? 0 : -LX_ENOENT;
    if (xrl_pty_stat(gabs, st)) return st->st_nlink ? 0 : -LX_ENOENT;
    if (!strcmp(gabs, "/dev/ptmx")) { xrl_stat_synth(st, LX_S_IFCHR | 0666, 13, 0); st->st_rdev = (5u << 8) | 2; return 0; }
    if (!strcmp(gabs, "/dev/null")) { xrl_stat_synth(st, LX_S_IFCHR | 0666, 3, 0); st->st_rdev = (1u << 8) | 3; return 0; }
    if (!strcmp(gabs, "/dev/zero")) { xrl_stat_synth(st, LX_S_IFCHR | 0666, 5, 0); st->st_rdev = (1u << 8) | 5; return 0; }
    if (!strcmp(gabs, "/dev/urandom") || !strcmp(gabs, "/dev/random")) { xrl_stat_synth(st, LX_S_IFCHR | 0666, 9, 0); st->st_rdev = (1u << 8) | 9; return 0; }
    if (!strcmp(gabs, "/dev/tty") || !strcmp(gabs, "/dev/console")) { xrl_stat_synth(st, LX_S_IFCHR | 0666, 11, 0); st->st_rdev = (5u << 8); return 0; }
    if (xrl_dev_evdev_stat(gabs, st) || xrl_dev_fb_stat(gabs, st)) return 0;

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
    if (g_meta_on) {
        int want = (mode & 4) | (mode & 2) | (mode & 1);
        return want && !perm_ok(&st, want) ? -LX_EACCES : 0;
    }
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
    char gabs[PATHBUF]; wchar_t hp[PATHBUF];
    slong r = xrl_vfs_resolve(dirfd, path, 0, gabs, sizeof gabs);
    if (r) return r;
    r = xrl_vfs_host_path(gabs, hp, ARRAY_LEN(hp));
    if (r) return r;
    if (!CreateDirectoryW(hp, NULL)) return xrl_lasterr();
    meta_created(gabs, 1, (u32)mode);
    return 0;
}

slong xrl_vfs_unlink(int dirfd, const char *path, int flags)
{
    char gabs[PATHBUF]; wchar_t hp[PATHBUF];
    slong r = xrl_vfs_resolve(dirfd, path, 0, gabs, sizeof gabs);
    if (r) return r;
    if (!(flags & LX_AT_REMOVEDIR) && sym_remove(gabs) == 0) return 0;
    if (vn_exists(gabs)) return (flags & LX_AT_REMOVEDIR) ? -LX_ENOTDIR : vn_remove(gabs);
    r = xrl_vfs_host_path(gabs, hp, ARRAY_LEN(hp));
    if (r) return r;
    DWORD a = GetFileAttributesW(hp);
    if (a == INVALID_FILE_ATTRIBUTES) return xrl_lasterr();
    if (a & FILE_ATTRIBUTE_DIRECTORY) {
        if (!(flags & LX_AT_REMOVEDIR)) return -LX_EISDIR;
        if (!RemoveDirectoryW(hp)) return xrl_lasterr();
        meta_drop(gabs);
        return 0;
    }
    if (flags & LX_AT_REMOVEDIR) return -LX_ENOTDIR;
    if (a & FILE_ATTRIBUTE_READONLY) SetFileAttributesW(hp, a & ~FILE_ATTRIBUTE_READONLY);
    if (!DeleteFileW(hp)) return xrl_lasterr();
    meta_drop(gabs);
    return 0;
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
    if (vn_exists(go)) return (flags & 1) && vn_exists(gn) ? -LX_EEXIST : vn_rename(go, gn);
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
    meta_move(go, gn, (ao & FILE_ATTRIBUTE_DIRECTORY) != 0);
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
    if (!CreateHardLinkW(hn, ho, NULL)) return xrl_lasterr();
    meta_copy(go, gn);
    return 0;
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
    strcpy_s(xrl_curproc()->cwd, XRL_CWD_MAX, gabs);
    ReleaseSRWLockExclusive(&G.cwd_lock);
    return 0;
}

slong xrl_vfs_getcwd(char *buf, u64 cap)
{
    AcquireSRWLockShared(&G.cwd_lock);
    const char *cwd = xrl_curproc()->cwd;
    size_t n = strlen(cwd) + 1;
    slong r = n > cap ? -LX_ERANGE : (slong)n;
    if (r > 0) memcpy(buf, cwd, n);
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
