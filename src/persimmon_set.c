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
    persimm_hamt_config(hamt, &set->layout, NULL, NULL, set->key_ops, set->key_ctx);
}

static persimm_status persimm_set_conj_in_place(persimm_set_t *set, const void *elem,
                                                bool immutable);
static persimm_status persimm_set_disj_in_place(persimm_set_t *set, const void *elem,
                                                bool immutable);

/* Initialising */

persimm_status persimm_set_init(persimm_set_t *set, size_t elem_size,
                                const persimm_key_ops *key_ops, void *key_ctx) {
    set->count = 0;
    set->layout.entry_size = elem_size;
    set->layout.key_size = elem_size;
    set->layout.value_offset = 0;
    set->layout.value_size = 0;
    set->key_ops = key_ops;
    set->key_ctx = key_ctx;
    set->root = NULL;

    if (0 == elem_size) return PERSIMM_ERR_INVALID;

    return PERSIMM_OK;
}

persimm_status persimm_set_clone(const persimm_set_t *src, persimm_set_t *dest) {
    if (src == dest) return PERSIMM_ERR_INVALID;
    dest->count = src->count;
    dest->layout = src->layout;
    dest->key_ops = src->key_ops;
    dest->key_ctx = src->key_ctx;
    dest->root = src->root;
    persimm_hamt_retain(dest->root);
    return PERSIMM_OK;
}

/* Transients */

persimm_status persimm_set_to_transient(const persimm_set_t *src,
                                        persimm_set_transient_t *transient) {
    persimm_status status = persimm_set_clone(src, &transient->value);
    if (PERSIMM_OK != status) return status;
    transient->active = true;
    return PERSIMM_OK;
}

persimm_status persimm_set_transient_init(persimm_set_transient_t *transient,
                                          size_t elem_size, const persimm_key_ops *key_ops,
                                          void *key_ctx) {
    persimm_status status = persimm_set_init(&transient->value, elem_size, key_ops, key_ctx);
    transient->active = PERSIMM_OK == status;
    return status;
}

void persimm_set_transient_deinit(persimm_set_transient_t *transient) {
    persimm_set_deinit(&transient->value);
    transient->active = false;
}

persimm_status persimm_set_transient_conj(persimm_set_transient_t *transient,
                                          const void *elem) {
    if (!transient->active) return PERSIMM_ERR_INVALID;
    return persimm_set_conj_in_place(&transient->value, elem, false);
}

persimm_status persimm_set_transient_disj(persimm_set_transient_t *transient,
                                          const void *elem) {
    if (!transient->active) return PERSIMM_ERR_INVALID;
    return persimm_set_disj_in_place(&transient->value, elem, false);
}

persimm_status persimm_set_transient_persist(persimm_set_transient_t *transient,
                                             persimm_set_t *dest) {
    if (&transient->value == dest) return PERSIMM_ERR_INVALID;
    if (!transient->active) {
        memset(dest, 0, sizeof(*dest));
        return PERSIMM_ERR_INVALID;
    }

    *dest = transient->value;
    memset(&transient->value, 0, sizeof(transient->value));
    transient->active = false;
    return PERSIMM_OK;
}

/* Deinitialising */

void persimm_set_deinit(persimm_set_t *set) {
    if (NULL != set->root) {
        persimm_hamt_t hamt;
        persimm_set_hamt(set, &hamt);
        persimm_hamt_release(set->root, &hamt);
    }
    set->root = NULL;
    set->count = 0;
}

/* Accessing */

const void *persimm_set_ref(const persimm_set_t *set, const void *elem) {
    persimm_hamt_t hamt;
    persimm_set_hamt(set, &hamt);
    return persimm_hamt_ref(set->root, elem, &hamt);
}

bool persimm_set_has(const persimm_set_t *set, const void *elem) {
    return NULL != persimm_set_ref(set, elem);
}

/* Inserting */

static persimm_status persimm_set_conj_in_place(persimm_set_t *set, const void *elem,
                                                bool immutable) {
    persimm_hamt_t hamt;
    persimm_set_hamt(set, &hamt);

    bool added = false;
    persimm_status status = persimm_hamt_assoc(&set->root, elem, &hamt, immutable, &added);
    if (PERSIMM_OK != status) return status;

    if (added) set->count++;

    return PERSIMM_OK;
}

/* Removing */

static persimm_status persimm_set_disj_in_place(persimm_set_t *set, const void *elem,
                                                bool immutable) {
    persimm_hamt_t hamt;
    persimm_set_hamt(set, &hamt);

    bool removed = false;
    persimm_status status = persimm_hamt_dissoc(&set->root, elem, &hamt, immutable, &removed);
    if (PERSIMM_OK != status) return status;

    if (removed) set->count--;

    return PERSIMM_OK;
}

persimm_status persimm_set_conj(const persimm_set_t *src, const void *elem,
                                persimm_set_t *dest) {
    persimm_status status = persimm_set_clone(src, dest);
    if (PERSIMM_OK != status) return status;
    status = persimm_set_conj_in_place(dest, elem, true);
    if (PERSIMM_OK != status) persimm_set_deinit(dest);
    return status;
}

persimm_status persimm_set_disj(const persimm_set_t *src, const void *elem,
                                persimm_set_t *dest) {
    persimm_status status = persimm_set_clone(src, dest);
    if (PERSIMM_OK != status) return status;
    status = persimm_set_disj_in_place(dest, elem, true);
    if (PERSIMM_OK != status) persimm_set_deinit(dest);
    return status;
}

/* Traversing */

void persimm_set_foreach(const persimm_set_t *set, persimm_visit_fn fn, void *ctx) {
    persimm_hamt_t hamt;
    persimm_set_hamt(set, &hamt);
    persimm_hamt_foreach(set->root, &hamt, fn, ctx);
}

const void *persimm_set_next(const persimm_set_t *set, const void *elem) {
    persimm_hamt_t hamt;
    persimm_set_hamt(set, &hamt);
    return persimm_hamt_next(set->root, elem, &hamt);
}

void persimm_set_trace(const persimm_set_t *set) {
    persimm_hamt_t hamt;
    persimm_set_hamt(set, &hamt);
    persimm_hamt_trace(set->root, &hamt);
}
