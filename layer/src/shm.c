/*
 * shm.c - shared memory: SysV segments (shmget/shmat/shmdt/shmctl) and shared mappings of memfd / /dev/shm files.
 *
 * What an X server and its clients (MIT-SHM, DRI3 and Wayland-style buffers, Firefox's shmem) need is that two
 * mappings of the same memory see each other's writes. Windows can alias pages at two addresses (sections), but
 * the layer's memory manager (mem.c) owns one reserved arena with its own VMA list, lazy commit and swap, and the
 * guest processes share one address space (proc.c). So the shared pages are one ordinary MAP_SHARED anonymous
 * region per object, and EVERY mapping or attach of the object returns the same address (plus the file offset).
 * Linux lets shmat(NULL) and mmap(NULL, ...) return any address, so this is legal; what it cannot do is put one
 * object at two addresses (MAP_FIXED over it, mapping a memfd twice side by side for a ring buffer, shmat at a
 * chosen address): those get the ordinary private-copy mapping or -EINVAL. Protection is per object, not per
 * mapping: a PROT_READ attach of a segment is writable.
 *
 * An object lives while something maps it:
 *   - a SysV segment: until shmctl(IPC_RMID) and the last shmdt (nattch), as on Linux; the pages are allocated at
 *     the first shmat. munmap() of the attach counts as shmdt.
 *   - a file object (memfd, /dev/shm/...): until every mapping of it is unmapped. The pages are read from the file
 *     when the first mapping is made and written back when the last one goes, so contents survive "map, unmap, map
 *     again". read()/write() on the file while it is mapped do not see the mapping's writes (mmap is the interface
 *     these files are used through). The file is kept referenced until then, so close(fd) after mmap works.
 * The guest's memory-map accounting (mem.c) sees the pages as one shared VMA named /SYSV%08x (deleted) or the
 * file's path. A process that exits or execs must not unmap them while another still uses them: proc.c's mm release
 * has to skip VMAs that xrl_shm_munmap claims (see docs/decisions.md).
 */
#include "xrl_internal.h"

#define LX_IPC_PRIVATE 0
#define LX_IPC_CREAT 01000
#define LX_IPC_EXCL 02000
#define LX_IPC_RMID 0
#define LX_IPC_SET 1
#define LX_IPC_STAT 2
#define LX_IPC_INFO 3
#define LX_SHM_LOCK 11
#define LX_SHM_UNLOCK 12
#define LX_SHM_STAT 13
#define LX_SHM_INFO 14
#define LX_SHM_RDONLY 010000
#define LX_SHM_RND 020000
#define LX_SHM_REMAP 040000
#define LX_EIDRM 43
#define SHM_MAX_IDS 4096
#define SHM_MAX_BYTES (1ull << 40)
#define SHM_WRITEBACK_MAX (64ull << 20)   /* a /dev/shm file bigger than this is not written back when its last mapping goes */

typedef struct att { u64 s, e; } att;

typedef struct shmseg {
    struct shmseg *next;
    int sysv;                          /* SysV segment, else a file object */
    int id, key;
    u32 mode, uid, gid, cuid, cgid;
    int cpid, lpid;
    s64 atime, dtime, ctime;
    int nattch, removed;
    char *fkey;                        /* file object: identity (memfd number or /dev/shm path) */
    int memfd, orphan;                 /* a memfd; every description of it is closed (vfs.c) */
    xfile *file;                       /* a /dev/shm file: referenced while mapped, for the write-back */
    u64 addr, size, reqsize;           /* the pages (0 until first use), their page-rounded size, the size asked for */
    att *maps; int nmaps, capmaps;     /* file object: the mapped ranges */
} shmseg;

static SRWLOCK g_shm_lock = SRWLOCK_INIT;
static shmseg *g_segs;
static int g_next_id;

static s64 now_s(void) { return (s64)(xrl_real_ns() / 1000000000ull); }

static void seg_name(const shmseg *g, char *out, size_t cap)
{
    if (g->sysv) snprintf(out, cap, "/SYSV%08x (deleted)", (unsigned)g->key);
    else snprintf(out, cap, "%s", g->file && g->file->gpath ? g->file->gpath : "/dev/shm/?");
}

static void seg_unlink(shmseg *g)   /* lock held */
{
    for (shmseg **pp = &g_segs; *pp; pp = &(*pp)->next) if (*pp == g) { *pp = g->next; break; }
}

/* pages go back, a file object is written back first (the file is still ours to write) */
static void seg_destroy(shmseg *g)   /* lock held, already unlinked */
{
    if (g->addr) {
        if (!g->sysv && g->file && g->file->ops->write && g->size <= SHM_WRITEBACK_MAX) {
            lx_stat st;
            if (g->file->ops->fstat && !g->file->ops->fstat(g->file, &st) && st.st_size > 0)
                g->file->ops->write(g->file, (const void *)g->addr, (u64)st.st_size < g->size ? (u64)st.st_size : g->size, 0);
        }
        xrl_mem_shared_free(g->addr, g->size);
    }
    if (g->file) xfile_unref(g->file);
    free(g->fkey); free(g->maps); free(g);
}

/* ================================================================ SysV */
static shmseg *by_id(int id)
{
    for (shmseg *g = g_segs; g; g = g->next) if (g->sysv && g->id == id) return g;
    return NULL;
}

slong xrl_shmget(int key, u64 size, int flags)
{
    slong r = 0;
    AcquireSRWLockExclusive(&g_shm_lock);
    shmseg *g = NULL;
    if (key != LX_IPC_PRIVATE) for (g = g_segs; g; g = g->next) if (g->sysv && !g->removed && g->key == key) break;
    if (g) {
        if ((flags & LX_IPC_CREAT) && (flags & LX_IPC_EXCL)) r = -LX_EEXIST;
        else if (size > g->size) r = -LX_EINVAL;
        else r = g->id;
    } else if (key != LX_IPC_PRIVATE && !(flags & LX_IPC_CREAT)) r = -LX_ENOENT;
    else if (size == 0 || size > SHM_MAX_BYTES) r = -LX_EINVAL;
    else {
        int n = 0;
        for (shmseg *x = g_segs; x; x = x->next) n += x->sysv;
        if (n >= SHM_MAX_IDS) r = -LX_ENOSPC;
        else {
            g = (shmseg *)calloc(1, sizeof *g);
            if (!g) r = -LX_ENOMEM;
            else {
                g->sysv = 1; g->id = g_next_id++; g->key = key;
                g->size = ALIGN_UP(size, LX_PAGE); g->reqsize = size;
                g->mode = (u32)flags & 0777;
                g->uid = g->cuid = xrl_cur_uid(); g->gid = g->cgid = xrl_cur_gid();
                g->cpid = G.pid; g->ctime = now_s();
                g->next = g_segs; g_segs = g;
                r = g->id;
            }
        }
    }
    ReleaseSRWLockExclusive(&g_shm_lock);
    return r;
}

slong xrl_shmat(int id, u64 addr, int flags)
{
    (void)flags;
    slong r;
    AcquireSRWLockExclusive(&g_shm_lock);
    shmseg *g = by_id(id);
    if (!g) r = -LX_EINVAL;
    else if (g->removed && g->nattch == 0) r = -LX_EIDRM;
    else {
        if (!g->addr) {
            char nm[40];
            seg_name(g, nm, sizeof nm);
            g->addr = xrl_mem_shared_alloc(g->size, nm);
        }
        if (!g->addr) r = -LX_ENOMEM;
        else if (addr && addr != g->addr) r = -LX_EINVAL;   /* one address per segment: see the header comment */
        else { g->nattch++; g->lpid = G.pid; g->atime = now_s(); r = (slong)g->addr; }
    }
    ReleaseSRWLockExclusive(&g_shm_lock);
    return r;
}

static void sysv_detach(shmseg *g)   /* lock held */
{
    if (g->nattch > 0) g->nattch--;
    g->dtime = now_s(); g->lpid = G.pid;
    if (g->removed && g->nattch == 0) { seg_unlink(g); seg_destroy(g); }
}

slong xrl_shmdt(u64 addr)
{
    slong r = -LX_EINVAL;
    AcquireSRWLockExclusive(&g_shm_lock);
    for (shmseg *g = g_segs; g; g = g->next)
        if (g->sysv && g->addr && g->addr == addr) { if (g->nattch > 0) { sysv_detach(g); r = 0; } break; }
    ReleaseSRWLockExclusive(&g_shm_lock);
    return r;
}

/* struct shmid_ds, x86-64: ipc_perm {key,uid,gid,cuid,cgid,mode,seq,pad,2 longs} = 48, then segsz, atime, dtime,
 * ctime, cpid, lpid, nattch, 2 longs = 112 bytes */
static void fill_ds(const shmseg *g, u8 *b)
{
    memset(b, 0, 112);
    s32 key = g->key; u32 mode = g->mode;
    memcpy(b + 0, &key, 4); memcpy(b + 4, &g->uid, 4); memcpy(b + 8, &g->gid, 4);
    memcpy(b + 12, &g->cuid, 4); memcpy(b + 16, &g->cgid, 4); memcpy(b + 20, &mode, 4);
    memcpy(b + 48, &g->reqsize, 8);
    memcpy(b + 56, &g->atime, 8); memcpy(b + 64, &g->dtime, 8); memcpy(b + 72, &g->ctime, 8);
    memcpy(b + 80, &g->cpid, 4); memcpy(b + 84, &g->lpid, 4);
    u64 n = (u64)g->nattch; memcpy(b + 88, &n, 8);
}

slong xrl_shmctl(int id, int cmd, u64 ubuf)
{
    slong r = 0;
    int c = cmd & ~0x100;   /* IPC_64 */
    if (c == LX_IPC_INFO) {   /* struct shminfo: shmmax, shmmin, shmmni, shmseg, shmall */
        u64 v[5] = { SHM_MAX_BYTES, 1, SHM_MAX_IDS, SHM_MAX_IDS, SHM_MAX_BYTES / LX_PAGE };
        return xrl_ctu(ubuf, v, sizeof v) ? -LX_EFAULT : 0;
    }
    AcquireSRWLockExclusive(&g_shm_lock);
    if (c == LX_SHM_INFO) {   /* struct shm_info: used_ids, shm_tot, shm_rss, shm_swp, swap_attempts, swap_successes */
        u64 tot = 0; int used = 0;
        for (shmseg *g = g_segs; g; g = g->next) if (g->sysv) { used++; tot += g->size / LX_PAGE; }
        struct { int used; int pad; u64 tot, rss, swp, a, s; } si = { used, 0, tot, tot, 0, 0, 0 };
        r = xrl_ctu(ubuf, &si, sizeof si) ? -LX_EFAULT : (used ? used - 1 : 0);
        ReleaseSRWLockExclusive(&g_shm_lock);
        return r;
    }
    shmseg *g = by_id(id);
    if (!g) r = -LX_EINVAL;
    else switch (c) {
    case LX_IPC_STAT: case LX_SHM_STAT: {
        u8 b[112];
        fill_ds(g, b);
        r = xrl_ctu(ubuf, b, sizeof b) ? -LX_EFAULT : (c == LX_SHM_STAT ? g->id : 0);
        break;
    }
    case LX_IPC_SET: {
        u8 b[112];
        if (xrl_cfu(b, ubuf, sizeof b)) { r = -LX_EFAULT; break; }
        u32 euid = xrl_cur_uid();
        if (euid != 0 && euid != g->uid && euid != g->cuid) { r = -LX_EPERM; break; }
        memcpy(&g->uid, b + 4, 4); memcpy(&g->gid, b + 8, 4);
        u32 m; memcpy(&m, b + 20, 4); g->mode = m & 0777;
        g->ctime = now_s();
        break;
    }
    case LX_IPC_RMID: {
        u32 euid = xrl_cur_uid();
        if (euid != 0 && euid != g->uid && euid != g->cuid) { r = -LX_EPERM; break; }
        g->removed = 1;
        if (g->nattch == 0) { seg_unlink(g); seg_destroy(g); }
        break;
    }
    case LX_SHM_LOCK: case LX_SHM_UNLOCK: break;
    default: r = -LX_EINVAL;
    }
    ReleaseSRWLockExclusive(&g_shm_lock);
    return r;
}

/* ====================================================== memfd / /dev/shm */
static const char *file_key(xfile *f, char *buf, size_t cap)
{
    if (!f->gpath) return NULL;
    if (!strncmp(f->gpath, "/memfd:", 7)) { snprintf(buf, cap, "memfd:%llu", (unsigned long long)(uintptr_t)f->priv); return buf; }   /* priv: the memfd's number (vfs.c) */
    if (!strncmp(f->gpath, "/dev/shm/", 9)) return f->gpath;
    return NULL;
}

static void maps_add(shmseg *g, u64 s, u64 e)
{
    if (g->nmaps == g->capmaps) { g->capmaps = g->capmaps ? g->capmaps * 2 : 8; g->maps = (att *)realloc(g->maps, (size_t)g->capmaps * sizeof(att)); }
    g->maps[g->nmaps++] = (att){ s, e };
}

/* take [s,e) out of the mapped ranges */
static void maps_sub(shmseg *g, u64 s, u64 e)
{
    for (int i = 0; i < g->nmaps;) {
        att a = g->maps[i];
        if (a.e <= s || a.s >= e) { i++; continue; }
        if (a.s < s && a.e > e) { g->maps[i].e = s; maps_add(g, e, a.e); return; }   /* a hole in the middle */
        if (a.s < s) { g->maps[i].e = s; i++; }
        else if (a.e > e) { g->maps[i].s = e; i++; }
        else g->maps[i] = g->maps[--g->nmaps];
    }
}

slong xrl_shm_map_file(xfile *f, u64 len, s64 off)
{
    char kb[64];
    const char *key = file_key(f, kb, sizeof kb);
    if (!key || !f->ops->fstat || !f->ops->read) return XRL_NOT_NET;
    slong r;
    AcquireSRWLockExclusive(&g_shm_lock);
    shmseg *g;
    for (g = g_segs; g; g = g->next) if (!g->sysv && !strcmp(g->fkey, key)) break;
    u64 need = ALIGN_UP((u64)off + len, LX_PAGE);
    if (!g) {
        lx_stat st;
        u64 fsize = f->ops->fstat(f, &st) ? 0 : (u64)st.st_size;
        u64 size = ALIGN_UP(fsize > need ? fsize : need, LX_PAGE);
        g = (shmseg *)calloc(1, sizeof *g);
        if (!g) { ReleaseSRWLockExclusive(&g_shm_lock); return -LX_ENOMEM; }
        g->fkey = _strdup(key); g->size = size;
        g->memfd = !strncmp(f->gpath, "/memfd:", 7);
        char nm[300];
        snprintf(nm, sizeof nm, "%s", f->gpath);
        if (!g->memfd) { g->file = f; xfile_ref(f); }   /* a memfd needs no write-back: its pages go when its last description does */
        g->addr = xrl_mem_shared_alloc(size, nm);
        if (!g->addr) { seg_destroy(g); ReleaseSRWLockExclusive(&g_shm_lock); return -LX_ENOMEM; }
        /* what was written before the first mmap. In 64 KiB pieces, zero ones skipped: reading straight into the
         * pages would commit all of a sparse 256 MiB memfd. */
        u8 *tmp = fsize ? (u8 *)malloc(65536) : NULL;
        for (u64 o = 0; tmp && o < fsize && o < size; o += 65536) {
            u64 n = fsize - o < 65536 ? fsize - o : 65536;
            slong got = f->ops->read(f, tmp, n, (s64)o);
            if (got <= 0) break;
            int nz = 0;
            for (slong i = 0; i < got && !nz; i++) nz = tmp[i] != 0;
            if (nz) xrl_ctu(g->addr + o, tmp, (size_t)got);
        }
        free(tmp);
        g->next = g_segs; g_segs = g;
    } else if (need > g->size) {
        static volatile LONG once;
        if (!InterlockedExchange(&once, 1)) LOGW("shm: %s mapped beyond the %llu bytes of its pages (grown after the first mmap): -ENOMEM", g->fkey, (unsigned long long)g->size);
        ReleaseSRWLockExclusive(&g_shm_lock);
        return -LX_ENOMEM;
    }
    maps_add(g, g->addr + (u64)off, g->addr + (u64)off + ALIGN_UP(len, LX_PAGE));
    r = (slong)(g->addr + (u64)off);
    ReleaseSRWLockExclusive(&g_shm_lock);
    return r;
}

/* munmap(addr,len) inside one of our regions: a file object loses that mapped range; a SysV attach is a shmdt.
 * Not ours: XRL_NOT_NET and mem.c unmaps as usual. */
slong xrl_shm_munmap(u64 addr, u64 len)
{
    slong r = XRL_NOT_NET;
    AcquireSRWLockExclusive(&g_shm_lock);
    for (shmseg *g = g_segs; g; g = g->next) {
        if (!g->addr || addr + len <= g->addr || addr >= g->addr + g->size) continue;
        if (addr < g->addr || addr + len > g->addr + g->size) { r = -LX_EINVAL; break; }   /* straddles the region's edge */
        if (g->sysv) { if (addr == g->addr && g->nattch > 0) sysv_detach(g); }
        else {
            maps_sub(g, addr, addr + len);
            if (g->nmaps == 0 && (!g->memfd || g->orphan)) { seg_unlink(g); seg_destroy(g); }
        }
        r = 0;
        break;
    }
    ReleaseSRWLockExclusive(&g_shm_lock);
    return r;
}

/* vfs.c: the last open file description of memfd `n` is closed. Its pages go once nothing maps them either. */
void xrl_shm_memfd_gone(u32 n)
{
    char key[32];
    snprintf(key, sizeof key, "memfd:%llu", (unsigned long long)n);
    AcquireSRWLockExclusive(&g_shm_lock);
    for (shmseg *g = g_segs; g; g = g->next)
        if (g->memfd && !strcmp(g->fkey, key)) {
            g->orphan = 1;
            if (g->nmaps == 0) { seg_unlink(g); seg_destroy(g); }
            break;
        }
    ReleaseSRWLockExclusive(&g_shm_lock);
}
