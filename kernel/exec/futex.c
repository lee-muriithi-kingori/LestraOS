/*
 * Lestra OS - Futex implementation with hash table + wait queues
 */
#include <lestra/types.h>
#include <lestra/printk.h>
#include <lestra/sched.h>
#include <lestra/uaccess.h>
#include <string.h>

#define FUTEX_WAIT 0
#define FUTEX_WAKE 1
#define FUTEX_WAIT_BITSET 9
#define FUTEX_WAKE_BITSET 10
#define FUTEX_REQUEUE 3
#define FUTEX_CMP_REQUEUE 4
#define FUTEX_WAKE_OP 5
#define FUTEX_PRIVATE_FLAG 128
#define FUTEX_CLOCK_REALTIME 256

#define MAX_FUTEX_WAITERS 256
#define FUTEX_HASH_BUCKETS 64

struct futex_waiter {
    int in_use;
    uint32_t* uaddr;
    void* task;
    uint32_t expected_val;
    uint32_t bitset;
    int pid;
};

static struct futex_waiter futex_waiters[MAX_FUTEX_WAITERS];
static int futex_buckets[FUTEX_HASH_BUCKETS][8];

static int futex_hash(uint32_t* uaddr) {
    uintptr_t v = (uintptr_t)uaddr >> 12;
    v ^= v >> 16; v ^= v >> 8;
    return (int)(v % FUTEX_HASH_BUCKETS);
}

static int alloc_waiter(void) {
    for (int i = 0; i < MAX_FUTEX_WAITERS; i++)
        if (!futex_waiters[i].in_use) return i;
    return -1;
}

static void bucket_add(int bucket, int idx) {
    for (int i = 0; i < 8; i++)
        if (futex_buckets[bucket][i] == 0) {
            futex_buckets[bucket][i] = idx + 1; return;
        }
}

static void bucket_remove(int bucket, int idx) {
    int target = idx + 1;
    for (int i = 0; i < 8; i++)
        if (futex_buckets[bucket][i] == target) {
            futex_buckets[bucket][i] = 0; return;
        }
}

int64_t futex_dispatch(uint32_t* uaddr, uint32_t op, uint32_t val,
                        uint64_t timeout, uint32_t* uaddr2, uint32_t val3) {
    (void)timeout; (void)uaddr2;
    if (!uaddr) return -14;
    /* Validate the user pointer range once on entry. The per-access
     * reads below use get_user() which re-checks + wraps with stac/clac
     * so we never #PF under SMAP. */
    if (!access_ok(uaddr, sizeof(uint32_t))) return -14;
    uint32_t op_clean = op & ~FUTEX_PRIVATE_FLAG;
    op_clean &= ~FUTEX_CLOCK_REALTIME;

    switch (op_clean) {
        case FUTEX_WAIT: {
            /* SMAP-safe read of the user word: get_user() wraps the
             * dereference with stac/clac. Returns -EFAULT (-14) on a
             * bad pointer; we map that to -EAGAIN so callers retry. */
            uint32_t kuval = 0;
            if (get_user(&kuval, uaddr) < 0) return -14;
            if (kuval != val) return -11;  /* -EAGAIN */
            int slot = alloc_waiter();
            if (slot < 0) return -12;      /* -ENOMEM */
            struct futex_waiter* w = &futex_waiters[slot];
            w->in_use = 1; w->uaddr = uaddr; w->expected_val = val;
            w->bitset = 0xFFFFFFFF;
            w->task = task_current();
            extern int proc_getpid(void);
            w->pid = proc_getpid();
            int bucket = futex_hash(uaddr);
            bucket_add(bucket, slot);
            task_block();
            return 0;
        }
        case FUTEX_WAIT_BITSET: {
            uint32_t kuval = 0;
            if (get_user(&kuval, uaddr) < 0) return -14;
            if (kuval != val) return -11;  /* -EAGAIN */
            int slot = alloc_waiter();
            if (slot < 0) return -12;
            struct futex_waiter* w = &futex_waiters[slot];
            w->in_use = 1; w->uaddr = uaddr; w->expected_val = val;
            w->bitset = val3 ? val3 : 0xFFFFFFFF;
            w->task = task_current();
            extern int proc_getpid(void);
            w->pid = proc_getpid();
            int bucket = futex_hash(uaddr);
            bucket_add(bucket, slot);
            task_block();
            return 0;
        }
        case FUTEX_WAKE: {
            int bucket = futex_hash(uaddr);
            int woken = 0;
            for (int i = 0; i < 8 && woken < (int)val; i++) {
                int idx = futex_buckets[bucket][i];
                if (idx == 0) continue;
                idx--;
                struct futex_waiter* w = &futex_waiters[idx];
                if (w->in_use && w->uaddr == uaddr) {
                    task_unblock(w->task);
                    w->in_use = 0;
                    bucket_remove(bucket, idx);
                    woken++; i--;
                }
            }
            return (int64_t)woken;
        }
        case FUTEX_WAKE_BITSET: {
            int bucket = futex_hash(uaddr);
            int woken = 0;
            for (int i = 0; i < 8 && woken < (int)val; i++) {
                int idx = futex_buckets[bucket][i];
                if (idx == 0) continue;
                idx--;
                struct futex_waiter* w = &futex_waiters[idx];
                if (w->in_use && w->uaddr == uaddr && (w->bitset & val3)) {
                    task_unblock(w->task);
                    w->in_use = 0;
                    bucket_remove(bucket, idx);
                    woken++; i--;
                }
            }
            return (int64_t)woken;
        }
        default:
            return -38;
    }
}
