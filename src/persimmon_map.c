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
    persimm_hamt_config(hamt, &map->layout, map->ops, map->key_ops, map->ctx);
}

/* Initialising */

persimm_status persimm_map_init(persimm_map_t *map, const persimm_entry_layout *layout,
                                const persimm_elem_ops *ops, const persimm_key_ops *key_ops,
                                void *ctx) {
    map->count = 0;
    map->layout = *layout;
    map->ops = ops;
    map->key_ops = key_ops;
    map->ctx = ctx;
    map->root = NULL;

    if (!persimm_hamt_layout_valid(layout)) return PERSIMM_ERR_INVALID;

    return PERSIMM_OK;
}

void persimm_map_clone(const persimm_map_t *src, persimm_map_t *dest) {
    dest->count = src->count;
    dest->layout = src->layout;
    dest->ops = src->ops;
    dest->key_ops = src->key_ops;
    dest->ctx = src->ctx;
    dest->root = src->root;
    persimm_hamt_retain(dest->root);
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

void *persimm_map_ref_entry(const persimm_map_t *map, const void *key) {
    persimm_hamt_t hamt;
    persimm_map_hamt(map, &hamt);
    return persimm_hamt_ref(map->root, key, &hamt);
}

void *persimm_map_ref(const persimm_map_t *map, const void *key) {
    if (0 == map->layout.value_size) return NULL;

    void *entry = persimm_map_ref_entry(map, key);
    if (NULL == entry) return NULL;

    return (unsigned char *)entry + map->layout.value_offset;
}

bool persimm_map_has(const persimm_map_t *map, const void *key) {
    return NULL != persimm_map_ref_entry(map, key);
}

/* Inserting */

persimm_status persimm_map_assoc(persimm_map_t *map, const void *entry, bool immutable) {
    persimm_hamt_t hamt;
    persimm_map_hamt(map, &hamt);

    bool added = false;
    persimm_status status = persimm_hamt_assoc(&map->root, entry, &hamt, immutable, &added);
    if (PERSIMM_OK != status) return status;

    if (added) map->count++;

    return PERSIMM_OK;
}

/* Removing */

persimm_status persimm_map_dissoc(persimm_map_t *map, const void *key, bool immutable) {
    persimm_hamt_t hamt;
    persimm_map_hamt(map, &hamt);

    bool removed = false;
    persimm_status status = persimm_hamt_dissoc(&map->root, key, &hamt, immutable, &removed);
    if (PERSIMM_OK != status) return status;

    if (removed) map->count--;

    return PERSIMM_OK;
}

/* Traversing */

void persimm_map_foreach(const persimm_map_t *map, persimm_visit_fn fn, void *ctx) {
    persimm_hamt_t hamt;
    persimm_map_hamt(map, &hamt);
    persimm_hamt_foreach(map->root, &hamt, fn, ctx);
}

void *persimm_map_next(const persimm_map_t *map, const void *key) {
    persimm_hamt_t hamt;
    persimm_map_hamt(map, &hamt);
    return persimm_hamt_next(map->root, key, &hamt);
}

void persimm_map_trace(const persimm_map_t *map) {
    persimm_hamt_t hamt;
    persimm_map_hamt(map, &hamt);
    persimm_hamt_trace(map->root, &hamt);
}
