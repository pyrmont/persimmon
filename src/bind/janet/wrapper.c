#include "wrapper.h"
#include "../../../inc/persimmon.h"

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

/* Entries */

/*
 * A map entry is a key and a value side by side. Describing the struct the
 * compiler produced, rather than letting the core place the value itself,
 * keeps an entry the size Janet's own pair would be.
 *
 * The core hands the trace callback the key and the value separately, each of
 * them one Janet, so the table above serves the map and the set as it does the
 * vector and the list.
 */
typedef struct {
    Janet key;
    Janet value;
} janet_persimm_entry_t;

static const persimm_entry_layout janet_persimm_map_layout = {
    sizeof(janet_persimm_entry_t),          /* Entry Size */
    sizeof(Janet),                          /* Key Size */
    offsetof(janet_persimm_entry_t, value), /* Value Offset */
    sizeof(Janet)                           /* Value Size */
};

static uint32_t janet_persimm_hash_key(const void *key, size_t key_size, void *ctx) {
    (void) key_size;
    (void) ctx;
    return (uint32_t)janet_hash(*(const Janet *)key);
}

static bool janet_persimm_equals_key(const void *key_a, const void *key_b, size_t key_size,
                                     void *ctx) {
    (void) key_size;
    (void) ctx;
    return janet_equals(*(const Janet *)key_a, *(const Janet *)key_b) ? true : false;
}

static const persimm_key_ops janet_persimm_key_ops = {
    janet_persimm_hash_key,
    janet_persimm_equals_key
};

/* Utility Methods */

static void janet_persimm_check(persimm_status status) {
    if (PERSIMM_OK != status) janet_panic(persimm_status_string(status));
}

/*
 * A nil key can never be looked up again, because nil is how Janet's iteration
 * protocol says "start from the beginning", so it is refused rather than
 * quietly stored.
 */
static void janet_persimm_check_key(Janet key) {
    if (janet_checktype(key, JANET_NIL)) janet_panic("expected a key, got nil");
}

static int64_t janet_persimm_integer(Janet input) {
    if (!janet_checktype(input, JANET_NUMBER)) janet_panic("expected index as number");
    int32_t value = janet_unwrap_integer(input);
    if (janet_unwrap_number(input) - (double)value != 0) janet_panic("expected index as integer");
    return value;
}

/* Unmarshalling moves its unpublished value through a transient one item at a
 * time. Persisting after each item keeps everything already read visible to
 * Janet's collector while the next item is decoded. */
static persimm_status janet_persimm_vector_build_push(persimm_vector_t *vector,
                                                      const void *elem) {
    persimm_vector_transient_t transient;
    persimm_vector_to_transient(vector, &transient);
    persimm_vector_deinit(vector);
    persimm_status status = persimm_vector_transient_push(&transient, elem);
    persimm_status persisted = persimm_vector_transient_persist(&transient, vector);
    return PERSIMM_OK == status ? persisted : status;
}

static persimm_status janet_persimm_list_build_cons(persimm_list_t *list,
                                                    const void *elem) {
    persimm_list_t next;
    persimm_status status = persimm_list_cons(list, elem, &next);
    if (PERSIMM_OK == status) {
        persimm_list_deinit(list);
        *list = next;
    }
    return status;
}

static persimm_status janet_persimm_map_build_assoc(persimm_map_t *map,
                                                    const void *entry) {
    persimm_map_transient_t transient;
    persimm_map_to_transient(map, &transient);
    persimm_map_deinit(map);
    persimm_status status = persimm_map_transient_assoc(&transient, entry);
    persimm_status persisted = persimm_map_transient_persist(&transient, map);
    return PERSIMM_OK == status ? persisted : status;
}

static persimm_status janet_persimm_set_build_conj(persimm_set_t *set,
                                                   const void *elem) {
    persimm_set_transient_t transient;
    persimm_set_to_transient(set, &transient);
    persimm_set_deinit(set);
    persimm_status status = persimm_set_transient_conj(&transient, elem);
    persimm_status persisted = persimm_set_transient_persist(&transient, set);
    return PERSIMM_OK == status ? persisted : status;
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
 * A vector and a list are hashed in order, because their order is what they
 * are. A map and a set are not: two of them holding the same entries may hand
 * them over in different orders when keys share a hash, so their entries are
 * combined by adding, which does not care.
 */

static void janet_persimm_set_hash_visit(void *slot, size_t index, void *ctx) {
    (void) index;
    uint32_t *hash = (uint32_t *)ctx;
    *hash += (uint32_t)janet_hash(*(Janet *)slot) * 2654435761u;
}

static void janet_persimm_map_hash_visit(void *slot, size_t index, void *ctx) {
    (void) index;
    uint32_t *hash = (uint32_t *)ctx;
    janet_persimm_entry_t *entry = (janet_persimm_entry_t *)slot;
    uint32_t key = (uint32_t)janet_hash(entry->key);
    uint32_t value = (uint32_t)janet_hash(entry->value);
    *hash += (key * 31u + value) * 2654435761u;
}

static void janet_persimm_map_to_string_visit(void *slot, size_t index, void *ctx) {
    JanetBuffer *buf = (JanetBuffer *)ctx;
    janet_persimm_entry_t *entry = (janet_persimm_entry_t *)slot;
    if (index > 0) janet_buffer_push_cstring(buf, " ");
    janet_buffer_push_string(buf, janet_to_string(entry->key));
    janet_buffer_push_cstring(buf, " ");
    janet_buffer_push_string(buf, janet_to_string(entry->value));
}

static void janet_persimm_map_to_array_visit(void *slot, size_t index, void *ctx) {
    (void) index;
    janet_persimm_entry_t *entry = (janet_persimm_entry_t *)slot;
    Janet pair[2] = { entry->key, entry->value };
    janet_array_push((JanetArray *)ctx, janet_wrap_tuple(janet_tuple_n(pair, 2)));
}

static void janet_persimm_map_to_table_visit(void *slot, size_t index, void *ctx) {
    (void) index;
    janet_persimm_entry_t *entry = (janet_persimm_entry_t *)slot;
    janet_table_put((JanetTable *)ctx, entry->key, entry->value);
}

/* Marshalling */

/*
 * A structure is written out as its contents and not as its shape: a count,
 * and then the elements. What comes back is built from those the way any other
 * structure is built, which is enough to give back what went in. A vector's
 * shape follows from its length, a list's from its elements, and a map or a
 * set is kept canonical, so one rebuilt from its entries has the shape the
 * original had.
 *
 * Marshalling copies. Two virtual machines that exchange a structure hold one
 * each, sharing no node and no cursor, so nothing here has to be safe to touch
 * from two of them at once.
 *
 * A structure can only be read back where the module has been loaded, since
 * that is what registers the abstract type its name refers to.
 */

/*
 * A value read back is held by the structure being built before the next one
 * is read, because a Janet in a local is invisible to the collector and
 * reading the next value may allocate and so collect. Nothing is gathered on
 * the side: a temporary would have to be a collector root, and a value that
 * cannot be read would leave that root behind for good.
 *
 * Building a structure allocates nothing Janet knows about, so no collection
 * can run partway through one.
 */

static void janet_persimm_marshal_visit(void *slot, size_t index, void *ctx) {
    (void) index;
    janet_marshal_janet((JanetMarshalContext *)ctx, *(Janet *)slot);
}

static void janet_persimm_map_marshal_visit(void *slot, size_t index, void *ctx) {
    (void) index;
    janet_persimm_entry_t *entry = (janet_persimm_entry_t *)slot;
    janet_marshal_janet((JanetMarshalContext *)ctx, entry->key);
    janet_marshal_janet((JanetMarshalContext *)ctx, entry->value);
}

/*
 * Turns a chain around. Consing puts elements on the front, so a list read
 * back in the order it was written comes out reversed and has to be built once
 * more the other way.
 */
typedef struct {
    persimm_list_t *into;
    persimm_status status;
} janet_persimm_reverse_t;

static void janet_persimm_reverse_visit(void *slot, size_t index, void *ctx) {
    (void) index;
    janet_persimm_reverse_t *state = (janet_persimm_reverse_t *)ctx;
    if (PERSIMM_OK != state->status) return;
    state->status = janet_persimm_list_build_cons(state->into, slot);
}

/*
 * Refuses a length the input cannot mean before anything is read on the
 * strength of it. What remains of an over-long one fails when the value that
 * is not there is asked for.
 */
static size_t janet_persimm_unmarshal_count(JanetMarshalContext *ctx) {
    size_t count = janet_unmarshal_size(ctx);
    if (count > (size_t)INT32_MAX) janet_panic("structure is too long to read back");
    return count;
}

/* Comparing */

/*
 * Janet has no separate equality slot for an abstract type, so `=`, `deep=`
 * and use as a table key all come through `compare`, and two structures are
 * equal exactly when it answers zero.
 *
 * It asks one operand to order the pair without first checking that the other
 * is the same kind of abstract, so every comparison below has to look for
 * itself. Reading a list as though it were a map would otherwise be a matter
 * of writing `(= a-map a-list)`.
 *
 * Where there is no meaningful order the addresses stand in. That is arbitrary
 * but answers the same way every time it is asked within a run, which is what
 * the protocol needs.
 */

static int janet_persimm_order_by_address(const void *a, const void *b) {
    if (a == b) return 0;
    return (a < b) ? -1 : 1;
}

static bool janet_persimm_is(void *p, const JanetAbstractType *type) {
    return janet_abstract_type(p) == type;
}

/*
 * Two structures of different kinds are ordered by their type tables, so that
 * every vector sorts to the same side of every list.
 */
static int janet_persimm_order_by_kind(void *other, const JanetAbstractType *type) {
    return janet_persimm_order_by_address(type, janet_abstract_type(other));
}

/*
 * Orders two counts, for the structures whose contents carry no order of their
 * own. A map or a set that differs from another in what it holds, rather than
 * how much, falls back to addresses.
 */
static int janet_persimm_order_by_count(size_t a, size_t b) {
    if (a == b) return 0;
    return (a < b) ? -1 : 1;
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
static int janet_persimm_vector_compare(void *p1, void *p2);
static void janet_persimm_vector_marshal(void *p, JanetMarshalContext *ctx);
static void *janet_persimm_vector_unmarshal(JanetMarshalContext *ctx);

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
    janet_persimm_vector_marshal, /* Marshall */
    janet_persimm_vector_unmarshal, /* Unmarshall */
    janet_persimm_vector_to_string, /* String */
    janet_persimm_vector_compare, /* Compare */
    janet_persimm_vector_hash, /* Hash */
    janet_persimm_vector_next, /* Next */
    NULL, /* Call */
    janet_persimm_vector_length, /* Length */
    JANET_ATEND_LENGTH
};

/*
 * Element by element, and only then by length, which is the order Janet puts
 * its own tuples and strings in: [1] before [1 9] before [2].
 *
 * Length is the cheaper test and it is tempting to reach for it first, but it
 * decides a different order. Taking it first would sort [2] before [1 9], as
 * nothing else in Janet does. Nor can it serve as a quick way out for two
 * vectors that cannot be equal, because this one function answers both
 * questions, and which way round the pair goes still rests on the elements
 * they share.
 */
static int janet_persimm_vector_compare(void *p1, void *p2) {
    if (p1 == p2) return 0;
    if (!janet_persimm_is(p2, &persimm_vector_type)) {
        return janet_persimm_order_by_kind(p2, &persimm_vector_type);
    }

    persimm_vector_t *a = (persimm_vector_t *)p1;
    persimm_vector_t *b = (persimm_vector_t *)p2;
    size_t shared = (a->count < b->count) ? a->count : b->count;

    for (size_t i = 0; i < shared; i++) {
        int order = janet_compare(*(Janet *)persimm_vector_ref(a, i),
                                  *(Janet *)persimm_vector_ref(b, i));
        if (0 != order) return order;
    }

    return janet_persimm_order_by_count(a->count, b->count);
}

static void janet_persimm_vector_marshal(void *p, JanetMarshalContext *ctx) {
    persimm_vector_t *vector = (persimm_vector_t *)p;
    janet_marshal_abstract(ctx, p);
    janet_marshal_size(ctx, vector->count);
    persimm_vector_foreach(vector, janet_persimm_marshal_visit, ctx);
}

static void *janet_persimm_vector_unmarshal(JanetMarshalContext *ctx) {
    persimm_vector_t *vector =
        (persimm_vector_t *)janet_unmarshal_abstract(ctx, sizeof(persimm_vector_t));
    /* Nothing allocates between the vector coming into existence and its being
       initialised, so a collection never meets one it cannot trace. */
    janet_persimm_check(persimm_vector_init(vector, sizeof(Janet), &janet_persimm_ops, NULL));

    size_t count = janet_persimm_unmarshal_count(ctx);
    for (size_t i = 0; i < count; i++) {
        Janet value = janet_unmarshal_janet(ctx);
        janet_persimm_check(janet_persimm_vector_build_push(vector, &value));
    }

    return vector;
}

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
 * per element. Nothing here needs a lock. A list never changes once handed
 * out, and although one can now be marshalled, what is written out is its
 * elements: reading it back builds a chain and a cursor of its own, so no two
 * virtual machines ever hold the same list.
 */
typedef struct {
    persimm_list_t list;
    persimm_list_cursor_t cursor;
} janet_persimm_list_t;

static JanetMethod persimm_list_methods[2];
static int janet_persimm_list_compare(void *p1, void *p2);
static void janet_persimm_list_marshal(void *p, JanetMarshalContext *ctx);
static void *janet_persimm_list_unmarshal(JanetMarshalContext *ctx);

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
    janet_persimm_list_marshal, /* Marshall */
    janet_persimm_list_unmarshal, /* Unmarshall */
    janet_persimm_list_to_string, /* String */
    janet_persimm_list_compare, /* Compare */
    janet_persimm_list_hash, /* Hash */
    janet_persimm_list_next, /* Next */
    NULL, /* Call */
    janet_persimm_list_length, /* Length */
    JANET_ATEND_LENGTH
};

/*
 * Element by element, resuming from each list's own cursor so that comparing
 * two lists costs one step per element rather than walking from the head once
 * for every index.
 */
static int janet_persimm_list_compare(void *p1, void *p2) {
    if (p1 == p2) return 0;
    if (!janet_persimm_is(p2, &persimm_list_type)) {
        return janet_persimm_order_by_kind(p2, &persimm_list_type);
    }

    janet_persimm_list_t *a = (janet_persimm_list_t *)p1;
    janet_persimm_list_t *b = (janet_persimm_list_t *)p2;
    size_t shared = (a->list.count < b->list.count) ? a->list.count : b->list.count;

    for (size_t i = 0; i < shared; i++) {
        int order = janet_compare(*(Janet *)persimm_list_ref_from(&a->list, &a->cursor, i),
                                  *(Janet *)persimm_list_ref_from(&b->list, &b->cursor, i));
        if (0 != order) return order;
    }

    return janet_persimm_order_by_count(a->list.count, b->list.count);
}

static void janet_persimm_list_marshal(void *p, JanetMarshalContext *ctx) {
    janet_persimm_list_t *wrapper = (janet_persimm_list_t *)p;
    janet_marshal_abstract(ctx, p);
    janet_marshal_size(ctx, wrapper->list.count);
    persimm_list_foreach(&wrapper->list, janet_persimm_marshal_visit, ctx);
}

static void *janet_persimm_list_unmarshal(JanetMarshalContext *ctx) {
    janet_persimm_list_t *wrapper =
        (janet_persimm_list_t *)janet_unmarshal_abstract(ctx, sizeof(janet_persimm_list_t));
    persimm_list_cursor_reset(&wrapper->cursor);
    janet_persimm_check(persimm_list_init(&wrapper->list, sizeof(Janet), &janet_persimm_ops,
                                          NULL));

    size_t count = janet_persimm_unmarshal_count(ctx);
    for (size_t i = 0; i < count; i++) {
        Janet value = janet_unmarshal_janet(ctx);
        janet_persimm_check(janet_persimm_list_build_cons(&wrapper->list, &value));
    }

    /* The elements went on the front as they arrived, so the chain reads
       backwards and is built once more the right way round. Neither step
       allocates anything Janet knows about, so nothing can be collected while
       the values are held by only one of the two chains. */
    persimm_list_t ordered;
    janet_persimm_check(persimm_list_init(&ordered, sizeof(Janet), &janet_persimm_ops, NULL));

    janet_persimm_reverse_t state = { &ordered, PERSIMM_OK };
    persimm_list_foreach(&wrapper->list, janet_persimm_reverse_visit, &state);
    if (PERSIMM_OK != state.status) {
        persimm_list_deinit(&ordered);
        janet_persimm_check(state.status);
    }

    persimm_list_deinit(&wrapper->list);
    persimm_list_clone(&ordered, &wrapper->list);
    persimm_list_deinit(&ordered);
    persimm_list_cursor_reset(&wrapper->cursor);

    return wrapper;
}

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

/* Maps */

static JanetMethod persimm_map_methods[2];
static int janet_persimm_map_compare(void *p1, void *p2);
static void janet_persimm_map_marshal(void *p, JanetMarshalContext *ctx);
static void *janet_persimm_map_unmarshal(JanetMarshalContext *ctx);

static int janet_persimm_map_gc(void *p, size_t size) {
    (void) size;
    persimm_map_deinit((persimm_map_t *)p);
    return 0;
}

static int janet_persimm_map_mark(void *p, size_t size) {
    (void) size;
    persimm_map_trace((persimm_map_t *)p);
    return 0;
}

/*
 * Unlike a vector, whose keys are numbers, a map may hold the very keywords
 * the method table answers to. The map is asked first so that a key it holds
 * is never shadowed by a method of the same name.
 */
static int janet_persimm_map_get(void *p, Janet key, Janet *out) {
    persimm_map_t *map = (persimm_map_t *)p;

    if (!janet_checktype(key, JANET_NIL)) {
        void *value = persimm_map_ref(map, &key);
        if (NULL != value) {
            *out = *(Janet *)value;
            return 1;
        }
    }

    if (janet_checktype(key, JANET_KEYWORD)) {
        return janet_getmethod(janet_unwrap_keyword(key), persimm_map_methods, out);
    }

    return 0;
}

static void janet_persimm_map_to_string(void *p, JanetBuffer *buf) {
    janet_buffer_push_cstring(buf, "{");
    persimm_map_foreach((persimm_map_t *)p, janet_persimm_map_to_string_visit, buf);
    janet_buffer_push_cstring(buf, "}");
}

static int32_t janet_persimm_map_hash(void *p, size_t size) {
    (void) size;
    uint32_t hash = 5381;
    persimm_map_foreach((persimm_map_t *)p, janet_persimm_map_hash_visit, &hash);
    return (int32_t)hash;
}

/*
 * Janet drives iteration by asking for the key after this one and then reading
 * that key back, which is what gives a map `each`, `keys`, `values` and
 * `pairs` without any of them being written here.
 */
static Janet janet_persimm_map_next(void *p, Janet key) {
    persimm_map_t *map = (persimm_map_t *)p;

    void *entry = janet_checktype(key, JANET_NIL) ? persimm_map_next(map, NULL)
                                                  : persimm_map_next(map, &key);
    if (NULL == entry) return janet_wrap_nil();

    return ((janet_persimm_entry_t *)entry)->key;
}

static size_t janet_persimm_map_length(void *p, size_t size) {
    (void) size;
    return ((persimm_map_t *)p)->count;
}

static const JanetAbstractType persimm_map_type = {
    "persimmon/map",
    janet_persimm_map_gc,
    janet_persimm_map_mark, /* GC Mark */
    janet_persimm_map_get, /* Get */
    NULL, /* Set */
    janet_persimm_map_marshal, /* Marshall */
    janet_persimm_map_unmarshal, /* Unmarshall */
    janet_persimm_map_to_string, /* String */
    janet_persimm_map_compare, /* Compare */
    janet_persimm_map_hash, /* Hash */
    janet_persimm_map_next, /* Next */
    NULL, /* Call */
    janet_persimm_map_length, /* Length */
    JANET_ATEND_LENGTH
};

/*
 * A map carries no order of its own, so equality is all this can answer
 * exactly: the same number of entries, and every key mapped to an equal value.
 * Two maps that differ fall back to their counts and then to their addresses,
 * which makes the order between unequal maps one not to rely on.
 *
 * The count is taken first here, unlike in a vector, and for the same reason
 * the vector cannot take it first: with no order among the entries themselves,
 * an order read off the counts is as good as any other.
 */
static int janet_persimm_map_compare(void *p1, void *p2) {
    if (p1 == p2) return 0;
    if (!janet_persimm_is(p2, &persimm_map_type)) {
        return janet_persimm_order_by_kind(p2, &persimm_map_type);
    }

    persimm_map_t *a = (persimm_map_t *)p1;
    persimm_map_t *b = (persimm_map_t *)p2;
    if (a->count != b->count) return janet_persimm_order_by_count(a->count, b->count);

    for (void *entry = persimm_map_next(a, NULL);
         NULL != entry;
         entry = persimm_map_next(a, entry)) {
        janet_persimm_entry_t *pair = (janet_persimm_entry_t *)entry;
        void *value = persimm_map_ref(b, &pair->key);
        if (NULL == value) return janet_persimm_order_by_address(p1, p2);
        if (!janet_equals(pair->value, *(Janet *)value)) {
            return janet_persimm_order_by_address(p1, p2);
        }
    }

    return 0;
}

static void janet_persimm_map_marshal(void *p, JanetMarshalContext *ctx) {
    persimm_map_t *map = (persimm_map_t *)p;
    janet_marshal_abstract(ctx, p);
    janet_marshal_size(ctx, map->count);
    persimm_map_foreach(map, janet_persimm_map_marshal_visit, ctx);
}

static void *janet_persimm_map_unmarshal(JanetMarshalContext *ctx) {
    persimm_map_t *map = (persimm_map_t *)janet_unmarshal_abstract(ctx, sizeof(persimm_map_t));
    janet_persimm_check(persimm_map_init(map, &janet_persimm_map_layout, &janet_persimm_ops,
                                         &janet_persimm_key_ops, NULL));

    size_t count = janet_persimm_unmarshal_count(ctx);
    for (size_t i = 0; i < count; i++) {
        Janet key = janet_unmarshal_janet(ctx);
        janet_persimm_check_key(key);

        /* The key is stored against itself so that the map holds it, and the
           collector can see it, while the value is read. Storing it a second
           time replaces the stand-in and leaves the count where it was. */
        janet_persimm_entry_t entry = { key, key };
        janet_persimm_check(janet_persimm_map_build_assoc(map, &entry));
        entry.value = janet_unmarshal_janet(ctx);
        janet_persimm_check(janet_persimm_map_build_assoc(map, &entry));
    }

    return map;
}

static Janet persimm_map_method_length(int32_t argc, Janet *argv) {
    janet_fixarity(argc, 1);
    persimm_map_t *map = (persimm_map_t *)janet_getabstract(argv, 0, &persimm_map_type);
    return janet_wrap_number((double)map->count);
}

static JanetMethod persimm_map_methods[] = {
    {"length", persimm_map_method_length},
    {NULL, NULL}
};

/* Sets */

static JanetMethod persimm_set_methods[2];
static int janet_persimm_set_compare(void *p1, void *p2);
static void janet_persimm_set_marshal(void *p, JanetMarshalContext *ctx);
static void *janet_persimm_set_unmarshal(JanetMarshalContext *ctx);

static int janet_persimm_set_gc(void *p, size_t size) {
    (void) size;
    persimm_set_deinit((persimm_set_t *)p);
    return 0;
}

static int janet_persimm_set_mark(void *p, size_t size) {
    (void) size;
    persimm_set_trace((persimm_set_t *)p);
    return 0;
}

/*
 * As in Clojure, looking an element up in a set answers with the element, so
 * that iterating a set yields what it holds rather than a run of `true`.
 */
static int janet_persimm_set_get(void *p, Janet key, Janet *out) {
    persimm_set_t *set = (persimm_set_t *)p;

    if (!janet_checktype(key, JANET_NIL)) {
        void *elem = persimm_set_ref(set, &key);
        if (NULL != elem) {
            *out = *(Janet *)elem;
            return 1;
        }
    }

    if (janet_checktype(key, JANET_KEYWORD)) {
        return janet_getmethod(janet_unwrap_keyword(key), persimm_set_methods, out);
    }

    return 0;
}

static void janet_persimm_set_to_string(void *p, JanetBuffer *buf) {
    janet_buffer_push_cstring(buf, "#{");
    persimm_set_foreach((persimm_set_t *)p, janet_persimm_to_string_visit, buf);
    janet_buffer_push_cstring(buf, "}");
}

static int32_t janet_persimm_set_hash(void *p, size_t size) {
    (void) size;
    uint32_t hash = 5381;
    persimm_set_foreach((persimm_set_t *)p, janet_persimm_set_hash_visit, &hash);
    return (int32_t)hash;
}

static Janet janet_persimm_set_next(void *p, Janet key) {
    persimm_set_t *set = (persimm_set_t *)p;

    void *elem = janet_checktype(key, JANET_NIL) ? persimm_set_next(set, NULL)
                                                 : persimm_set_next(set, &key);
    if (NULL == elem) return janet_wrap_nil();

    return *(Janet *)elem;
}

static size_t janet_persimm_set_length(void *p, size_t size) {
    (void) size;
    return ((persimm_set_t *)p)->count;
}

static const JanetAbstractType persimm_set_type = {
    "persimmon/set",
    janet_persimm_set_gc,
    janet_persimm_set_mark, /* GC Mark */
    janet_persimm_set_get, /* Get */
    NULL, /* Set */
    janet_persimm_set_marshal, /* Marshall */
    janet_persimm_set_unmarshal, /* Unmarshall */
    janet_persimm_set_to_string, /* String */
    janet_persimm_set_compare, /* Compare */
    janet_persimm_set_hash, /* Hash */
    janet_persimm_set_next, /* Next */
    NULL, /* Call */
    janet_persimm_set_length, /* Length */
    JANET_ATEND_LENGTH
};

/*
 * As for a map, and for the same reason: the same number of elements, and
 * every one of them held by both.
 */
static int janet_persimm_set_compare(void *p1, void *p2) {
    if (p1 == p2) return 0;
    if (!janet_persimm_is(p2, &persimm_set_type)) {
        return janet_persimm_order_by_kind(p2, &persimm_set_type);
    }

    persimm_set_t *a = (persimm_set_t *)p1;
    persimm_set_t *b = (persimm_set_t *)p2;
    if (a->count != b->count) return janet_persimm_order_by_count(a->count, b->count);

    for (void *elem = persimm_set_next(a, NULL);
         NULL != elem;
         elem = persimm_set_next(a, elem)) {
        if (!persimm_set_has(b, elem)) return janet_persimm_order_by_address(p1, p2);
    }

    return 0;
}

static void janet_persimm_set_marshal(void *p, JanetMarshalContext *ctx) {
    persimm_set_t *set = (persimm_set_t *)p;
    janet_marshal_abstract(ctx, p);
    janet_marshal_size(ctx, set->count);
    persimm_set_foreach(set, janet_persimm_marshal_visit, ctx);
}

static void *janet_persimm_set_unmarshal(JanetMarshalContext *ctx) {
    persimm_set_t *set = (persimm_set_t *)janet_unmarshal_abstract(ctx, sizeof(persimm_set_t));
    janet_persimm_check(persimm_set_init(set, sizeof(Janet), &janet_persimm_ops,
                                         &janet_persimm_key_ops, NULL));

    size_t count = janet_persimm_unmarshal_count(ctx);
    for (size_t i = 0; i < count; i++) {
        Janet elem = janet_unmarshal_janet(ctx);
        janet_persimm_check_key(elem);
        janet_persimm_check(janet_persimm_set_build_conj(set, &elem));
    }

    return set;
}

static Janet persimm_set_method_length(int32_t argc, Janet *argv) {
    janet_fixarity(argc, 1);
    persimm_set_t *set = (persimm_set_t *)janet_getabstract(argv, 0, &persimm_set_type);
    return janet_wrap_number((double)set->count);
}

static JanetMethod persimm_set_methods[] = {
    {"length", persimm_set_method_length},
    {NULL, NULL}
};

/* Transients */

static void janet_persimm_require_active(bool active) {
    if (!active) janet_panic("transient is no longer active");
}

static int janet_persimm_transient_compare(void *a, void *b) {
    (void) a;
    (void) b;
    janet_panic("cannot compare a transient");
}

static int32_t janet_persimm_transient_hash(void *p, size_t size) {
    (void) p;
    (void) size;
    janet_panic("cannot hash a transient");
}

static int janet_persimm_vector_transient_gc(void *p, size_t size) {
    (void) size;
    persimm_vector_transient_deinit((persimm_vector_transient_t *)p);
    return 0;
}

static int janet_persimm_vector_transient_mark(void *p, size_t size) {
    (void) size;
    persimm_vector_transient_t *transient = (persimm_vector_transient_t *)p;
    if (transient->active) persimm_vector_trace(&transient->value);
    return 0;
}

static int janet_persimm_vector_transient_get(void *p, Janet key, Janet *out) {
    persimm_vector_transient_t *transient = (persimm_vector_transient_t *)p;
    janet_persimm_require_active(transient->active);

    size_t index;
    if (!persimm_vector_index(&transient->value, janet_persimm_integer(key), &index)) return 0;
    *out = janet_persimm_vector_at(&transient->value, index);
    return 1;
}

static Janet janet_persimm_vector_transient_next(void *p, Janet key) {
    persimm_vector_transient_t *transient = (persimm_vector_transient_t *)p;
    janet_persimm_require_active(transient->active);
    return janet_persimm_next_index(transient->value.count, key);
}

static size_t janet_persimm_vector_transient_length(void *p, size_t size) {
    (void) size;
    persimm_vector_transient_t *transient = (persimm_vector_transient_t *)p;
    janet_persimm_require_active(transient->active);
    return transient->value.count;
}

static const JanetAbstractType persimm_vector_transient_type = {
    "persimmon/vector-transient",
    janet_persimm_vector_transient_gc,
    janet_persimm_vector_transient_mark, /* GC Mark */
    janet_persimm_vector_transient_get, /* Get */
    NULL, /* Set */
    NULL, /* Marshall */
    NULL, /* Unmarshall */
    NULL, /* String */
    janet_persimm_transient_compare, /* Compare */
    janet_persimm_transient_hash, /* Hash */
    janet_persimm_vector_transient_next, /* Next */
    NULL, /* Call */
    janet_persimm_vector_transient_length, /* Length */
    JANET_ATEND_LENGTH
};

static int janet_persimm_map_transient_gc(void *p, size_t size) {
    (void) size;
    persimm_map_transient_deinit((persimm_map_transient_t *)p);
    return 0;
}

static int janet_persimm_map_transient_mark(void *p, size_t size) {
    (void) size;
    persimm_map_transient_t *transient = (persimm_map_transient_t *)p;
    if (transient->active) persimm_map_trace(&transient->value);
    return 0;
}

static int janet_persimm_map_transient_get(void *p, Janet key, Janet *out) {
    persimm_map_transient_t *transient = (persimm_map_transient_t *)p;
    janet_persimm_require_active(transient->active);
    if (janet_checktype(key, JANET_NIL)) return 0;

    void *value = persimm_map_ref(&transient->value, &key);
    if (NULL == value) return 0;
    *out = *(Janet *)value;
    return 1;
}

static Janet janet_persimm_map_transient_next(void *p, Janet key) {
    persimm_map_transient_t *transient = (persimm_map_transient_t *)p;
    janet_persimm_require_active(transient->active);
    void *entry = janet_checktype(key, JANET_NIL)
        ? persimm_map_next(&transient->value, NULL)
        : persimm_map_next(&transient->value, &key);
    return (NULL == entry) ? janet_wrap_nil() : ((janet_persimm_entry_t *)entry)->key;
}

static size_t janet_persimm_map_transient_length(void *p, size_t size) {
    (void) size;
    persimm_map_transient_t *transient = (persimm_map_transient_t *)p;
    janet_persimm_require_active(transient->active);
    return transient->value.count;
}

static const JanetAbstractType persimm_map_transient_type = {
    "persimmon/map-transient",
    janet_persimm_map_transient_gc,
    janet_persimm_map_transient_mark, /* GC Mark */
    janet_persimm_map_transient_get, /* Get */
    NULL, /* Set */
    NULL, /* Marshall */
    NULL, /* Unmarshall */
    NULL, /* String */
    janet_persimm_transient_compare, /* Compare */
    janet_persimm_transient_hash, /* Hash */
    janet_persimm_map_transient_next, /* Next */
    NULL, /* Call */
    janet_persimm_map_transient_length, /* Length */
    JANET_ATEND_LENGTH
};

static int janet_persimm_set_transient_gc(void *p, size_t size) {
    (void) size;
    persimm_set_transient_deinit((persimm_set_transient_t *)p);
    return 0;
}

static int janet_persimm_set_transient_mark(void *p, size_t size) {
    (void) size;
    persimm_set_transient_t *transient = (persimm_set_transient_t *)p;
    if (transient->active) persimm_set_trace(&transient->value);
    return 0;
}

static int janet_persimm_set_transient_get(void *p, Janet key, Janet *out) {
    persimm_set_transient_t *transient = (persimm_set_transient_t *)p;
    janet_persimm_require_active(transient->active);
    if (janet_checktype(key, JANET_NIL)) return 0;

    void *elem = persimm_set_ref(&transient->value, &key);
    if (NULL == elem) return 0;
    *out = *(Janet *)elem;
    return 1;
}

static Janet janet_persimm_set_transient_next(void *p, Janet key) {
    persimm_set_transient_t *transient = (persimm_set_transient_t *)p;
    janet_persimm_require_active(transient->active);
    void *elem = janet_checktype(key, JANET_NIL)
        ? persimm_set_next(&transient->value, NULL)
        : persimm_set_next(&transient->value, &key);
    return (NULL == elem) ? janet_wrap_nil() : *(Janet *)elem;
}

static size_t janet_persimm_set_transient_length(void *p, size_t size) {
    (void) size;
    persimm_set_transient_t *transient = (persimm_set_transient_t *)p;
    janet_persimm_require_active(transient->active);
    return transient->value.count;
}

static const JanetAbstractType persimm_set_transient_type = {
    "persimmon/set-transient",
    janet_persimm_set_transient_gc,
    janet_persimm_set_transient_mark, /* GC Mark */
    janet_persimm_set_transient_get, /* Get */
    NULL, /* Set */
    NULL, /* Marshall */
    NULL, /* Unmarshall */
    NULL, /* String */
    janet_persimm_transient_compare, /* Compare */
    janet_persimm_transient_hash, /* Hash */
    janet_persimm_set_transient_next, /* Next */
    NULL, /* Call */
    janet_persimm_set_transient_length, /* Length */
    JANET_ATEND_LENGTH
};

/* Constructing */

static persimm_vector_t *janet_persimm_alloc_vector(void) {
    return (persimm_vector_t *)janet_abstract(&persimm_vector_type, sizeof(persimm_vector_t));
}

static persimm_vector_t *janet_persimm_new_vector(void) {
    persimm_vector_t *vector = janet_persimm_alloc_vector();
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

static persimm_map_t *janet_persimm_alloc_map(void) {
    return (persimm_map_t *)janet_abstract(&persimm_map_type, sizeof(persimm_map_t));
}

static persimm_map_t *janet_persimm_new_map(void) {
    persimm_map_t *map = janet_persimm_alloc_map();
    janet_persimm_check(persimm_map_init(map, &janet_persimm_map_layout, &janet_persimm_ops,
                                         &janet_persimm_key_ops, NULL));
    return map;
}

static persimm_set_t *janet_persimm_alloc_set(void) {
    return (persimm_set_t *)janet_abstract(&persimm_set_type, sizeof(persimm_set_t));
}

static persimm_set_t *janet_persimm_new_set(void) {
    persimm_set_t *set = janet_persimm_alloc_set();
    janet_persimm_check(persimm_set_init(set, sizeof(Janet), &janet_persimm_ops,
                                         &janet_persimm_key_ops, NULL));
    return set;
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

        /* Every value remains reachable through argv[0], so construction can
           keep one transient for the whole batch. */
        persimm_vector_transient_t transient;
        persimm_vector_to_transient(vector, &transient);
        persimm_vector_deinit(vector);
        for (int32_t i = 0; i < view.len; i++) {
            persimm_status status = persimm_vector_transient_push(&transient, &view.items[i]);
            if (PERSIMM_OK != status) {
                persimm_vector_transient_deinit(&transient);
                janet_panic(persimm_status_string(status));
            }
        }
        janet_persimm_check(persimm_vector_transient_persist(&transient, vector));
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
            janet_persimm_check(janet_persimm_list_build_cons(&wrapper->list, &view.items[i]));
        }
    }

    return janet_wrap_abstract(wrapper);
}

static Janet cfun_persimm_map(int32_t argc, Janet *argv) {
    janet_arity(argc, 0, 1);

    persimm_map_t *map = janet_persimm_new_map();

    if (1 == argc) {
        const JanetKV *kvs;
        int32_t len;
        int32_t cap;
        if (!janet_dictionary_view(argv[0], &kvs, &len, &cap)) {
            janet_panicf("expected a table or struct, got %v", argv[0]);
        }
        persimm_map_transient_t transient;
        persimm_map_to_transient(map, &transient);
        persimm_map_deinit(map);
        for (int32_t i = 0; i < cap; i++) {
            /* An unused slot carries a nil key, and a nil value is no entry. */
            if (janet_checktype(kvs[i].key, JANET_NIL)) continue;
            if (janet_checktype(kvs[i].value, JANET_NIL)) continue;
            janet_persimm_entry_t entry = { kvs[i].key, kvs[i].value };
            persimm_status status = persimm_map_transient_assoc(&transient, &entry);
            if (PERSIMM_OK != status) {
                persimm_map_transient_deinit(&transient);
                janet_panic(persimm_status_string(status));
            }
        }
        janet_persimm_check(persimm_map_transient_persist(&transient, map));
    }

    return janet_wrap_abstract(map);
}

static Janet cfun_persimm_set(int32_t argc, Janet *argv) {
    janet_arity(argc, 0, 1);

    persimm_set_t *set = janet_persimm_new_set();

    if (1 == argc) {
        JanetView view;
        janet_persimm_view(argv[0], &view);
        for (int32_t i = 0; i < view.len; i++) {
            janet_persimm_check_key(view.items[i]);
        }

        persimm_set_transient_t transient;
        persimm_set_to_transient(set, &transient);
        persimm_set_deinit(set);
        for (int32_t i = 0; i < view.len; i++) {
            persimm_status status = persimm_set_transient_conj(&transient, &view.items[i]);
            if (PERSIMM_OK != status) {
                persimm_set_transient_deinit(&transient);
                janet_panic(persimm_status_string(status));
            }
        }
        janet_persimm_check(persimm_set_transient_persist(&transient, set));
    }

    return janet_wrap_abstract(set);
}

/*
 * As in Clojure, conj adds an element wherever the structure can take one
 * cheapest: the end of a vector, the front of a list, anywhere in a set.
 */
static Janet cfun_persimm_conj(int32_t argc, Janet *argv) {
    janet_fixarity(argc, 2);

    if (janet_checkabstract(argv[0], &persimm_set_type)) {
        persimm_set_t *old_set = (persimm_set_t *)janet_unwrap_abstract(argv[0]);
        janet_persimm_check_key(argv[1]);
        persimm_set_t *new_set = janet_persimm_alloc_set();
        janet_persimm_check(persimm_set_conj(old_set, argv + 1, new_set));
        return janet_wrap_abstract(new_set);
    }

    if (janet_checkabstract(argv[0], &persimm_vector_type)) {
        persimm_vector_t *old_vector = (persimm_vector_t *)janet_unwrap_abstract(argv[0]);
        persimm_vector_t *new_vector = janet_persimm_alloc_vector();
        janet_persimm_check(persimm_vector_push(old_vector, argv + 1, new_vector));
        return janet_wrap_abstract(new_vector);
    }

    if (janet_checkabstract(argv[0], &persimm_list_type)) {
        janet_persimm_list_t *old_list = (janet_persimm_list_t *)janet_unwrap_abstract(argv[0]);
        janet_persimm_list_t *new_list = janet_persimm_alloc_list();
        janet_persimm_check(persimm_list_cons(&old_list->list, argv + 1, &new_list->list));
        return janet_wrap_abstract(new_list);
    }

    janet_panicf("expected a persimmon vector, list or set, got %v", argv[0]);
}

static Janet cfun_persimm_assoc(int32_t argc, Janet *argv) {
    janet_fixarity(argc, 3);

    if (janet_checkabstract(argv[0], &persimm_map_type)) {
        persimm_map_t *old_map = (persimm_map_t *)janet_unwrap_abstract(argv[0]);
        janet_persimm_check_key(argv[1]);

        persimm_map_t *new_map = janet_persimm_alloc_map();

        /* A nil value is no value, as it is for a Janet table, so storing one
           takes the key away rather than leaving it holding nothing. */
        if (janet_checktype(argv[2], JANET_NIL)) {
            janet_persimm_check(persimm_map_dissoc(old_map, argv + 1, new_map));
        } else {
            janet_persimm_entry_t entry = { argv[1], argv[2] };
            janet_persimm_check(persimm_map_assoc(old_map, &entry, new_map));
        }

        return janet_wrap_abstract(new_map);
    }

    persimm_vector_t *old_vector =
        (persimm_vector_t *)janet_getabstract(argv, 0, &persimm_vector_type);

    size_t index;
    if (!persimm_vector_index(old_vector, janet_persimm_integer(argv[1]), &index)) {
        janet_panic("index out of bounds");
    }

    persimm_vector_t *new_vector = janet_persimm_alloc_vector();
    janet_persimm_check(persimm_vector_update(old_vector, index, argv + 2, new_vector));

    return janet_wrap_abstract(new_vector);
}

static Janet cfun_persimm_dissoc(int32_t argc, Janet *argv) {
    janet_fixarity(argc, 2);

    persimm_map_t *old_map = (persimm_map_t *)janet_getabstract(argv, 0, &persimm_map_type);
    janet_persimm_check_key(argv[1]);

    persimm_map_t *new_map = janet_persimm_alloc_map();
    janet_persimm_check(persimm_map_dissoc(old_map, argv + 1, new_map));

    return janet_wrap_abstract(new_map);
}

static Janet cfun_persimm_disj(int32_t argc, Janet *argv) {
    janet_fixarity(argc, 2);

    persimm_set_t *old_set = (persimm_set_t *)janet_getabstract(argv, 0, &persimm_set_type);
    janet_persimm_check_key(argv[1]);

    persimm_set_t *new_set = janet_persimm_alloc_set();
    janet_persimm_check(persimm_set_disj(old_set, argv + 1, new_set));

    return janet_wrap_abstract(new_set);
}

static Janet cfun_persimm_transient(int32_t argc, Janet *argv) {
    janet_fixarity(argc, 1);

    if (janet_checkabstract(argv[0], &persimm_vector_type)) {
        persimm_vector_transient_t *transient =
            (persimm_vector_transient_t *)janet_abstract(
                &persimm_vector_transient_type, sizeof(persimm_vector_transient_t));
        persimm_vector_to_transient((persimm_vector_t *)janet_unwrap_abstract(argv[0]), transient);
        return janet_wrap_abstract(transient);
    }

    if (janet_checkabstract(argv[0], &persimm_map_type)) {
        persimm_map_transient_t *transient =
            (persimm_map_transient_t *)janet_abstract(
                &persimm_map_transient_type, sizeof(persimm_map_transient_t));
        persimm_map_to_transient((persimm_map_t *)janet_unwrap_abstract(argv[0]), transient);
        return janet_wrap_abstract(transient);
    }

    if (janet_checkabstract(argv[0], &persimm_set_type)) {
        persimm_set_transient_t *transient =
            (persimm_set_transient_t *)janet_abstract(
                &persimm_set_transient_type, sizeof(persimm_set_transient_t));
        persimm_set_to_transient((persimm_set_t *)janet_unwrap_abstract(argv[0]), transient);
        return janet_wrap_abstract(transient);
    }

    janet_panicf("expected a persimmon vector, map or set, got %v", argv[0]);
}

static Janet cfun_persimm_persistent(int32_t argc, Janet *argv) {
    janet_fixarity(argc, 1);

    if (janet_checkabstract(argv[0], &persimm_vector_transient_type)) {
        persimm_vector_t *vector =
            (persimm_vector_t *)janet_abstract(&persimm_vector_type, sizeof(persimm_vector_t));
        janet_persimm_check(persimm_vector_transient_persist(
            (persimm_vector_transient_t *)janet_unwrap_abstract(argv[0]), vector));
        return janet_wrap_abstract(vector);
    }

    if (janet_checkabstract(argv[0], &persimm_map_transient_type)) {
        persimm_map_t *map =
            (persimm_map_t *)janet_abstract(&persimm_map_type, sizeof(persimm_map_t));
        janet_persimm_check(persimm_map_transient_persist(
            (persimm_map_transient_t *)janet_unwrap_abstract(argv[0]), map));
        return janet_wrap_abstract(map);
    }

    if (janet_checkabstract(argv[0], &persimm_set_transient_type)) {
        persimm_set_t *set =
            (persimm_set_t *)janet_abstract(&persimm_set_type, sizeof(persimm_set_t));
        janet_persimm_check(persimm_set_transient_persist(
            (persimm_set_transient_t *)janet_unwrap_abstract(argv[0]), set));
        return janet_wrap_abstract(set);
    }

    janet_panicf("expected a persimmon transient, got %v", argv[0]);
}

static Janet cfun_persimm_conj_mut(int32_t argc, Janet *argv) {
    janet_fixarity(argc, 2);

    if (janet_checkabstract(argv[0], &persimm_vector_transient_type)) {
        janet_persimm_check(persimm_vector_transient_push(
            (persimm_vector_transient_t *)janet_unwrap_abstract(argv[0]), argv + 1));
        return argv[0];
    }

    if (janet_checkabstract(argv[0], &persimm_set_transient_type)) {
        janet_persimm_check_key(argv[1]);
        janet_persimm_check(persimm_set_transient_conj(
            (persimm_set_transient_t *)janet_unwrap_abstract(argv[0]), argv + 1));
        return argv[0];
    }

    janet_panicf("expected a persimmon vector or set transient, got %v", argv[0]);
}

static Janet cfun_persimm_assoc_mut(int32_t argc, Janet *argv) {
    janet_fixarity(argc, 3);

    if (janet_checkabstract(argv[0], &persimm_map_transient_type)) {
        persimm_map_transient_t *transient =
            (persimm_map_transient_t *)janet_unwrap_abstract(argv[0]);
        janet_persimm_check_key(argv[1]);
        if (janet_checktype(argv[2], JANET_NIL)) {
            janet_persimm_check(persimm_map_transient_dissoc(transient, argv + 1));
        } else {
            janet_persimm_entry_t entry = { argv[1], argv[2] };
            janet_persimm_check(persimm_map_transient_assoc(transient, &entry));
        }
        return argv[0];
    }

    persimm_vector_transient_t *transient =
        (persimm_vector_transient_t *)janet_getabstract(
            argv, 0, &persimm_vector_transient_type);
    size_t index;
    if (!persimm_vector_index(&transient->value, janet_persimm_integer(argv[1]), &index)) {
        janet_panic("index out of bounds");
    }
    janet_persimm_check(persimm_vector_transient_update(transient, index, argv + 2));
    return argv[0];
}

static Janet cfun_persimm_dissoc_mut(int32_t argc, Janet *argv) {
    janet_fixarity(argc, 2);
    janet_persimm_check_key(argv[1]);
    persimm_map_transient_t *transient =
        (persimm_map_transient_t *)janet_getabstract(argv, 0, &persimm_map_transient_type);
    janet_persimm_check(persimm_map_transient_dissoc(transient, argv + 1));
    return argv[0];
}

static Janet cfun_persimm_disj_mut(int32_t argc, Janet *argv) {
    janet_fixarity(argc, 2);
    janet_persimm_check_key(argv[1]);
    persimm_set_transient_t *transient =
        (persimm_set_transient_t *)janet_getabstract(argv, 0, &persimm_set_transient_type);
    janet_persimm_check(persimm_set_transient_disj(transient, argv + 1));
    return argv[0];
}

/*
 * A set's elements are its keys, so this answers for both structures. Nothing
 * holds nil as a key, so asking after it is false rather than an error.
 */
static Janet cfun_persimm_has_key(int32_t argc, Janet *argv) {
    janet_fixarity(argc, 2);

    if (janet_checktype(argv[1], JANET_NIL)) return janet_wrap_false();

    if (janet_checkabstract(argv[0], &persimm_map_type)) {
        persimm_map_t *map = (persimm_map_t *)janet_unwrap_abstract(argv[0]);
        return janet_wrap_boolean(persimm_map_has(map, argv + 1));
    }

    if (janet_checkabstract(argv[0], &persimm_set_type)) {
        persimm_set_t *set = (persimm_set_t *)janet_unwrap_abstract(argv[0]);
        return janet_wrap_boolean(persimm_set_has(set, argv + 1));
    }

    janet_panicf("expected a persimmon map or set, got %v", argv[0]);
}

static Janet cfun_persimm_to_table(int32_t argc, Janet *argv) {
    janet_fixarity(argc, 1);

    persimm_map_t *map = (persimm_map_t *)janet_getabstract(argv, 0, &persimm_map_type);
    JanetTable *table = janet_table((int32_t)map->count);
    persimm_map_foreach(map, janet_persimm_map_to_table_visit, table);

    return janet_wrap_table(table);
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

    janet_persimm_list_t *new_list = janet_persimm_alloc_list();

    if (old_list->list.count > 0) {
        janet_persimm_check(persimm_list_rest(&old_list->list, &new_list->list));
    } else {
        persimm_list_clone(&old_list->list, &new_list->list);
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

    if (janet_checkabstract(argv[0], &persimm_set_type)) {
        persimm_set_t *set = (persimm_set_t *)janet_unwrap_abstract(argv[0]);
        JanetArray *array = janet_array((int32_t)set->count);
        persimm_set_foreach(set, janet_persimm_to_array_visit, array);
        return janet_wrap_array(array);
    }

    /* A map's elements are its entries, so each one arrives as a pair. */
    if (janet_checkabstract(argv[0], &persimm_map_type)) {
        persimm_map_t *map = (persimm_map_t *)janet_unwrap_abstract(argv[0]);
        JanetArray *array = janet_array((int32_t)map->count);
        persimm_map_foreach(map, janet_persimm_map_to_array_visit, array);
        return janet_wrap_array(array);
    }

    janet_panicf("expected a persimmon collection, got %v", argv[0]);
}

static const JanetReg cfuns[] = {
    {"vec", cfun_persimm_vec, NULL},
    {"list", cfun_persimm_list, NULL},
    {"map", cfun_persimm_map, NULL},
    {"set", cfun_persimm_set, NULL},
    {"assoc", cfun_persimm_assoc, NULL},
    {"dissoc", cfun_persimm_dissoc, NULL},
    {"conj", cfun_persimm_conj, NULL},
    {"disj", cfun_persimm_disj, NULL},
    {"transient", cfun_persimm_transient, NULL},
    {"persistent!", cfun_persimm_persistent, NULL},
    {"conj!", cfun_persimm_conj_mut, NULL},
    {"assoc!", cfun_persimm_assoc_mut, NULL},
    {"dissoc!", cfun_persimm_dissoc_mut, NULL},
    {"disj!", cfun_persimm_disj_mut, NULL},
    {"has-key?", cfun_persimm_has_key, NULL},
    {"first", cfun_persimm_first, NULL},
    {"rest", cfun_persimm_rest, NULL},
    {"to-array", cfun_persimm_to_array, NULL},
    {"to-table", cfun_persimm_to_table, NULL},
    {NULL, NULL, NULL}
};

/* Environment Registration */

void persimm_register_type(JanetTable *env) {
    (void) env;
    janet_register_abstract_type(&persimm_vector_type);
    janet_register_abstract_type(&persimm_list_type);
    janet_register_abstract_type(&persimm_map_type);
    janet_register_abstract_type(&persimm_set_type);
    janet_register_abstract_type(&persimm_vector_transient_type);
    janet_register_abstract_type(&persimm_map_transient_type);
    janet_register_abstract_type(&persimm_set_transient_type);
}

void persimm_register_functions(JanetTable *env) {
    janet_cfuns(env, "persimmon", cfuns);
}

JANET_MODULE_ENTRY(JanetTable *env) {
    persimm_register_type(env);
    persimm_register_functions(env);
}
