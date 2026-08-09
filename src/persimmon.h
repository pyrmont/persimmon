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

/* Entries */

/*
 * A map's elements are entries: a key followed by a value. The host lays the
 * entry out and describes it here rather than letting the core compute it,
 * because a core that placed the value itself would have to pad the key to
 * max_align_t. On a target where that is 16 bytes and a key is 8, every entry
 * would then cost twice what the host's own struct costs.
 *
 * The key occupies the first `key_size` bytes, so a pointer to an entry is
 * also a pointer to its key. `value_size` is zero for a set, whose entries are
 * keys and nothing more.
 */
typedef struct {
    size_t entry_size;   // the stride from one entry to the next
    size_t key_size;     // the key occupies the first key_size bytes
    size_t value_offset; // where the value begins, ignored when value_size is 0
    size_t value_size;
} persimm_entry_layout;

/*
 * Keys are hashed and compared through this table. Either callback may be
 * NULL, as may the whole table, in which case keys are hashed as bytes with
 * FNV-1a and compared with memcmp.
 *
 * The two must agree: keys that compare equal have to hash equally, or a
 * lookup will miss an entry the map holds. Note that the byte defaults do not
 * agree for a key type with padding bytes or several representations of one
 * value, which is why a host with such keys must supply both.
 */
typedef struct {
    uint32_t (*hash)(const void *key, size_t key_size, void *ctx);
    bool (*equals)(const void *key_a, const void *key_b, size_t key_size, void *ctx);
} persimm_key_ops;

/* Types */

typedef struct persimm_vector_node persimm_vector_node_t;
typedef struct persimm_list_cell persimm_list_cell_t;
typedef struct persimm_hamt_node persimm_hamt_node_t;

/*
 * In each structure below, fields are readable by the host (`count` in
 * particular) but must only be written through the functions that follow.
 */

/*
 * A bit-partitioned trie of PERSIMM_WIDTH-way nodes with a tail buffer.
 * Indexing and appending are effectively constant time.
 */
typedef struct {
    size_t shift;
    size_t count;
    size_t tail_count;
    size_t elem_size;
    const persimm_elem_ops *ops;
    void *ctx;
    persimm_vector_node_t *root;
    persimm_vector_node_t *tail;
} persimm_vector_t;

/*
 * A singly linked chain of cells. Consing and taking the rest are constant
 * time whatever the length; indexing is linear.
 */
typedef struct {
    size_t count;
    size_t elem_size;
    const persimm_elem_ops *ops;
    void *ctx;
    persimm_list_cell_t *head;
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
 * Treat the fields as opaque. A cursor is safe to point at any list, or at a
 * list that has since changed: it notices and starts again from the head.
 */
typedef struct {
    const persimm_list_t *list;
    size_t count;
    size_t index;
    persimm_list_cell_t *cell;
} persimm_list_cursor_t;

/*
 * A CHAMP trie, which is a hash array mapped trie whose nodes carry one bitmap
 * for the slots holding an entry and another for the slots holding a child.
 * Looking a key up, storing one and dropping one are all effectively constant
 * time.
 *
 * A trie is kept in canonical form, so two maps holding the same entries have
 * the same shape however they were built, and therefore iterate in the same
 * order and hash alike.
 */
typedef struct {
    size_t count;
    persimm_entry_layout layout;
    const persimm_elem_ops *ops;
    const persimm_key_ops *key_ops;
    void *ctx;
    persimm_hamt_node_t *root;
} persimm_map_t;

/*
 * The same trie over entries that are keys and nothing else. A set is to a map
 * what a map with no values would be, and shares its whole implementation.
 */
typedef struct {
    size_t count;
    persimm_entry_layout layout;
    const persimm_elem_ops *ops;
    const persimm_key_ops *key_ops;
    void *ctx;
    persimm_hamt_node_t *root;
} persimm_set_t;

/* Reference Counting */

/*
 * Nodes and cells are shared between structures and their reference counts are
 * atomic where the toolchain provides atomics. This returns whether that was
 * the case for this build: when it is false, a graph of structures must not be
 * shared across threads.
 */
bool persimm_has_atomic_refcounts(void);

/* Vectors */

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

void persimm_vector_deinit(persimm_vector_t *vector);

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

/*
 * Walks the trie once, in index order, calling `fn` with each element's slot.
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
 * Points `dest` at the same chain as `src`. `dest` need not be initialised and
 * must not be already, or its cells will leak.
 */
void persimm_list_clone(const persimm_list_t *src, persimm_list_t *dest);

void persimm_list_deinit(persimm_list_t *list);

/*
 * Returns a pointer to the head element's storage slot, or NULL if the list is
 * empty. As with a vector, the pointer does not survive later operations.
 */
void *persimm_list_first(const persimm_list_t *list);

/*
 * Returns a pointer to the storage slot for `index`, or NULL if the index is
 * out of bounds. This walks the chain from the head, so it is linear in
 * `index`.
 */
void *persimm_list_ref(const persimm_list_t *list, size_t index);

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
void *persimm_list_ref_from(const persimm_list_t *list, persimm_list_cursor_t *cursor,
                            size_t index);

bool persimm_list_index(const persimm_list_t *list, int64_t input, size_t *index);

/*
 * Prepends `elem` to the list, copying `elem_size` bytes out of it and
 * retaining the result. There is no mutable variant: a cons never writes to an
 * existing cell, so it is safe whether or not the chain is shared.
 */
persimm_status persimm_list_cons(persimm_list_t *list, const void *elem);

/*
 * Drops the head of the list, releasing it. Returns PERSIMM_ERR_BOUNDS if the
 * list is already empty. The cells behind the new head are untouched and stay
 * shared with any list still holding the old head.
 */
persimm_status persimm_list_rest(persimm_list_t *list);

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
 * whose key starts at offset zero and whose value lies within it; `ops`,
 * `key_ops` and `ctx` may be NULL. Nothing is allocated until the first entry
 * is stored.
 */
persimm_status persimm_map_init(persimm_map_t *map, const persimm_entry_layout *layout,
                                const persimm_elem_ops *ops, const persimm_key_ops *key_ops,
                                void *ctx);

/*
 * Points `dest` at the same trie as `src`, sharing its nodes. `dest` need not
 * be initialised and must not be already, or its nodes will leak.
 */
void persimm_map_clone(const persimm_map_t *src, persimm_map_t *dest);

void persimm_map_deinit(persimm_map_t *map);

/*
 * Returns a pointer to the storage for `key`'s value, or NULL when the map
 * does not hold the key. As with a vector, the pointer does not survive a
 * later operation on any map sharing that node.
 *
 * A map whose entries have no value has nothing to return, so ask
 * persimm_map_has instead.
 */
void *persimm_map_ref(const persimm_map_t *map, const void *key);

/*
 * Returns the whole entry rather than its value, which is what a host wants
 * when it needs the key the map actually holds rather than the one it looked
 * up with. Storing a key that already has an equal counterpart leaves the
 * earlier one in place.
 */
void *persimm_map_ref_entry(const persimm_map_t *map, const void *key);

bool persimm_map_has(const persimm_map_t *map, const void *key);

/*
 * Stores `entry`, which the host lays out as its `layout` describes. Where the
 * key is already present only the value is replaced, and the key already
 * stored stays. `immutable` carries the same meaning as for a vector: when it
 * is false a node no other map shares may be written in place.
 *
 * On failure the map is unchanged and remains safe to deinitialise.
 */
persimm_status persimm_map_assoc(persimm_map_t *map, const void *entry, bool immutable);

/*
 * Drops `key` and its value, releasing both. A key the map does not hold is
 * not an error and leaves the map alone.
 */
persimm_status persimm_map_dissoc(persimm_map_t *map, const void *key, bool immutable);

/*
 * Walks the trie once, calling `fn` with each entry. The order is not the
 * order entries were stored in, but it is the order persimm_map_next follows,
 * and two maps holding the same keys agree on it however they were built.
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
void *persimm_map_next(const persimm_map_t *map, const void *key);

/*
 * Calls the element table's `trace` callback for every key and every value.
 */
void persimm_map_trace(const persimm_map_t *map);

/* Sets */

/*
 * Prepares an empty set for use. `elem_size` must be non-zero; `ops`,
 * `key_ops` and `ctx` may be NULL.
 */
persimm_status persimm_set_init(persimm_set_t *set, size_t elem_size, const persimm_elem_ops *ops,
                                const persimm_key_ops *key_ops, void *ctx);

void persimm_set_clone(const persimm_set_t *src, persimm_set_t *dest);

void persimm_set_deinit(persimm_set_t *set);

/*
 * Returns a pointer to the element the set holds, or NULL. This is the element
 * stored rather than the one looked up with, which is what a host interning
 * values through a set is after.
 */
void *persimm_set_ref(const persimm_set_t *set, const void *elem);

bool persimm_set_has(const persimm_set_t *set, const void *elem);

/*
 * Adds `elem`, retaining it. An element the set already holds leaves it
 * untouched, so the first of a run of equal elements is the one kept.
 */
persimm_status persimm_set_conj(persimm_set_t *set, const void *elem, bool immutable);

persimm_status persimm_set_disj(persimm_set_t *set, const void *elem, bool immutable);

void persimm_set_foreach(const persimm_set_t *set, persimm_visit_fn fn, void *ctx);

void *persimm_set_next(const persimm_set_t *set, const void *elem);

void persimm_set_trace(const persimm_set_t *set);

#endif /* end of include guard */
