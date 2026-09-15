#if defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64)
#include "utils/rlock.h"
#include <stdio.h>

void rlock_lock(rlock_t *l, lock_t lock, owner_t owner) {
    lock_t desired = (lock_t)lock;
#ifdef NO_CC
    for (;;) {
        lock_t expected = 0;
        if (l->lock_.compare_exchange_strong(expected, desired)) break;
        l->owner_ = owner;
    }
#else
    while (!__sync_bool_compare_and_swap(&(l->lock_), 0, desired)) {
        l->owner_ = owner;
    }
#endif
}

void rlock_unlock(rlock_t *l) {
    // assert (l->lock_ != 0);
#ifdef NO_CC
    l->lock_.store(0);
#else
    l->lock_ = 0;
#endif
    l->owner_ = 0;
}

void rlock_st(rlock_t *l, const lock_t value) {
#ifdef NO_CC
    l->lock_.store(value);
#else
    l->lock_ = value;
#endif
}

lock_t rlock_ld(rlock_t *l) {
#ifdef NO_CC
    return l->lock_.load();
#else
    return l->lock_;
#endif
}

lock_t rlock_cas(rlock_t *l, lock_t expected, lock_t desired) {
    lock_t ll;
#ifdef NO_CC
    ll = l->lock_.load();
    lock_t exp = expected;
    (void) l->lock_.compare_exchange_strong(exp, desired);
#else
    ll = __sync_val_compare_and_swap(&(l->lock_), expected, desired);
#endif
    return ll;
}

lock_t rlock_tas(rlock_t *l) {
    lock_t oldval;
#ifdef NO_CC
    for (;;) {
        oldval = l->lock_.load();
        lock_t expected = oldval;
        if (l->lock_.compare_exchange_strong(expected, (lock_t)0xFF)) break;
    }
#else
    volatile lock_t* addr = &(l->lock_);
    __asm__ __volatile__("xchgb %0,%1"
            : "=q"(oldval), "=m"(*addr)
            : "0"((unsigned char) 0xff), "m"(*addr) : "memory");
#endif
    return (lock_t) oldval;
}
#endif
