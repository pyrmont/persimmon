#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "persimmon.h"

/*
 * Exercises the core through its own interface, with no host language
 * involved. Several things the Janet suite cannot reach live here.
 *
 * The first is collision nodes. A trie only builds one for keys whose hashes
 * agree in all 32 bits, and nothing a Janet test can write will make
 * janet_hash collide on demand. A host supplies the hash, so this one supplies
 * a deliberately terrible hash with four values in it and drives thousands of
 * entries through the paths that result.
 *
 * The second is the element callbacks. Janet traces rather than counts, so it
 * leaves `retain` and `release` NULL and never calls them. Everything a
 * reference counting host relies on, above all the copies made when a node is
 * shared and the hand-over when a collapsed subtree is pulled back into its
 * parent, is therefore untested from Janet's side. The host below counts every
 * element it is handed and checks the books balance.
 *
 * Allocation is also replaceable in this test build, so each point along
 * representative trie updates can fail in turn and prove it leaks no partial
 * path and leaves the original structure untouched.
 *
 * Building this at all is worth something on its own: it includes no host
 * header, so the core failing to be host-agnostic is a link error.
 */

static int failures = 0;

#define CHECK(cond, ...) do {                     \
    if (!(cond)) {                                \
        printf("  FAIL %s:%d: ", __func__, __LINE__); \
        printf(__VA_ARGS__);                      \
        printf("\n");                             \
        failures++;                               \
    }                                             \
} while (0)

/* Allocation Failure Injection */

#if defined(PERSIMM_TEST_ALLOC)
static size_t allocated_blocks = 0;
static int allocations_before_failure = -1;

void *persimm_test_calloc(size_t count, size_t size) {
    if (0 == allocations_before_failure) return NULL;
    if (allocations_before_failure > 0) allocations_before_failure--;
    void *ptr = calloc(count, size);
    if (NULL != ptr) allocated_blocks++;
    return ptr;
}

void persimm_test_free(void *ptr) {
    if (NULL == ptr) return;
    CHECK(allocated_blocks > 0, "allocator: freed more blocks than were allocated");
    if (allocated_blocks > 0) allocated_blocks--;
    free(ptr);
}

static void fail_allocation_after(int successful) {
    allocations_before_failure = successful;
}

static void allow_allocations(void) {
    allocations_before_failure = -1;
}
#endif

/* Entries */

typedef struct {
    int key;
    int value;
} entry_t;

static const persimm_entry_layout map_layout = {
    sizeof(entry_t),            /* Entry Size */
    sizeof(int),                /* Key Size */
    offsetof(entry_t, value),   /* Value Offset */
    sizeof(int)                 /* Value Size */
};

/* Keys */

static uint32_t int_hash(const void *key, size_t key_size, void *ctx) {
    (void) key_size;
    (void) ctx;
    uint32_t hash = (uint32_t)(*(const int *)key);
    hash *= 2654435761u;
    return hash ^ (hash >> 16);
}

/*
 * Four hashes for however many keys, so every key past the fourth shares its
 * hash with something and the trie has no choice but to build collision nodes.
 */
static uint32_t crowded_hash(const void *key, size_t key_size, void *ctx) {
    (void) key_size;
    (void) ctx;
    return (uint32_t)(*(const int *)key) & 3u;
}

static bool int_equals(const void *key_a, const void *key_b, size_t key_size, void *ctx) {
    (void) key_size;
    (void) ctx;
    return *(const int *)key_a == *(const int *)key_b;
}

static const persimm_key_ops spread_ops = { int_hash, int_equals, NULL, NULL, NULL };
static const persimm_key_ops crowded_ops = { crowded_hash, int_equals, NULL, NULL, NULL };

static uint32_t zero_hash(const void *key, size_t key_size, void *ctx) {
    (void) key;
    (void) key_size;
    (void) ctx;
    return 0;
}

static const persimm_key_ops zero_hash_ops = { zero_hash, NULL, NULL, NULL, NULL };

/* Test Construction Helpers */

/* These helpers repeatedly advance a local value while keeping the operation
 * being exercised explicit. Bulk fixture construction uses transients;
 * persistence tests use the public source-to-destination operations. */
static persimm_status test_vector_transient_push(persimm_vector_t *vector,
                                                 const void *elem) {
    persimm_vector_transient_t transient;
    persimm_status status = persimm_vector_to_transient(vector, &transient);
    if (PERSIMM_OK != status) return status;
    persimm_vector_deinit(vector);
    status = persimm_vector_transient_push(&transient, elem);
    persimm_status persisted = persimm_vector_transient_persist(&transient, vector);
    return PERSIMM_OK == status ? persisted : status;
}

static persimm_status test_vector_transient_update(persimm_vector_t *vector, size_t index,
                                                   const void *elem) {
    persimm_vector_transient_t transient;
    persimm_status status = persimm_vector_to_transient(vector, &transient);
    if (PERSIMM_OK != status) return status;
    persimm_vector_deinit(vector);
    status = persimm_vector_transient_update(&transient, index, elem);
    persimm_status persisted = persimm_vector_transient_persist(&transient, vector);
    return PERSIMM_OK == status ? persisted : status;
}

static persimm_status test_list_advance_cons(persimm_list_t *list, const void *elem) {
    persimm_list_t next;
    persimm_status status = persimm_list_cons(list, elem, &next);
    if (PERSIMM_OK == status) {
        persimm_list_deinit(list);
        *list = next;
    }
    return status;
}

static persimm_status test_list_advance_rest(persimm_list_t *list) {
    persimm_list_t next;
    persimm_status status = persimm_list_rest(list, &next);
    if (PERSIMM_OK == status) {
        persimm_list_deinit(list);
        *list = next;
    }
    return status;
}

static persimm_status test_map_transient_assoc(persimm_map_t *map, const void *entry) {
    persimm_map_transient_t transient;
    persimm_status status = persimm_map_to_transient(map, &transient);
    if (PERSIMM_OK != status) return status;
    persimm_map_deinit(map);
    status = persimm_map_transient_assoc(&transient, entry);
    persimm_status persisted = persimm_map_transient_persist(&transient, map);
    return PERSIMM_OK == status ? persisted : status;
}

static persimm_status test_map_transient_dissoc(persimm_map_t *map, const void *key) {
    persimm_map_transient_t transient;
    persimm_status status = persimm_map_to_transient(map, &transient);
    if (PERSIMM_OK != status) return status;
    persimm_map_deinit(map);
    status = persimm_map_transient_dissoc(&transient, key);
    persimm_status persisted = persimm_map_transient_persist(&transient, map);
    return PERSIMM_OK == status ? persisted : status;
}

static persimm_status test_map_advance_assoc(persimm_map_t *map, const void *entry) {
    persimm_map_t next;
    persimm_status status = persimm_map_assoc(map, entry, &next);
    if (PERSIMM_OK == status) {
        persimm_map_deinit(map);
        *map = next;
    }
    return status;
}

static persimm_status test_map_advance_dissoc(persimm_map_t *map, const void *key) {
    persimm_map_t next;
    persimm_status status = persimm_map_dissoc(map, key, &next);
    if (PERSIMM_OK == status) {
        persimm_map_deinit(map);
        *map = next;
    }
    return status;
}

static persimm_status test_set_transient_conj(persimm_set_t *set, const void *elem) {
    persimm_set_transient_t transient;
    persimm_status status = persimm_set_to_transient(set, &transient);
    if (PERSIMM_OK != status) return status;
    persimm_set_deinit(set);
    status = persimm_set_transient_conj(&transient, elem);
    persimm_status persisted = persimm_set_transient_persist(&transient, set);
    return PERSIMM_OK == status ? persisted : status;
}

static persimm_status test_set_transient_disj(persimm_set_t *set, const void *elem) {
    persimm_set_transient_t transient;
    persimm_status status = persimm_set_to_transient(set, &transient);
    if (PERSIMM_OK != status) return status;
    persimm_set_deinit(set);
    status = persimm_set_transient_disj(&transient, elem);
    persimm_status persisted = persimm_set_transient_persist(&transient, set);
    return PERSIMM_OK == status ? persisted : status;
}

static persimm_status test_set_advance_disj(persimm_set_t *set, const void *elem) {
    persimm_set_t next;
    persimm_status status = persimm_set_disj(set, elem, &next);
    if (PERSIMM_OK == status) {
        persimm_set_deinit(set);
        *set = next;
    }
    return status;
}

/* Traversing */

static void collect_visit(const void *slot, size_t index, void *ctx) {
    (void) index;
    entry_t **cursor = (entry_t **)ctx;
    **cursor = *(entry_t *)slot;
    (*cursor)++;
}

/* Fills `into` with every entry in iteration order and answers how many. */
static size_t drain(const persimm_map_t *map, entry_t *into) {
    entry_t *cursor = into;
    persimm_map_foreach(map, collect_visit, &cursor);
    return (size_t)(cursor - into);
}

/* Storing and Reading */

static void test_assoc_and_ref(const persimm_key_ops *ops, const char *label, int n) {
    persimm_map_t map;
    persimm_map_init(&map, &map_layout, NULL, NULL, ops, NULL);

    for (int i = 0; i < n; i++) {
        entry_t entry = { i, i * 3 };
        CHECK(PERSIMM_OK == test_map_transient_assoc(&map, &entry), "%s: assoc %d", label, i);
    }
    CHECK(map.count == (size_t)n, "%s: count is %zu, wanted %d", label, map.count, n);

    for (int i = 0; i < n; i++) {
        const int *value = (const int *)persimm_map_find(&map, &i);
        CHECK(NULL != value && *value == i * 3, "%s: find %d", label, i);
    }

    int absent = n + 1;
    CHECK(NULL == persimm_map_find(&map, &absent), "%s: found a key it does not hold", label);

    /* Storing a key again replaces the value and leaves the count alone. */
    if (n > 0) {
        entry_t again = { 0, 999 };
        test_map_transient_assoc(&map, &again);
        CHECK(map.count == (size_t)n, "%s: replacing a value grew the count", label);
        CHECK(999 == *(const int *)persimm_map_find(&map, &again.key),
              "%s: value not replaced", label);
    }

    persimm_map_deinit(&map);
}

/* Walking with next must produce what walking with foreach produces. */
static void test_iteration_agrees(const persimm_key_ops *ops, const char *label, int n) {
    persimm_map_t map;
    persimm_map_init(&map, &map_layout, NULL, NULL, ops, NULL);
    for (int i = 0; i < n; i++) {
        entry_t entry = { i, i };
        test_map_transient_assoc(&map, &entry);
    }

    entry_t *seen = malloc(sizeof(entry_t) * (size_t)(n + 1));
    size_t count = drain(&map, seen);
    CHECK(count == (size_t)n, "%s: foreach saw %zu of %d", label, count, n);

    size_t steps = 0;
    const void *entry = persimm_map_next(&map, NULL);
    while (NULL != entry && steps < (size_t)n) {
        CHECK(((entry_t *)entry)->key == seen[steps].key, "%s: next parted from foreach at %zu",
              label, steps);
        entry = persimm_map_next(&map, entry);
        steps++;
    }
    CHECK(steps == (size_t)n, "%s: next stopped after %zu of %d", label, steps, n);
    CHECK(NULL == entry, "%s: next ran past the end", label);

    free(seen);
    persimm_map_deinit(&map);
}

/* Dropping */

static void test_dissoc(const persimm_key_ops *ops, const char *label, int n) {
    persimm_map_t map;
    persimm_map_init(&map, &map_layout, NULL, NULL, ops, NULL);
    for (int i = 0; i < n; i++) {
        entry_t entry = { i, i };
        test_map_transient_assoc(&map, &entry);
    }

    for (int i = 0; i < n; i += 2) {
        CHECK(PERSIMM_OK == test_map_transient_dissoc(&map, &i), "%s: dissoc %d", label, i);
    }
    CHECK(map.count == (size_t)(n / 2), "%s: count is %zu after dropping the evens", label,
          map.count);

    for (int i = 0; i < n; i++) {
        const void *value = persimm_map_find(&map, &i);
        if (0 == i % 2) {
            CHECK(NULL == value, "%s: %d survived being dropped", label, i);
        } else {
            CHECK(NULL != value && i == *(int *)value, "%s: %d went with its neighbour", label, i);
        }
    }

    /* Dropping a key the map does not hold is not an error. */
    int zero = 0;
    CHECK(PERSIMM_OK == test_map_transient_dissoc(&map, &zero), "%s: repeated dissoc", label);
    CHECK(map.count == (size_t)(n / 2), "%s: repeated dissoc moved the count", label);

    for (int i = 1; i < n; i += 2) test_map_transient_dissoc(&map, &i);
    CHECK(0 == map.count, "%s: count is %zu once emptied", label, map.count);
    CHECK(NULL == map.root, "%s: the root outlived the last entry", label);

    persimm_map_deinit(&map);
}

/*
 * What canonical form is for: a map that reached its contents by adding and
 * removing entries is indistinguishable from one handed them outright.
 *
 * Two maps built in opposite orders agree as well, but only where no two keys
 * share a hash. A collision node keeps its entries in the order they arrived
 * and opaque keys offer no order to sort them into, so `ordered` says whether
 * that comparison is a fair one to make.
 */
static void test_canonical(const persimm_key_ops *ops, const char *label, int n, bool ordered) {
    persimm_map_t forward;
    persimm_map_t backward;
    persimm_map_t pruned;
    persimm_map_init(&forward, &map_layout, NULL, NULL, ops, NULL);
    persimm_map_init(&backward, &map_layout, NULL, NULL, ops, NULL);
    persimm_map_init(&pruned, &map_layout, NULL, NULL, ops, NULL);

    for (int i = 0; i < n; i++) {
        entry_t entry = { i, i };
        test_map_transient_assoc(&forward, &entry);
    }
    for (int i = n - 1; i >= 0; i--) {
        entry_t entry = { i, i };
        test_map_transient_assoc(&backward, &entry);
    }
    for (int i = 0; i < n * 2; i++) {
        entry_t entry = { i, i };
        test_map_transient_assoc(&pruned, &entry);
    }
    for (int i = n; i < n * 2; i++) {
        test_map_transient_dissoc(&pruned, &i);
    }

    entry_t *a = malloc(sizeof(entry_t) * (size_t)(n + 1));
    entry_t *b = malloc(sizeof(entry_t) * (size_t)(n + 1));
    entry_t *c = malloc(sizeof(entry_t) * (size_t)(n + 1));
    size_t na = drain(&forward, a);
    size_t nb = drain(&backward, b);
    size_t nc = drain(&pruned, c);

    CHECK(na == nb && nb == nc, "%s: counts %zu, %zu and %zu", label, na, nb, nc);
    for (size_t i = 0; i < na && i < nb && i < nc; i++) {
        if (ordered) {
            CHECK(a[i].key == b[i].key, "%s: build order changed the shape at %zu", label, i);
        }
        CHECK(a[i].key == c[i].key, "%s: dissoc left a different shape at %zu", label, i);
    }

    free(a);
    free(b);
    free(c);
    persimm_map_deinit(&forward);
    persimm_map_deinit(&backward);
    persimm_map_deinit(&pruned);
}

/* Changing a map that shares nodes must leave what it shares them with alone. */
static void test_sharing(const persimm_key_ops *ops, const char *label, int n) {
    persimm_map_t base;
    persimm_map_init(&base, &map_layout, NULL, NULL, ops, NULL);
    for (int i = 0; i < n; i++) {
        entry_t entry = { i, i };
        test_map_transient_assoc(&base, &entry);
    }

    persimm_map_t added;
    persimm_map_t replaced;
    persimm_map_t dropped;
    CHECK(PERSIMM_OK == persimm_map_clone(&base, &added), "%s: clone for add failed", label);
    CHECK(PERSIMM_OK == persimm_map_clone(&base, &replaced),
          "%s: clone for replacement failed", label);
    CHECK(PERSIMM_OK == persimm_map_clone(&base, &dropped),
          "%s: clone for removal failed", label);

    entry_t fresh = { n + 100, 7 };
    test_map_advance_assoc(&added, &fresh);

    CHECK(base.count == (size_t)n, "%s: the original's count moved to %zu", label, base.count);
    CHECK(NULL == persimm_map_find(&base, &fresh.key), "%s: the original gained a key", label);
    CHECK(added.count == (size_t)n + 1, "%s: the copy did not grow", label);

    int victim = n / 2;
    if (n > 0) {
        entry_t over = { victim, -1 };
        test_map_advance_assoc(&replaced, &over);
        test_map_advance_dissoc(&dropped, &victim);

        CHECK(victim == *(const int *)persimm_map_find(&base, &victim),
              "%s: the original changed", label);
        CHECK(-1 == *(const int *)persimm_map_find(&replaced, &victim),
              "%s: the copy did not", label);
        CHECK(NULL == persimm_map_find(&dropped, &victim),
              "%s: the copy kept a dropped key", label);
        CHECK(replaced.count == (size_t)n, "%s: replacing on a copy grew it", label);
        CHECK(dropped.count == (size_t)n - 1, "%s: dropping on a copy did not shrink it", label);
    }

    for (int i = 0; i < n; i++) {
        CHECK(NULL != persimm_map_find(&base, &i), "%s: the original lost %d", label, i);
        CHECK(NULL != persimm_map_find(&added, &i), "%s: the copy lost %d", label, i);
    }

    persimm_map_deinit(&added);
    persimm_map_deinit(&replaced);
    persimm_map_deinit(&dropped);

    /* Whatever the copies shared has outlived every one of them. */
    for (int i = 0; i < n; i++) {
        CHECK(NULL != persimm_map_find(&base, &i), "%s: a copy took %d with it", label, i);
    }

    persimm_map_deinit(&base);
}

/*
 * A set's elements are four bytes wide here, so a node holding an odd number
 * of them ends its entries partway through a word. That makes this the only
 * test that puts the padding before a node's children to work.
 */
static void test_set(const persimm_key_ops *ops, const char *label, int n) {
    persimm_set_t set;
    persimm_set_init(&set, sizeof(int), ops, NULL);

    for (int i = 0; i < n; i++) {
        CHECK(PERSIMM_OK == test_set_transient_conj(&set, &i), "%s: conj %d", label, i);
    }
    CHECK(set.count == (size_t)n, "%s: count is %zu, wanted %d", label, set.count, n);

    for (int i = 0; i < n; i++) test_set_transient_conj(&set, &i);
    CHECK(set.count == (size_t)n, "%s: duplicates grew the count to %zu", label, set.count);

    for (int i = 0; i < n; i++) CHECK(persimm_set_has(&set, &i), "%s: lost %d", label, i);
    int absent = n + 1;
    CHECK(!persimm_set_has(&set, &absent), "%s: found an element it does not hold", label);

    for (int i = 0; i < n; i += 2) test_set_transient_disj(&set, &i);
    CHECK(set.count == (size_t)(n / 2), "%s: count is %zu after disj", label, set.count);
    for (int i = 1; i < n; i += 2) CHECK(persimm_set_has(&set, &i), "%s: disj took %d", label, i);

    persimm_set_deinit(&set);
}

/* Reference Counting */

/*
 * A host that counts. Every key and every value is a different integer, so one
 * counter per integer records exactly how many places hold it, and the count a
 * live entry should be at is one.
 */

#define RC_SPACE 200000
#define RC_VALUE_BASE 100000

static int live[RC_SPACE];
static int rc_underflows = 0;

static void rc_retain(const void *slot, void *ctx) {
    (void) ctx;
    live[*(const int *)slot]++;
}

static void rc_release(const void *slot, void *ctx) {
    (void) ctx;
    if (--live[*(const int *)slot] < 0) rc_underflows++;
}

static const persimm_elem_ops rc_ops = { rc_retain, rc_release, NULL };

static persimm_key_ops rc_key_ops(const persimm_key_ops *ops) {
    persimm_key_ops managed = *ops;
    managed.retain = rc_retain;
    managed.release = rc_release;
    return managed;
}

typedef struct {
    int retained;
    int released;
    int traced;
} lifecycle_counts;

static void count_retain(const void *slot, void *ctx) {
    (void) slot;
    ((lifecycle_counts *)ctx)->retained++;
}

static void count_release(const void *slot, void *ctx) {
    (void) slot;
    ((lifecycle_counts *)ctx)->released++;
}

static void count_trace(const void *slot, void *ctx) {
    (void) slot;
    ((lifecycle_counts *)ctx)->traced++;
}

static void test_map_separates_key_and_value_lifecycles(void) {
    lifecycle_counts keys = { 0, 0, 0 };
    lifecycle_counts values = { 0, 0, 0 };
    persimm_key_ops key_ops = {
        int_hash, int_equals, count_retain, count_release, count_trace
    };
    persimm_elem_ops value_ops = { count_retain, count_release, count_trace };

    persimm_map_t map;
    CHECK(PERSIMM_OK ==
              persimm_map_init(&map, &map_layout, &value_ops, &values, &key_ops, &keys),
          "lifecycles: map init failed");
    entry_t entry = { 1, 2 };
    CHECK(PERSIMM_OK == test_map_transient_assoc(&map, &entry),
          "lifecycles: map assoc failed");
    CHECK(1 == keys.retained && 1 == values.retained,
          "lifecycles: map did not retain key and value separately");

    persimm_map_trace(&map);
    CHECK(1 == keys.traced && 1 == values.traced,
          "lifecycles: map did not trace key and value separately");

    persimm_map_deinit(&map);
    CHECK(1 == keys.released && 1 == values.released,
          "lifecycles: map did not release key and value separately");
}

static void check_live(const char *label, const char *when, int lo, int hi) {
    int wrong = 0;
    for (int i = 0; i < RC_SPACE; i++) {
        int expected = ((i >= lo && i < hi) ||
                        (i >= RC_VALUE_BASE + lo && i < RC_VALUE_BASE + hi)) ? 1 : 0;
        if (live[i] != expected) wrong++;
    }
    CHECK(0 == wrong, "%s: %s, %d elements at the wrong count", label, when, wrong);
}

static void test_map_refcounts(const persimm_key_ops *ops, const char *label, int n) {
    memset(live, 0, sizeof(live));
    rc_underflows = 0;

    persimm_key_ops managed_keys = rc_key_ops(ops);

    persimm_map_t base;
    persimm_map_init(&base, &map_layout, &rc_ops, NULL, &managed_keys, NULL);
    for (int i = 0; i < n; i++) {
        entry_t entry = { i, RC_VALUE_BASE + i };
        test_map_transient_assoc(&base, &entry);
    }
    check_live(label, "once built", 0, n);

    /* A copy that grows, changes and shrinks shares a great deal with what it
       was copied from, and must leave every one of those elements held once. */
    persimm_map_t copy;
    CHECK(PERSIMM_OK == persimm_map_clone(&base, &copy), "%s: map clone failed", label);
    for (int i = n; i < n + 50; i++) {
        entry_t entry = { i, RC_VALUE_BASE + i };
        test_map_advance_assoc(&copy, &entry);
    }
    for (int i = 0; i < n; i += 3) {
        test_map_advance_dissoc(&copy, &i);
    }
    for (int i = 1; i < n; i += 7) {
        entry_t entry = { i, RC_VALUE_BASE + i };
        test_map_advance_assoc(&copy, &entry);
    }

    for (int i = 0; i < n; i++) {
        CHECK(live[i] >= 1, "%s: key %d was released while the original held it", label, i);
        const int *value = (const int *)persimm_map_find(&base, &i);
        CHECK(NULL != value && RC_VALUE_BASE + i == *value, "%s: the original lost %d", label, i);
    }

    persimm_map_deinit(&copy);
    check_live(label, "once the copy went", 0, n);

    persimm_map_deinit(&base);
    check_live(label, "once the original went", 0, 0);

    CHECK(0 == rc_underflows, "%s: %d elements released more often than retained", label,
          rc_underflows);
}

static void test_set_refcounts(const persimm_key_ops *ops, const char *label, int n) {
    memset(live, 0, sizeof(live));
    rc_underflows = 0;

    persimm_key_ops managed_keys = rc_key_ops(ops);

    persimm_set_t base;
    persimm_set_init(&base, sizeof(int), &managed_keys, NULL);
    for (int i = 0; i < n; i++) test_set_transient_conj(&base, &i);

    /* A set's entries have no value, so nothing above the elements is held. */
    int wrong = 0;
    for (int i = 0; i < RC_SPACE; i++) {
        if (live[i] != ((i < n) ? 1 : 0)) wrong++;
    }
    CHECK(0 == wrong, "%s: %d elements at the wrong count once built", label, wrong);

    persimm_set_t copy;
    CHECK(PERSIMM_OK == persimm_set_clone(&base, &copy), "%s: set clone failed", label);
    for (int i = 0; i < n; i += 2) test_set_advance_disj(&copy, &i);
    for (int i = 0; i < n; i++) CHECK(persimm_set_has(&base, &i), "%s: the original lost %d",
                                      label, i);

    persimm_set_deinit(&copy);
    persimm_set_deinit(&base);

    wrong = 0;
    for (int i = 0; i < RC_SPACE; i++) {
        if (0 != live[i]) wrong++;
    }
    CHECK(0 == wrong, "%s: %d elements outlived the set", label, wrong);
    CHECK(0 == rc_underflows, "%s: %d elements over-released", label, rc_underflows);
}

/* Defaults */

/* What a host storing plain data gets without supplying anything: FNV-1a over
   the key's bytes, and memcmp. */
static void test_byte_defaults(void) {
    persimm_map_t map;
    persimm_map_init(&map, &map_layout, NULL, NULL, NULL, NULL);

    for (int i = 0; i < 500; i++) {
        entry_t entry = { i, i * 2 };
        test_map_transient_assoc(&map, &entry);
    }
    CHECK(500 == map.count, "defaults: count is %zu", map.count);
    for (int i = 0; i < 500; i++) {
        const int *value = (const int *)persimm_map_find(&map, &i);
        CHECK(NULL != value && i * 2 == *value, "defaults: find %d", i);
    }

    persimm_map_deinit(&map);
}

static void test_rejects_a_bad_layout(void) {
    persimm_map_t map;

    persimm_entry_layout no_key = { sizeof(entry_t), 0, 4, 4 };
    CHECK(PERSIMM_ERR_INVALID == persimm_map_init(&map, &no_key, NULL, NULL, NULL, NULL),
          "layout: a key of no size was accepted");
    persimm_map_deinit(&map);

    persimm_entry_layout overlapping = { 8, 4, 2, 4 };
    CHECK(PERSIMM_ERR_INVALID ==
              persimm_map_init(&map, &overlapping, NULL, NULL, NULL, NULL),
          "layout: a value inside the key was accepted");
    persimm_map_deinit(&map);

    persimm_entry_layout overrunning = { 8, 4, 4, 8 };
    CHECK(PERSIMM_ERR_INVALID ==
              persimm_map_init(&map, &overrunning, NULL, NULL, NULL, NULL),
          "layout: a value past the end of the entry was accepted");
    persimm_map_deinit(&map);

    persimm_set_t set;
    CHECK(PERSIMM_ERR_INVALID == persimm_set_init(&set, 0, NULL, NULL),
          "layout: an element of no size was accepted");
    persimm_set_deinit(&set);
}

/* Defensive Edges */

static void test_persistent_operation_contracts(void) {
    int one = 1;
    int two = 2;

    persimm_vector_t vector;
    persimm_vector_t pushed;
    persimm_vector_t updated;
    CHECK(PERSIMM_OK == persimm_vector_init(&vector, sizeof(int), NULL, NULL),
          "contract: vector init failed");
    CHECK(PERSIMM_OK == persimm_vector_push(&vector, &one, &pushed),
          "contract: vector push failed");
    CHECK(0 == vector.count && 1 == pushed.count,
          "contract: vector push changed its source");
    CHECK(PERSIMM_OK == persimm_vector_update(&pushed, 0, &two, &updated),
          "contract: vector update failed");
    const int *pushed_value = (const int *)persimm_vector_at(&pushed, 0);
    const int *updated_value = (const int *)persimm_vector_at(&updated, 0);
    CHECK(NULL != pushed_value && 1 == *pushed_value,
          "contract: vector update changed its source");
    CHECK(NULL != updated_value && 2 == *updated_value,
          "contract: vector update produced the wrong result");
    CHECK(PERSIMM_ERR_INVALID == persimm_vector_push(&pushed, &two, &pushed),
          "contract: vector push accepted an aliased destination");
    CHECK(PERSIMM_ERR_INVALID == persimm_vector_update(&pushed, 0, &two, &pushed),
          "contract: vector update accepted an aliased destination");
    CHECK(PERSIMM_ERR_INVALID == persimm_vector_clone(&pushed, &pushed),
          "contract: vector clone accepted an aliased destination");
    CHECK(1 == pushed.count && 1 == *(const int *)persimm_vector_at(&pushed, 0),
          "contract: rejected vector clone changed its source");
    persimm_vector_deinit(&updated);
    persimm_vector_deinit(&pushed);
    persimm_vector_deinit(&pushed);
    persimm_vector_deinit(&vector);

    persimm_list_t list;
    persimm_list_t consed;
    persimm_list_t rest;
    CHECK(PERSIMM_OK == persimm_list_init(&list, sizeof(int), NULL, NULL),
          "contract: list init failed");
    CHECK(PERSIMM_OK == persimm_list_cons(&list, &one, &consed),
          "contract: list cons failed");
    CHECK(0 == list.count && 1 == consed.count,
          "contract: list cons changed its source");
    CHECK(PERSIMM_OK == persimm_list_rest(&consed, &rest),
          "contract: list rest failed");
    CHECK(1 == consed.count && 0 == rest.count,
          "contract: list rest changed its source");
    CHECK(PERSIMM_ERR_INVALID == persimm_list_cons(&consed, &two, &consed),
          "contract: list cons accepted an aliased destination");
    CHECK(PERSIMM_ERR_INVALID == persimm_list_rest(&consed, &consed),
          "contract: list rest accepted an aliased destination");
    CHECK(PERSIMM_ERR_INVALID == persimm_list_clone(&consed, &consed),
          "contract: list clone accepted an aliased destination");
    CHECK(1 == consed.count && 1 == *(const int *)persimm_list_first(&consed),
          "contract: rejected list clone changed its source");
    persimm_list_deinit(&rest);
    persimm_list_deinit(&consed);
    persimm_list_deinit(&consed);
    persimm_list_deinit(&list);

    persimm_map_t map;
    persimm_map_t associated;
    persimm_map_t dissociated;
    entry_t entry = { one, two };
    CHECK(PERSIMM_OK ==
              persimm_map_init(&map, &map_layout, NULL, NULL, &spread_ops, NULL),
          "contract: map init failed");
    CHECK(PERSIMM_OK == persimm_map_assoc(&map, &entry, &associated),
          "contract: map assoc failed");
    CHECK(0 == map.count && 1 == associated.count && !persimm_map_has(&map, &one),
          "contract: map assoc changed its source");
    CHECK(PERSIMM_OK == persimm_map_dissoc(&associated, &one, &dissociated),
          "contract: map dissoc failed");
    CHECK(1 == associated.count && 0 == dissociated.count && persimm_map_has(&associated, &one),
          "contract: map dissoc changed its source");
    CHECK(PERSIMM_ERR_INVALID == persimm_map_assoc(&associated, &entry, &associated),
          "contract: map assoc accepted an aliased destination");
    CHECK(PERSIMM_ERR_INVALID == persimm_map_dissoc(&associated, &one, &associated),
          "contract: map dissoc accepted an aliased destination");
    CHECK(PERSIMM_ERR_INVALID == persimm_map_clone(&associated, &associated),
          "contract: map clone accepted an aliased destination");
    CHECK(1 == associated.count && persimm_map_has(&associated, &one),
          "contract: rejected map clone changed its source");
    persimm_map_deinit(&dissociated);
    persimm_map_deinit(&associated);
    persimm_map_deinit(&associated);
    persimm_map_deinit(&map);

    persimm_set_t set;
    persimm_set_t conjoined;
    persimm_set_t disjoined;
    CHECK(PERSIMM_OK == persimm_set_init(&set, sizeof(int), &spread_ops, NULL),
          "contract: set init failed");
    CHECK(PERSIMM_OK == persimm_set_conj(&set, &one, &conjoined),
          "contract: set conj failed");
    CHECK(0 == set.count && 1 == conjoined.count && !persimm_set_has(&set, &one),
          "contract: set conj changed its source");
    CHECK(PERSIMM_OK == persimm_set_disj(&conjoined, &one, &disjoined),
          "contract: set disj failed");
    CHECK(1 == conjoined.count && 0 == disjoined.count && persimm_set_has(&conjoined, &one),
          "contract: set disj changed its source");
    CHECK(PERSIMM_ERR_INVALID == persimm_set_conj(&conjoined, &two, &conjoined),
          "contract: set conj accepted an aliased destination");
    CHECK(PERSIMM_ERR_INVALID == persimm_set_disj(&conjoined, &one, &conjoined),
          "contract: set disj accepted an aliased destination");
    CHECK(PERSIMM_ERR_INVALID == persimm_set_clone(&conjoined, &conjoined),
          "contract: set clone accepted an aliased destination");
    CHECK(1 == conjoined.count && persimm_set_has(&conjoined, &one),
          "contract: rejected set clone changed its source");
    persimm_set_deinit(&disjoined);
    persimm_set_deinit(&conjoined);
    persimm_set_deinit(&conjoined);
    persimm_set_deinit(&set);
}

static void test_empty_transient_initialisers(void) {
    int value = 7;

    persimm_vector_transient_t vector_transient;
    persimm_vector_t vector;
    CHECK(PERSIMM_OK == persimm_vector_transient_init(
                              &vector_transient, sizeof(int), NULL, NULL),
          "transient init: vector init failed");
    CHECK(PERSIMM_OK == persimm_vector_transient_push(&vector_transient, &value),
          "transient init: vector push failed");
    CHECK(PERSIMM_ERR_INVALID ==
              persimm_vector_to_transient(&vector_transient.value, &vector_transient),
          "transient init: vector conversion accepted overlapping storage");
    CHECK(vector_transient.active && 1 == vector_transient.value.count,
          "transient init: rejected vector conversion changed its input");
    CHECK(PERSIMM_ERR_INVALID ==
              persimm_vector_transient_persist(&vector_transient,
                                                &vector_transient.value),
          "transient init: vector persist accepted overlapping storage");
    CHECK(vector_transient.active && 1 == vector_transient.value.count,
          "transient init: rejected vector persist consumed its input");
    CHECK(PERSIMM_OK == persimm_vector_transient_persist(&vector_transient, &vector),
          "transient init: vector persist failed");
    CHECK(1 == vector.count, "transient init: vector result is empty");
    persimm_vector_transient_deinit(&vector_transient);
    persimm_vector_deinit(&vector);

    persimm_map_transient_t map_transient;
    persimm_map_t map;
    entry_t entry = { value, value * 2 };
    CHECK(PERSIMM_OK == persimm_map_transient_init(
                              &map_transient, &map_layout, NULL, NULL, &spread_ops, NULL),
          "transient init: map init failed");
    CHECK(PERSIMM_OK == persimm_map_transient_assoc(&map_transient, &entry),
          "transient init: map assoc failed");
    CHECK(PERSIMM_ERR_INVALID ==
              persimm_map_to_transient(&map_transient.value, &map_transient),
          "transient init: map conversion accepted overlapping storage");
    CHECK(map_transient.active && 1 == map_transient.value.count,
          "transient init: rejected map conversion changed its input");
    CHECK(PERSIMM_ERR_INVALID ==
              persimm_map_transient_persist(&map_transient, &map_transient.value),
          "transient init: map persist accepted overlapping storage");
    CHECK(map_transient.active && 1 == map_transient.value.count,
          "transient init: rejected map persist consumed its input");
    CHECK(PERSIMM_OK == persimm_map_transient_persist(&map_transient, &map),
          "transient init: map persist failed");
    CHECK(1 == map.count, "transient init: map result is empty");
    persimm_map_transient_deinit(&map_transient);
    persimm_map_deinit(&map);

    persimm_set_transient_t set_transient;
    persimm_set_t set;
    CHECK(PERSIMM_OK == persimm_set_transient_init(
                              &set_transient, sizeof(int), &spread_ops, NULL),
          "transient init: set init failed");
    CHECK(PERSIMM_OK == persimm_set_transient_conj(&set_transient, &value),
          "transient init: set conj failed");
    CHECK(PERSIMM_ERR_INVALID ==
              persimm_set_to_transient(&set_transient.value, &set_transient),
          "transient init: set conversion accepted overlapping storage");
    CHECK(set_transient.active && 1 == set_transient.value.count,
          "transient init: rejected set conversion changed its input");
    CHECK(PERSIMM_ERR_INVALID ==
              persimm_set_transient_persist(&set_transient, &set_transient.value),
          "transient init: set persist accepted overlapping storage");
    CHECK(set_transient.active && 1 == set_transient.value.count,
          "transient init: rejected set persist consumed its input");
    CHECK(PERSIMM_OK == persimm_set_transient_persist(&set_transient, &set),
          "transient init: set persist failed");
    CHECK(1 == set.count, "transient init: set result is empty");
    persimm_set_transient_deinit(&set_transient);
    persimm_set_deinit(&set);
}

static void test_cursor_survives_same_count_change(void) {
    persimm_list_t list;
    persimm_list_cursor_t cursor;
    persimm_list_init(&list, sizeof(int), NULL, NULL);
    persimm_list_cursor_reset(&cursor);

    int tail = 1;
    int head = 2;
    int replacement = 3;
    test_list_advance_cons(&list, &tail);
    test_list_advance_cons(&list, &head);

    const int *seen = (const int *)persimm_list_at_from(&list, &cursor, 0);
    CHECK(NULL != seen && 2 == *seen, "cursor: initial head was wrong");

    test_list_advance_rest(&list);
    test_list_advance_cons(&list, &replacement);
    seen = (const int *)persimm_list_at_from(&list, &cursor, 0);
    CHECK(NULL != seen && 3 == *seen, "cursor: reused a stale cell after rest and cons");
    seen = (const int *)persimm_list_at_from(&list, &cursor, 1);
    CHECK(NULL != seen && 1 == *seen, "cursor: stale traversal lost the tail");

    persimm_list_deinit(&list);
}

static void test_replacement_may_alias_storage(void) {
    memset(live, 0, sizeof(live));
    rc_underflows = 0;

    persimm_vector_t vector;
    int value = 17;
    persimm_vector_init(&vector, sizeof(value), &rc_ops, NULL);
    test_vector_transient_push(&vector, &value);
    const void *stored = persimm_vector_at(&vector, 0);
    CHECK(PERSIMM_OK == test_vector_transient_update(&vector, 0, stored),
          "alias: vector rejected its stored element");
    CHECK(1 == live[value] && 0 == rc_underflows,
          "alias: vector released its replacement before retaining it");
    persimm_vector_deinit(&vector);

    persimm_map_t map;
    entry_t entry = { 23, RC_VALUE_BASE + 23 };
    persimm_key_ops managed_keys = rc_key_ops(&spread_ops);
    persimm_map_init(&map, &map_layout, &rc_ops, NULL, &managed_keys, NULL);
    test_map_transient_assoc(&map, &entry);
    stored = persimm_map_find_entry(&map, &entry.key);
    CHECK(PERSIMM_OK == test_map_transient_assoc(&map, stored),
          "alias: map rejected its stored entry");
    CHECK(1 == live[entry.key] && 1 == live[entry.value] && 0 == rc_underflows,
          "alias: map released its replacement before retaining it");
    persimm_map_deinit(&map);

    check_live("alias", "once the structures went", 0, 0);
}

static void test_rejects_overflowing_allocations(void) {
    int value = 1;
    persimm_vector_t vector;
    CHECK(PERSIMM_ERR_ALLOC == persimm_vector_init(&vector, SIZE_MAX, NULL, NULL),
          "allocation: vector size overflow was accepted");
    persimm_vector_deinit(&vector);

    persimm_list_t list;
    CHECK(PERSIMM_OK == persimm_list_init(&list, SIZE_MAX, NULL, NULL),
          "allocation: list init unexpectedly failed");
    CHECK(PERSIMM_ERR_ALLOC == test_list_advance_cons(&list, &value),
          "allocation: list size overflow was accepted");
    persimm_list_deinit(&list);

    persimm_entry_layout huge = { SIZE_MAX, 1, 0, 0 };
    persimm_map_t map;
    CHECK(PERSIMM_OK == persimm_map_init(&map, &huge, NULL, NULL, &zero_hash_ops, NULL),
          "allocation: huge map layout was rejected before use");
    CHECK(PERSIMM_ERR_ALLOC == test_map_transient_assoc(&map, &value),
          "allocation: map size overflow was accepted");
    persimm_map_deinit(&map);

    persimm_set_t set;
    CHECK(PERSIMM_OK == persimm_set_init(&set, SIZE_MAX, &zero_hash_ops, NULL),
          "allocation: huge set layout was rejected before use");
    CHECK(PERSIMM_ERR_ALLOC == test_set_transient_conj(&set, &value),
          "allocation: set size overflow was accepted");
    persimm_set_deinit(&set);
}

static void test_vector_transient(void) {
    persimm_vector_t base;
    persimm_vector_init(&base, sizeof(int), NULL, NULL);
    for (int i = 0; i < 100; i++) test_vector_transient_push(&base, &i);

    persimm_vector_transient_t transient;
    CHECK(PERSIMM_OK == persimm_vector_to_transient(&base, &transient),
          "transient vector: conversion failed");
    int replacement = -1;
    int fresh = 100;
    CHECK(PERSIMM_OK == persimm_vector_transient_update(&transient, 10, &replacement),
          "transient vector: update failed");
    CHECK(PERSIMM_OK == persimm_vector_transient_push(&transient, &fresh),
          "transient vector: push failed");
    CHECK(10 == *(const int *)persimm_vector_at(&base, 10) && 100 == base.count,
          "transient vector: original changed");

    persimm_vector_t result;
    CHECK(PERSIMM_OK == persimm_vector_transient_persist(&transient, &result),
          "transient vector: persist failed");
    CHECK(101 == result.count && -1 == *(const int *)persimm_vector_at(&result, 10),
          "transient vector: result is wrong");
    CHECK(PERSIMM_ERR_INVALID == persimm_vector_transient_push(&transient, &fresh),
          "transient vector: accepted an edit after persist");

    persimm_vector_t second;
    CHECK(PERSIMM_ERR_INVALID == persimm_vector_transient_persist(&transient, &second),
          "transient vector: persisted twice");
    persimm_vector_deinit(&second);
    persimm_vector_transient_deinit(&transient);
    persimm_vector_deinit(&result);
    persimm_vector_deinit(&base);
}

static void test_map_transient(void) {
    memset(live, 0, sizeof(live));
    rc_underflows = 0;

    persimm_map_t base;
    persimm_key_ops managed_keys = rc_key_ops(&spread_ops);
    persimm_map_init(&base, &map_layout, &rc_ops, NULL, &managed_keys, NULL);
    for (int i = 0; i < 100; i++) {
        entry_t entry = { i, RC_VALUE_BASE + i };
        test_map_transient_assoc(&base, &entry);
    }

    persimm_map_transient_t transient;
    CHECK(PERSIMM_OK == persimm_map_to_transient(&base, &transient),
          "transient map: conversion failed");
    int victim = 10;
    entry_t replacement = { 11, RC_VALUE_BASE + 11 };
    entry_t fresh = { 100, RC_VALUE_BASE + 100 };
    CHECK(PERSIMM_OK == persimm_map_transient_dissoc(&transient, &victim),
          "transient map: dissoc failed");
    CHECK(PERSIMM_OK == persimm_map_transient_assoc(&transient, &replacement),
          "transient map: replacement failed");
    CHECK(PERSIMM_OK == persimm_map_transient_assoc(&transient, &fresh),
          "transient map: assoc failed");
    CHECK(100 == base.count && NULL != persimm_map_find(&base, &victim),
          "transient map: original changed");

    persimm_map_t result;
    CHECK(PERSIMM_OK == persimm_map_transient_persist(&transient, &result),
          "transient map: persist failed");
    CHECK(100 == result.count && NULL == persimm_map_find(&result, &victim) &&
          NULL != persimm_map_find(&result, &fresh.key),
          "transient map: result is wrong");
    CHECK(PERSIMM_ERR_INVALID == persimm_map_transient_assoc(&transient, &fresh),
          "transient map: accepted an edit after persist");

    persimm_map_transient_deinit(&transient);
    persimm_map_deinit(&result);
    persimm_map_deinit(&base);
    check_live("transient map", "once the structures went", 0, 0);
    CHECK(0 == rc_underflows, "transient map: elements were released too often");
}

static void test_set_transient(void) {
    persimm_set_t base;
    persimm_set_init(&base, sizeof(int), &spread_ops, NULL);
    for (int i = 0; i < 100; i++) test_set_transient_conj(&base, &i);

    persimm_set_transient_t transient;
    CHECK(PERSIMM_OK == persimm_set_to_transient(&base, &transient),
          "transient set: conversion failed");
    int victim = 10;
    int fresh = 100;
    CHECK(PERSIMM_OK == persimm_set_transient_disj(&transient, &victim),
          "transient set: disj failed");
    CHECK(PERSIMM_OK == persimm_set_transient_conj(&transient, &fresh),
          "transient set: conj failed");
    CHECK(persimm_set_has(&base, &victim) && !persimm_set_has(&base, &fresh),
          "transient set: original changed");

    persimm_set_t result;
    CHECK(PERSIMM_OK == persimm_set_transient_persist(&transient, &result),
          "transient set: persist failed");
    CHECK(!persimm_set_has(&result, &victim) && persimm_set_has(&result, &fresh),
          "transient set: result is wrong");
    CHECK(PERSIMM_ERR_INVALID == persimm_set_transient_disj(&transient, &fresh),
          "transient set: accepted an edit after persist");

    persimm_set_transient_deinit(&transient);
    persimm_set_deinit(&result);
    persimm_set_deinit(&base);
}

#if defined(PERSIMM_TEST_ALLOC)
static void test_persistent_failure_contracts(void) {
    int value = 1;

    persimm_vector_t vector;
    persimm_vector_t vector_dest;
    persimm_vector_init(&vector, sizeof(int), NULL, NULL);
    fail_allocation_after(0);
    CHECK(PERSIMM_ERR_ALLOC == persimm_vector_push(&vector, &value, &vector_dest),
          "contract: failed vector push returned the wrong status");
    allow_allocations();
    CHECK(0 == vector.count && 0 == vector_dest.count && NULL == vector_dest.tail,
          "contract: failed vector push left a changed source or live destination");
    persimm_vector_deinit(&vector_dest);
    persimm_vector_deinit(&vector);

    persimm_list_t list;
    persimm_list_t list_dest;
    persimm_list_init(&list, sizeof(int), NULL, NULL);
    fail_allocation_after(0);
    CHECK(PERSIMM_ERR_ALLOC == persimm_list_cons(&list, &value, &list_dest),
          "contract: failed list cons returned the wrong status");
    allow_allocations();
    CHECK(0 == list.count && 0 == list_dest.count && NULL == list_dest.head,
          "contract: failed list cons left a changed source or live destination");
    persimm_list_deinit(&list_dest);
    persimm_list_deinit(&list);

    persimm_map_t map;
    persimm_map_t map_dest;
    entry_t entry = { value, value };
    persimm_map_init(&map, &map_layout, NULL, NULL, &spread_ops, NULL);
    fail_allocation_after(0);
    CHECK(PERSIMM_ERR_ALLOC == persimm_map_assoc(&map, &entry, &map_dest),
          "contract: failed map assoc returned the wrong status");
    allow_allocations();
    CHECK(0 == map.count && 0 == map_dest.count && NULL == map_dest.root,
          "contract: failed map assoc left a changed source or live destination");
    persimm_map_deinit(&map_dest);
    persimm_map_deinit(&map);

    persimm_set_t set;
    persimm_set_t set_dest;
    persimm_set_init(&set, sizeof(int), &spread_ops, NULL);
    fail_allocation_after(0);
    CHECK(PERSIMM_ERR_ALLOC == persimm_set_conj(&set, &value, &set_dest),
          "contract: failed set conj returned the wrong status");
    allow_allocations();
    CHECK(0 == set.count && 0 == set_dest.count && NULL == set_dest.root,
          "contract: failed set conj left a changed source or live destination");
    persimm_set_deinit(&set_dest);
    persimm_set_deinit(&set);

    persimm_vector_transient_t transient;
    fail_allocation_after(0);
    CHECK(PERSIMM_ERR_ALLOC == persimm_vector_transient_init(
                                    &transient, sizeof(int), NULL, NULL),
          "contract: failed transient init returned the wrong status");
    allow_allocations();
    persimm_vector_transient_deinit(&transient);
    CHECK(0 == allocated_blocks, "contract: failure checks leaked %zu blocks", allocated_blocks);
}

static void test_transient_allocation_failures(void) {
    persimm_vector_t vector;
    persimm_vector_init(&vector, sizeof(int), NULL, NULL);
    for (int i = 0; i < 32; i++) test_vector_transient_push(&vector, &i);
    persimm_vector_transient_t vector_transient;
    CHECK(PERSIMM_OK == persimm_vector_to_transient(&vector, &vector_transient),
          "transient allocation: vector conversion failed");

    int fresh = 32;
    fail_allocation_after(0);
    CHECK(PERSIMM_ERR_ALLOC == persimm_vector_transient_push(&vector_transient, &fresh),
          "transient allocation: vector did not report failure");
    allow_allocations();
    CHECK(vector_transient.active && 32 == vector_transient.value.count,
          "transient allocation: failed vector push changed the transient");
    CHECK(PERSIMM_OK == persimm_vector_transient_push(&vector_transient, &fresh),
          "transient allocation: vector could not retry");
    persimm_vector_transient_deinit(&vector_transient);
    persimm_vector_deinit(&vector);

    persimm_map_t map;
    persimm_map_init(&map, &map_layout, NULL, NULL, &spread_ops, NULL);
    for (int i = 0; i < 64; i++) {
        entry_t entry = { i, i };
        test_map_transient_assoc(&map, &entry);
    }
    persimm_map_transient_t map_transient;
    CHECK(PERSIMM_OK == persimm_map_to_transient(&map, &map_transient),
          "transient allocation: map conversion failed");
    entry_t entry = { 1000, 1000 };

    fail_allocation_after(0);
    CHECK(PERSIMM_ERR_ALLOC == persimm_map_transient_assoc(&map_transient, &entry),
          "transient allocation: map did not report failure");
    allow_allocations();
    CHECK(map_transient.active && 64 == map_transient.value.count &&
          !persimm_map_has(&map_transient.value, &entry.key),
          "transient allocation: failed map assoc changed the transient");
    CHECK(PERSIMM_OK == persimm_map_transient_assoc(&map_transient, &entry),
          "transient allocation: map could not retry");
    persimm_map_transient_deinit(&map_transient);
    persimm_map_deinit(&map);
    CHECK(0 == allocated_blocks, "transient allocation: leaked %zu blocks", allocated_blocks);
}

static void build_fault_map(persimm_map_t *map, const persimm_key_ops *ops) {
    persimm_map_init(map, &map_layout, NULL, NULL, ops, NULL);
    for (int i = 0; i < 64; i++) {
        entry_t entry = { i, i };
        CHECK(PERSIMM_OK == test_map_transient_assoc(map, &entry),
              "allocation: could not build fault-test map");
    }
}

static void test_assoc_allocation_failures(void) {
    bool reached_success = false;
    for (int fail = 0; fail < 32 && !reached_success; fail++) {
        persimm_map_t base;
        persimm_map_t copy;
        build_fault_map(&base, &spread_ops);
        CHECK(PERSIMM_OK == persimm_map_clone(&base, &copy),
              "allocation: map clone failed");

        entry_t fresh = { 1000, 1000 };
        fail_allocation_after(fail);
        persimm_status status = test_map_advance_assoc(&copy, &fresh);
        allow_allocations();

        if (PERSIMM_ERR_ALLOC == status) {
            CHECK(copy.count == base.count && !persimm_map_has(&copy, &fresh.key),
                  "allocation: failed assoc changed the map");
        } else {
            CHECK(PERSIMM_OK == status, "allocation: assoc returned an unexpected status");
            reached_success = true;
        }

        persimm_map_deinit(&copy);
        persimm_map_deinit(&base);
        CHECK(0 == allocated_blocks, "allocation: assoc failure leaked %zu blocks",
              allocated_blocks);
    }
    CHECK(reached_success, "allocation: assoc did not succeed after all failure points");
}

static void test_dissoc_allocation_failures(void) {
    bool reached_success = false;
    for (int fail = 0; fail < 32 && !reached_success; fail++) {
        persimm_map_t base;
        persimm_map_t copy;
        build_fault_map(&base, &spread_ops);
        CHECK(PERSIMM_OK == persimm_map_clone(&base, &copy),
              "allocation: map clone failed");

        int victim = 17;
        fail_allocation_after(fail);
        persimm_status status = test_map_advance_dissoc(&copy, &victim);
        allow_allocations();

        if (PERSIMM_ERR_ALLOC == status) {
            CHECK(copy.count == base.count && persimm_map_has(&copy, &victim),
                  "allocation: failed dissoc changed the map");
        } else {
            CHECK(PERSIMM_OK == status, "allocation: dissoc returned an unexpected status");
            reached_success = true;
        }

        persimm_map_deinit(&copy);
        persimm_map_deinit(&base);
        CHECK(0 == allocated_blocks, "allocation: dissoc failure leaked %zu blocks",
              allocated_blocks);
    }
    CHECK(reached_success, "allocation: dissoc did not succeed after all failure points");
}

static void test_collision_reparent_allocation_failures(void) {
    bool reached_success = false;
    for (int fail = 0; fail < 32 && !reached_success; fail++) {
        persimm_map_t base;
        persimm_map_t copy;
        persimm_map_init(&base, &map_layout, NULL, NULL, &crowded_ops, NULL);
        entry_t first = { 0, 0 };
        entry_t second = { 4, 4 };
        test_map_transient_assoc(&base, &first);
        test_map_transient_assoc(&base, &second);
        CHECK(PERSIMM_OK == persimm_map_clone(&base, &copy),
              "allocation: collision map clone failed");

        entry_t fresh = { 1, 1 };
        fail_allocation_after(fail);
        persimm_status status = test_map_advance_assoc(&copy, &fresh);
        allow_allocations();

        if (PERSIMM_ERR_ALLOC == status) {
            CHECK(2 == copy.count && !persimm_map_has(&copy, &fresh.key),
                  "allocation: failed collision assoc changed the map");
        } else {
            CHECK(PERSIMM_OK == status, "allocation: collision assoc returned a bad status");
            reached_success = true;
        }

        persimm_map_deinit(&copy);
        persimm_map_deinit(&base);
        CHECK(0 == allocated_blocks, "allocation: collision assoc leaked %zu blocks",
              allocated_blocks);
    }
    CHECK(reached_success, "allocation: collision assoc never reached success");
}
#endif

/* Running */

int main(void) {
    /* Nothing, one entry, a node's worth either side of full, and enough to
       carry the trie down several levels. */
    static const int sizes[] = { 0, 1, 2, 31, 32, 33, 1000, 20000 };

    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        int n = sizes[i];
        char label[64];

        snprintf(label, sizeof(label), "spread/%d", n);
        test_assoc_and_ref(&spread_ops, label, n);
        test_iteration_agrees(&spread_ops, label, n);
        test_dissoc(&spread_ops, label, n);
        test_canonical(&spread_ops, label, n, true);
        test_sharing(&spread_ops, label, n);
        test_set(&spread_ops, label, n);
        test_map_refcounts(&spread_ops, label, n);
        test_set_refcounts(&spread_ops, label, n);

        /* The same again with every key crowded into one of four hashes. The
           largest size is left out because a collision node is a flat run of
           entries, so searching one is linear and 20000 of them is quadratic. */
        if (n > 1000) continue;
        snprintf(label, sizeof(label), "crowded/%d", n);
        test_assoc_and_ref(&crowded_ops, label, n);
        test_iteration_agrees(&crowded_ops, label, n);
        test_dissoc(&crowded_ops, label, n);
        test_canonical(&crowded_ops, label, n, false);
        test_sharing(&crowded_ops, label, n);
        test_set(&crowded_ops, label, n);
        test_map_refcounts(&crowded_ops, label, n);
        test_set_refcounts(&crowded_ops, label, n);
    }

    test_byte_defaults();
    test_map_separates_key_and_value_lifecycles();
    test_rejects_a_bad_layout();
    test_persistent_operation_contracts();
    test_empty_transient_initialisers();
    test_cursor_survives_same_count_change();
    test_replacement_may_alias_storage();
    test_rejects_overflowing_allocations();
    test_vector_transient();
    test_map_transient();
    test_set_transient();
#if defined(PERSIMM_TEST_ALLOC)
    test_persistent_failure_contracts();
    test_transient_allocation_failures();
    test_assoc_allocation_failures();
    test_dissoc_allocation_failures();
    test_collision_reparent_allocation_failures();
#endif

    if (failures > 0) {
        printf("%d checks failed\n", failures);
        return 1;
    }

    printf("core checks passed\n");
    printf("atomic reference counts: %s\n", persimm_has_atomic_refcounts() ? "yes" : "no");

    return 0;
}
