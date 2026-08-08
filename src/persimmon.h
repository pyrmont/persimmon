#ifndef PERSIMMON_H
#define PERSIMMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Persimmon is a host-agnostic implementation of persistent immutable data
 * structures. It knows nothing about the language binding to it: elements are
 * opaque byte blobs of a fixed size, stored inline in the trie, and their
 * lifecycle is delegated to a table of callbacks supplied at initialisation.
 *
 * Elements must not require an alignment stricter than max_align_t.
 */

/* Configuration */

#define PERSIMM_BITS 5
#define PERSIMM_WIDTH (1 << PERSIMM_BITS) // 2^5 = 32
#define PERSIMM_MASK (PERSIMM_WIDTH - 1) // 31, or 0x1f

/* Status Codes */

typedef enum {
    PERSIMM_OK = 0,
    PERSIMM_ERR_ALLOC,   // allocation failed
    PERSIMM_ERR_BOUNDS,  // index outside the vector
    PERSIMM_ERR_INVALID, // argument rejected
    PERSIMM_ERR_CORRUPT  // internal invariant violated
} persimm_status;

const char *persimm_status_string(persimm_status status);

/* Elements */

/*
 * Each callback receives a pointer to the element's storage slot, not the
 * element itself. A host storing pointers reads through twice, e.g.
 * `Py_DECREF(*(PyObject **)slot)`. Any callback may be NULL, as may the whole
 * table, in which case elements are treated as plain data.
 *
 * `retain` is called whenever a new reference to an element comes into
 * existence: when one is inserted, and for every live element of a leaf that
 * gets copied for structural sharing. `release` is called symmetrically.
 * `trace` exists for hosts with a tracing collector and is only ever called
 * via persimm_vector_trace.
 */
typedef struct {
    void (*retain)(void *slot, void *ctx);
    void (*release)(void *slot, void *ctx);
    void (*trace)(void *slot, void *ctx);
} persimm_elem_ops;

typedef void (*persimm_visit_fn)(void *slot, size_t index, void *ctx);

/* Types */

typedef struct persimm_node persimm_node_t;

/*
 * Fields are readable by the host (`count` in particular) but must only be
 * written through the functions below.
 */
typedef struct {
    size_t shift;
    size_t count;
    size_t tail_count;
    size_t elem_size;
    const persimm_elem_ops *ops;
    void *ctx;
    persimm_node_t *root;
    persimm_node_t *tail;
} persimm_vector_t;

/* Reference Counting */

/*
 * Nodes are shared between vectors and their reference counts are atomic where
 * the toolchain provides atomics. This returns whether that was the case for
 * this build: when it is false, a graph of vectors must not be shared across
 * threads.
 */
bool persimm_has_atomic_refcounts(void);

/* Initialising */

/*
 * Prepares a vector for use. `elem_size` must be non-zero. `ops` and `ctx` may
 * be NULL. The vector is left safe to deinitialise even if this fails.
 */
persimm_status persimm_vector_init(persimm_vector_t *vector, size_t elem_size,
                                   const persimm_elem_ops *ops, void *ctx);

/*
 * Points `dest` at the same nodes as `src`, sharing their structure. `dest`
 * need not be initialised and must not be already, or its nodes will leak.
 */
void persimm_vector_clone(const persimm_vector_t *src, persimm_vector_t *dest);

/* Deinitialising */

void persimm_vector_deinit(persimm_vector_t *vector);

/* Accessing */

/*
 * Returns a pointer to the storage slot for `index`, or NULL if the index is
 * out of bounds. The pointer is invalidated by any subsequent operation on any
 * vector sharing that node, so copy the value out rather than holding on to it.
 */
void *persimm_vector_ref(const persimm_vector_t *vector, size_t index);

/*
 * Resolves a possibly negative index against the vector, as hosts that count
 * backwards from the end require. Returns false if the result is out of bounds.
 */
bool persimm_vector_index(const persimm_vector_t *vector, int64_t input, size_t *index);

/* Inserting */

/*
 * Appends `elem` to the vector, copying `elem_size` bytes out of it and
 * retaining the result. When `immutable` is true, any node on the path that is
 * shared with another vector is copied first; when false, the vector is
 * modified in place, which is only safe while no other vector shares it.
 *
 * On failure the vector remains valid and safe to deinitialise, but whether the
 * element was appended is unspecified beyond the return value.
 */
persimm_status persimm_vector_push(persimm_vector_t *vector, const void *elem, bool immutable);

/*
 * Replaces the element at `index`, releasing the one it displaces. `immutable`
 * carries the same meaning as for persimm_vector_push.
 */
persimm_status persimm_vector_update(persimm_vector_t *vector, size_t index, const void *elem,
                                     bool immutable);

/* Traversing */

/*
 * Walks the trie once, in index order, calling `fn` with each element's slot.
 */
void persimm_vector_foreach(const persimm_vector_t *vector, persimm_visit_fn fn, void *ctx);

/*
 * Calls the element table's `trace` callback for every element.
 */
void persimm_vector_trace(const persimm_vector_t *vector);

#endif /* end of include guard */
