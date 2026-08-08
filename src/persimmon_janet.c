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

static int64_t janet_persimm_integer(Janet input) {
    if (!janet_checktype(input, JANET_NUMBER)) janet_panic("expected index as number");
    int32_t value = janet_unwrap_integer(input);
    if (janet_unwrap_number(input) - (double)value != 0) janet_panic("expected index as integer");
    return value;
}

static void janet_persimm_to_array_visit(void *slot, size_t index, void *ctx) {
    (void) index;
    janet_array_push((JanetArray *)ctx, *(Janet *)slot);
}

static void janet_persimm_to_string_visit(void *slot, size_t index, void *ctx) {
    JanetBuffer *buf = (JanetBuffer *)ctx;
    if (index > 0) janet_buffer_push_cstring(buf, " ");
    janet_buffer_push_string(buf, janet_to_string(*(Janet *)slot));
}

static void janet_persimm_hash_visit(void *slot, size_t index, void *ctx) {
    (void) index;
    uint32_t *hash = (uint32_t *)ctx;
    *hash = (*hash << 5) + *hash + (uint32_t)janet_hash(*(Janet *)slot);
}

/*
 * Identity comparison. Ordering distinct structures by address is arbitrary
 * but consistent, which is what the protocol asks for.
 */
static int janet_persimm_compare(void *p1, void *p2) {
    if (p1 == p2) return 0;
    return (p1 < p2) ? -1 : 1;
}

/*
 * Shared by both types: `count` occupies the same position in each struct, but
 * relying on that would be a trap, so each caller passes its own count.
 */
static Janet janet_persimm_next_index(size_t count, Janet key) {
    if (janet_checktype(key, JANET_NIL)) {
        if (count > 0) {
            return janet_wrap_number(0);
        } else {
            return janet_wrap_nil();
        }
    }

    if (!janet_checksize(key)) janet_panic("expected size as key");
    size_t index = (size_t)janet_unwrap_integer(key);
    index++;

    if (index < count) {
        return janet_wrap_number((double)index);
    } else {
        return janet_wrap_nil();
    }
}

/* Vectors */

static JanetMethod persimm_vector_methods[2];

static int janet_persimm_vector_gc(void *p, size_t size) {
    (void) size;
    persimm_vector_deinit((persimm_vector_t *)p);
    return 0;
}

static int janet_persimm_vector_mark(void *p, size_t size) {
    (void) size;
    persimm_vector_trace((persimm_vector_t *)p);
    return 0;
}

static Janet janet_persimm_vector_at(persimm_vector_t *vector, size_t index) {
    void *slot = persimm_vector_ref(vector, index);
    if (NULL == slot) janet_panic("invalid index");
    return *(Janet *)slot;
}

static int janet_persimm_vector_get(void *p, Janet key, Janet *out) {
    if (janet_checktype(key, JANET_KEYWORD)) {
        return janet_getmethod(janet_unwrap_keyword(key), persimm_vector_methods, out);
    }

    persimm_vector_t *vector = (persimm_vector_t *)p;

    size_t index;
    if (!persimm_vector_index(vector, janet_persimm_integer(key), &index)) return 0;

    *out = janet_persimm_vector_at(vector, index);
    return 1;
}

static void janet_persimm_vector_to_string(void *p, JanetBuffer *buf) {
    janet_buffer_push_cstring(buf, "[");
    persimm_vector_foreach((persimm_vector_t *)p, janet_persimm_to_string_visit, buf);
    janet_buffer_push_cstring(buf, "]");
}

static int32_t janet_persimm_vector_hash(void *p, size_t size) {
    (void) size;
    uint32_t hash = 5381;
    persimm_vector_foreach((persimm_vector_t *)p, janet_persimm_hash_visit, &hash);
    return (int32_t)hash;
}

static Janet janet_persimm_vector_next(void *p, Janet key) {
    return janet_persimm_next_index(((persimm_vector_t *)p)->count, key);
}

static size_t janet_persimm_vector_length(void *p, size_t size) {
    (void) size;
    return ((persimm_vector_t *)p)->count;
}

static const JanetAbstractType persimm_vector_type = {
    "persimmon/vector",
    janet_persimm_vector_gc,
    janet_persimm_vector_mark, /* GC Mark */
    janet_persimm_vector_get, /* Get */
    NULL, /* Set */
    NULL, /* Marshall */
    NULL, /* Unmarshall */
    janet_persimm_vector_to_string, /* String */
    janet_persimm_compare, /* Compare */
    janet_persimm_vector_hash, /* Hash */
    janet_persimm_vector_next, /* Next */
    NULL, /* Call */
    janet_persimm_vector_length, /* Length */
    JANET_ATEND_LENGTH
};

static Janet persimm_vector_method_length(int32_t argc, Janet *argv) {
    janet_fixarity(argc, 1);
    persimm_vector_t *vector = (persimm_vector_t *)janet_getabstract(argv, 0, &persimm_vector_type);
    return janet_wrap_number((double)vector->count);
}

static JanetMethod persimm_vector_methods[] = {
    {"length", persimm_vector_method_length},
    {NULL, NULL}
};

/* Lists */

/*
 * Each list carries a cursor so that `get` can resume where it last stopped.
 * Janet iterates an abstract by asking `next` for a key and then `get` to
 * resolve it, which without a cursor would walk the chain from the head once
 * per element. Nothing here needs a lock: a list has no marshaller, so it
 * cannot reach a second Janet VM, and it never changes once handed out.
 */
typedef struct {
    persimm_list_t list;
    persimm_list_cursor_t cursor;
} janet_persimm_list_t;

static JanetMethod persimm_list_methods[2];

static int janet_persimm_list_gc(void *p, size_t size) {
    (void) size;
    persimm_list_deinit(&((janet_persimm_list_t *)p)->list);
    return 0;
}

static int janet_persimm_list_mark(void *p, size_t size) {
    (void) size;
    persimm_list_trace(&((janet_persimm_list_t *)p)->list);
    return 0;
}

static int janet_persimm_list_get(void *p, Janet key, Janet *out) {
    if (janet_checktype(key, JANET_KEYWORD)) {
        return janet_getmethod(janet_unwrap_keyword(key), persimm_list_methods, out);
    }

    janet_persimm_list_t *wrapper = (janet_persimm_list_t *)p;

    size_t index;
    if (!persimm_list_index(&wrapper->list, janet_persimm_integer(key), &index)) return 0;

    void *slot = persimm_list_ref_from(&wrapper->list, &wrapper->cursor, index);
    if (NULL == slot) janet_panic("invalid index");

    *out = *(Janet *)slot;
    return 1;
}

static void janet_persimm_list_to_string(void *p, JanetBuffer *buf) {
    janet_buffer_push_cstring(buf, "(");
    persimm_list_foreach(&((janet_persimm_list_t *)p)->list, janet_persimm_to_string_visit, buf);
    janet_buffer_push_cstring(buf, ")");
}

static int32_t janet_persimm_list_hash(void *p, size_t size) {
    (void) size;
    uint32_t hash = 5381;
    persimm_list_foreach(&((janet_persimm_list_t *)p)->list, janet_persimm_hash_visit, &hash);
    return (int32_t)hash;
}

static Janet janet_persimm_list_next(void *p, Janet key) {
    return janet_persimm_next_index(((janet_persimm_list_t *)p)->list.count, key);
}

static size_t janet_persimm_list_length(void *p, size_t size) {
    (void) size;
    return ((janet_persimm_list_t *)p)->list.count;
}

static const JanetAbstractType persimm_list_type = {
    "persimmon/list",
    janet_persimm_list_gc,
    janet_persimm_list_mark, /* GC Mark */
    janet_persimm_list_get, /* Get */
    NULL, /* Set */
    NULL, /* Marshall */
    NULL, /* Unmarshall */
    janet_persimm_list_to_string, /* String */
    janet_persimm_compare, /* Compare */
    janet_persimm_list_hash, /* Hash */
    janet_persimm_list_next, /* Next */
    NULL, /* Call */
    janet_persimm_list_length, /* Length */
    JANET_ATEND_LENGTH
};

static Janet persimm_list_method_length(int32_t argc, Janet *argv) {
    janet_fixarity(argc, 1);
    janet_persimm_list_t *wrapper =
        (janet_persimm_list_t *)janet_getabstract(argv, 0, &persimm_list_type);
    return janet_wrap_number((double)wrapper->list.count);
}

static JanetMethod persimm_list_methods[] = {
    {"length", persimm_list_method_length},
    {NULL, NULL}
};

/* Constructing */

static persimm_vector_t *janet_persimm_new_vector(void) {
    persimm_vector_t *vector =
        (persimm_vector_t *)janet_abstract(&persimm_vector_type, sizeof(persimm_vector_t));
    janet_persimm_check(persimm_vector_init(vector, sizeof(Janet), &janet_persimm_ops, NULL));
    return vector;
}

/*
 * janet_abstract hands back uninitialised storage, so the cursor is reset
 * before anything can consult it. Both constructors go through here for that
 * reason.
 */
static janet_persimm_list_t *janet_persimm_alloc_list(void) {
    janet_persimm_list_t *wrapper =
        (janet_persimm_list_t *)janet_abstract(&persimm_list_type, sizeof(janet_persimm_list_t));
    persimm_list_cursor_reset(&wrapper->cursor);
    return wrapper;
}

static janet_persimm_list_t *janet_persimm_new_list(void) {
    janet_persimm_list_t *wrapper = janet_persimm_alloc_list();
    janet_persimm_check(persimm_list_init(&wrapper->list, sizeof(Janet), &janet_persimm_ops, NULL));
    return wrapper;
}

static janet_persimm_list_t *janet_persimm_clone_list(const persimm_list_t *src) {
    janet_persimm_list_t *wrapper = janet_persimm_alloc_list();
    persimm_list_clone(src, &wrapper->list);
    return wrapper;
}

static void janet_persimm_view(Janet coll, JanetView *view) {
    if (janet_checktypes(coll, JANET_TFLAG_INDEXED)) {
        janet_indexed_view(coll, &view->items, &view->len);
    } else if (janet_checktypes(coll, JANET_TFLAG_DICTIONARY)) {
        janet_panic("cannot seed with dictionary");
    } else {
        janet_panic("cannot seed with this type");
    }
}

/* C Functions */

static Janet cfun_persimm_vec(int32_t argc, Janet *argv) {
    janet_arity(argc, 0, 1);

    persimm_vector_t *vector = janet_persimm_new_vector();

    if (1 == argc) {
        JanetView view;
        janet_persimm_view(argv[0], &view);
        for (int32_t i = 0; i < view.len; i++) {
            janet_persimm_check(persimm_vector_push(vector, &view.items[i], false));
        }
    }

    return janet_wrap_abstract(vector);
}

static Janet cfun_persimm_list(int32_t argc, Janet *argv) {
    janet_arity(argc, 0, 1);

    janet_persimm_list_t *wrapper = janet_persimm_new_list();

    if (1 == argc) {
        JanetView view;
        janet_persimm_view(argv[0], &view);
        /* Consing walks the collection backwards so the list reads in the
           same order as the collection it came from. */
        for (int32_t i = view.len - 1; i >= 0; i--) {
            janet_persimm_check(persimm_list_cons(&wrapper->list, &view.items[i]));
        }
    }

    return janet_wrap_abstract(wrapper);
}

/*
 * As in Clojure, conj adds an element wherever the structure can take one
 * cheapest: the end of a vector, the front of a list.
 */
static Janet cfun_persimm_conj(int32_t argc, Janet *argv) {
    janet_fixarity(argc, 2);

    if (janet_checkabstract(argv[0], &persimm_vector_type)) {
        persimm_vector_t *old_vector = (persimm_vector_t *)janet_unwrap_abstract(argv[0]);
        persimm_vector_t *new_vector =
            (persimm_vector_t *)janet_abstract(&persimm_vector_type, sizeof(persimm_vector_t));
        persimm_vector_clone(old_vector, new_vector);
        janet_persimm_check(persimm_vector_push(new_vector, argv + 1, true));
        return janet_wrap_abstract(new_vector);
    }

    if (janet_checkabstract(argv[0], &persimm_list_type)) {
        janet_persimm_list_t *old_list = (janet_persimm_list_t *)janet_unwrap_abstract(argv[0]);
        janet_persimm_list_t *new_list = janet_persimm_clone_list(&old_list->list);
        janet_persimm_check(persimm_list_cons(&new_list->list, argv + 1));
        return janet_wrap_abstract(new_list);
    }

    janet_panicf("expected a persimmon vector or list, got %v", argv[0]);
}

static Janet cfun_persimm_assoc(int32_t argc, Janet *argv) {
    janet_fixarity(argc, 3);

    persimm_vector_t *old_vector =
        (persimm_vector_t *)janet_getabstract(argv, 0, &persimm_vector_type);

    size_t index;
    if (!persimm_vector_index(old_vector, janet_persimm_integer(argv[1]), &index)) {
        janet_panic("index out of bounds");
    }

    persimm_vector_t *new_vector =
        (persimm_vector_t *)janet_abstract(&persimm_vector_type, sizeof(persimm_vector_t));
    persimm_vector_clone(old_vector, new_vector);

    janet_persimm_check(persimm_vector_update(new_vector, index, argv + 2, true));

    return janet_wrap_abstract(new_vector);
}

static Janet cfun_persimm_first(int32_t argc, Janet *argv) {
    janet_fixarity(argc, 1);

    janet_persimm_list_t *wrapper =
        (janet_persimm_list_t *)janet_getabstract(argv, 0, &persimm_list_type);

    void *slot = persimm_list_first(&wrapper->list);
    if (NULL == slot) return janet_wrap_nil();

    return *(Janet *)slot;
}

/*
 * The rest of an empty list is an empty list, as in Clojure, rather than an
 * error.
 */
static Janet cfun_persimm_rest(int32_t argc, Janet *argv) {
    janet_fixarity(argc, 1);

    janet_persimm_list_t *old_list =
        (janet_persimm_list_t *)janet_getabstract(argv, 0, &persimm_list_type);

    janet_persimm_list_t *new_list = janet_persimm_clone_list(&old_list->list);

    if (new_list->list.count > 0) {
        janet_persimm_check(persimm_list_rest(&new_list->list));
    }

    return janet_wrap_abstract(new_list);
}

static Janet cfun_persimm_to_array(int32_t argc, Janet *argv) {
    janet_fixarity(argc, 1);

    if (janet_checkabstract(argv[0], &persimm_vector_type)) {
        persimm_vector_t *vector = (persimm_vector_t *)janet_unwrap_abstract(argv[0]);
        JanetArray *array = janet_array((int32_t)vector->count);
        persimm_vector_foreach(vector, janet_persimm_to_array_visit, array);
        return janet_wrap_array(array);
    }

    if (janet_checkabstract(argv[0], &persimm_list_type)) {
        janet_persimm_list_t *wrapper = (janet_persimm_list_t *)janet_unwrap_abstract(argv[0]);
        JanetArray *array = janet_array((int32_t)wrapper->list.count);
        persimm_list_foreach(&wrapper->list, janet_persimm_to_array_visit, array);
        return janet_wrap_array(array);
    }

    janet_panicf("expected a persimmon vector or list, got %v", argv[0]);
}

static const JanetReg cfuns[] = {
    {"vec", cfun_persimm_vec, NULL},
    {"list", cfun_persimm_list, NULL},
    {"assoc", cfun_persimm_assoc, NULL},
    {"conj", cfun_persimm_conj, NULL},
    {"first", cfun_persimm_first, NULL},
    {"rest", cfun_persimm_rest, NULL},
    {"to-array", cfun_persimm_to_array, NULL},
    {NULL, NULL, NULL}
};

/* Environment Registration */

void persimm_register_type(JanetTable *env) {
    (void) env;
    janet_register_abstract_type(&persimm_vector_type);
    janet_register_abstract_type(&persimm_list_type);
}

void persimm_register_functions(JanetTable *env) {
    janet_cfuns(env, "persimmon", cfuns);
}

JANET_MODULE_ENTRY(JanetTable *env) {
    persimm_register_type(env);
    persimm_register_functions(env);
}
