#include <stdlib.h>
#include <string.h>
#include "persimmon_internal.h"

/*
 * The map's face on the CHAMP trie in persimmon_hamt.c. Everything here is
 * bookkeeping: the trie holds the entries and the map holds how many there
 * are and how they are laid out.
 */

/* Configuring */

static void persimm_map_hamt(const persimm_map_t *map, persimm_hamt_t *hamt) {
    persimm_hamt_config(hamt, &map->layout, map->value_ops, map->value_ctx, map->key_ops,
                        map->key_ctx);
}

static persimm_status persimm_map_assoc_in_place(persimm_map_t *map, const void *entry,
                                                 bool immutable);
static persimm_status persimm_map_dissoc_in_place(persimm_map_t *map, const void *key,
                                                  bool immutable);

/* Initialising */

persimm_status persimm_map_init(persimm_map_t *map, const persimm_entry_layout *layout,
                                const persimm_elem_ops *value_ops, void *value_ctx,
                                const persimm_key_ops *key_ops, void *key_ctx) {
    map->count = 0;
    map->layout = *layout;
    map->value_ops = value_ops;
    map->key_ops = key_ops;
    map->value_ctx = value_ctx;
    map->key_ctx = key_ctx;
    map->root = NULL;

    if (!persimm_hamt_layout_valid(layout)) return PERSIMM_ERR_INVALID;

    return PERSIMM_OK;
}

void persimm_map_clone(const persimm_map_t *src, persimm_map_t *dest) {
    dest->count = src->count;
    dest->layout = src->layout;
    dest->value_ops = src->value_ops;
    dest->key_ops = src->key_ops;
    dest->value_ctx = src->value_ctx;
    dest->key_ctx = src->key_ctx;
    dest->root = src->root;
    persimm_hamt_retain(dest->root);
}

/* Transients */

void persimm_map_to_transient(const persimm_map_t *src, persimm_map_transient_t *transient) {
    persimm_map_clone(src, &transient->value);
    transient->active = true;
}

persimm_status persimm_map_transient_init(persimm_map_transient_t *transient,
                                          const persimm_entry_layout *layout,
                                          const persimm_elem_ops *value_ops, void *value_ctx,
                                          const persimm_key_ops *key_ops, void *key_ctx) {
    persimm_status status = persimm_map_init(&transient->value, layout, value_ops, value_ctx,
                                             key_ops, key_ctx);
    transient->active = PERSIMM_OK == status;
    return status;
}

void persimm_map_transient_deinit(persimm_map_transient_t *transient) {
    persimm_map_deinit(&transient->value);
    transient->active = false;
}

persimm_status persimm_map_transient_assoc(persimm_map_transient_t *transient,
                                           const void *entry) {
    if (!transient->active) return PERSIMM_ERR_INVALID;
    return persimm_map_assoc_in_place(&transient->value, entry, false);
}

persimm_status persimm_map_transient_dissoc(persimm_map_transient_t *transient,
                                            const void *key) {
    if (!transient->active) return PERSIMM_ERR_INVALID;
    return persimm_map_dissoc_in_place(&transient->value, key, false);
}

persimm_status persimm_map_transient_persist(persimm_map_transient_t *transient,
                                             persimm_map_t *dest) {
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

void persimm_map_deinit(persimm_map_t *map) {
    persimm_hamt_t hamt;
    persimm_map_hamt(map, &hamt);
    persimm_hamt_release(map->root, &hamt);
    map->root = NULL;
    map->count = 0;
}

/* Accessing */

const void *persimm_map_ref_entry(const persimm_map_t *map, const void *key) {
    persimm_hamt_t hamt;
    persimm_map_hamt(map, &hamt);
    return persimm_hamt_ref(map->root, key, &hamt);
}

const void *persimm_map_ref(const persimm_map_t *map, const void *key) {
    if (0 == map->layout.value_size) return NULL;

    const void *entry = persimm_map_ref_entry(map, key);
    if (NULL == entry) return NULL;

    return (const unsigned char *)entry + map->layout.value_offset;
}

bool persimm_map_has(const persimm_map_t *map, const void *key) {
    return NULL != persimm_map_ref_entry(map, key);
}

/* Inserting */

static persimm_status persimm_map_assoc_in_place(persimm_map_t *map, const void *entry,
                                                 bool immutable) {
    persimm_hamt_t hamt;
    persimm_map_hamt(map, &hamt);

    bool added = false;
    persimm_status status = persimm_hamt_assoc(&map->root, entry, &hamt, immutable, &added);
    if (PERSIMM_OK != status) return status;

    if (added) map->count++;

    return PERSIMM_OK;
}

/* Removing */

static persimm_status persimm_map_dissoc_in_place(persimm_map_t *map, const void *key,
                                                  bool immutable) {
    persimm_hamt_t hamt;
    persimm_map_hamt(map, &hamt);

    bool removed = false;
    persimm_status status = persimm_hamt_dissoc(&map->root, key, &hamt, immutable, &removed);
    if (PERSIMM_OK != status) return status;

    if (removed) map->count--;

    return PERSIMM_OK;
}

persimm_status persimm_map_assoc(const persimm_map_t *src, const void *entry,
                                 persimm_map_t *dest) {
    if ((const void *)src == (const void *)dest) return PERSIMM_ERR_INVALID;
    persimm_map_clone(src, dest);
    persimm_status status = persimm_map_assoc_in_place(dest, entry, true);
    if (PERSIMM_OK != status) persimm_map_deinit(dest);
    return status;
}

persimm_status persimm_map_dissoc(const persimm_map_t *src, const void *key,
                                  persimm_map_t *dest) {
    if ((const void *)src == (const void *)dest) return PERSIMM_ERR_INVALID;
    persimm_map_clone(src, dest);
    persimm_status status = persimm_map_dissoc_in_place(dest, key, true);
    if (PERSIMM_OK != status) persimm_map_deinit(dest);
    return status;
}

/* Traversing */

void persimm_map_foreach(const persimm_map_t *map, persimm_visit_fn fn, void *ctx) {
    persimm_hamt_t hamt;
    persimm_map_hamt(map, &hamt);
    persimm_hamt_foreach(map->root, &hamt, fn, ctx);
}

const void *persimm_map_next(const persimm_map_t *map, const void *key) {
    persimm_hamt_t hamt;
    persimm_map_hamt(map, &hamt);
    return persimm_hamt_next(map->root, key, &hamt);
}

void persimm_map_trace(const persimm_map_t *map) {
    persimm_hamt_t hamt;
    persimm_map_hamt(map, &hamt);
    persimm_hamt_trace(map->root, &hamt);
}
