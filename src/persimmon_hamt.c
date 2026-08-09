#include <stdlib.h>
#include <string.h>
#include "persimmon_internal.h"

/*
 * A CHAMP trie: a hash array mapped trie in the form Steindorfer and Vinju
 * described, shared by the map and the set.
 *
 * Five bits of a key's hash pick a slot at each level. A node carries two
 * bitmaps rather than the one Clojure's map uses: `datamap` marks the slots
 * holding an entry and `nodemap` the slots holding a child. Clojure tells the
 * two apart by storing a null in the key half of a pair, which is not open to
 * a core whose keys are opaque bytes and so have no value to reserve.
 *
 * The second bitmap also buys a canonical shape. A subtree is never a child
 * when it could be a single inline entry, which `dissoc` maintains by pulling
 * a collapsed child back into its parent. Two tries holding the same keys
 * therefore have the same shape however they were built, so a map that reached
 * its contents by adding and removing entries iterates in the same order as
 * one that was handed them.
 *
 * Keys whose hashes are equal in all 32 bits cannot be separated by any depth,
 * so they share a collision node instead: a flat run of entries carrying the
 * hash they have in common. A collision node is the one exception to the
 * paragraph above. Its entries sit in the order they arrived, and since keys
 * are opaque there is no order to sort them into, so two tries can hold one
 * group of fully colliding keys in different orders. Anything a host derives
 * from a whole map, a hash above all, must therefore not depend on the order
 * its entries come out in.
 */

/* Types */

typedef enum {
    PERSIMM_HAMT_BITMAP,
    PERSIMM_HAMT_COLLISION
} persimm_hamt_node_type;

struct persimm_hamt_node {
    persimm_refcount_t ref_count;
    uint32_t kind;
    uint32_t datamap; // bitmap: the slots holding an entry. collision: how many entries
    uint32_t nodemap; // bitmap: the slots holding a child. collision: zero
    uint32_t hash;    // collision: the hash every entry shares. bitmap: unused
    persimm_align_t data[];
};

/*
 * A node holds its entries and then its children in one allocation, with the
 * boundary rounded up so that both regions land on an address either can use.
 */

static uint32_t persimm_hamt_data_count(persimm_hamt_node_t *node) {
    return (PERSIMM_HAMT_COLLISION == node->kind) ? node->datamap
                                                  : PERSIMM_POPCOUNT(node->datamap);
}

static uint32_t persimm_hamt_child_count(persimm_hamt_node_t *node) {
    return PERSIMM_POPCOUNT(node->nodemap);
}

static void *persimm_hamt_entry(persimm_hamt_node_t *node, uint32_t index, size_t entry_size) {
    return (unsigned char *)node->data + ((size_t)index * entry_size);
}

static persimm_hamt_node_t **persimm_hamt_children(persimm_hamt_node_t *node, size_t entry_size) {
    size_t bytes = (size_t)persimm_hamt_data_count(node) * entry_size;
    return (persimm_hamt_node_t **)((unsigned char *)node->data + PERSIMM_ALIGN_UP(bytes));
}

/* Bitmaps */

static uint32_t persimm_hamt_bit(uint32_t hash, size_t shift) {
    return (uint32_t)1 << ((hash >> shift) & PERSIMM_MASK);
}

static uint32_t persimm_hamt_data_index(persimm_hamt_node_t *node, uint32_t bit) {
    return PERSIMM_POPCOUNT(node->datamap & (bit - 1));
}

static uint32_t persimm_hamt_child_index(persimm_hamt_node_t *node, uint32_t bit) {
    return PERSIMM_POPCOUNT(node->nodemap & (bit - 1));
}

/*
 * Canonical form: only the root may hold a lone entry and nothing else. When a
 * removal leaves any other node in that state its parent inlines the entry.
 */
static bool persimm_hamt_is_single(persimm_hamt_node_t *node) {
    return 1 == persimm_hamt_data_count(node) && 0 == persimm_hamt_child_count(node);
}

/* Keys */

static uint32_t persimm_hamt_byte_hash(const void *key, size_t key_size, void *ctx) {
    (void) ctx;
    const unsigned char *bytes = (const unsigned char *)key;
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < key_size; i++) {
        hash ^= bytes[i];
        hash *= 16777619u;
    }
    return hash;
}

static bool persimm_hamt_byte_equals(const void *key_a, const void *key_b, size_t key_size,
                                     void *ctx) {
    (void) ctx;
    return 0 == memcmp(key_a, key_b, key_size);
}

void persimm_hamt_config(persimm_hamt_t *hamt, const persimm_entry_layout *layout,
                         const persimm_elem_ops *ops, const persimm_key_ops *key_ops, void *ctx) {
    hamt->layout = *layout;
    hamt->ops = ops;
    hamt->hash = (NULL != key_ops && NULL != key_ops->hash) ? key_ops->hash
                                                            : persimm_hamt_byte_hash;
    hamt->equals = (NULL != key_ops && NULL != key_ops->equals) ? key_ops->equals
                                                                : persimm_hamt_byte_equals;
    hamt->ctx = ctx;
}

bool persimm_hamt_layout_valid(const persimm_entry_layout *layout) {
    if (0 == layout->key_size || 0 == layout->entry_size) return false;
    if (layout->key_size > layout->entry_size) return false;
    if (0 == layout->value_size) return true;
    if (layout->value_offset < layout->key_size) return false;
    if (layout->value_offset > layout->entry_size) return false;
    return layout->value_size <= layout->entry_size - layout->value_offset;
}

static uint32_t persimm_hamt_hash_of(const persimm_hamt_t *hamt, const void *key) {
    return hamt->hash(key, hamt->layout.key_size, hamt->ctx);
}

static bool persimm_hamt_keys_equal(const persimm_hamt_t *hamt, const void *key_a,
                                    const void *key_b) {
    return hamt->equals(key_a, key_b, hamt->layout.key_size, hamt->ctx);
}

/* Entries */

/*
 * An entry's key and value are separate elements as far as the host is
 * concerned, so each gets its own call. A set's entries have no value.
 */

static void *persimm_hamt_value(const persimm_hamt_t *hamt, void *entry) {
    return (unsigned char *)entry + hamt->layout.value_offset;
}

static void persimm_hamt_entry_retain(const persimm_hamt_t *hamt, void *entry) {
    persimm_elem_retain(hamt->ops, hamt->ctx, entry);
    if (hamt->layout.value_size > 0) {
        persimm_elem_retain(hamt->ops, hamt->ctx, persimm_hamt_value(hamt, entry));
    }
}

static void persimm_hamt_entry_release(const persimm_hamt_t *hamt, void *entry) {
    persimm_elem_release(hamt->ops, hamt->ctx, entry);
    if (hamt->layout.value_size > 0) {
        persimm_elem_release(hamt->ops, hamt->ctx, persimm_hamt_value(hamt, entry));
    }
}

/* Deinitialising */

void persimm_hamt_retain(persimm_hamt_node_t *root) {
    if (NULL != root) PERSIMM_RC_INC(root->ref_count);
}

void persimm_hamt_release(persimm_hamt_node_t *node, const persimm_hamt_t *hamt) {
    if (NULL == node) return;
    if (PERSIMM_RC_DEC(node->ref_count) > 1) return;

    size_t entry_size = hamt->layout.entry_size;

    uint32_t data_count = persimm_hamt_data_count(node);
    for (uint32_t i = 0; i < data_count; i++) {
        persimm_hamt_entry_release(hamt, persimm_hamt_entry(node, i, entry_size));
    }

    uint32_t child_count = persimm_hamt_child_count(node);
    persimm_hamt_node_t **children = persimm_hamt_children(node, entry_size);
    for (uint32_t i = 0; i < child_count; i++) {
        persimm_hamt_release(children[i], hamt);
    }

    free(node);
}

/* Initialising */

static persimm_hamt_node_t *persimm_hamt_node_new(uint32_t kind, uint32_t data_count,
                                                  uint32_t child_count, size_t entry_size) {
    size_t bytes;
    if (!persimm_size_mul((size_t)data_count, entry_size, &bytes)) return NULL;

    if (child_count > 0) {
        if (bytes > SIZE_MAX - (PERSIMM_ALIGNMENT - 1)) return NULL;
        bytes = PERSIMM_ALIGN_UP(bytes);

        size_t child_bytes;
        if (!persimm_size_mul((size_t)child_count, sizeof(persimm_hamt_node_t *), &child_bytes) ||
            !persimm_size_add(bytes, child_bytes, &bytes)) {
            return NULL;
        }
    }

    if (!persimm_size_add(offsetof(struct persimm_hamt_node, data), bytes, &bytes)) return NULL;
    persimm_hamt_node_t *node = calloc(1, bytes);
    if (NULL == node) return NULL;

    node->kind = kind;
    PERSIMM_RC_SET(node->ref_count, 1);

    return node;
}

/*
 * Each of the six that follow consumes the caller's reference to `node` and
 * returns a node the caller owns, which may be `node` itself when it was
 * uniquely held and the change fitted in place. They return NULL and leave
 * `node` untouched if the copy could not be allocated.
 *
 * A copy retains every element it takes over before the original is released,
 * so a host that reference counts never sees an element's count reach zero
 * while the trie still holds it.
 */

typedef enum {
    PERSIMM_HAMT_CHILD_KEEP,    // every child carries over
    PERSIMM_HAMT_CHILD_REPLACE, // `child` stands in for the one at `at`
    PERSIMM_HAMT_CHILD_INSERT,  // `child` takes slot `at` and the rest shift up
    PERSIMM_HAMT_CHILD_REMOVE   // the child at `at` is left behind
} persimm_hamt_child_edit;

/*
 * Copies `from`'s children into `to` with one slot changed, retaining each
 * child that carries over. A `child` passed in arrives with a reference `to`
 * takes over, so it is not retained again. `to` must already carry the bitmaps
 * it will end up with, since they are what say where its children begin.
 */
static void persimm_hamt_copy_children(persimm_hamt_node_t *from, persimm_hamt_node_t *to,
                                       persimm_hamt_child_edit edit, uint32_t at,
                                       persimm_hamt_node_t *child, size_t entry_size) {
    uint32_t count = persimm_hamt_child_count(from);
    persimm_hamt_node_t **source = persimm_hamt_children(from, entry_size);
    persimm_hamt_node_t **dest = persimm_hamt_children(to, entry_size);

    uint32_t out = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (i == at) {
            if (PERSIMM_HAMT_CHILD_REMOVE == edit) continue;
            if (PERSIMM_HAMT_CHILD_REPLACE == edit) {
                dest[out++] = child;
                continue;
            }
            if (PERSIMM_HAMT_CHILD_INSERT == edit) dest[out++] = child;
        }
        dest[out] = source[i];
        PERSIMM_RC_INC(dest[out]->ref_count);
        out++;
    }

    /* An insert past the last child has nothing to fall in front of. */
    if (PERSIMM_HAMT_CHILD_INSERT == edit && at >= count) dest[out] = child;
}

/* Replaces the value of the entry at `index`, keeping the key already stored. */
static persimm_hamt_node_t *persimm_hamt_with_value(persimm_hamt_node_t *node, uint32_t index,
                                                    const void *entry, const persimm_hamt_t *hamt,
                                                    bool immutable) {
    size_t entry_size = hamt->layout.entry_size;
    size_t value_size = hamt->layout.value_size;

    /* A set's entries are keys alone, so one already present is unchanged. */
    if (0 == value_size) return node;

    if (!immutable && 1 == PERSIMM_RC_LOAD(node->ref_count)) {
        void *slot = persimm_hamt_value(hamt, persimm_hamt_entry(node, index, entry_size));
        void *replacement = persimm_hamt_value(hamt, (void *)entry);
        if (replacement == slot) return node;
        persimm_elem_release(hamt->ops, hamt->ctx, slot);
        memcpy(slot, replacement, value_size);
        persimm_elem_retain(hamt->ops, hamt->ctx, slot);
        return node;
    }

    uint32_t data_count = persimm_hamt_data_count(node);
    uint32_t child_count = persimm_hamt_child_count(node);

    persimm_hamt_node_t *copy = persimm_hamt_node_new(node->kind, data_count, child_count,
                                                      entry_size);
    if (NULL == copy) return NULL;
    copy->datamap = node->datamap;
    copy->nodemap = node->nodemap;
    copy->hash = node->hash;

    memcpy(copy->data, node->data, (size_t)data_count * entry_size);
    /* The new value displaces the old before anything retains it, so the old
       one is only ever released with the node it came from. */
    memcpy(persimm_hamt_value(hamt, persimm_hamt_entry(copy, index, entry_size)),
           persimm_hamt_value(hamt, (void *)entry), value_size);
    for (uint32_t i = 0; i < data_count; i++) {
        persimm_hamt_entry_retain(hamt, persimm_hamt_entry(copy, i, entry_size));
    }

    persimm_hamt_copy_children(node, copy, PERSIMM_HAMT_CHILD_KEEP, 0, NULL, entry_size);
    persimm_hamt_release(node, hamt);

    return copy;
}

/* Adds an entry at `index`, marking `bit` in the datamap of a bitmap node. */
static persimm_hamt_node_t *persimm_hamt_with_entry(persimm_hamt_node_t *node, uint32_t bit,
                                                    uint32_t index, const void *entry,
                                                    const persimm_hamt_t *hamt) {
    size_t entry_size = hamt->layout.entry_size;
    uint32_t data_count = persimm_hamt_data_count(node);
    uint32_t child_count = persimm_hamt_child_count(node);

    if (UINT32_MAX == data_count) return NULL;

    persimm_hamt_node_t *copy = persimm_hamt_node_new(node->kind, data_count + 1, child_count,
                                                      entry_size);
    if (NULL == copy) return NULL;
    copy->datamap = (PERSIMM_HAMT_COLLISION == node->kind) ? node->datamap + 1
                                                           : (node->datamap | bit);
    copy->nodemap = node->nodemap;
    copy->hash = node->hash;

    memcpy(persimm_hamt_entry(copy, 0, entry_size), persimm_hamt_entry(node, 0, entry_size),
           (size_t)index * entry_size);
    memcpy(persimm_hamt_entry(copy, index, entry_size), entry, entry_size);
    memcpy(persimm_hamt_entry(copy, index + 1, entry_size),
           persimm_hamt_entry(node, index, entry_size),
           (size_t)(data_count - index) * entry_size);

    for (uint32_t i = 0; i <= data_count; i++) {
        persimm_hamt_entry_retain(hamt, persimm_hamt_entry(copy, i, entry_size));
    }

    persimm_hamt_copy_children(node, copy, PERSIMM_HAMT_CHILD_KEEP, 0, NULL, entry_size);
    persimm_hamt_release(node, hamt);

    return copy;
}

/* Drops the entry at `index`, clearing `bit` in the datamap of a bitmap node. */
static persimm_hamt_node_t *persimm_hamt_without_entry(persimm_hamt_node_t *node, uint32_t bit,
                                                       uint32_t index,
                                                       const persimm_hamt_t *hamt) {
    size_t entry_size = hamt->layout.entry_size;
    uint32_t data_count = persimm_hamt_data_count(node);
    uint32_t child_count = persimm_hamt_child_count(node);

    persimm_hamt_node_t *copy = persimm_hamt_node_new(node->kind, data_count - 1, child_count,
                                                      entry_size);
    if (NULL == copy) return NULL;
    copy->datamap = (PERSIMM_HAMT_COLLISION == node->kind) ? node->datamap - 1
                                                           : (node->datamap & ~bit);
    copy->nodemap = node->nodemap;
    copy->hash = node->hash;

    memcpy(persimm_hamt_entry(copy, 0, entry_size), persimm_hamt_entry(node, 0, entry_size),
           (size_t)index * entry_size);
    memcpy(persimm_hamt_entry(copy, index, entry_size),
           persimm_hamt_entry(node, index + 1, entry_size),
           (size_t)(data_count - index - 1) * entry_size);

    for (uint32_t i = 0; i + 1 < data_count; i++) {
        persimm_hamt_entry_retain(hamt, persimm_hamt_entry(copy, i, entry_size));
    }

    persimm_hamt_copy_children(node, copy, PERSIMM_HAMT_CHILD_KEEP, 0, NULL, entry_size);
    persimm_hamt_release(node, hamt);

    return copy;
}

/* Points slot `index` at `child`, taking over the caller's reference to it. */
static persimm_hamt_node_t *persimm_hamt_with_child(persimm_hamt_node_t *node, uint32_t index,
                                                    persimm_hamt_node_t *child,
                                                    const persimm_hamt_t *hamt, bool immutable) {
    size_t entry_size = hamt->layout.entry_size;

    if (!immutable && 1 == PERSIMM_RC_LOAD(node->ref_count)) {
        persimm_hamt_node_t **children = persimm_hamt_children(node, entry_size);
        persimm_hamt_node_t *old = children[index];
        children[index] = child;
        persimm_hamt_release(old, hamt);
        return node;
    }

    uint32_t data_count = persimm_hamt_data_count(node);
    uint32_t child_count = persimm_hamt_child_count(node);

    persimm_hamt_node_t *copy = persimm_hamt_node_new(node->kind, data_count, child_count,
                                                      entry_size);
    if (NULL == copy) return NULL;
    copy->datamap = node->datamap;
    copy->nodemap = node->nodemap;
    copy->hash = node->hash;

    memcpy(copy->data, node->data, (size_t)data_count * entry_size);
    for (uint32_t i = 0; i < data_count; i++) {
        persimm_hamt_entry_retain(hamt, persimm_hamt_entry(copy, i, entry_size));
    }

    persimm_hamt_copy_children(node, copy, PERSIMM_HAMT_CHILD_REPLACE, index, child, entry_size);
    persimm_hamt_release(node, hamt);

    return copy;
}

/* Turns the entry in slot `bit` into the child that now holds it and one more. */
static persimm_hamt_node_t *persimm_hamt_promote(persimm_hamt_node_t *node, uint32_t bit,
                                                 persimm_hamt_node_t *child,
                                                 const persimm_hamt_t *hamt) {
    size_t entry_size = hamt->layout.entry_size;
    uint32_t data_count = persimm_hamt_data_count(node);
    uint32_t child_count = persimm_hamt_child_count(node);
    uint32_t data_index = persimm_hamt_data_index(node, bit);
    uint32_t child_index = persimm_hamt_child_index(node, bit);

    persimm_hamt_node_t *copy = persimm_hamt_node_new(node->kind, data_count - 1, child_count + 1,
                                                      entry_size);
    if (NULL == copy) return NULL;
    copy->datamap = node->datamap & ~bit;
    copy->nodemap = node->nodemap | bit;
    copy->hash = node->hash;

    memcpy(persimm_hamt_entry(copy, 0, entry_size), persimm_hamt_entry(node, 0, entry_size),
           (size_t)data_index * entry_size);
    memcpy(persimm_hamt_entry(copy, data_index, entry_size),
           persimm_hamt_entry(node, data_index + 1, entry_size),
           (size_t)(data_count - data_index - 1) * entry_size);

    for (uint32_t i = 0; i + 1 < data_count; i++) {
        persimm_hamt_entry_retain(hamt, persimm_hamt_entry(copy, i, entry_size));
    }

    persimm_hamt_copy_children(node, copy, PERSIMM_HAMT_CHILD_INSERT, child_index, child,
                               entry_size);
    persimm_hamt_release(node, hamt);

    return copy;
}

/* Pulls `entry` out of the child in slot `bit` and inlines it in the child's place. */
static persimm_hamt_node_t *persimm_hamt_demote(persimm_hamt_node_t *node, uint32_t bit,
                                                const void *entry, const persimm_hamt_t *hamt) {
    size_t entry_size = hamt->layout.entry_size;
    uint32_t data_count = persimm_hamt_data_count(node);
    uint32_t child_count = persimm_hamt_child_count(node);
    uint32_t data_index = PERSIMM_POPCOUNT((node->datamap | bit) & (bit - 1));
    uint32_t child_index = persimm_hamt_child_index(node, bit);

    persimm_hamt_node_t *copy = persimm_hamt_node_new(node->kind, data_count + 1, child_count - 1,
                                                      entry_size);
    if (NULL == copy) return NULL;
    copy->datamap = node->datamap | bit;
    copy->nodemap = node->nodemap & ~bit;
    copy->hash = node->hash;

    memcpy(persimm_hamt_entry(copy, 0, entry_size), persimm_hamt_entry(node, 0, entry_size),
           (size_t)data_index * entry_size);
    memcpy(persimm_hamt_entry(copy, data_index, entry_size), entry, entry_size);
    memcpy(persimm_hamt_entry(copy, data_index + 1, entry_size),
           persimm_hamt_entry(node, data_index, entry_size),
           (size_t)(data_count - data_index) * entry_size);

    /* The inlined entry is retained here, while the child that held it is
       still alive, and only then does the caller let that child go. */
    for (uint32_t i = 0; i <= data_count; i++) {
        persimm_hamt_entry_retain(hamt, persimm_hamt_entry(copy, i, entry_size));
    }

    persimm_hamt_copy_children(node, copy, PERSIMM_HAMT_CHILD_REMOVE, child_index, NULL,
                               entry_size);
    persimm_hamt_release(node, hamt);

    return copy;
}

/*
 * Builds the subtree holding two entries whose keys differ, copying and
 * retaining both. Equal hashes cannot be told apart at any depth and so go
 * straight to a collision node; otherwise the two must part at or before the
 * level that reads the topmost bits, which bounds the recursion.
 */
static persimm_hamt_node_t *persimm_hamt_merge(size_t shift, const void *entry_a, uint32_t hash_a,
                                               const void *entry_b, uint32_t hash_b,
                                               const persimm_hamt_t *hamt) {
    size_t entry_size = hamt->layout.entry_size;
    persimm_hamt_node_t *node;

    if (hash_a == hash_b) {
        node = persimm_hamt_node_new(PERSIMM_HAMT_COLLISION, 2, 0, entry_size);
        if (NULL == node) return NULL;
        node->datamap = 2;
        node->hash = hash_a;
        memcpy(persimm_hamt_entry(node, 0, entry_size), entry_a, entry_size);
        memcpy(persimm_hamt_entry(node, 1, entry_size), entry_b, entry_size);
        persimm_hamt_entry_retain(hamt, persimm_hamt_entry(node, 0, entry_size));
        persimm_hamt_entry_retain(hamt, persimm_hamt_entry(node, 1, entry_size));
        return node;
    }

    uint32_t bit_a = persimm_hamt_bit(hash_a, shift);
    uint32_t bit_b = persimm_hamt_bit(hash_b, shift);

    if (bit_a == bit_b) {
        persimm_hamt_node_t *child = persimm_hamt_merge(shift + PERSIMM_BITS, entry_a, hash_a,
                                                        entry_b, hash_b, hamt);
        if (NULL == child) return NULL;
        node = persimm_hamt_node_new(PERSIMM_HAMT_BITMAP, 0, 1, entry_size);
        if (NULL == node) {
            persimm_hamt_release(child, hamt);
            return NULL;
        }
        node->nodemap = bit_a;
        persimm_hamt_children(node, entry_size)[0] = child;
        return node;
    }

    node = persimm_hamt_node_new(PERSIMM_HAMT_BITMAP, 2, 0, entry_size);
    if (NULL == node) return NULL;
    node->datamap = bit_a | bit_b;

    /* Entries sit in bit order, so the lower slot takes the first place. */
    const void *first = (bit_a < bit_b) ? entry_a : entry_b;
    const void *second = (bit_a < bit_b) ? entry_b : entry_a;
    memcpy(persimm_hamt_entry(node, 0, entry_size), first, entry_size);
    memcpy(persimm_hamt_entry(node, 1, entry_size), second, entry_size);
    persimm_hamt_entry_retain(hamt, persimm_hamt_entry(node, 0, entry_size));
    persimm_hamt_entry_retain(hamt, persimm_hamt_entry(node, 1, entry_size));

    return node;
}

/* Accessing */

void *persimm_hamt_ref(persimm_hamt_node_t *root, const void *key, const persimm_hamt_t *hamt) {
    size_t entry_size = hamt->layout.entry_size;
    uint32_t hash = persimm_hamt_hash_of(hamt, key);
    persimm_hamt_node_t *node = root;
    size_t shift = 0;

    while (NULL != node) {
        if (PERSIMM_HAMT_COLLISION == node->kind) {
            if (node->hash != hash) return NULL;
            for (uint32_t i = 0; i < node->datamap; i++) {
                void *entry = persimm_hamt_entry(node, i, entry_size);
                if (persimm_hamt_keys_equal(hamt, key, entry)) return entry;
            }
            return NULL;
        }

        uint32_t bit = persimm_hamt_bit(hash, shift);

        if (node->datamap & bit) {
            void *entry = persimm_hamt_entry(node, persimm_hamt_data_index(node, bit), entry_size);
            return persimm_hamt_keys_equal(hamt, key, entry) ? entry : NULL;
        }

        if (node->nodemap & bit) {
            node = persimm_hamt_children(node, entry_size)[persimm_hamt_child_index(node, bit)];
            shift += PERSIMM_BITS;
            continue;
        }

        return NULL;
    }

    return NULL;
}

/* Inserting */

/*
 * Consumes the reference to `node` and returns the node its parent should
 * hold, or NULL if an allocation failed, in which case `node` is untouched.
 */
static persimm_hamt_node_t *persimm_hamt_node_assoc(persimm_hamt_node_t *node, size_t shift,
                                                    uint32_t hash, const void *entry,
                                                    const persimm_hamt_t *hamt, bool immutable,
                                                    bool *added) {
    size_t entry_size = hamt->layout.entry_size;

    if (PERSIMM_HAMT_COLLISION == node->kind) {
        if (node->hash == hash) {
            for (uint32_t i = 0; i < node->datamap; i++) {
                if (persimm_hamt_keys_equal(hamt, entry, persimm_hamt_entry(node, i, entry_size))) {
                    return persimm_hamt_with_value(node, i, entry, hamt, immutable);
                }
            }
            *added = true;
            return persimm_hamt_with_entry(node, 0, node->datamap, entry, hamt);
        }

        /* Another hash cannot belong here, so the collision node gains a
           parent that separates the two and the insert starts again there. */
        persimm_hamt_node_t *parent = persimm_hamt_node_new(PERSIMM_HAMT_BITMAP, 0, 1, entry_size);
        if (NULL == parent) return NULL;
        parent->nodemap = persimm_hamt_bit(node->hash, shift);
        persimm_hamt_children(parent, entry_size)[0] = node;
        persimm_hamt_node_t *result =
            persimm_hamt_node_assoc(parent, shift, hash, entry, hamt, immutable, added);
        if (NULL == result) {
            /* The caller still owns node when the operation fails. Parent's
             * temporary reference was a transfer only if the update commits. */
            free(parent);
        }
        return result;
    }

    uint32_t bit = persimm_hamt_bit(hash, shift);

    if (node->datamap & bit) {
        uint32_t index = persimm_hamt_data_index(node, bit);
        void *existing = persimm_hamt_entry(node, index, entry_size);

        if (persimm_hamt_keys_equal(hamt, entry, existing)) {
            return persimm_hamt_with_value(node, index, entry, hamt, immutable);
        }

        persimm_hamt_node_t *child = persimm_hamt_merge(shift + PERSIMM_BITS, existing,
                                                        persimm_hamt_hash_of(hamt, existing),
                                                        entry, hash, hamt);
        if (NULL == child) return NULL;

        persimm_hamt_node_t *result = persimm_hamt_promote(node, bit, child, hamt);
        if (NULL == result) {
            persimm_hamt_release(child, hamt);
            return NULL;
        }

        *added = true;
        return result;
    }

    if (node->nodemap & bit) {
        uint32_t index = persimm_hamt_child_index(node, bit);
        persimm_hamt_node_t *child = persimm_hamt_children(node, entry_size)[index];
        PERSIMM_RC_INC(child->ref_count);

        persimm_hamt_node_t *updated = persimm_hamt_node_assoc(child, shift + PERSIMM_BITS, hash,
                                                               entry, hamt, immutable, added);
        if (NULL == updated) {
            persimm_hamt_release(child, hamt);
            return NULL;
        }

        persimm_hamt_node_t *result =
            persimm_hamt_with_child(node, index, updated, hamt, immutable);
        if (NULL == result) persimm_hamt_release(updated, hamt);
        return result;
    }

    *added = true;
    return persimm_hamt_with_entry(node, bit, persimm_hamt_data_index(node, bit), entry, hamt);
}

persimm_status persimm_hamt_assoc(persimm_hamt_node_t **root, const void *entry,
                                  const persimm_hamt_t *hamt, bool immutable, bool *added) {
    size_t entry_size = hamt->layout.entry_size;
    uint32_t hash = persimm_hamt_hash_of(hamt, entry);

    *added = false;

    if (NULL == *root) {
        persimm_hamt_node_t *node = persimm_hamt_node_new(PERSIMM_HAMT_BITMAP, 1, 0, entry_size);
        if (NULL == node) return PERSIMM_ERR_ALLOC;
        node->datamap = persimm_hamt_bit(hash, 0);
        memcpy(persimm_hamt_entry(node, 0, entry_size), entry, entry_size);
        persimm_hamt_entry_retain(hamt, persimm_hamt_entry(node, 0, entry_size));
        *root = node;
        *added = true;
        return PERSIMM_OK;
    }

    persimm_hamt_node_t *updated = persimm_hamt_node_assoc(*root, 0, hash, entry, hamt, immutable,
                                                           added);
    if (NULL == updated) return PERSIMM_ERR_ALLOC;

    *root = updated;

    return PERSIMM_OK;
}

/* Removing */

/*
 * Consumes the reference to `node` and returns the node its parent should
 * hold, or NULL if an allocation failed. A node that is not the root always
 * keeps at least one slot, so only the root can come back empty.
 */
static persimm_hamt_node_t *persimm_hamt_node_dissoc(persimm_hamt_node_t *node, size_t shift,
                                                     uint32_t hash, const void *key,
                                                     const persimm_hamt_t *hamt, bool immutable,
                                                     bool *removed) {
    size_t entry_size = hamt->layout.entry_size;

    if (PERSIMM_HAMT_COLLISION == node->kind) {
        if (node->hash != hash) return node;
        for (uint32_t i = 0; i < node->datamap; i++) {
            if (persimm_hamt_keys_equal(hamt, key, persimm_hamt_entry(node, i, entry_size))) {
                *removed = true;
                return persimm_hamt_without_entry(node, 0, i, hamt);
            }
        }
        return node;
    }

    uint32_t bit = persimm_hamt_bit(hash, shift);

    if (node->datamap & bit) {
        uint32_t index = persimm_hamt_data_index(node, bit);
        if (!persimm_hamt_keys_equal(hamt, key, persimm_hamt_entry(node, index, entry_size))) {
            return node;
        }
        *removed = true;
        return persimm_hamt_without_entry(node, bit, index, hamt);
    }

    if (node->nodemap & bit) {
        uint32_t index = persimm_hamt_child_index(node, bit);
        persimm_hamt_node_t *child = persimm_hamt_children(node, entry_size)[index];
        PERSIMM_RC_INC(child->ref_count);

        persimm_hamt_node_t *updated = persimm_hamt_node_dissoc(child, shift + PERSIMM_BITS, hash,
                                                                key, hamt, immutable, removed);
        if (NULL == updated) {
            persimm_hamt_release(child, hamt);
            return NULL;
        }
        if (!*removed) {
            persimm_hamt_release(updated, hamt);
            return node;
        }

        /* Canonical form again: a child left holding one entry and nothing
           else belongs inline, and that may cascade the whole way up. */
        if (persimm_hamt_is_single(updated)) {
            persimm_hamt_node_t *result =
                persimm_hamt_demote(node, bit, persimm_hamt_entry(updated, 0, entry_size), hamt);
            persimm_hamt_release(updated, hamt);
            return result;
        }

        persimm_hamt_node_t *result =
            persimm_hamt_with_child(node, index, updated, hamt, immutable);
        if (NULL == result) persimm_hamt_release(updated, hamt);
        return result;
    }

    return node;
}

persimm_status persimm_hamt_dissoc(persimm_hamt_node_t **root, const void *key,
                                   const persimm_hamt_t *hamt, bool immutable, bool *removed) {
    *removed = false;

    if (NULL == *root) return PERSIMM_OK;

    uint32_t hash = persimm_hamt_hash_of(hamt, key);
    persimm_hamt_node_t *updated = persimm_hamt_node_dissoc(*root, 0, hash, key, hamt, immutable,
                                                            removed);
    if (NULL == updated) return PERSIMM_ERR_ALLOC;

    if (0 == persimm_hamt_data_count(updated) && 0 == persimm_hamt_child_count(updated)) {
        persimm_hamt_release(updated, hamt);
        updated = NULL;
    }

    *root = updated;

    return PERSIMM_OK;
}

/* Traversing */

static void persimm_hamt_node_foreach(persimm_hamt_node_t *node, const persimm_hamt_t *hamt,
                                      persimm_visit_fn fn, void *ctx, size_t *index) {
    size_t entry_size = hamt->layout.entry_size;

    uint32_t data_count = persimm_hamt_data_count(node);
    for (uint32_t i = 0; i < data_count; i++) {
        fn(persimm_hamt_entry(node, i, entry_size), *index, ctx);
        (*index)++;
    }

    uint32_t child_count = persimm_hamt_child_count(node);
    persimm_hamt_node_t **children = persimm_hamt_children(node, entry_size);
    for (uint32_t i = 0; i < child_count; i++) {
        persimm_hamt_node_foreach(children[i], hamt, fn, ctx, index);
    }
}

void persimm_hamt_foreach(persimm_hamt_node_t *root, const persimm_hamt_t *hamt,
                          persimm_visit_fn fn, void *ctx) {
    size_t index = 0;
    if (NULL == root) return;
    persimm_hamt_node_foreach(root, hamt, fn, ctx, &index);
}

/* The entries of a node come before its children, as they do in foreach. */
static void *persimm_hamt_node_first(persimm_hamt_node_t *node, const persimm_hamt_t *hamt) {
    size_t entry_size = hamt->layout.entry_size;

    while (NULL != node) {
        if (persimm_hamt_data_count(node) > 0) return persimm_hamt_entry(node, 0, entry_size);
        if (0 == persimm_hamt_child_count(node)) return NULL;
        node = persimm_hamt_children(node, entry_size)[0];
    }

    return NULL;
}

typedef enum {
    PERSIMM_HAMT_ABSENT, // the key is not in this subtree
    PERSIMM_HAMT_FOUND,  // the key is here and so is what follows it
    PERSIMM_HAMT_LAST    // the key is here and nothing follows it in this subtree
} persimm_hamt_seek;

static persimm_hamt_seek persimm_hamt_node_next(persimm_hamt_node_t *node, size_t shift,
                                                uint32_t hash, const void *key,
                                                const persimm_hamt_t *hamt, void **out) {
    size_t entry_size = hamt->layout.entry_size;

    if (PERSIMM_HAMT_COLLISION == node->kind) {
        if (node->hash != hash) return PERSIMM_HAMT_ABSENT;
        for (uint32_t i = 0; i < node->datamap; i++) {
            if (!persimm_hamt_keys_equal(hamt, key, persimm_hamt_entry(node, i, entry_size))) {
                continue;
            }
            if (i + 1 < node->datamap) {
                *out = persimm_hamt_entry(node, i + 1, entry_size);
                return PERSIMM_HAMT_FOUND;
            }
            return PERSIMM_HAMT_LAST;
        }
        return PERSIMM_HAMT_ABSENT;
    }

    uint32_t bit = persimm_hamt_bit(hash, shift);
    uint32_t data_count = persimm_hamt_data_count(node);
    uint32_t child_count = persimm_hamt_child_count(node);
    persimm_hamt_node_t **children = persimm_hamt_children(node, entry_size);

    if (node->datamap & bit) {
        uint32_t index = persimm_hamt_data_index(node, bit);
        if (!persimm_hamt_keys_equal(hamt, key, persimm_hamt_entry(node, index, entry_size))) {
            return PERSIMM_HAMT_ABSENT;
        }
        if (index + 1 < data_count) {
            *out = persimm_hamt_entry(node, index + 1, entry_size);
            return PERSIMM_HAMT_FOUND;
        }
        if (child_count > 0) {
            *out = persimm_hamt_node_first(children[0], hamt);
            return PERSIMM_HAMT_FOUND;
        }
        return PERSIMM_HAMT_LAST;
    }

    if (node->nodemap & bit) {
        uint32_t index = persimm_hamt_child_index(node, bit);
        persimm_hamt_seek result = persimm_hamt_node_next(children[index], shift + PERSIMM_BITS,
                                                          hash, key, hamt, out);
        if (PERSIMM_HAMT_LAST != result) return result;
        if (index + 1 < child_count) {
            *out = persimm_hamt_node_first(children[index + 1], hamt);
            return PERSIMM_HAMT_FOUND;
        }
        return PERSIMM_HAMT_LAST;
    }

    return PERSIMM_HAMT_ABSENT;
}

void *persimm_hamt_next(persimm_hamt_node_t *root, const void *key, const persimm_hamt_t *hamt) {
    if (NULL == root) return NULL;
    if (NULL == key) return persimm_hamt_node_first(root, hamt);

    void *out = NULL;
    uint32_t hash = persimm_hamt_hash_of(hamt, key);
    if (PERSIMM_HAMT_FOUND != persimm_hamt_node_next(root, 0, hash, key, hamt, &out)) return NULL;

    return out;
}

static void persimm_hamt_trace_visit(void *slot, size_t index, void *ctx) {
    (void) index;
    const persimm_hamt_t *hamt = (const persimm_hamt_t *)ctx;
    hamt->ops->trace(slot, hamt->ctx);
    if (hamt->layout.value_size > 0) {
        hamt->ops->trace(persimm_hamt_value(hamt, slot), hamt->ctx);
    }
}

void persimm_hamt_trace(persimm_hamt_node_t *root, const persimm_hamt_t *hamt) {
    if (NULL == hamt->ops || NULL == hamt->ops->trace) return;
    persimm_hamt_foreach(root, hamt, persimm_hamt_trace_visit, (void *)hamt);
}
