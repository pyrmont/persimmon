#include <stdbool.h>
#include "persimmon.h"

#define BITS 5
#define WIDTH (1 << BITS) // 2^5 = 32
#define MASK (WIDTH - 1) // 31, or 0x1f

/* Types */

typedef enum {
    PERSIMM_NODE_INNER,
    PERSIMM_NODE_LEAF
} persimm_node_type;

typedef struct {
    persimm_node_type kind;
    size_t ref_count;
    void* items[WIDTH];
} persimm_node_t;

typedef struct {
    size_t shift;
    size_t count;
    size_t tail_count;
    persimm_node_t *root;
    persimm_node_t *tail;
} persimm_vector_t;

/* Forward Declarations */

static void persimm_vector_push(persimm_vector_t *vector, Janet *item, bool immutable);
static Janet persimm_vector_get_at_index(persimm_vector_t *vector, size_t index);

/* Utility Methods */

static int persimm_vector_index(persimm_vector_t *vector, Janet input, size_t *index) {
    if (!janet_checktype(input, JANET_NUMBER)) janet_panic("expected index as number");

    int32_t vector_length = (int32_t)vector->count;

    int32_t input_index = janet_unwrap_integer(input);
    if (janet_unwrap_number(input) - (double)input_index != 0) janet_panic("expected index as integer");
    if ((vector_length + input_index) < 0 || input_index >= vector_length) return 0;

    *index = (input_index < 0) ? (vector_length + input_index) : input_index;
    return 1;
}

static void persimm_vector_seed(persimm_vector_t *vector, Janet coll) {
    if (janet_checktypes(coll, JANET_TFLAG_INDEXED)) {
        JanetView view;
        janet_indexed_view(coll, &view.items, &view.len);
        for (size_t i = 0; i < (size_t)view.len; i++) {
            persimm_vector_push(vector, (Janet *)&view.items[i], false);
        }
    } else if (janet_checktypes(coll, JANET_TFLAG_DICTIONARY)) {
        janet_panic("cannot seed with dictionary");
    } else {
        janet_panic("cannot seed with this type");
    }
}

/* Deinitilising */

static void persimm_node_free(persimm_node_t *node) {
    if (NULL == node) return;

    if (node->ref_count > 1) {
        node->ref_count -= 1;
        return;
    }

    // This could be optimised
    if (node->kind == PERSIMM_NODE_INNER) {
        for (size_t i = 0; i < WIDTH; i++) {
            if (NULL == node->items[i]) continue;
            persimm_node_free((persimm_node_t *)node->items[i]);
            node->items[i] = NULL;
        }
    }

    free(node);
}

static void persimm_vector_deinit(persimm_vector_t *vector) {
    persimm_node_free(vector->root);
    persimm_node_free(vector->tail);
}

static int persimm_vector_gc(void *p, size_t size) {
    (void) size;
    persimm_vector_t *vector = (persimm_vector_t *)p;
    persimm_vector_deinit(vector);
    return 0;
}

/* Initialising  */

static void persimm_vector_clone(persimm_vector_t *old_v, persimm_vector_t *new_v) {
    new_v->shift = old_v->shift;
    new_v->count = old_v->count;
    new_v->tail_count = old_v->tail_count;
    new_v->root = old_v->root;
    if (NULL != old_v->root) old_v->root->ref_count++;
    new_v->tail = old_v->tail;
    if (NULL != old_v->tail) old_v->tail->ref_count++;
}

static persimm_node_t *persimm_vector_copy_node(persimm_node_t *node) {
    persimm_node_t *copy = malloc(sizeof(persimm_node_t));
    copy->kind = node->kind;
    copy->ref_count = 1;
    for (size_t i = 0; i < WIDTH; i++) {
        copy->items[i] = node->items[i];
        if (copy->kind == PERSIMM_NODE_INNER && NULL != copy->items[i]) {
            ((persimm_node_t *)copy->items[i])->ref_count++;
        }
    }

    if (node->ref_count > 1) {
        node->ref_count--;
    } else {
        persimm_node_free(node);
    }

    return copy;
}

static persimm_node_t *persimm_vector_new_node(persimm_node_type kind) {
    persimm_node_t *node = malloc(sizeof(persimm_node_t));
    node->kind = kind;
    node->ref_count = 1;
    for (size_t i = 0; i < WIDTH; i++) {
        node->items[i] = NULL;
    }
    return node;
}

static void persimm_vector_init(persimm_vector_t *vector) {
    vector->shift = 0;
    vector->count = 0;
    vector->tail_count = 0;
    vector->root = NULL;
    vector->tail = persimm_vector_new_node(PERSIMM_NODE_LEAF);
}

/* Marking */

static int persimm_vector_mark(void *p, size_t size) {
    (void) size;
    persimm_vector_t *vector = (persimm_vector_t *)p;
    for (size_t i = 0; i < vector->count; i++) {
        Janet value = persimm_vector_get_at_index(vector, i);
        janet_mark(value);
    }
    return 0;
}

/* Accessing */

static JanetMethod persimm_vector_methods[2];

static Janet persimm_vector_get_at_index(persimm_vector_t *vector, size_t index) {
    size_t tail_offset = vector->count - vector->tail_count;
    if (index >= tail_offset) {
        return *(Janet *)vector->tail->items[index - tail_offset];
    }

    persimm_node_t *node = vector->root;
    for (size_t level = vector->shift; level > 0; level -= BITS) {
        size_t curr_index = (index >> level) & MASK;
        if (NULL == node->items[curr_index]) {
            janet_panic("invalid index");
        }
        node = (persimm_node_t *)node->items[curr_index];
    }

    return *(Janet *)node->items[index & MASK];
}

static int persimm_vector_get(void *p, Janet key, Janet *out) {
    if (janet_checktype(key, JANET_KEYWORD)) {
        return janet_getmethod(janet_unwrap_keyword(key), persimm_vector_methods, out);
    }

    persimm_vector_t *vector = (persimm_vector_t *)p;

    size_t index;
    if (!persimm_vector_index(vector, key, &index)) return 0;

    Janet val = persimm_vector_get_at_index(vector, index);
    *out = val;
    return 1;
}

/* Inserting */

static void persimm_vector_push(persimm_vector_t *vector, Janet *item, bool immutable) {
    if (vector->tail_count < WIDTH) {
        if (immutable) vector->tail = persimm_vector_copy_node(vector->tail);
        vector->tail->items[vector->tail_count] = item;
        vector->tail_count++;
        vector->count++;
        return;
    }

    persimm_node_t *old_tail = vector->tail;
    persimm_node_t *new_tail = persimm_vector_new_node(PERSIMM_NODE_LEAF);
    vector->tail = new_tail;
    vector->tail_count = 0;
    new_tail->items[vector->tail_count] = item;
    vector->tail_count++;
    vector->count++;

    size_t old_count = vector->count - 1;
    if (old_count == WIDTH) {
        vector->root = old_tail;
        return;
    }

    if ((old_count >> BITS) > (size_t)(1 << vector->shift)) {
        persimm_node_t *old_root = vector->root;
        persimm_node_t *new_root = persimm_vector_new_node(PERSIMM_NODE_INNER);
        vector->shift += BITS;
        vector->root = new_root;
        new_root->items[0] = old_root;
    } else {
        if (immutable) vector->root = persimm_vector_copy_node(vector->root);
    }

    size_t index = old_count - WIDTH;
    persimm_node_t *node = vector->root;
    for (size_t level = vector->shift; level > BITS; level -= BITS) {
        size_t curr_index = (index >> level) & MASK;
        if (NULL == node->items[curr_index]) {
            node->items[curr_index] = persimm_vector_new_node(PERSIMM_NODE_INNER);
        }
        if (immutable) {
            persimm_node_t *child = (persimm_node_t *)node->items[curr_index];
            node->items[curr_index] = persimm_vector_copy_node(child);
        }
        node = (persimm_node_t *)node->items[curr_index];
    }
    node->items[(index >> BITS) & MASK] = old_tail;
}

static void persimm_vector_update(persimm_vector_t *vector, size_t index, Janet *item, bool immutable) {
    if (index >= (vector->count - vector->tail_count)) {
        if (immutable) vector->tail = persimm_vector_copy_node(vector->tail);
        vector->tail->items[index] = item;
        return;
    }

    if (immutable) vector->root = persimm_vector_copy_node(vector->root);

    persimm_node_t *node = vector->root;
    for (size_t level = vector->shift; level > 0; level -= BITS) {
        size_t curr_index = (index >> level) & MASK;
        if (NULL == node->items[curr_index]) {
            node->items[curr_index] = persimm_vector_new_node(PERSIMM_NODE_INNER);
        }
        if (immutable) {
            persimm_node_t *child = (persimm_node_t *)node->items[curr_index];
            node->items[curr_index] = persimm_vector_copy_node(child);
        }
        node = (persimm_node_t *)node->items[curr_index];
    }
    node->items[index & MASK] = item;
}

/* Stringifying */

static void persimm_vector_to_string(void *p, JanetBuffer *buf) {
    persimm_vector_t *vector = (persimm_vector_t *)p;
    janet_buffer_push_cstring(buf, "[");
    for (size_t i = 0; i < vector->count; i++) {
        if (i > 0) janet_buffer_push_cstring(buf, " ");
        Janet val = persimm_vector_get_at_index(vector, i);
        janet_buffer_push_string(buf, janet_to_string(val));
    }
    janet_buffer_push_cstring(buf, "]");
}

/* Comparing */

static int persimm_vector_compare(void *p1, void *p2) {
    persimm_vector_t *a = (persimm_vector_t *)p1;
    persimm_vector_t *b = (persimm_vector_t *)p2;
    return a == b;
}

/* Traversing */

static Janet persimm_vector_next(void *p, Janet key) {
    persimm_vector_t *vector = (persimm_vector_t *)p;

    if (janet_checktype(key, JANET_NIL)) {
        if (vector->count > 0) {
            return janet_wrap_number(0);
        } else {
            return janet_wrap_nil();
        }
    }

    if (!janet_checksize(key)) janet_panic("expected size as key");
    size_t index = (size_t)janet_unwrap_integer(key);
    index++;

    if (index < vector->count) {
        return janet_wrap_number((double)index);
    } else {
        return janet_wrap_nil();
    }
}

/* Type Definition */

static const JanetAbstractType persimm_vector_type = {
    "persimmon/vector",
    persimm_vector_gc,
    persimm_vector_mark, /* GC Mark */
    persimm_vector_get, /* Get */
    NULL, /* Set */
    NULL, /* Marshall */
    NULL, /* Unmarshall */
    persimm_vector_to_string, /* String */
    persimm_vector_compare, /* Compare */
    NULL, /* Hash */
    persimm_vector_next, /* Next */
    JANET_ATEND_NEXT
};

/* C Functions */

static Janet cfun_persimm_vec(int32_t argc, Janet *argv) {
    janet_arity(argc, 0, 1);

    persimm_vector_t *vector = (persimm_vector_t *)janet_abstract(&persimm_vector_type, sizeof(persimm_vector_t));
    persimm_vector_init(vector);

    if (1 == argc) {
        persimm_vector_seed(vector, argv[0]);
    }

    return janet_wrap_abstract(vector);
}

static Janet cfun_persimm_assoc(int32_t argc, Janet *argv) {
    janet_fixarity(argc, 3);

    persimm_vector_t *old_vector = (persimm_vector_t *)janet_getabstract(argv, 0, &persimm_vector_type);

    size_t index;
    if (!persimm_vector_index(old_vector, argv[1], &index)) janet_panic("index out of bounds");

    Janet *item = malloc(sizeof(Janet));
    memcpy(item, argv + 2, sizeof(Janet));

    persimm_vector_t *new_vector = (persimm_vector_t *)janet_abstract(&persimm_vector_type, sizeof(persimm_vector_t));
    persimm_vector_clone(old_vector, new_vector);

    persimm_vector_update(new_vector, index, item, true);

    return janet_wrap_abstract(new_vector);
}

static Janet cfun_persimm_conj(int32_t argc, Janet *argv) {
    janet_fixarity(argc, 2);

    persimm_vector_t *old_vector = (persimm_vector_t *)janet_getabstract(argv, 0, &persimm_vector_type);
    persimm_vector_t *new_vector = (persimm_vector_t *)janet_abstract(&persimm_vector_type, sizeof(persimm_vector_t));
    persimm_vector_clone(old_vector, new_vector);

    Janet *item = malloc(sizeof(Janet));
    memcpy(item, argv + 1, sizeof(Janet));

    persimm_vector_push(new_vector, item, true);

    return janet_wrap_abstract(new_vector);
}

static Janet cfun_persimm_to_array(int32_t argc, Janet *argv) {
    janet_fixarity(argc, 1);

    persimm_vector_t *vector = (persimm_vector_t *)janet_getabstract(argv, 0, &persimm_vector_type);

    JanetArray *array = janet_array(vector->count);
    for (size_t i = 0; i < vector->count; i++) {
        janet_array_push(array, persimm_vector_get_at_index(vector, i));
    }

    return janet_wrap_array(array);
}

static const JanetReg cfuns[] = {
    {"vec", cfun_persimm_vec, NULL},
    {"assoc", cfun_persimm_assoc, NULL},
    {"conj", cfun_persimm_conj, NULL},
    {"to-array", cfun_persimm_to_array, NULL},
    {NULL, NULL, NULL}
};

/* Methods */

static Janet persimm_vector_method_length(int32_t argc, Janet *argv) {
    janet_fixarity(argc, 1);
    persimm_vector_t *vector = (persimm_vector_t *)janet_getabstract(argv, 0, &persimm_vector_type);
    return janet_wrap_number(vector->count);
}

static JanetMethod persimm_vector_methods[] = {
    {"length", persimm_vector_method_length},
    {NULL, NULL}
};

/* Environment Registration */

void persimm_register_type(JanetTable *env) {
    (void) env;
    janet_register_abstract_type(&persimm_vector_type);
}

void persimm_register_functions(JanetTable *env) {
    janet_cfuns(env, "persimmon", cfuns);
}

JANET_MODULE_ENTRY(JanetTable *env) {
    persimm_register_type(env);
    persimm_register_functions(env);
}
