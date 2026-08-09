#include <stdlib.h>
#include <string.h>
#include "persimmon_internal.h"

/*
 * A persistent list is a singly linked chain of reference counted cells. Two
 * lists share every cell from their first common element onwards, so consing
 * onto a list costs one cell however long the list is.
 */

/* Types */

struct persimm_list_cell {
    persimm_refcount_t ref_count;
    persimm_list_cell_t *next;
    persimm_align_t data[];
};

/* Element Access */

static void *persimm_list_cell_slot(persimm_list_cell_t *cell) {
    return (void *)cell->data;
}

/* Deinitialising */

/*
 * Walks the chain rather than recursing down it. A list is as long as the
 * host cares to make it, and a release that recursed would run out of stack
 * long before the list ran out of cells.
 */
static void persimm_list_cell_release(persimm_list_cell_t *cell, const persimm_elem_ops *ops,
                                      void *ctx) {
    while (NULL != cell) {
        if (PERSIMM_RC_DEC(cell->ref_count) > 1) return;

        persimm_list_cell_t *next = cell->next;
        persimm_elem_release(ops, ctx, persimm_list_cell_slot(cell));
        free(cell);
        cell = next;
    }
}

void persimm_list_deinit(persimm_list_t *list) {
    persimm_list_cell_release(list->head, list->ops, list->ctx);
    list->head = NULL;
    list->count = 0;
}

/* Initialising */

static persimm_list_cell_t *persimm_list_cell_new(size_t elem_size) {
    size_t bytes;
    if (!persimm_size_add(offsetof(struct persimm_list_cell, data), elem_size, &bytes)) return NULL;
    persimm_list_cell_t *cell = calloc(1, bytes);
    if (NULL == cell) return NULL;
    PERSIMM_RC_SET(cell->ref_count, 1);
    return cell;
}

persimm_status persimm_list_init(persimm_list_t *list, size_t elem_size,
                                 const persimm_elem_ops *ops, void *ctx) {
    list->count = 0;
    list->generation = 0;
    list->elem_size = elem_size;
    list->ops = ops;
    list->ctx = ctx;
    list->head = NULL;

    if (0 == elem_size) return PERSIMM_ERR_INVALID;

    return PERSIMM_OK;
}

persimm_status persimm_list_clone(const persimm_list_t *src, persimm_list_t *dest) {
    if (src == dest) return PERSIMM_ERR_INVALID;
    dest->count = src->count;
    dest->generation = src->generation;
    dest->elem_size = src->elem_size;
    dest->ops = src->ops;
    dest->ctx = src->ctx;
    dest->head = src->head;
    if (NULL != dest->head) PERSIMM_RC_INC(dest->head->ref_count);
    return PERSIMM_OK;
}

/* Accessing */

const void *persimm_list_first(const persimm_list_t *list) {
    if (NULL == list->head) return NULL;
    return persimm_list_cell_slot(list->head);
}

const void *persimm_list_at(const persimm_list_t *list, size_t index) {
    if (index >= list->count) return NULL;

    persimm_list_cell_t *cell = list->head;
    for (size_t i = 0; i < index; i++) {
        if (NULL == cell) return NULL;
        cell = cell->next;
    }
    if (NULL == cell) return NULL;

    return persimm_list_cell_slot(cell);
}

void persimm_list_cursor_reset(persimm_list_cursor_t *cursor) {
    cursor->list = NULL;
    cursor->generation = 0;
    cursor->index = 0;
    cursor->cell = NULL;
}

const void *persimm_list_at_from(const persimm_list_t *list, persimm_list_cursor_t *cursor,
                                 size_t index) {
    if (index >= list->count) return NULL;

    persimm_list_cell_t *cell = list->head;
    size_t position = 0;

    /* The generation notices changes even when a run of operations restores
       the old count. */
    if (NULL != cursor->cell &&
        cursor->list == list &&
        cursor->generation == list->generation &&
        cursor->index <= index) {
        cell = cursor->cell;
        position = cursor->index;
    }

    for (; position < index; position++) {
        if (NULL == cell) return NULL;
        cell = cell->next;
    }
    if (NULL == cell) return NULL;

    cursor->list = list;
    cursor->generation = list->generation;
    cursor->index = index;
    cursor->cell = cell;

    return persimm_list_cell_slot(cell);
}

/* Inserting */

static persimm_status persimm_list_cons_in_place(persimm_list_t *list, const void *elem) {
    persimm_list_cell_t *cell = persimm_list_cell_new(list->elem_size);
    if (NULL == cell) return PERSIMM_ERR_ALLOC;

    memcpy(persimm_list_cell_slot(cell), elem, list->elem_size);
    persimm_elem_retain(list->ops, list->ctx, persimm_list_cell_slot(cell));

    /* The list's reference to its old head becomes the new cell's. */
    cell->next = list->head;
    list->head = cell;
    list->count++;
    list->generation++;

    return PERSIMM_OK;
}

/* Removing */

static persimm_status persimm_list_rest_in_place(persimm_list_t *list) {
    if (NULL == list->head) return PERSIMM_ERR_BOUNDS;

    persimm_list_cell_t *head = list->head;
    persimm_list_cell_t *next = head->next;

    /* Take a reference to the new head before letting go of the old one, or
       releasing the old head could take the rest of the chain with it. */
    if (NULL != next) PERSIMM_RC_INC(next->ref_count);
    persimm_list_cell_release(head, list->ops, list->ctx);

    list->head = next;
    list->count--;
    list->generation++;

    return PERSIMM_OK;
}

persimm_status persimm_list_cons(const persimm_list_t *src, const void *elem,
                                 persimm_list_t *dest) {
    persimm_status status = persimm_list_clone(src, dest);
    if (PERSIMM_OK != status) return status;
    status = persimm_list_cons_in_place(dest, elem);
    if (PERSIMM_OK != status) persimm_list_deinit(dest);
    return status;
}

persimm_status persimm_list_rest(const persimm_list_t *src, persimm_list_t *dest) {
    persimm_status status = persimm_list_clone(src, dest);
    if (PERSIMM_OK != status) return status;
    status = persimm_list_rest_in_place(dest);
    if (PERSIMM_OK != status) persimm_list_deinit(dest);
    return status;
}

/* Traversing */

void persimm_list_foreach(const persimm_list_t *list, persimm_visit_fn fn, void *ctx) {
    size_t index = 0;
    for (persimm_list_cell_t *cell = list->head; NULL != cell; cell = cell->next) {
        fn(persimm_list_cell_slot(cell), index, ctx);
        index++;
    }
}

static void persimm_list_trace_visit(const void *slot, size_t index, void *ctx) {
    (void) index;
    const persimm_list_t *list = (const persimm_list_t *)ctx;
    list->ops->trace(slot, list->ctx);
}

void persimm_list_trace(const persimm_list_t *list) {
    if (NULL == list->ops || NULL == list->ops->trace) return;
    persimm_list_foreach(list, persimm_list_trace_visit, (void *)list);
}
