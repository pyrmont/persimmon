#include "wrapper.h"
#include "../../../include/persimmon.h"

/*
 * The Janet binding. Elements are Janet values stored inline, so no reference
 * counting is needed: the collector traces them instead.
 */

/* Elements */

static void janet_persimm_trace(const void *slot, void *ctx) {
    (void) ctx;
    janet_mark(*(const Janet *)slot);
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
    janet_persimm_equals_key,
    NULL, /* Retain */
    NULL, /* Release */
    janet_persimm_trace
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

static bool janet_persimm_index(size_t count, Janet input, size_t *index) {
    if (!janet_checktype(input, JANET_NUMBER)) janet_panic("expected index as number");
    int32_t value = janet_unwrap_integer(input);
    if (janet_unwrap_number(input) - (double)value != 0) janet_panic("expected index as integer");

    if (value >= 0) {
        uint32_t position = (uint32_t)value;
        if (position >= count) return false;
        *index = (size_t)position;
        return true;
    }

    uint32_t distance = (uint32_t)(-(value + 1)) + 1;
    if (distance > count) return false;
    *index = count - (size_t)distance;
    return true;
}

/* Unmarshalling moves its unpublished value through a transient one item at a
 * time. Persisting after each item keeps everything already read visible to
 * Janet's collector while the next item is decoded. */
static persimm_status janet_persimm_vector_build_push(persimm_vector_t *vector,
                                                      const void *elem) {
    persimm_vector_transient_t transient;
    persimm_status status = persimm_vector_to_transient(vector, &transient);
    if (PERSIMM_OK != status) return status;
    persimm_vector_deinit(vector);
    status = persimm_vector_transient_push(&transient, elem);
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
    persimm_status status = persimm_map_to_transient(map, &transient);
    if (PERSIMM_OK != status) return status;
    persimm_map_deinit(map);
    status = persimm_map_transient_assoc(&transient, entry);
    persimm_status persisted = persimm_map_transient_persist(&transient, map);
    return PERSIMM_OK == status ? persisted : status;
}

static persimm_status janet_persimm_set_build_conj(persimm_set_t *set,
                                                   const void *elem) {
    persimm_set_transient_t transient;
    persimm_status status = persimm_set_to_transient(set, &transient);
    if (PERSIMM_OK != status) return status;
    persimm_set_deinit(set);
    status = persimm_set_transient_conj(&transient, elem);
    persimm_status persisted = persimm_set_transient_persist(&transient, set);
    return PERSIMM_OK == status ? persisted : status;
}

static void janet_persimm_to_array_visit(const void *slot, size_t index, void *ctx) {
    (void) index;
    janet_array_push((JanetArray *)ctx, *(const Janet *)slot);
}

static void janet_persimm_to_string_visit(const void *slot, size_t index, void *ctx) {
    JanetBuffer *buf = (JanetBuffer *)ctx;
    if (index > 0) janet_buffer_push_cstring(buf, " ");
    janet_buffer_push_string(buf, janet_to_string(*(const Janet *)slot));
}

static void janet_persimm_hash_visit(const void *slot, size_t index, void *ctx) {
    (void) index;
    uint32_t *hash = (uint32_t *)ctx;
    *hash = (*hash << 5) + *hash + (uint32_t)janet_hash(*(const Janet *)slot);
}

/*
 * A vector and a list are hashed in order, because their order is what they
 * are. A map and a set are not: two of them holding the same entries may hand
 * them over in different orders when keys share a hash, so their entries are
 * combined by adding, which does not care.
 */

static void janet_persimm_set_hash_visit(const void *slot, size_t index, void *ctx) {
    (void) index;
    uint32_t *hash = (uint32_t *)ctx;
    *hash += (uint32_t)janet_hash(*(const Janet *)slot) * 2654435761u;
}

static void janet_persimm_map_hash_visit(const void *slot, size_t index, void *ctx) {
    (void) index;
    uint32_t *hash = (uint32_t *)ctx;
    const janet_persimm_entry_t *entry = (const janet_persimm_entry_t *)slot;
    uint32_t key = (uint32_t)janet_hash(entry->key);
    uint32_t value = (uint32_t)janet_hash(entry->value);
    *hash += (key * 31u + value) * 2654435761u;
}

static void janet_persimm_map_to_string_visit(const void *slot, size_t index, void *ctx) {
    JanetBuffer *buf = (JanetBuffer *)ctx;
    const janet_persimm_entry_t *entry = (const janet_persimm_entry_t *)slot;
    if (index > 0) janet_buffer_push_cstring(buf, " ");
    janet_buffer_push_string(buf, janet_to_string(entry->key));
    janet_buffer_push_cstring(buf, " ");
    janet_buffer_push_string(buf, janet_to_string(entry->value));
}

static void janet_persimm_map_to_array_visit(const void *slot, size_t index, void *ctx) {
    (void) index;
    const janet_persimm_entry_t *entry = (const janet_persimm_entry_t *)slot;
    Janet pair[2] = { entry->key, entry->value };
    janet_array_push((JanetArray *)ctx, janet_wrap_tuple(janet_tuple_n(pair, 2)));
}

static void janet_persimm_map_to_table_visit(const void *slot, size_t index, void *ctx) {
    (void) index;
    const janet_persimm_entry_t *entry = (const janet_persimm_entry_t *)slot;
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

static void janet_persimm_marshal_visit(const void *slot, size_t index, void *ctx) {
    (void) index;
    janet_marshal_janet((JanetMarshalContext *)ctx, *(const Janet *)slot);
}

static void janet_persimm_map_marshal_visit(const void *slot, size_t index, void *ctx) {
    (void) index;
    const janet_persimm_entry_t *entry = (const janet_persimm_entry_t *)slot;
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

static void janet_persimm_reverse_visit(const void *slot, size_t index, void *ctx) {
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

/* Calling */

/*
 * A structure in the operator position looks something up in itself, so that
 * one can stand where a function is wanted: `(filter a-set xs)` keeps the
 * elements the set holds, and `(map a-map ks)` reads a value for each key.
 *
 * Janet's own structures take exactly one argument there and answer as `in`
 * does, and these do the same, which divides the two kinds as it divides
 * Janet's. A key a map or a set does not hold is merely absent and answers
 * nil, as it does for a struct, which is what lets a set be a predicate: one
 * that raised at the first element it did not hold could not be used as one.
 * An index outside a vector or a list is a bad key and raises, as it does for
 * a tuple.
 *
 * A map and a set can therefore ask `get` and be done with it. A vector and a
 * list cannot ask `in`, because the message it raises for an abstract names no
 * range and reads as a dictionary miss. They look the key up themselves so
 * that the one message Janet gives for a bad index, whatever is wrong with it,
 * can be given here too. The range that message names takes in the negative
 * indices the wrapper accepts and Janet's own types do not.
 */
static Janet janet_persimm_call_get(void *p, int32_t argc, Janet *argv) {
    janet_fixarity(argc, 1);
    return janet_get(janet_wrap_abstract(p), argv[0]);
}

static Janet janet_persimm_call_indexed(void *p, Janet key, size_t count,
                                        const JanetMethod *methods,
                                        int (*get)(void *, Janet, Janet *)) {
    Janet out;

    if (janet_checktype(key, JANET_KEYWORD)) {
        if (NULL != methods && janet_getmethod(janet_unwrap_keyword(key), methods, &out)) {
            return out;
        }
    /* An integer is never a key janet_persimm_index refuses, so `get` reaches
       the lookup itself and answers falsely only for one out of range. */
    } else if (janet_checkint(key) && get(p, key, &out)) {
        return out;
    }

    janet_panicf("expected integer key for %s in range [%d, %d), got %v",
                 janet_abstract_type(p)->name, -(int)count, (int)count, key);
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
    const void *slot = persimm_vector_at(vector, index);
    if (NULL == slot) janet_panic("invalid index");
    return *(const Janet *)slot;
}

static int janet_persimm_vector_get(void *p, Janet key, Janet *out) {
    if (janet_checktype(key, JANET_KEYWORD)) {
        return janet_getmethod(janet_unwrap_keyword(key), persimm_vector_methods, out);
    }

    persimm_vector_t *vector = (persimm_vector_t *)p;

    size_t index;
    if (!janet_persimm_index(vector->count, key, &index)) return 0;

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

static Janet janet_persimm_vector_call(void *p, int32_t argc, Janet *argv) {
    janet_fixarity(argc, 1);
    return janet_persimm_call_indexed(p, argv[0], ((persimm_vector_t *)p)->count,
                                      persimm_vector_methods, janet_persimm_vector_get);
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
    janet_persimm_vector_call, /* Call */
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
        int order = janet_compare(*(const Janet *)persimm_vector_at(a, i),
                                  *(const Janet *)persimm_vector_at(b, i));
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
    if (!janet_persimm_index(wrapper->list.count, key, &index)) return 0;

    const void *slot = persimm_list_at_from(&wrapper->list, &wrapper->cursor, index);
    if (NULL == slot) janet_panic("invalid index");

    *out = *(const Janet *)slot;
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

static Janet janet_persimm_list_call(void *p, int32_t argc, Janet *argv) {
    janet_fixarity(argc, 1);
    return janet_persimm_call_indexed(p, argv[0], ((janet_persimm_list_t *)p)->list.count,
                                      persimm_list_methods, janet_persimm_list_get);
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
    janet_persimm_list_call, /* Call */
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
        int order = janet_compare(
            *(const Janet *)persimm_list_at_from(&a->list, &a->cursor, i),
            *(const Janet *)persimm_list_at_from(&b->list, &b->cursor, i));
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
    janet_persimm_check(persimm_list_clone(&ordered, &wrapper->list));
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
        const void *value = persimm_map_find(map, &key);
        if (NULL != value) {
            *out = *(const Janet *)value;
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

    const void *entry = janet_checktype(key, JANET_NIL) ? persimm_map_next(map, NULL)
                                                        : persimm_map_next(map, &key);
    if (NULL == entry) return janet_wrap_nil();

    return ((const janet_persimm_entry_t *)entry)->key;
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
    janet_persimm_call_get, /* Call */
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

    for (const void *entry = persimm_map_next(a, NULL);
         NULL != entry;
         entry = persimm_map_next(a, entry)) {
        const janet_persimm_entry_t *pair = (const janet_persimm_entry_t *)entry;
        const void *value = persimm_map_find(b, &pair->key);
        if (NULL == value) return janet_persimm_order_by_address(p1, p2);
        if (!janet_equals(pair->value, *(const Janet *)value)) {
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
                                         NULL, &janet_persimm_key_ops, NULL));

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
        const void *elem = persimm_set_find(set, &key);
        if (NULL != elem) {
            *out = *(const Janet *)elem;
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

    const void *elem = janet_checktype(key, JANET_NIL) ? persimm_set_next(set, NULL)
                                                       : persimm_set_next(set, &key);
    if (NULL == elem) return janet_wrap_nil();

    return *(const Janet *)elem;
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
    janet_persimm_call_get, /* Call */
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

    for (const void *elem = persimm_set_next(a, NULL);
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
    janet_persimm_check(
        persimm_set_init(set, sizeof(Janet), &janet_persimm_key_ops, NULL));

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
    if (!janet_persimm_index(transient->value.count, key, &index)) return 0;
    *out = janet_persimm_vector_at(&transient->value, index);
    return 1;
}

static Janet janet_persimm_vector_transient_next(void *p, Janet key) {
    persimm_vector_transient_t *transient = (persimm_vector_transient_t *)p;
    janet_persimm_require_active(transient->active);
    return janet_persimm_next_index(transient->value.count, key);
}

/* A transient has no method table, and the active check comes first so that a
   spent one says so rather than naming a range it no longer has. */
static Janet janet_persimm_vector_transient_call(void *p, int32_t argc, Janet *argv) {
    janet_fixarity(argc, 1);
    persimm_vector_transient_t *transient = (persimm_vector_transient_t *)p;
    janet_persimm_require_active(transient->active);
    return janet_persimm_call_indexed(p, argv[0], transient->value.count, NULL,
                                      janet_persimm_vector_transient_get);
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
    janet_persimm_vector_transient_call, /* Call */
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

    const void *value = persimm_map_find(&transient->value, &key);
    if (NULL == value) return 0;
    *out = *(const Janet *)value;
    return 1;
}

static Janet janet_persimm_map_transient_next(void *p, Janet key) {
    persimm_map_transient_t *transient = (persimm_map_transient_t *)p;
    janet_persimm_require_active(transient->active);
    const void *entry = janet_checktype(key, JANET_NIL)
        ? persimm_map_next(&transient->value, NULL)
        : persimm_map_next(&transient->value, &key);
    return (NULL == entry) ? janet_wrap_nil()
                           : ((const janet_persimm_entry_t *)entry)->key;
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
    janet_persimm_call_get, /* Call */
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

    const void *elem = persimm_set_find(&transient->value, &key);
    if (NULL == elem) return 0;
    *out = *(const Janet *)elem;
    return 1;
}

static Janet janet_persimm_set_transient_next(void *p, Janet key) {
    persimm_set_transient_t *transient = (persimm_set_transient_t *)p;
    janet_persimm_require_active(transient->active);
    const void *elem = janet_checktype(key, JANET_NIL)
        ? persimm_set_next(&transient->value, NULL)
        : persimm_set_next(&transient->value, &key);
    return (NULL == elem) ? janet_wrap_nil() : *(const Janet *)elem;
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
    janet_persimm_call_get, /* Call */
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
                                         NULL, &janet_persimm_key_ops, NULL));
    return map;
}

static persimm_set_t *janet_persimm_alloc_set(void) {
    return (persimm_set_t *)janet_abstract(&persimm_set_type, sizeof(persimm_set_t));
}

static persimm_set_t *janet_persimm_new_set(void) {
    persimm_set_t *set = janet_persimm_alloc_set();
    janet_persimm_check(
        persimm_set_init(set, sizeof(Janet), &janet_persimm_key_ops, NULL));
    return set;
}

/*
 * Builders
 *
 * Each builder seeds a collection through a single transient rather than one
 * persistent update per element. The values stay reachable through the
 * caller's arguments for the whole batch, so nothing can be collected
 * part-way through. The variadic constructors and the from-* conversions
 * share these, differing only in where their elements come from.
 */

static persimm_vector_t *janet_persimm_build_vector(const Janet *items, int32_t len) {
    persimm_vector_t *vector = janet_persimm_new_vector();
    if (0 == len) return vector;

    persimm_vector_transient_t transient;
    janet_persimm_check(persimm_vector_to_transient(vector, &transient));
    persimm_vector_deinit(vector);
    for (int32_t i = 0; i < len; i++) {
        persimm_status status = persimm_vector_transient_push(&transient, &items[i]);
        if (PERSIMM_OK != status) {
            persimm_vector_transient_deinit(&transient);
            janet_panic(persimm_status_string(status));
        }
    }
    janet_persimm_check(persimm_vector_transient_persist(&transient, vector));

    return vector;
}

static janet_persimm_list_t *janet_persimm_build_list(const Janet *items, int32_t len) {
    janet_persimm_list_t *wrapper = janet_persimm_new_list();

    /* Consing walks the elements backwards so the list reads in the same
       order as the arguments it came from. */
    for (int32_t i = len - 1; i >= 0; i--) {
        janet_persimm_check(janet_persimm_list_build_cons(&wrapper->list, &items[i]));
    }

    return wrapper;
}

static persimm_set_t *janet_persimm_build_set(const Janet *items, int32_t len) {
    persimm_set_t *set = janet_persimm_new_set();
    if (0 == len) return set;

    /* Reject nil before building so a bad element leaves nothing half-made. */
    for (int32_t i = 0; i < len; i++) janet_persimm_check_key(items[i]);

    persimm_set_transient_t transient;
    janet_persimm_check(persimm_set_to_transient(set, &transient));
    persimm_set_deinit(set);
    for (int32_t i = 0; i < len; i++) {
        persimm_status status = persimm_set_transient_conj(&transient, &items[i]);
        if (PERSIMM_OK != status) {
            persimm_set_transient_deinit(&transient);
            janet_panic(persimm_status_string(status));
        }
    }
    janet_persimm_check(persimm_set_transient_persist(&transient, set));

    return set;
}

static void janet_persimm_seed_entry(persimm_map_transient_t *transient,
                                     Janet key, Janet value) {
    janet_persimm_entry_t entry = { key, value };
    persimm_status status = persimm_map_transient_assoc(transient, &entry);
    if (PERSIMM_OK != status) {
        persimm_map_transient_deinit(transient);
        janet_panic(persimm_status_string(status));
    }
}

/* Pairs arrive flat: a key at each even offset and its value just after it. */
static persimm_map_t *janet_persimm_build_map(const Janet *pairs, int32_t len) {
    persimm_map_t *map = janet_persimm_new_map();
    if (0 == len) return map;

    for (int32_t i = 0; i < len; i += 2) janet_persimm_check_key(pairs[i]);

    persimm_map_transient_t transient;
    janet_persimm_check(persimm_map_to_transient(map, &transient));
    persimm_map_deinit(map);
    for (int32_t i = 0; i < len; i += 2) {
        /* A nil value is no entry, as it is in a Janet table. */
        if (janet_checktype(pairs[i + 1], JANET_NIL)) continue;
        janet_persimm_seed_entry(&transient, pairs[i], pairs[i + 1]);
    }
    janet_persimm_check(persimm_map_transient_persist(&transient, map));

    return map;
}

/*
 * Sources
 *
 * `into` reads its source as a sequence of elements, or of entries when the
 * destination is a map. Everything that is not already a Janet indexed
 * collection is copied into an array first, which keeps one walk in one place
 * at the cost of a shallow copy. A map's entries become key-value tuples, as
 * they do in to-array, so a map reads as a sequence of pairs anywhere
 * elements are wanted.
 */
static JanetArray *janet_persimm_elements(Janet coll) {
    const Janet *items;
    int32_t len;
    if (janet_indexed_view(coll, &items, &len)) {
        JanetArray *array = janet_array(len);
        for (int32_t i = 0; i < len; i++) janet_array_push(array, items[i]);
        return array;
    }

    const JanetKV *kvs;
    int32_t cap;
    if (janet_dictionary_view(coll, &kvs, &len, &cap)) {
        JanetArray *array = janet_array(len);
        for (int32_t i = 0; i < cap; i++) {
            if (janet_checktype(kvs[i].key, JANET_NIL)) continue;
            if (janet_checktype(kvs[i].value, JANET_NIL)) continue;
            Janet pair[2] = { kvs[i].key, kvs[i].value };
            janet_array_push(array, janet_wrap_tuple(janet_tuple_n(pair, 2)));
        }
        return array;
    }

    if (janet_checkabstract(coll, &persimm_vector_type)) {
        persimm_vector_t *vector = (persimm_vector_t *)janet_unwrap_abstract(coll);
        JanetArray *array = janet_array((int32_t)vector->count);
        persimm_vector_foreach(vector, janet_persimm_to_array_visit, array);
        return array;
    }

    if (janet_checkabstract(coll, &persimm_list_type)) {
        janet_persimm_list_t *wrapper = (janet_persimm_list_t *)janet_unwrap_abstract(coll);
        JanetArray *array = janet_array((int32_t)wrapper->list.count);
        persimm_list_foreach(&wrapper->list, janet_persimm_to_array_visit, array);
        return array;
    }

    if (janet_checkabstract(coll, &persimm_set_type)) {
        persimm_set_t *set = (persimm_set_t *)janet_unwrap_abstract(coll);
        JanetArray *array = janet_array((int32_t)set->count);
        persimm_set_foreach(set, janet_persimm_to_array_visit, array);
        return array;
    }

    if (janet_checkabstract(coll, &persimm_map_type)) {
        persimm_map_t *map = (persimm_map_t *)janet_unwrap_abstract(coll);
        JanetArray *array = janet_array((int32_t)map->count);
        persimm_map_foreach(map, janet_persimm_map_to_array_visit, array);
        return array;
    }

    janet_panicf("cannot read elements from %v", coll);
}

/* An entry source is read as pairs, so anything but a pair is an error. */
static void janet_persimm_pair(Janet elem, Janet *key, Janet *value) {
    const Janet *items;
    int32_t len;
    if (!janet_indexed_view(elem, &items, &len) || 2 != len) {
        janet_panicf("expected a key-value pair, got %v", elem);
    }
    *key = items[0];
    *value = items[1];
}

/*
 * Filling a destination allocates it through janet_abstract, which can
 * collect. The elements are only reachable from a C local until then, so they
 * are rooted across that one allocation. Nothing the persistent core does
 * afterwards allocates through Janet.
 */
static persimm_vector_t *janet_persimm_into_vector(const persimm_vector_t *target,
                                                   JanetArray *elems) {
    janet_gcroot(janet_wrap_array(elems));
    persimm_vector_t *result = janet_persimm_alloc_vector();
    janet_gcunroot(janet_wrap_array(elems));

    persimm_vector_transient_t transient;
    janet_persimm_check(persimm_vector_to_transient(target, &transient));
    for (int32_t i = 0; i < elems->count; i++) {
        persimm_status status = persimm_vector_transient_push(&transient, &elems->data[i]);
        if (PERSIMM_OK != status) {
            persimm_vector_transient_deinit(&transient);
            janet_panic(persimm_status_string(status));
        }
    }
    janet_persimm_check(persimm_vector_transient_persist(&transient, result));

    return result;
}

/*
 * The elements go in front of the target, in their own order, so the result
 * reads as the source followed by what the target already held.
 */
static janet_persimm_list_t *janet_persimm_into_list(const persimm_list_t *target,
                                                     JanetArray *elems) {
    janet_gcroot(janet_wrap_array(elems));
    janet_persimm_list_t *result = janet_persimm_alloc_list();
    janet_gcunroot(janet_wrap_array(elems));

    janet_persimm_check(persimm_list_clone(target, &result->list));
    for (int32_t i = elems->count - 1; i >= 0; i--) {
        janet_persimm_check(janet_persimm_list_build_cons(&result->list, &elems->data[i]));
    }

    return result;
}

static persimm_set_t *janet_persimm_into_set(const persimm_set_t *target,
                                             JanetArray *elems) {
    for (int32_t i = 0; i < elems->count; i++) janet_persimm_check_key(elems->data[i]);

    janet_gcroot(janet_wrap_array(elems));
    persimm_set_t *result = janet_persimm_alloc_set();
    janet_gcunroot(janet_wrap_array(elems));

    persimm_set_transient_t transient;
    janet_persimm_check(persimm_set_to_transient(target, &transient));
    for (int32_t i = 0; i < elems->count; i++) {
        persimm_status status = persimm_set_transient_conj(&transient, &elems->data[i]);
        if (PERSIMM_OK != status) {
            persimm_set_transient_deinit(&transient);
            janet_panic(persimm_status_string(status));
        }
    }
    janet_persimm_check(persimm_set_transient_persist(&transient, result));

    return result;
}

static persimm_map_t *janet_persimm_into_map(const persimm_map_t *target,
                                             JanetArray *pairs) {
    for (int32_t i = 0; i < pairs->count; i++) {
        Janet key;
        Janet value;
        janet_persimm_pair(pairs->data[i], &key, &value);
        janet_persimm_check_key(key);
    }

    janet_gcroot(janet_wrap_array(pairs));
    persimm_map_t *result = janet_persimm_alloc_map();
    janet_gcunroot(janet_wrap_array(pairs));

    persimm_map_transient_t transient;
    janet_persimm_check(persimm_map_to_transient(target, &transient));
    for (int32_t i = 0; i < pairs->count; i++) {
        Janet key;
        Janet value;
        janet_persimm_pair(pairs->data[i], &key, &value);
        /* A nil value is no entry, as it is in a Janet table. */
        if (janet_checktype(value, JANET_NIL)) continue;
        janet_persimm_seed_entry(&transient, key, value);
    }
    janet_persimm_check(persimm_map_transient_persist(&transient, result));

    return result;
}

/* C Functions */

/*
 * The constructors are variadic like Janet's own, so an argument is an
 * element rather than something to copy out of. Splicing converts an existing
 * collection, `(vec ;coll)`, and into does the same while naming its
 * destination rather than its source.
 */
JANET_FN(cfun_persimm_vec,
         "(vec & xs)",
         "Creates a persistent vector whose elements are xs, in order. Splice "
         "a collection or use into to convert one. Returns the vector.") {
    return janet_wrap_abstract(janet_persimm_build_vector(argv, argc));
}

JANET_FN(cfun_persimm_list,
         "(list & xs)",
         "Creates a persistent list whose elements are xs, in order. Splice a "
         "collection to convert one. Returns the list.") {
    return janet_wrap_abstract(janet_persimm_build_list(argv, argc));
}

JANET_FN(cfun_persimm_map,
         "(map & kvs)",
         "Creates a persistent map from alternating keys and values. A key "
         "whose value is nil adds no entry. Use into to convert a dictionary. "
         "Returns the map.") {
    if (0 != argc % 2) janet_panic("expected even number of arguments");

    return janet_wrap_abstract(janet_persimm_build_map(argv, argc));
}

JANET_FN(cfun_persimm_set,
         "(set & xs)",
         "Creates a persistent set whose elements are xs. Nil cannot be an "
         "element. Splice a collection to convert one. Returns the set.") {
    return janet_wrap_abstract(janet_persimm_build_set(argv, argc));
}

/*
 * into names its destination the way to-array and to-table name theirs, and
 * reads whatever it is given: a Janet indexed collection or dictionary, or
 * any persistent collection. It is how an existing collection is converted
 * now that a constructor argument is an element rather than a source.
 */
JANET_FN(cfun_persimm_into,
         "(into target coll)",
         "Returns a new persistent collection of target's kind holding "
         "target's elements and those of coll. A map takes coll's entries, or "
         "its elements when each is a key-value pair. Elements go at the end "
         "of a vector and in front of a list. target is unchanged.") {
    janet_fixarity(argc, 2);

    if (janet_checkabstract(argv[0], &persimm_vector_type)) {
        persimm_vector_t *target = (persimm_vector_t *)janet_unwrap_abstract(argv[0]);
        return janet_wrap_abstract(
            janet_persimm_into_vector(target, janet_persimm_elements(argv[1])));
    }

    if (janet_checkabstract(argv[0], &persimm_list_type)) {
        janet_persimm_list_t *target = (janet_persimm_list_t *)janet_unwrap_abstract(argv[0]);
        return janet_wrap_abstract(
            janet_persimm_into_list(&target->list, janet_persimm_elements(argv[1])));
    }

    if (janet_checkabstract(argv[0], &persimm_set_type)) {
        persimm_set_t *target = (persimm_set_t *)janet_unwrap_abstract(argv[0]);
        return janet_wrap_abstract(
            janet_persimm_into_set(target, janet_persimm_elements(argv[1])));
    }

    if (janet_checkabstract(argv[0], &persimm_map_type)) {
        persimm_map_t *target = (persimm_map_t *)janet_unwrap_abstract(argv[0]);
        return janet_wrap_abstract(
            janet_persimm_into_map(target, janet_persimm_elements(argv[1])));
    }

    janet_panicf("expected a persimmon collection, got %v", argv[0]);
}

/*
 * As in Clojure, conj adds an element wherever the structure can take one
 * cheapest: the end of a vector, the front of a list, anywhere in a set.
 */
JANET_FN(cfun_persimm_conj,
         "(conj coll x)",
         "Returns a new persistent collection with x added: at the end of a "
         "vector, at the front of a list, or as an element of a set. coll is "
         "unchanged.") {
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

JANET_FN(cfun_persimm_assoc,
         "(assoc coll key value)",
         "Returns a new persistent vector or map with key associated with "
         "value. Vector keys are indices and may be negative. Associating nil "
         "in a map removes the key. coll is unchanged.") {
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
    if (!janet_persimm_index(old_vector->count, argv[1], &index)) {
        janet_panic("index out of bounds");
    }

    persimm_vector_t *new_vector = janet_persimm_alloc_vector();
    janet_persimm_check(persimm_vector_update(old_vector, index, argv + 2, new_vector));

    return janet_wrap_abstract(new_vector);
}

JANET_FN(cfun_persimm_dissoc,
         "(dissoc map key)",
         "Returns a new persistent map without key. map is unchanged.") {
    janet_fixarity(argc, 2);

    persimm_map_t *old_map = (persimm_map_t *)janet_getabstract(argv, 0, &persimm_map_type);
    janet_persimm_check_key(argv[1]);

    persimm_map_t *new_map = janet_persimm_alloc_map();
    janet_persimm_check(persimm_map_dissoc(old_map, argv + 1, new_map));

    return janet_wrap_abstract(new_map);
}

JANET_FN(cfun_persimm_disj,
         "(disj set x)",
         "Returns a new persistent set without x. set is unchanged.") {
    janet_fixarity(argc, 2);

    persimm_set_t *old_set = (persimm_set_t *)janet_getabstract(argv, 0, &persimm_set_type);
    janet_persimm_check_key(argv[1]);

    persimm_set_t *new_set = janet_persimm_alloc_set();
    janet_persimm_check(persimm_set_disj(old_set, argv + 1, new_set));

    return janet_wrap_abstract(new_set);
}

JANET_FN(cfun_persimm_transient,
         "(transient coll)",
         "Creates a mutable, uniquely owned transient from a persistent vector, "
         "map or set. coll is unchanged. Returns the transient.") {
    janet_fixarity(argc, 1);

    if (janet_checkabstract(argv[0], &persimm_vector_type)) {
        persimm_vector_transient_t *transient =
            (persimm_vector_transient_t *)janet_abstract(
                &persimm_vector_transient_type, sizeof(persimm_vector_transient_t));
        janet_persimm_check(persimm_vector_to_transient(
            (persimm_vector_t *)janet_unwrap_abstract(argv[0]), transient));
        return janet_wrap_abstract(transient);
    }

    if (janet_checkabstract(argv[0], &persimm_map_type)) {
        persimm_map_transient_t *transient =
            (persimm_map_transient_t *)janet_abstract(
                &persimm_map_transient_type, sizeof(persimm_map_transient_t));
        janet_persimm_check(persimm_map_to_transient(
            (persimm_map_t *)janet_unwrap_abstract(argv[0]), transient));
        return janet_wrap_abstract(transient);
    }

    if (janet_checkabstract(argv[0], &persimm_set_type)) {
        persimm_set_transient_t *transient =
            (persimm_set_transient_t *)janet_abstract(
                &persimm_set_transient_type, sizeof(persimm_set_transient_t));
        janet_persimm_check(persimm_set_to_transient(
            (persimm_set_t *)janet_unwrap_abstract(argv[0]), transient));
        return janet_wrap_abstract(transient);
    }

    janet_panicf("expected a persimmon vector, map or set, got %v", argv[0]);
}

JANET_FN(cfun_persimm_persistent,
         "(persistent! trans)",
         "Consumes a vector, map or set transient and returns its persistent "
         "collection. Using trans afterwards is an error.") {
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

JANET_FN(cfun_persimm_conj_mut,
         "(conj! trans x)",
         "Adds x to a vector or set transient in place. Returns trans.") {
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

JANET_FN(cfun_persimm_assoc_mut,
         "(assoc! trans key value)",
         "Associates key with value in a vector or map transient in place. "
         "Vector keys are indices and may be negative. Associating nil in a "
         "map removes the key. Returns trans.") {
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
    if (!janet_persimm_index(transient->value.count, argv[1], &index)) {
        janet_panic("index out of bounds");
    }
    janet_persimm_check(persimm_vector_transient_update(transient, index, argv + 2));
    return argv[0];
}

JANET_FN(cfun_persimm_dissoc_mut,
         "(dissoc! trans key)",
         "Removes key from a map transient in place. Returns trans.") {
    janet_fixarity(argc, 2);
    janet_persimm_check_key(argv[1]);
    persimm_map_transient_t *transient =
        (persimm_map_transient_t *)janet_getabstract(argv, 0, &persimm_map_transient_type);
    janet_persimm_check(persimm_map_transient_dissoc(transient, argv + 1));
    return argv[0];
}

JANET_FN(cfun_persimm_disj_mut,
         "(disj! trans x)",
         "Removes x from a set transient in place. Returns trans.") {
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
JANET_FN(cfun_persimm_has_key,
         "(has-key? coll key)",
         "Returns true if the persistent map or set coll contains key, or false "
         "otherwise. A nil key always returns false.") {
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

JANET_FN(cfun_persimm_to_table,
         "(to-table map)",
         "Copies the entries of a persistent map into a mutable Janet table. "
         "Returns the table.") {
    janet_fixarity(argc, 1);

    persimm_map_t *map = (persimm_map_t *)janet_getabstract(argv, 0, &persimm_map_type);
    JanetTable *table = janet_table((int32_t)map->count);
    persimm_map_foreach(map, janet_persimm_map_to_table_visit, table);

    return janet_wrap_table(table);
}

JANET_FN(cfun_persimm_first,
         "(first list)",
         "Returns the first element of a persistent list, or nil if the list is "
         "empty.") {
    janet_fixarity(argc, 1);

    janet_persimm_list_t *wrapper =
        (janet_persimm_list_t *)janet_getabstract(argv, 0, &persimm_list_type);

    const void *slot = persimm_list_first(&wrapper->list);
    if (NULL == slot) return janet_wrap_nil();

    return *(const Janet *)slot;
}

/*
 * The rest of an empty list is an empty list, as in Clojure, rather than an
 * error.
 */
JANET_FN(cfun_persimm_rest,
         "(rest list)",
         "Returns a persistent list without its first element. The rest of an "
         "empty list is an empty list. list is unchanged.") {
    janet_fixarity(argc, 1);

    janet_persimm_list_t *old_list =
        (janet_persimm_list_t *)janet_getabstract(argv, 0, &persimm_list_type);

    janet_persimm_list_t *new_list = janet_persimm_alloc_list();

    if (old_list->list.count > 0) {
        janet_persimm_check(persimm_list_rest(&old_list->list, &new_list->list));
    } else {
        janet_persimm_check(persimm_list_clone(&old_list->list, &new_list->list));
    }

    return janet_wrap_abstract(new_list);
}

JANET_FN(cfun_persimm_to_array,
         "(to-array coll)",
         "Copies a persistent collection into a mutable Janet array. Map "
         "entries become key-value tuples. Returns the array.") {
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

/* The table is a local rather than a file-scope static because JANET_REG
 * initialises each entry's source line from a const variable emitted by
 * JANET_FN. That is not a constant expression, so MSVC rejects it in an
 * object with static storage duration. janet_cfuns_ext copies what it
 * needs during the call, so the table need not outlive it. */
void persimm_register_functions(JanetTable *env) {
    JanetRegExt cfuns[] = {
        JANET_REG("vec", cfun_persimm_vec),
        JANET_REG("list", cfun_persimm_list),
        JANET_REG("map", cfun_persimm_map),
        JANET_REG("set", cfun_persimm_set),
        JANET_REG("assoc", cfun_persimm_assoc),
        JANET_REG("dissoc", cfun_persimm_dissoc),
        JANET_REG("conj", cfun_persimm_conj),
        JANET_REG("disj", cfun_persimm_disj),
        JANET_REG("transient", cfun_persimm_transient),
        JANET_REG("persistent!", cfun_persimm_persistent),
        JANET_REG("conj!", cfun_persimm_conj_mut),
        JANET_REG("assoc!", cfun_persimm_assoc_mut),
        JANET_REG("dissoc!", cfun_persimm_dissoc_mut),
        JANET_REG("disj!", cfun_persimm_disj_mut),
        JANET_REG("has-key?", cfun_persimm_has_key),
        JANET_REG("first", cfun_persimm_first),
        JANET_REG("rest", cfun_persimm_rest),
        JANET_REG("into", cfun_persimm_into),
        JANET_REG("to-array", cfun_persimm_to_array),
        JANET_REG("to-table", cfun_persimm_to_table),
        JANET_REG_END
    };

    janet_cfuns_ext(env, "persimmon", cfuns);
}

JANET_MODULE_ENTRY(JanetTable *env) {
    persimm_register_type(env);
    persimm_register_functions(env);
}
