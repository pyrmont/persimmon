#include "persimmon_janet.h"
#include "persimmon.h"

/*
 * The Janet binding. Elements are Janet values stored inline, so no reference
 * counting is needed: the collector traces them instead.
 */

/* Elements */

static void janet_persimm_trace(void *slot, void *ctx) {
    (void) ctx;
    janet_mark(*(Janet *)slot);
}

static const persimm_elem_ops janet_persimm_ops = {
    NULL, /* Retain */
    NULL, /* Release */
    janet_persimm_trace
};

/* Utility Methods */

static void janet_persimm_check(persimm_status status) {
    if (PERSIMM_OK != status) janet_panic(persimm_status_string(status));
}

static size_t janet_persimm_index(persimm_vector_t *vector, Janet input) {
    if (!janet_checktype(input, JANET_NUMBER)) janet_panic("expected index as number");

    int32_t value = janet_unwrap_integer(input);
    if (janet_unwrap_number(input) - (double)value != 0) janet_panic("expected index as integer");

    size_t index;
    if (!persimm_vector_index(vector, value, &index)) janet_panic("index out of bounds");

    return index;
}

static Janet janet_persimm_get_at_index(persimm_vector_t *vector, size_t index) {
    void *slot = persimm_vector_ref(vector, index);
    if (NULL == slot) janet_panic("invalid index");
    return *(Janet *)slot;
}

static void janet_persimm_seed(persimm_vector_t *vector, Janet coll) {
    if (janet_checktypes(coll, JANET_TFLAG_INDEXED)) {
        JanetView view;
        janet_indexed_view(coll, &view.items, &view.len);
        for (size_t i = 0; i < (size_t)view.len; i++) {
            janet_persimm_check(persimm_vector_push(vector, &view.items[i], false));
        }
    } else if (janet_checktypes(coll, JANET_TFLAG_DICTIONARY)) {
        janet_panic("cannot seed with dictionary");
    } else {
        janet_panic("cannot seed with this type");
    }
}

/* Deinitilising */

static int janet_persimm_gc(void *p, size_t size) {
    (void) size;
    persimm_vector_deinit((persimm_vector_t *)p);
    return 0;
}

/* Marking */

static int janet_persimm_mark(void *p, size_t size) {
    (void) size;
    persimm_vector_trace((persimm_vector_t *)p);
    return 0;
}

/* Accessing */

static JanetMethod persimm_vector_methods[2];

static int janet_persimm_get(void *p, Janet key, Janet *out) {
    if (janet_checktype(key, JANET_KEYWORD)) {
        return janet_getmethod(janet_unwrap_keyword(key), persimm_vector_methods, out);
    }

    persimm_vector_t *vector = (persimm_vector_t *)p;

    if (!janet_checktype(key, JANET_NUMBER)) janet_panic("expected index as number");
    int32_t value = janet_unwrap_integer(key);
    if (janet_unwrap_number(key) - (double)value != 0) janet_panic("expected index as integer");

    size_t index;
    if (!persimm_vector_index(vector, value, &index)) return 0;

    *out = janet_persimm_get_at_index(vector, index);
    return 1;
}

/* Stringifying */

static void janet_persimm_to_string_visit(void *slot, size_t index, void *ctx) {
    JanetBuffer *buf = (JanetBuffer *)ctx;
    if (index > 0) janet_buffer_push_cstring(buf, " ");
    janet_buffer_push_string(buf, janet_to_string(*(Janet *)slot));
}

static void janet_persimm_to_string(void *p, JanetBuffer *buf) {
    janet_buffer_push_cstring(buf, "[");
    persimm_vector_foreach((persimm_vector_t *)p, janet_persimm_to_string_visit, buf);
    janet_buffer_push_cstring(buf, "]");
}

/* Comparing */

static int janet_persimm_compare(void *p1, void *p2) {
    if (p1 == p2) return 0;
    return (p1 < p2) ? -1 : 1;
}

/* Hashing */

static void janet_persimm_hash_visit(void *slot, size_t index, void *ctx) {
    (void) index;
    uint32_t *hash = (uint32_t *)ctx;
    *hash = (*hash << 5) + *hash + (uint32_t)janet_hash(*(Janet *)slot);
}

static int32_t janet_persimm_hash(void *p, size_t size) {
    (void) size;
    uint32_t hash = 5381;
    persimm_vector_foreach((persimm_vector_t *)p, janet_persimm_hash_visit, &hash);
    return (int32_t)hash;
}

/* Traversing */

static Janet janet_persimm_next(void *p, Janet key) {
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
    janet_persimm_gc,
    janet_persimm_mark, /* GC Mark */
    janet_persimm_get, /* Get */
    NULL, /* Set */
    NULL, /* Marshall */
    NULL, /* Unmarshall */
    janet_persimm_to_string, /* String */
    janet_persimm_compare, /* Compare */
    janet_persimm_hash, /* Hash */
    janet_persimm_next, /* Next */
    JANET_ATEND_NEXT
};

/* C Functions */

static persimm_vector_t *janet_persimm_new(void) {
    persimm_vector_t *vector =
        (persimm_vector_t *)janet_abstract(&persimm_vector_type, sizeof(persimm_vector_t));
    janet_persimm_check(persimm_vector_init(vector, sizeof(Janet), &janet_persimm_ops, NULL));
    return vector;
}

static Janet cfun_persimm_vec(int32_t argc, Janet *argv) {
    janet_arity(argc, 0, 1);

    persimm_vector_t *vector = janet_persimm_new();

    if (1 == argc) {
        janet_persimm_seed(vector, argv[0]);
    }

    return janet_wrap_abstract(vector);
}

static Janet cfun_persimm_assoc(int32_t argc, Janet *argv) {
    janet_fixarity(argc, 3);

    persimm_vector_t *old_vector = (persimm_vector_t *)janet_getabstract(argv, 0, &persimm_vector_type);
    size_t index = janet_persimm_index(old_vector, argv[1]);

    persimm_vector_t *new_vector =
        (persimm_vector_t *)janet_abstract(&persimm_vector_type, sizeof(persimm_vector_t));
    persimm_vector_clone(old_vector, new_vector);

    janet_persimm_check(persimm_vector_update(new_vector, index, argv + 2, true));

    return janet_wrap_abstract(new_vector);
}

static Janet cfun_persimm_conj(int32_t argc, Janet *argv) {
    janet_fixarity(argc, 2);

    persimm_vector_t *old_vector = (persimm_vector_t *)janet_getabstract(argv, 0, &persimm_vector_type);

    persimm_vector_t *new_vector =
        (persimm_vector_t *)janet_abstract(&persimm_vector_type, sizeof(persimm_vector_t));
    persimm_vector_clone(old_vector, new_vector);

    janet_persimm_check(persimm_vector_push(new_vector, argv + 1, true));

    return janet_wrap_abstract(new_vector);
}

static void janet_persimm_to_array_visit(void *slot, size_t index, void *ctx) {
    (void) index;
    janet_array_push((JanetArray *)ctx, *(Janet *)slot);
}

static Janet cfun_persimm_to_array(int32_t argc, Janet *argv) {
    janet_fixarity(argc, 1);

    persimm_vector_t *vector = (persimm_vector_t *)janet_getabstract(argv, 0, &persimm_vector_type);

    JanetArray *array = janet_array((int32_t)vector->count);
    persimm_vector_foreach(vector, janet_persimm_to_array_visit, array);

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
    return janet_wrap_number((double)vector->count);
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
