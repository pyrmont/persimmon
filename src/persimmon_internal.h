#ifndef PERSIMMON_INTERNAL_H
#define PERSIMMON_INTERNAL_H

#include <string.h>
#include "persimmon.h"

/*
 * Shared by the implementation of each structure. Not installed and not part
 * of the library's interface.
 */

/* Atomics */

/*
 * Reference counts are manipulated with acquire/release ordering so that a node
 * dropping to zero on one thread sees every write made through the references
 * that preceded it. Increments can be relaxed: the caller already holds a
 * reference, so the node cannot be freed underneath it.
 *
 * PERSIMM_RC_INC and PERSIMM_RC_DEC both evaluate to the count as it was
 * before the operation, following the C11 fetch functions.
 */

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_ATOMICS__)

#include <stdatomic.h>
typedef _Atomic size_t persimm_refcount_t;
#define PERSIMM_RC_ATOMIC 1
#define PERSIMM_RC_SET(rc, v) atomic_init(&(rc), (size_t)(v))
#define PERSIMM_RC_LOAD(rc) atomic_load_explicit(&(rc), memory_order_acquire)
#define PERSIMM_RC_INC(rc) atomic_fetch_add_explicit(&(rc), 1, memory_order_relaxed)
#define PERSIMM_RC_DEC(rc) atomic_fetch_sub_explicit(&(rc), 1, memory_order_acq_rel)

#elif defined(__GNUC__) || defined(__clang__)

typedef size_t persimm_refcount_t;
#define PERSIMM_RC_ATOMIC 1
#define PERSIMM_RC_SET(rc, v) __atomic_store_n(&(rc), (size_t)(v), __ATOMIC_RELAXED)
#define PERSIMM_RC_LOAD(rc) __atomic_load_n(&(rc), __ATOMIC_ACQUIRE)
#define PERSIMM_RC_INC(rc) __atomic_fetch_add(&(rc), 1, __ATOMIC_RELAXED)
#define PERSIMM_RC_DEC(rc) __atomic_fetch_sub(&(rc), 1, __ATOMIC_ACQ_REL)

#elif defined(_WIN32)

#include <windows.h>
typedef volatile size_t persimm_refcount_t;
#define PERSIMM_RC_ATOMIC 1
#define PERSIMM_RC_SET(rc, v) ((rc) = (size_t)(v))
#if defined(_WIN64)
#define PERSIMM_RC_LOAD(rc) ((size_t)InterlockedCompareExchange64((volatile LONG64 *)&(rc), 0, 0))
#define PERSIMM_RC_INC(rc) ((size_t)InterlockedIncrement64((volatile LONG64 *)&(rc)) - 1)
#define PERSIMM_RC_DEC(rc) ((size_t)InterlockedDecrement64((volatile LONG64 *)&(rc)) + 1)
#else
#define PERSIMM_RC_LOAD(rc) ((size_t)InterlockedCompareExchange((volatile LONG *)&(rc), 0, 0))
#define PERSIMM_RC_INC(rc) ((size_t)InterlockedIncrement((volatile LONG *)&(rc)) - 1)
#define PERSIMM_RC_DEC(rc) ((size_t)InterlockedDecrement((volatile LONG *)&(rc)) + 1)
#endif

#else

typedef size_t persimm_refcount_t;
#define PERSIMM_RC_ATOMIC 0
#define PERSIMM_RC_SET(rc, v) ((rc) = (size_t)(v))
#define PERSIMM_RC_LOAD(rc) (rc)
#define PERSIMM_RC_INC(rc) ((rc)++)
#define PERSIMM_RC_DEC(rc) ((rc)--)

#endif

/* Alignment */

/*
 * Never read from. It exists so that a flexible array member declared with it
 * inherits the widest fundamental alignment, which lets one allocation hold
 * either pointers or arbitrary element bytes.
 */
typedef union {
    long long as_long_long;
    long double as_long_double;
    void *as_pointer;
} persimm_align_t;

/* Elements */

static inline void persimm_elem_retain(const persimm_elem_ops *ops, void *ctx, void *slot) {
    if (NULL != ops && NULL != ops->retain) ops->retain(slot, ctx);
}

static inline void persimm_elem_release(const persimm_elem_ops *ops, void *ctx, void *slot) {
    if (NULL != ops && NULL != ops->release) ops->release(slot, ctx);
}

#endif /* end of include guard */
