/*
 * futex.c - Linux futex(2) on Win32 (WaitOnAddress / WakeByAddressSingle).
 *
 * We do NOT put WaitOnAddress directly on the guest futex word. Linux semantics
 * need exact wake counts, bitsets and requeue, and WaitOnAddress offers neither
 * (it also wakes spuriously on hash collisions). Instead this is a faithful
 * miniature of the kernel's futex hash table:
 *
 *   waiter:  lock(bucket); if (*uaddr != val) -> -EAGAIN; enqueue; unlock;
 *            park on a private word (WaitOnAddress on waiter.state)
 *   waker:   lock(bucket); dequeue up to N matching waiters; unlock;
 *            set waiter.state and WakeByAddressSingle on it
 *
 * The check-and-enqueue is atomic w.r.t. wakers because both run under the
 * bucket lock, so no wakeup is lost. Parking is on a per-waiter word, so wake
 * counts are exact and there are no foreign spurious wakes.
 *
 * Supported ops: WAIT, WAKE, WAIT_BITSET, WAKE_BITSET, REQUEUE, CMP_REQUEUE,
 * WAKE_OP (+ PRIVATE/CLOCK_REALTIME flags). PI futexes return -ENOSYS (logged).
 * Timeouts have millisecond granularity (WaitOnAddress), rounded UP.
 */
#include "xrl_internal.h"

#define NBUCKET 512

typedef struct waiter {
    struct waiter *next, *prev;
    u64 key;
    u32 bitset;
    volatile LONG state;       /* 0 = waiting, 1 = woken, 2 = interrupted */
    int queued;
    struct bucket *b;
} waiter;

typedef struct bucket {
    SRWLOCK lock;
    waiter *head, *tail;
} bucket;

static bucket g_b[NBUCKET];

static bucket *bucket_of(u64 key)
{
    u64 h = (key >> 2) * 0x9E3779B97F4A7C15ull;
    return &g_b[(h >> 40) % NBUCKET];
}

void xrl_futex_init(void)
{
    for (int i = 0; i < NBUCKET; i++) InitializeSRWLock(&g_b[i].lock);
}

static void q_add(bucket *b, waiter *w)
{
    w->b = b; w->next = NULL; w->prev = b->tail;
    if (b->tail) b->tail->next = w; else b->head = w;
    b->tail = w;
    w->queued = 1;
}
static void q_del(bucket *b, waiter *w)
{
    if (w->prev) w->prev->next = w->next; else b->head = w->next;
    if (w->next) w->next->prev = w->prev; else b->tail = w->prev;
    w->queued = 0;
}

static int read_user_u32(u64 uaddr, u32 *out)
{
    if ((uaddr & 3) || uaddr < 0x10000) return -LX_EINVAL;
    __try { *out = *(volatile u32 *)uaddr; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -LX_EFAULT; }
    return 0;
}

/* wake up to n waiters on key matching bitset; returns number woken. */
static slong do_wake(u64 key, int n, u32 bitset)
{
    bucket *b = bucket_of(key);
    volatile LONG *wake_list[64];
    slong woken = 0;
    while (n > 0) {
        int cnt = 0;
        AcquireSRWLockExclusive(&b->lock);
        for (waiter *w = b->head; w && n > 0 && cnt < 64;) {
            waiter *nx = w->next;
            if (w->key == key && (w->bitset & bitset)) {
                q_del(b, w);
                w->state = 1;
                wake_list[cnt++] = &w->state;   /* only the address is used after unlock */
                n--; woken++;
            }
            w = nx;
        }
        ReleaseSRWLockExclusive(&b->lock);
        for (int i = 0; i < cnt; i++) WakeByAddressSingle((PVOID)wake_list[i]);
        if (cnt < 64) break;
    }
    return woken;
}

slong xrl_futex_wake_addr(u64 uaddr, int n) { return do_wake(uaddr, n, LX_FUTEX_BITSET_MATCH_ANY); }

static slong do_wait(u64 uaddr, u32 val, u64 timeout_ns_rel /* ~0 = infinite */, u32 bitset)
{
    u32 cur;
    slong r = read_user_u32(uaddr, &cur);
    if (r) return r;
    waiter w; memset(&w, 0, sizeof w);
    w.key = uaddr; w.bitset = bitset;
    bucket *b = bucket_of(uaddr);

    AcquireSRWLockExclusive(&b->lock);
    r = read_user_u32(uaddr, &cur);
    if (r) { ReleaseSRWLockExclusive(&b->lock); return r; }
    if (cur != val) { ReleaseSRWLockExclusive(&b->lock); return -LX_EAGAIN; }
    q_add(b, &w);
    ReleaseSRWLockExclusive(&b->lock);

    xrl_thread *t = xrl_cur();
    if (t) t->futex_node = &w;

    u64 deadline = (timeout_ns_rel == ~0ull) ? ~0ull : xrl_mono_ns() + timeout_ns_rel;
    LONG zero = 0;
    for (;;) {
        if (w.state) break;
        u64 left = ~0ull;
        if (deadline != ~0ull) {
            u64 now = xrl_mono_ns();
            if (now >= deadline) break;
            left = deadline - now;
        }
        xrl_wait_addr(&w.state, &zero, sizeof zero, left);
    }

    if (t) t->futex_node = NULL;
    if (w.state == 1) return 0;
    if (w.state == 2) return -LX_EINTR;
    /* timed out (or spurious loop exit): dequeue unless a waker got us first */
    AcquireSRWLockExclusive(&w.b->lock);
    int still = w.queued;
    if (still) q_del(w.b, &w);
    ReleaseSRWLockExclusive(&w.b->lock);
    if (!still) {   /* raced with a waker: it will set state; consume as a wake */
        while (!w.state) { LONG z = 0; xrl_wait_addr(&w.state, &z, sizeof z, 1000000ull); }
        return w.state == 1 ? 0 : -LX_EINTR;
    }
    return -LX_ETIMEDOUT;
}

/* Interrupt a thread blocked in futex wait (used by signal delivery). */
void xrl_futex_interrupt(xrl_thread *t)
{
    waiter *w = (waiter *)t->futex_node;
    if (!w) return;
    bucket *b = w->b;
    if (!b) return;
    AcquireSRWLockExclusive(&b->lock);
    if (w->queued && w == (waiter *)t->futex_node) {
        q_del(b, w);
        w->state = 2;
        ReleaseSRWLockExclusive(&b->lock);
        WakeByAddressSingle((PVOID)&w->state);
        return;
    }
    ReleaseSRWLockExclusive(&b->lock);
}

static slong do_requeue(u64 uaddr, int nwake, int nrequeue, u64 uaddr2, int cmp, u32 cmpval)
{
    bucket *b1 = bucket_of(uaddr), *b2 = bucket_of(uaddr2);
    volatile LONG *wake_list[64];
    int cnt = 0;
    slong total = 0;
    /* lock in address order */
    if (b1 == b2) AcquireSRWLockExclusive(&b1->lock);
    else if (b1 < b2) { AcquireSRWLockExclusive(&b1->lock); AcquireSRWLockExclusive(&b2->lock); }
    else { AcquireSRWLockExclusive(&b2->lock); AcquireSRWLockExclusive(&b1->lock); }

    slong r = 0;
    if (cmp) {
        u32 cur;
        r = read_user_u32(uaddr, &cur);
        if (!r && cur != cmpval) r = -LX_EAGAIN;
    }
    if (!r) {
        for (waiter *w = b1->head; w;) {
            waiter *nx = w->next;
            if (w->key == uaddr) {
                if (nwake > 0 && cnt < 64) {
                    q_del(b1, w); w->state = 1; wake_list[cnt++] = &w->state; nwake--; total++;
                } else if (nrequeue > 0) {
                    q_del(b1, w); w->key = uaddr2; q_add(b2, w); nrequeue--; total++;
                } else break;
            }
            w = nx;
        }
        r = total;
    }
    if (b1 == b2) ReleaseSRWLockExclusive(&b1->lock);
    else { ReleaseSRWLockExclusive(&b1->lock); ReleaseSRWLockExclusive(&b2->lock); }
    for (int i = 0; i < cnt; i++) WakeByAddressSingle((PVOID)wake_list[i]);
    return r;
}

static slong do_wake_op(u64 uaddr, int nwake, u64 uaddr2, int nwake2, u32 op)
{
    int o = (op >> 28) & 7, cmp = (op >> 24) & 15;
    int oparg = (int)((op >> 12) & 0xfff), cmparg = (int)(op & 0xfff);
    if (oparg & 0x800) oparg |= ~0xfff;
    if (cmparg & 0x800) cmparg |= ~0xfff;
    if (op & (1u << 31)) oparg = 1 << (oparg & 31);
    if ((uaddr2 & 3) || uaddr2 < 0x10000) return -LX_EFAULT;
    int old;
    __try {
        volatile LONG *p = (volatile LONG *)uaddr2;
        for (;;) {
            LONG cur = *p, nv;
            switch (o) {
            case 0: nv = oparg; break;
            case 1: nv = cur + oparg; break;
            case 2: nv = cur | oparg; break;
            case 3: nv = cur & ~oparg; break;
            case 4: nv = cur ^ oparg; break;
            default: return -LX_ENOSYS;
            }
            if (InterlockedCompareExchange(p, nv, cur) == cur) { old = cur; break; }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -LX_EFAULT; }
    int hit = 0;
    switch (cmp) {
    case 0: hit = old == cmparg; break;
    case 1: hit = old != cmparg; break;
    case 2: hit = old < cmparg; break;
    case 3: hit = old <= cmparg; break;
    case 4: hit = old > cmparg; break;
    case 5: hit = old >= cmparg; break;
    default: return -LX_ENOSYS;
    }
    slong n = do_wake(uaddr, nwake, LX_FUTEX_BITSET_MATCH_ANY);
    if (hit) n += do_wake(uaddr2, nwake2, LX_FUTEX_BITSET_MATCH_ANY);
    return n;
}

slong xrl_sys_futex(u64 uaddr, int op, u32 val, u64 timeout_or_val2, u64 uaddr2, u32 val3)
{
    int cmd = op & LX_FUTEX_CMD_MASK;
    switch (cmd) {
    case LX_FUTEX_WAIT:
    case LX_FUTEX_WAIT_BITSET: {
        u64 rel = ~0ull;
        u32 bits = LX_FUTEX_BITSET_MATCH_ANY;
        if (cmd == LX_FUTEX_WAIT_BITSET) { bits = val3; if (!bits) return -LX_EINVAL; }
        if (timeout_or_val2) {
            lx_timespec ts;
            if (xrl_cfu(&ts, timeout_or_val2, sizeof ts)) return -LX_EFAULT;
            u64 ns = xrl_ts_to_ns(&ts);
            if (ns == ~0ull) return -LX_EINVAL;
            if (cmd == LX_FUTEX_WAIT) rel = ns;                       /* relative */
            else {                                                     /* absolute */
                u64 now = (op & LX_FUTEX_CLOCK_REALTIME) ? xrl_real_ns() : xrl_mono_ns();
                rel = ns > now ? ns - now : 0;
            }
        }
        if (rel == 0 && timeout_or_val2) {
            u32 cur; slong r = read_user_u32(uaddr, &cur);
            if (r) return r;
            return cur != val ? -LX_EAGAIN : -LX_ETIMEDOUT;
        }
        return do_wait(uaddr, val, rel, bits);
    }
    case LX_FUTEX_WAKE:
        if ((int)val < 0) return -LX_EINVAL;
        return do_wake(uaddr, (int)val, LX_FUTEX_BITSET_MATCH_ANY);
    case LX_FUTEX_WAKE_BITSET:
        if (!val3) return -LX_EINVAL;
        return do_wake(uaddr, (int)val, val3);
    case LX_FUTEX_REQUEUE:
        return do_requeue(uaddr, (int)val, (int)timeout_or_val2, uaddr2, 0, 0);
    case LX_FUTEX_CMP_REQUEUE:
        return do_requeue(uaddr, (int)val, (int)timeout_or_val2, uaddr2, 1, val3);
    case LX_FUTEX_WAKE_OP:
        return do_wake_op(uaddr, (int)val, uaddr2, (int)timeout_or_val2, val3);
    default:
        LOGW("futex: unsupported op %d (cmd %d) uaddr=%llx", op, cmd, uaddr);
        return -LX_ENOSYS;
    }
}
