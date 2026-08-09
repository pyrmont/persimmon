#ifndef PERSIMMON_H
#define PERSIMMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Persimmon is a host-agnostic implementation of persistent immutable data
 * structures. It knows nothing about the language binding to it: elements are
 * opaque byte blobs of a fixed size, stored inline in the collection, and their
 * lifecycle is delegated to a table of callbacks supplied at initialisation.
 *
 * Elements must not require an alignment stricter than max_align_t.
 */

/* Status Codes */

typedef enum {
    PERSIMM_OK = 0,
    PERSIMM_ERR_ALLOC,   // allocation failed
    PERSIMM_ERR_BOUNDS,  // index outside the vector
    PERSIMM_ERR_INVALID, // argument rejected
    PERSIMM_ERR_CORRUPT  // internal invariant violated
} persimm_status;

const char *persimm_status_string(persimm_status status);

/* Element and Value Lifecycles */

/*
 * Each callback receives a read-only pointer to the element's storage slot,
 * not the element itself. A host storing pointers reads through twice, e.g.
 * `Py_DECREF(*(PyObject *const *)slot)`. Any callback may be NULL, as may the
 * whole table, in which case elements are treated as plain data. Vectors and
 * lists use this table for their elements; maps use it for their values.
 *
 * `retain` is called whenever a new reference to an element comes into
 * existence: when one is inserted, and for every live element of a leaf that
 * gets copied for structural sharing. `release` is called symmetrically.
 * `trace` exists for hosts with a tracing collector and is only ever called
 * via the corresponding `*_trace` function.
 */
typedef struct {
    void (*retain)(const void *slot, void *ctx);
    void (*release)(const void *slot, void *ctx);
    void (*trace)(const void *slot, void *ctx);
} persimm_elem_ops;

typedef void (*persimm_visit_fn)(const void *slot, size_t index, void *ctx);

/* Entries */

/*
 * A map's elements are entries containing a key and a value. The host lays an
 * entry out and describes its exact representation here. The key occupies the
 * first `key_size` bytes, so a pointer to an entry is also a pointer to its key.
 * `value_size` is zero for a set, whose entries are keys and nothing more.
 */
typedef struct {
    size_t entry_size;   // the stride from one entry to the next
    size_t key_size;     // the key occupies the first key_size bytes
    size_t value_offset; // where the value begins, ignored when value_size is 0
    size_t value_size;
} persimm_entry_layout;

/*
 * Map and set keys are hashed, compared and managed through this table. Any
 * callback may be NULL, as may the whole table. Missing hash and equality
 * callbacks use FNV-1a over the key bytes and memcmp respectively; missing
 * lifecycle callbacks do nothing.
 *
 * The two must agree: keys that compare equal have to hash equally, or a
 * lookup will miss an entry the map holds. Note that the byte defaults do not
 * agree for a key type with padding bytes or several representations of one
 * value, which is why a host with such keys must supply both.
 *
 * `retain`, `release` and `trace` have the same slot and lifecycle semantics
 * as their `persimm_elem_ops` counterparts. For a map they apply only to its
 * keys; values use the separate element table.
 */
typedef struct {
    uint32_t (*hash)(const void *key, size_t key_size, void *ctx);
    bool (*equals)(const void *key_a, const void *key_b, size_t key_size, void *ctx);
    void (*retain)(const void *slot, void *ctx);
    void (*release)(const void *slot, void *ctx);
    void (*trace)(const void *slot, void *ctx);
} persimm_key_ops;

/*
 * Operation tables and contexts are borrowed rather than copied. They must
 * outlive the collection and every clone or transient derived from it.
 * Deinitialisation releases a collection's current storage and may safely be
 * repeated.
 */

/* Types */

/*
 * In each structure below, fields are readable by the host (`count` in
 * particular) but must only be written through the functions that follow.
 */

/*
 * A persistent indexed sequence. Indexing and appending are effectively
 * constant time.
 */
typedef struct {
    size_t shift;
    size_t count;
    size_t tail_count;
    size_t elem_size;
    const persimm_elem_ops *ops;
    void *ctx;
    struct persimm_vector_node *root;
    struct persimm_vector_node *tail;
} persimm_vector_t;

/*
 * A persistent sequence optimised for access and updates at the front.
 * Consing and taking the rest are constant time; indexing is linear.
 */
typedef struct {
    size_t count;
    /* Distinguishes structural versions for host-owned cursors. */
    size_t generation;
    size_t elem_size;
    const persimm_elem_ops *ops;
    void *ctx;
    struct persimm_list_cell *head;
} persimm_list_t;

/*
 * A resumable position in a list, owned by the host. persimm_list_ref_from
 * walks forward from one rather than from the head, which is what turns a
 * front-to-back traversal from quadratic into linear.
 *
 * The core deliberately keeps no cursor inside persimm_list_t. A read would
 * then write to a structure another thread may be reading, which is exactly
 * what the atomic reference counts exist to avoid. A host that shares a list
 * across threads gives each thread its own cursor.
 *
 * The fields are reserved for library bookkeeping and must not be modified. A
 * cursor is safe to point at any list, or at a list that has since changed: it
 * notices the structural generation and starts again from the head.
 */
typedef struct {
    const persimm_list_t *list;
    size_t generation;
    size_t index;
    struct persimm_list_cell *cell;
} persimm_list_cursor_t;

/*
 * A persistent hash map. Looking a key up, storing one and dropping one are
 * all effectively constant time. Two maps holding the same entries iterate in
 * the same order except where distinct keys have identical hashes.
 */
typedef struct {
    size_t count;
    persimm_entry_layout layout;
    const persimm_elem_ops *value_ops;
    const persimm_key_ops *key_ops;
    void *value_ctx;
    void *key_ctx;
    struct persimm_hamt_node *root;
} persimm_map_t;

/*
 * A persistent hash set, equivalent in behaviour to a map with keys and no
 * values.
 */
typedef struct {
    size_t count;
    persimm_entry_layout layout;
    const persimm_key_ops *key_ops;
    void *key_ctx;
    struct persimm_hamt_node *root;
} persimm_set_t;

/*
 * Temporary, mutable views of persistent structures. A transient starts by
 * sharing its source, copies a shared path the first time it is touched, and
 * may then reuse the path for later edits. Persisting consumes it: every later
 * edit or second attempt to persist returns PERSIMM_ERR_INVALID.
 *
 * The fields are reserved for library bookkeeping and must not be modified.
 * Transients are uniquely owned and must not be shared between threads.
 */
typedef struct {
    persimm_vector_t value;
    bool active;
} persimm_vector_transient_t;

typedef struct {
    persimm_map_t value;
    bool active;
} persimm_map_transient_t;

typedef struct {
    persimm_set_t value;
    bool active;
} persimm_set_transient_t;

/* Reference Counting */

/*
 * Internal storage is shared between structures and reference counted. The
 * counts are atomic where the toolchain provides atomics. This returns whether
 * that was the case for this build: when it is false, a graph of structures
 * must not be shared across threads.
 */
bool persimm_has_atomic_refcounts(void);

/* Persistent Updates */

/*
 * Every persistent update takes a const source and writes its result to a
 * separate, uninitialised destination. The source remains unchanged. On
 * success, source and destination each own a reference and must eventually be
 * deinitialised. On failure, the destination is still safe to deinitialise.
 * Passing the source itself as the destination returns PERSIMM_ERR_INVALID.
 */

/* Transients */

/*
 * A `*_to_transient` function starts from a persistent source without changing
 * it. The destination must be uninitialised, and its embedded collection must
 * be distinct from the source; an alias is rejected without changing either
 * argument. A `*_transient_init` function instead starts an empty transient
 * and leaves it safe to deinitialise if initialisation fails. Mutations keep
 * the transient active even when they return an error.
 *
 * Persisting writes to an uninitialised destination and consumes the
 * transient. The destination must not be the transient's embedded collection;
 * that alias is rejected without consuming the transient. Deinitialising a
 * consumed transient is safe.
 */
persimm_status persimm_vector_to_transient(const persimm_vector_t *src,
                                           persimm_vector_transient_t *transient);
persimm_status persimm_vector_transient_init(persimm_vector_transient_t *transient,
                                             size_t elem_size,
                                             const persimm_elem_ops *ops, void *ctx);
void persimm_vector_transient_deinit(persimm_vector_transient_t *transient);
persimm_status persimm_vector_transient_push(persimm_vector_transient_t *transient,
                                             const void *elem);
persimm_status persimm_vector_transient_update(persimm_vector_transient_t *transient,
                                               size_t index, const void *elem);
persimm_status persimm_vector_transient_persist(persimm_vector_transient_t *transient,
                                                persimm_vector_t *dest);

persimm_status persimm_map_to_transient(const persimm_map_t *src,
                                        persimm_map_transient_t *transient);
persimm_status persimm_map_transient_init(persimm_map_transient_t *transient,
                                          const persimm_entry_layout *layout,
                                          const persimm_elem_ops *value_ops, void *value_ctx,
                                          const persimm_key_ops *key_ops, void *key_ctx);
void persimm_map_transient_deinit(persimm_map_transient_t *transient);
persimm_status persimm_map_transient_assoc(persimm_map_transient_t *transient,
                                           const void *entry);
persimm_status persimm_map_transient_dissoc(persimm_map_transient_t *transient,
                                            const void *key);
persimm_status persimm_map_transient_persist(persimm_map_transient_t *transient,
                                             persimm_map_t *dest);

persimm_status persimm_set_to_transient(const persimm_set_t *src,
                                        persimm_set_transient_t *transient);
persimm_status persimm_set_transient_init(persimm_set_transient_t *transient,
                                          size_t elem_size, const persimm_key_ops *key_ops,
                                          void *key_ctx);
void persimm_set_transient_deinit(persimm_set_transient_t *transient);
persimm_status persimm_set_transient_conj(persimm_set_transient_t *transient,
                                          const void *elem);
persimm_status persimm_set_transient_disj(persimm_set_transient_t *transient,
                                          const void *elem);
persimm_status persimm_set_transient_persist(persimm_set_transient_t *transient,
                                             persimm_set_t *dest);

/* Vectors */

/*
 * Prepares a vector for use. `elem_size` must be non-zero. `ops` and `ctx` may
 * be NULL. The vector is left safe to deinitialise even if this fails.
 */
persimm_status persimm_vector_init(persimm_vector_t *vector, size_t elem_size,
                                   const persimm_elem_ops *ops, void *ctx);

/*
 * Points `dest` at the same storage as `src`, sharing its structure. `dest`
 * must be uninitialised and distinct from `src`. An alias is rejected without
 * changing either argument.
 */
persimm_status persimm_vector_clone(const persimm_vector_t *src,
                                    persimm_vector_t *dest);

void persimm_vector_deinit(persimm_vector_t *vector);

/*
 * Returns a read-only pointer to the storage slot for `index`, or NULL if the
 * index is out of bounds. The pointer remains valid until `vector` is
 * deinitialised.
 */
const void *persimm_vector_ref(const persimm_vector_t *vector, size_t index);

/*
 * Resolves a possibly negative index against the vector, as hosts that count
 * backwards from the end require. Returns false if the result is out of bounds.
 */
bool persimm_vector_index(const persimm_vector_t *vector, int64_t input, size_t *index);

/*
 * Appends `elem` to `src`, placing the resulting persistent vector in `dest`.
 * `src` is unchanged and `dest` follows the persistent update contract above.
 * The result shares every node the new path does not need to copy.
 *
 */
persimm_status persimm_vector_push(const persimm_vector_t *src, const void *elem,
                                   persimm_vector_t *dest);

/*
 * Replaces the element at `index`, leaving `src` unchanged and placing the
 * resulting persistent vector in `dest`.
 */
persimm_status persimm_vector_update(const persimm_vector_t *src, size_t index,
                                     const void *elem, persimm_vector_t *dest);

/*
 * Visits each element once, in index order.
 */
void persimm_vector_foreach(const persimm_vector_t *vector, persimm_visit_fn fn, void *ctx);

/*
 * Calls the element table's `trace` callback for every element.
 */
void persimm_vector_trace(const persimm_vector_t *vector);

/* Lists */

/*
 * Prepares an empty list for use. `elem_size` must be non-zero. `ops` and `ctx`
 * may be NULL. Unlike a vector, a list allocates nothing until something is
 * consed onto it.
 */
persimm_status persimm_list_init(persimm_list_t *list, size_t elem_size,
                                 const persimm_elem_ops *ops, void *ctx);

/*
 * Points `dest` at the same storage as `src`. `dest` must be uninitialised and
 * distinct from `src`. An alias is rejected without changing either argument.
 */
persimm_status persimm_list_clone(const persimm_list_t *src,
                                  persimm_list_t *dest);

void persimm_list_deinit(persimm_list_t *list);

/*
 * Returns a read-only pointer to the head element's storage slot, or NULL if
 * the list is empty. The pointer remains valid until `list` is deinitialised.
 */
const void *persimm_list_first(const persimm_list_t *list);

/*
 * Returns a read-only pointer to the storage slot for `index`, or NULL if the
 * index is out of bounds. The pointer remains valid until `list` is
 * deinitialised. This walks from the head, so it is linear in `index`.
 */
const void *persimm_list_ref(const persimm_list_t *list, size_t index);

/*
 * Puts a cursor back into its initial state, from which it walks from the
 * head. A cursor must be reset before its first use: it is otherwise whatever
 * its storage happened to hold.
 */
void persimm_list_cursor_reset(persimm_list_cursor_t *cursor);

/*
 * As persimm_list_ref, but resumes from `cursor` when it belongs to this list
 * and sits at or before `index`, and leaves it pointing at what was returned.
 * Walking a list from front to back this way costs one step per element.
 * Indices that go backwards are answered from the head, and so cost no more
 * than persimm_list_ref would.
 */
const void *persimm_list_ref_from(const persimm_list_t *list, persimm_list_cursor_t *cursor,
                                  size_t index);

bool persimm_list_index(const persimm_list_t *list, int64_t input, size_t *index);

/*
 * Prepends `elem` to `src`, leaving it unchanged and placing the resulting
 * persistent list in `dest`.
 */
persimm_status persimm_list_cons(const persimm_list_t *src, const void *elem,
                                 persimm_list_t *dest);

/*
 * Drops the head of `src`, leaving it unchanged and placing the result in
 * `dest`. Returns PERSIMM_ERR_BOUNDS if `src` is already empty.
 */
persimm_status persimm_list_rest(const persimm_list_t *src, persimm_list_t *dest);

/*
 * Walks the chain once, from head to tail, calling `fn` with each element's
 * slot.
 */
void persimm_list_foreach(const persimm_list_t *list, persimm_visit_fn fn, void *ctx);

/*
 * Calls the element table's `trace` callback for every element.
 */
void persimm_list_trace(const persimm_list_t *list);

/* Maps */

/*
 * Prepares an empty map for use. `layout` is copied and must describe an entry
 * whose key starts at offset zero and whose value lies within it. `value_ops`
 * manages values, while `key_ops` hashes, compares and manages keys. Either
 * table and either context may be NULL. Nothing is allocated until the first
 * entry is stored.
 */
persimm_status persimm_map_init(persimm_map_t *map, const persimm_entry_layout *layout,
                                const persimm_elem_ops *value_ops, void *value_ctx,
                                const persimm_key_ops *key_ops, void *key_ctx);

/*
 * Points `dest` at the same storage as `src`, sharing its structure. `dest`
 * must be uninitialised and distinct from `src`. An alias is rejected without
 * changing either argument.
 */
persimm_status persimm_map_clone(const persimm_map_t *src, persimm_map_t *dest);

void persimm_map_deinit(persimm_map_t *map);

/*
 * Returns a read-only pointer to the storage for `key`'s value, or NULL when
 * the map does not hold the key. The pointer remains valid until `map` is
 * deinitialised.
 *
 * A map whose entries have no value has nothing to return, so ask
 * persimm_map_has instead.
 */
const void *persimm_map_ref(const persimm_map_t *map, const void *key);

/*
 * Returns the whole entry rather than its value, which is what a host wants
 * when it needs the key the map actually holds rather than the one it looked
 * up with. Storing a key that already has an equal counterpart leaves the
 * earlier one in place.
 */
const void *persimm_map_ref_entry(const persimm_map_t *map, const void *key);

bool persimm_map_has(const persimm_map_t *map, const void *key);

/*
 * Stores `entry` in a persistent copy of `src`. Where the key is already
 * present only the value is replaced, and the key already stored stays.
 */
persimm_status persimm_map_assoc(const persimm_map_t *src, const void *entry,
                                 persimm_map_t *dest);

/*
 * Drops `key` and its value, releasing both. A key the map does not hold is
 * not an error and leaves the map alone.
 */
persimm_status persimm_map_dissoc(const persimm_map_t *src, const void *key,
                                  persimm_map_t *dest);

/*
 * Visits each entry once. The order is not the order entries were stored in,
 * but it is the order persimm_map_next follows, and two maps holding the same
 * keys agree on it however they were built.
 *
 * Keys whose hashes are equal in all 32 bits are the exception: they sit in
 * the order they arrived, and no order exists to sort opaque keys into. A host
 * deriving a hash from a whole map should combine its entries commutatively so
 * that equal maps still agree.
 */
void persimm_map_foreach(const persimm_map_t *map, persimm_visit_fn fn, void *ctx);

/*
 * Returns the entry following `key`'s, the first entry when `key` is NULL, or
 * NULL at the end. Nothing is kept between calls, so a map shared in several
 * places can be walked from each of them at once.
 */
const void *persimm_map_next(const persimm_map_t *map, const void *key);

/*
 * Calls the key table's `trace` callback for every key and the value table's
 * callback for every value.
 */
void persimm_map_trace(const persimm_map_t *map);

/* Sets */

/*
 * Prepares an empty set for use. `elem_size` must be non-zero; `key_ops` and
 * `key_ctx` may be NULL.
 */
persimm_status persimm_set_init(persimm_set_t *set, size_t elem_size,
                                const persimm_key_ops *key_ops, void *key_ctx);

/*
 * Points `dest` at the same storage as `src`. `dest` must be uninitialised and
 * distinct from `src`. An alias is rejected without changing either argument.
 */
persimm_status persimm_set_clone(const persimm_set_t *src, persimm_set_t *dest);

void persimm_set_deinit(persimm_set_t *set);

/*
 * Returns a read-only pointer to the element the set holds, or NULL. This is
 * the element stored rather than the one looked up with, which is what a host
 * interning values through a set is after. The pointer remains valid until
 * `set` is deinitialised.
 */
const void *persimm_set_ref(const persimm_set_t *set, const void *elem);

bool persimm_set_has(const persimm_set_t *set, const void *elem);

/*
 * Adds `elem`, retaining it. An element the set already holds leaves it
 * untouched, so the first of a run of equal elements is the one kept.
 */
persimm_status persimm_set_conj(const persimm_set_t *src, const void *elem,
                                persimm_set_t *dest);

persimm_status persimm_set_disj(const persimm_set_t *src, const void *elem,
                                persimm_set_t *dest);

void persimm_set_foreach(const persimm_set_t *set, persimm_visit_fn fn, void *ctx);

const void *persimm_set_next(const persimm_set_t *set, const void *elem);

void persimm_set_trace(const persimm_set_t *set);

#endif /* end of include guard */
