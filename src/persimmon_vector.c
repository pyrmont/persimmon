#include <stdlib.h>
#include <string.h>
#include "persimmon_internal.h"

/* Types */

typedef enum {
    PERSIMM_NODE_INNER,
    PERSIMM_NODE_LEAF
} persimm_node_type;

struct persimm_node {
    persimm_node_type kind;
    persimm_refcount_t ref_count;
    persimm_align_t data[];
};

/* Element Access */

static persimm_node_t **persimm_node_children(persimm_node_t *node) {
    return (persimm_node_t **)node->data;
}

static void *persimm_node_slot(persimm_node_t *node, size_t index, size_t elem_size) {
    return (unsigned char *)node->data + (index * elem_size);
}

/* Deinitialising */

/*
 * `live` is the number of leaf slots holding elements. Every leaf in the trie
 * is full by construction, so only the tail is ever partial and only its owner
 * knows how partial it is.
 */
static void persimm_node_release(persimm_node_t *node, size_t live, size_t elem_size,
                                 const persimm_elem_ops *ops, void *ctx) {
    if (NULL == node) return;

    if (PERSIMM_RC_DEC(node->ref_count) > 1) return;

    if (node->kind == PERSIMM_NODE_INNER) {
        persimm_node_t **children = persimm_node_children(node);
        for (size_t i = 0; i < PERSIMM_WIDTH; i++) {
            if (NULL == children[i]) continue;
            persimm_node_release(children[i], PERSIMM_WIDTH, elem_size, ops, ctx);
            children[i] = NULL;
        }
    } else if (NULL != ops && NULL != ops->release) {
        for (size_t i = 0; i < live; i++) {
            ops->release(persimm_node_slot(node, i, elem_size), ctx);
        }
    }

    free(node);
}

void persimm_vector_deinit(persimm_vector_t *vector) {
    persimm_node_release(vector->root, PERSIMM_WIDTH, vector->elem_size, vector->ops, vector->ctx);
    persimm_node_release(vector->tail, vector->tail_count, vector->elem_size, vector->ops,
                         vector->ctx);
    vector->root = NULL;
    vector->tail = NULL;
    vector->count = 0;
    vector->tail_count = 0;
}

/* Initialising */

static persimm_node_t *persimm_node_new(persimm_node_type kind, size_t elem_size) {
    size_t stride = (kind == PERSIMM_NODE_INNER) ? sizeof(persimm_node_t *) : elem_size;
    persimm_node_t *node = calloc(1, offsetof(struct persimm_node, data) + (PERSIMM_WIDTH * stride));
    if (NULL == node) return NULL;
    node->kind = kind;
    PERSIMM_RC_SET(node->ref_count, 1);
    return node;
}

/*
 * Returns a node the caller owns exclusively, consuming the reference it passed
 * in. When that reference was already the only one the node is returned as-is.
 * Returns NULL, leaving `node` untouched, if the copy could not be allocated.
 */
static persimm_node_t *persimm_node_make_unique(persimm_node_t *node, size_t live, size_t elem_size,
                                                const persimm_elem_ops *ops, void *ctx) {
    if (PERSIMM_RC_LOAD(node->ref_count) == 1) return node;

    persimm_node_t *copy = persimm_node_new(node->kind, elem_size);
    if (NULL == copy) return NULL;

    if (node->kind == PERSIMM_NODE_INNER) {
        persimm_node_t **children = persimm_node_children(node);
        persimm_node_t **copies = persimm_node_children(copy);
        for (size_t i = 0; i < PERSIMM_WIDTH; i++) {
            copies[i] = children[i];
            if (NULL != copies[i]) PERSIMM_RC_INC(copies[i]->ref_count);
        }
    } else {
        memcpy(copy->data, node->data, PERSIMM_WIDTH * elem_size);
        for (size_t i = 0; i < live; i++) {
            persimm_elem_retain(ops, ctx, persimm_node_slot(copy, i, elem_size));
        }
    }

    persimm_node_release(node, live, elem_size, ops, ctx);

    return copy;
}

persimm_status persimm_vector_init(persimm_vector_t *vector, size_t elem_size,
                                   const persimm_elem_ops *ops, void *ctx) {
    vector->shift = 0;
    vector->count = 0;
    vector->tail_count = 0;
    vector->elem_size = elem_size;
    vector->ops = ops;
    vector->ctx = ctx;
    vector->root = NULL;
    vector->tail = NULL;

    if (0 == elem_size) return PERSIMM_ERR_INVALID;

    vector->tail = persimm_node_new(PERSIMM_NODE_LEAF, elem_size);
    if (NULL == vector->tail) return PERSIMM_ERR_ALLOC;

    return PERSIMM_OK;
}

void persimm_vector_clone(const persimm_vector_t *src, persimm_vector_t *dest) {
    dest->shift = src->shift;
    dest->count = src->count;
    dest->tail_count = src->tail_count;
    dest->elem_size = src->elem_size;
    dest->ops = src->ops;
    dest->ctx = src->ctx;
    dest->root = src->root;
    if (NULL != dest->root) PERSIMM_RC_INC(dest->root->ref_count);
    dest->tail = src->tail;
    if (NULL != dest->tail) PERSIMM_RC_INC(dest->tail->ref_count);
}

/* Accessing */

void *persimm_vector_ref(const persimm_vector_t *vector, size_t index) {
    if (index >= vector->count) return NULL;

    size_t tail_offset = vector->count - vector->tail_count;
    if (index >= tail_offset) {
        return persimm_node_slot(vector->tail, index - tail_offset, vector->elem_size);
    }

    persimm_node_t *node = vector->root;
    for (size_t level = vector->shift; level > 0; level -= PERSIMM_BITS) {
        if (NULL == node) return NULL;
        node = persimm_node_children(node)[(index >> level) & PERSIMM_MASK];
    }
    if (NULL == node) return NULL;

    return persimm_node_slot(node, index & PERSIMM_MASK, vector->elem_size);
}

bool persimm_vector_index(const persimm_vector_t *vector, int64_t input, size_t *index) {
    int64_t length = (int64_t)vector->count;
    if ((length + input) < 0 || input >= length) return false;
    *index = (size_t)((input < 0) ? (length + input) : input);
    return true;
}

/* Inserting */

static void persimm_elem_store(const persimm_vector_t *vector, void *slot, const void *elem) {
    memcpy(slot, elem, vector->elem_size);
    persimm_elem_retain(vector->ops, vector->ctx, slot);
}

/*
 * Grafts a full tail into the trie, growing the root by one level first if
 * there is no longer room beneath it. `old_count` is the vector's count before
 * the push that displaced the tail.
 */
static persimm_status persimm_vector_graft(persimm_vector_t *vector, persimm_node_t *tail,
                                           size_t old_count, bool immutable) {
    if (old_count == PERSIMM_WIDTH) {
        vector->root = tail;
        return PERSIMM_OK;
    }

    if ((old_count >> PERSIMM_BITS) > ((size_t)1 << vector->shift)) {
        persimm_node_t *root = persimm_node_new(PERSIMM_NODE_INNER, vector->elem_size);
        if (NULL == root) return PERSIMM_ERR_ALLOC;
        persimm_node_children(root)[0] = vector->root;
        vector->root = root;
        vector->shift += PERSIMM_BITS;
    } else if (immutable) {
        persimm_node_t *root = persimm_node_make_unique(vector->root, PERSIMM_WIDTH,
                                                        vector->elem_size, vector->ops, vector->ctx);
        if (NULL == root) return PERSIMM_ERR_ALLOC;
        vector->root = root;
    }

    size_t index = old_count - PERSIMM_WIDTH;
    persimm_node_t *node = vector->root;
    for (size_t level = vector->shift; level > PERSIMM_BITS; level -= PERSIMM_BITS) {
        size_t curr_index = (index >> level) & PERSIMM_MASK;
        persimm_node_t *child = persimm_node_children(node)[curr_index];
        if (NULL == child) {
            child = persimm_node_new(PERSIMM_NODE_INNER, vector->elem_size);
            if (NULL == child) return PERSIMM_ERR_ALLOC;
        } else if (immutable) {
            child = persimm_node_make_unique(child, PERSIMM_WIDTH, vector->elem_size, vector->ops,
                                             vector->ctx);
            if (NULL == child) return PERSIMM_ERR_ALLOC;
        }
        persimm_node_children(node)[curr_index] = child;
        node = child;
    }
    persimm_node_children(node)[(index >> PERSIMM_BITS) & PERSIMM_MASK] = tail;

    return PERSIMM_OK;
}

persimm_status persimm_vector_push(persimm_vector_t *vector, const void *elem, bool immutable) {
    if (NULL == vector->tail) return PERSIMM_ERR_CORRUPT;

    if (vector->tail_count < PERSIMM_WIDTH) {
        if (immutable) {
            persimm_node_t *tail = persimm_node_make_unique(vector->tail, vector->tail_count,
                                                            vector->elem_size, vector->ops,
                                                            vector->ctx);
            if (NULL == tail) return PERSIMM_ERR_ALLOC;
            vector->tail = tail;
        }
        persimm_elem_store(vector, persimm_node_slot(vector->tail, vector->tail_count,
                                                     vector->elem_size), elem);
        vector->tail_count++;
        vector->count++;
        return PERSIMM_OK;
    }

    persimm_node_t *tail = persimm_node_new(PERSIMM_NODE_LEAF, vector->elem_size);
    if (NULL == tail) return PERSIMM_ERR_ALLOC;

    persimm_node_t *old_tail = vector->tail;
    persimm_status status = persimm_vector_graft(vector, old_tail, vector->count, immutable);
    if (PERSIMM_OK != status) {
        free(tail);
        return status;
    }

    vector->tail = tail;
    vector->tail_count = 0;
    persimm_elem_store(vector, persimm_node_slot(tail, 0, vector->elem_size), elem);
    vector->tail_count = 1;
    vector->count++;

    return PERSIMM_OK;
}

persimm_status persimm_vector_update(persimm_vector_t *vector, size_t index, const void *elem,
                                     bool immutable) {
    if (index >= vector->count) return PERSIMM_ERR_BOUNDS;

    size_t tail_offset = vector->count - vector->tail_count;
    if (index >= tail_offset) {
        if (immutable) {
            persimm_node_t *tail = persimm_node_make_unique(vector->tail, vector->tail_count,
                                                            vector->elem_size, vector->ops,
                                                            vector->ctx);
            if (NULL == tail) return PERSIMM_ERR_ALLOC;
            vector->tail = tail;
        }
        void *slot = persimm_node_slot(vector->tail, index - tail_offset, vector->elem_size);
        persimm_elem_release(vector->ops, vector->ctx, slot);
        persimm_elem_store(vector, slot, elem);
        return PERSIMM_OK;
    }

    if (NULL == vector->root) return PERSIMM_ERR_CORRUPT;

    if (immutable) {
        persimm_node_t *root = persimm_node_make_unique(vector->root, PERSIMM_WIDTH,
                                                        vector->elem_size, vector->ops, vector->ctx);
        if (NULL == root) return PERSIMM_ERR_ALLOC;
        vector->root = root;
    }

    persimm_node_t *node = vector->root;
    for (size_t level = vector->shift; level > 0; level -= PERSIMM_BITS) {
        size_t curr_index = (index >> level) & PERSIMM_MASK;
        persimm_node_t *child = persimm_node_children(node)[curr_index];
        if (NULL == child) return PERSIMM_ERR_CORRUPT;
        if (immutable) {
            child = persimm_node_make_unique(child, PERSIMM_WIDTH, vector->elem_size, vector->ops,
                                             vector->ctx);
            if (NULL == child) return PERSIMM_ERR_ALLOC;
            persimm_node_children(node)[curr_index] = child;
        }
        node = child;
    }

    void *slot = persimm_node_slot(node, index & PERSIMM_MASK, vector->elem_size);
    persimm_elem_release(vector->ops, vector->ctx, slot);
    persimm_elem_store(vector, slot, elem);

    return PERSIMM_OK;
}

/* Traversing */

static void persimm_node_foreach(persimm_node_t *node, size_t elem_size, size_t *index,
                                 size_t limit, persimm_visit_fn fn, void *ctx) {
    if (NULL == node || *index >= limit) return;

    if (node->kind == PERSIMM_NODE_LEAF) {
        for (size_t i = 0; i < PERSIMM_WIDTH && *index < limit; i++) {
            fn(persimm_node_slot(node, i, elem_size), *index, ctx);
            (*index)++;
        }
        return;
    }

    persimm_node_t **children = persimm_node_children(node);
    for (size_t i = 0; i < PERSIMM_WIDTH && *index < limit; i++) {
        persimm_node_foreach(children[i], elem_size, index, limit, fn, ctx);
    }
}

void persimm_vector_foreach(const persimm_vector_t *vector, persimm_visit_fn fn, void *ctx) {
    size_t index = 0;
    size_t tail_offset = vector->count - vector->tail_count;

    persimm_node_foreach(vector->root, vector->elem_size, &index, tail_offset, fn, ctx);

    for (size_t i = 0; i < vector->tail_count; i++) {
        fn(persimm_node_slot(vector->tail, i, vector->elem_size), tail_offset + i, ctx);
    }
}

static void persimm_trace_visit(void *slot, size_t index, void *ctx) {
    (void) index;
    const persimm_vector_t *vector = (const persimm_vector_t *)ctx;
    vector->ops->trace(slot, vector->ctx);
}

void persimm_vector_trace(const persimm_vector_t *vector) {
    if (NULL == vector->ops || NULL == vector->ops->trace) return;
    persimm_vector_foreach(vector, persimm_trace_visit, (void *)vector);
}
