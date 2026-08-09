#include <stdlib.h>
#include <string.h>
#include "persimmon_internal.h"

/*
 * The set's face on the CHAMP trie in persimmon_hamt.c. A set's entries are
 * keys and nothing else, which the layout says by giving them no value, and
 * from there the trie needs to know nothing about which of the two it is
 * serving.
 */

/* Configuring */

static void persimm_set_hamt(const persimm_set_t *set, persimm_hamt_t *hamt) {
    persimm_hamt_config(hamt, &set->layout, set->ops, set->key_ops, set->ctx);
}

/* Initialising */

persimm_status persimm_set_init(persimm_set_t *set, size_t elem_size, const persimm_elem_ops *ops,
                                const persimm_key_ops *key_ops, void *ctx) {
    set->count = 0;
    set->layout.entry_size = elem_size;
    set->layout.key_size = elem_size;
    set->layout.value_offset = 0;
    set->layout.value_size = 0;
    set->ops = ops;
    set->key_ops = key_ops;
    set->ctx = ctx;
    set->root = NULL;

    if (0 == elem_size) return PERSIMM_ERR_INVALID;

    return PERSIMM_OK;
}

void persimm_set_clone(const persimm_set_t *src, persimm_set_t *dest) {
    dest->count = src->count;
    dest->layout = src->layout;
    dest->ops = src->ops;
    dest->key_ops = src->key_ops;
    dest->ctx = src->ctx;
    dest->root = src->root;
    persimm_hamt_retain(dest->root);
}

/* Deinitialising */

void persimm_set_deinit(persimm_set_t *set) {
    persimm_hamt_t hamt;
    persimm_set_hamt(set, &hamt);
    persimm_hamt_release(set->root, &hamt);
    set->root = NULL;
    set->count = 0;
}

/* Accessing */

void *persimm_set_ref(const persimm_set_t *set, const void *elem) {
    persimm_hamt_t hamt;
    persimm_set_hamt(set, &hamt);
    return persimm_hamt_ref(set->root, elem, &hamt);
}

bool persimm_set_has(const persimm_set_t *set, const void *elem) {
    return NULL != persimm_set_ref(set, elem);
}

/* Inserting */

persimm_status persimm_set_conj(persimm_set_t *set, const void *elem, bool immutable) {
    persimm_hamt_t hamt;
    persimm_set_hamt(set, &hamt);

    bool added = false;
    persimm_status status = persimm_hamt_assoc(&set->root, elem, &hamt, immutable, &added);
    if (PERSIMM_OK != status) return status;

    if (added) set->count++;

    return PERSIMM_OK;
}

/* Removing */

persimm_status persimm_set_disj(persimm_set_t *set, const void *elem, bool immutable) {
    persimm_hamt_t hamt;
    persimm_set_hamt(set, &hamt);

    bool removed = false;
    persimm_status status = persimm_hamt_dissoc(&set->root, elem, &hamt, immutable, &removed);
    if (PERSIMM_OK != status) return status;

    if (removed) set->count--;

    return PERSIMM_OK;
}

/* Traversing */

void persimm_set_foreach(const persimm_set_t *set, persimm_visit_fn fn, void *ctx) {
    persimm_hamt_t hamt;
    persimm_set_hamt(set, &hamt);
    persimm_hamt_foreach(set->root, &hamt, fn, ctx);
}

void *persimm_set_next(const persimm_set_t *set, const void *elem) {
    persimm_hamt_t hamt;
    persimm_set_hamt(set, &hamt);
    return persimm_hamt_next(set->root, elem, &hamt);
}

void persimm_set_trace(const persimm_set_t *set) {
    persimm_hamt_t hamt;
    persimm_set_hamt(set, &hamt);
    persimm_hamt_trace(set->root, &hamt);
}
