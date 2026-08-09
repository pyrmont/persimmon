#ifndef PERSIMMON_INTERNAL_H
#define PERSIMMON_INTERNAL_H

#include <string.h>
#include "persimmon.h"

/* Allocation */

/* Core-only tests replace allocation so that every failure path can be
 * exercised. Normal builds continue to call the C allocator directly. */
#if defined(PERSIMM_TEST_ALLOC)
void *persimm_test_calloc(size_t count, size_t size);
void persimm_test_free(void *ptr);
#define calloc persimm_test_calloc
#define free persimm_test_free
#endif

static inline bool persimm_size_add(size_t a, size_t b, size_t *result) {
    if (a > SIZE_MAX - b) return false;
    *result = a + b;
    return true;
}

static inline bool persimm_size_mul(size_t a, size_t b, size_t *result) {
    if (0 != a && b > SIZE_MAX / a) return false;
    *result = a * b;
    return true;
}

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

/*
 * C99 has no _Alignof, so the alignment is recovered from where a member lands
 * after a single char. A HAMT node holds entries and child pointers in one
 * allocation and rounds the boundary between them up to this.
 */
typedef struct {
    char pad;
    persimm_align_t value;
} persimm_align_probe_t;

#define PERSIMM_ALIGNMENT (offsetof(persimm_align_probe_t, value))
#define PERSIMM_ALIGN_UP(n) (((n) + PERSIMM_ALIGNMENT - 1) & ~(PERSIMM_ALIGNMENT - 1))

/* Bit Counting */

/*
 * A HAMT node turns a bitmap into an array index by counting the bits below
 * the one it is looking at, so this sits on the hot path of every lookup.
 */

#if defined(__GNUC__) || defined(__clang__)

#define PERSIMM_POPCOUNT(x) ((uint32_t)__builtin_popcount((unsigned int)(x)))

#else

static inline uint32_t persimm_popcount(uint32_t x) {
    x = x - ((x >> 1) & 0x55555555u);
    x = (x & 0x33333333u) + ((x >> 2) & 0x33333333u);
    x = (x + (x >> 4)) & 0x0f0f0f0fu;
    return (x * 0x01010101u) >> 24;
}

#define PERSIMM_POPCOUNT(x) persimm_popcount(x)

#endif

/* Elements */

static inline void persimm_elem_retain(const persimm_elem_ops *ops, void *ctx, void *slot) {
    if (NULL != ops && NULL != ops->retain) ops->retain(slot, ctx);
}

static inline void persimm_elem_release(const persimm_elem_ops *ops, void *ctx, void *slot) {
    if (NULL != ops && NULL != ops->release) ops->release(slot, ctx);
}

/* Hash Array Mapped Tries */

/*
 * The CHAMP trie behind both the map and the set, implemented in
 * persimmon_hamt.c. A set is a trie whose layout gives entries no value, so
 * neither public face needs any part of this beyond passing it along.
 *
 * Everything here works on a root pointer rather than a structure, which is
 * what lets persimm_map_t and persimm_set_t stay distinct types over one
 * implementation without either of them casting to the other.
 */
typedef struct {
    persimm_entry_layout layout;
    const persimm_elem_ops *ops;
    uint32_t (*hash)(const void *key, size_t key_size, void *ctx);
    bool (*equals)(const void *key_a, const void *key_b, size_t key_size, void *ctx);
    void *ctx;
} persimm_hamt_t;

/*
 * Resolves `key_ops` against the byte defaults once, so that no lookup has to
 * test for a NULL callback on its way down the trie.
 */
void persimm_hamt_config(persimm_hamt_t *hamt, const persimm_entry_layout *layout,
                         const persimm_elem_ops *ops, const persimm_key_ops *key_ops, void *ctx);

bool persimm_hamt_layout_valid(const persimm_entry_layout *layout);

void persimm_hamt_retain(persimm_hamt_node_t *root);

void persimm_hamt_release(persimm_hamt_node_t *root, const persimm_hamt_t *hamt);

/*
 * Returns the entry whose key matches, or NULL. The key is the first
 * `key_size` bytes of an entry, so the result is also a pointer to the stored
 * key, and the value sits `value_offset` bytes further along.
 */
void *persimm_hamt_ref(persimm_hamt_node_t *root, const void *key, const persimm_hamt_t *hamt);

/*
 * Stores `entry`, replacing only the value when the key is already present so
 * that the key first stored is the key the trie keeps. `*added` reports
 * whether the entry count grew. On failure `*root` is left as it was.
 */
persimm_status persimm_hamt_assoc(persimm_hamt_node_t **root, const void *entry,
                                  const persimm_hamt_t *hamt, bool immutable, bool *added);

persimm_status persimm_hamt_dissoc(persimm_hamt_node_t **root, const void *key,
                                   const persimm_hamt_t *hamt, bool immutable, bool *removed);

/*
 * Walks the trie once, calling `fn` with each entry. persimm_hamt_next follows
 * the same order, so a host may drive iteration either way and see the same
 * sequence.
 */
void persimm_hamt_foreach(persimm_hamt_node_t *root, const persimm_hamt_t *hamt,
                          persimm_visit_fn fn, void *ctx);

/*
 * Returns the entry after the one `key` belongs to, the first entry when `key`
 * is NULL, or NULL at the end. This descends by hash rather than resuming from
 * a cursor, so it costs the depth of the trie and needs nothing kept between
 * calls, which is what lets a shared trie be iterated from several places.
 */
void *persimm_hamt_next(persimm_hamt_node_t *root, const void *key, const persimm_hamt_t *hamt);

void persimm_hamt_trace(persimm_hamt_node_t *root, const persimm_hamt_t *hamt);

#endif /* end of include guard */
