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

static const persimm_key_ops spread_ops = { int_hash, int_equals };
static const persimm_key_ops crowded_ops = { crowded_hash, int_equals };

static uint32_t zero_hash(const void *key, size_t key_size, void *ctx) {
    (void) key;
    (void) key_size;
    (void) ctx;
    return 0;
}

static const persimm_key_ops zero_hash_ops = { zero_hash, NULL };

/* Traversing */

static void collect_visit(void *slot, size_t index, void *ctx) {
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
    persimm_map_init(&map, &map_layout, NULL, ops, NULL);

    for (int i = 0; i < n; i++) {
        entry_t entry = { i, i * 3 };
        CHECK(PERSIMM_OK == persimm_map_assoc(&map, &entry, false), "%s: assoc %d", label, i);
    }
    CHECK(map.count == (size_t)n, "%s: count is %zu, wanted %d", label, map.count, n);

    for (int i = 0; i < n; i++) {
        int *value = (int *)persimm_map_ref(&map, &i);
        CHECK(NULL != value && *value == i * 3, "%s: ref %d", label, i);
    }

    int absent = n + 1;
    CHECK(NULL == persimm_map_ref(&map, &absent), "%s: found a key it does not hold", label);

    /* Storing a key again replaces the value and leaves the count alone. */
    if (n > 0) {
        entry_t again = { 0, 999 };
        persimm_map_assoc(&map, &again, false);
        CHECK(map.count == (size_t)n, "%s: replacing a value grew the count", label);
        CHECK(999 == *(int *)persimm_map_ref(&map, &again.key), "%s: value not replaced", label);
    }

    persimm_map_deinit(&map);
}

/* Walking with next must produce what walking with foreach produces. */
static void test_iteration_agrees(const persimm_key_ops *ops, const char *label, int n) {
    persimm_map_t map;
    persimm_map_init(&map, &map_layout, NULL, ops, NULL);
    for (int i = 0; i < n; i++) {
        entry_t entry = { i, i };
        persimm_map_assoc(&map, &entry, false);
    }

    entry_t *seen = malloc(sizeof(entry_t) * (size_t)(n + 1));
    size_t count = drain(&map, seen);
    CHECK(count == (size_t)n, "%s: foreach saw %zu of %d", label, count, n);

    size_t steps = 0;
    void *entry = persimm_map_next(&map, NULL);
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
    persimm_map_init(&map, &map_layout, NULL, ops, NULL);
    for (int i = 0; i < n; i++) {
        entry_t entry = { i, i };
        persimm_map_assoc(&map, &entry, false);
    }

    for (int i = 0; i < n; i += 2) {
        CHECK(PERSIMM_OK == persimm_map_dissoc(&map, &i, false), "%s: dissoc %d", label, i);
    }
    CHECK(map.count == (size_t)(n / 2), "%s: count is %zu after dropping the evens", label,
          map.count);

    for (int i = 0; i < n; i++) {
        void *value = persimm_map_ref(&map, &i);
        if (0 == i % 2) {
            CHECK(NULL == value, "%s: %d survived being dropped", label, i);
        } else {
            CHECK(NULL != value && i == *(int *)value, "%s: %d went with its neighbour", label, i);
        }
    }

    /* Dropping a key the map does not hold is not an error. */
    int zero = 0;
    CHECK(PERSIMM_OK == persimm_map_dissoc(&map, &zero, false), "%s: repeated dissoc", label);
    CHECK(map.count == (size_t)(n / 2), "%s: repeated dissoc moved the count", label);

    for (int i = 1; i < n; i += 2) persimm_map_dissoc(&map, &i, false);
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
    persimm_map_init(&forward, &map_layout, NULL, ops, NULL);
    persimm_map_init(&backward, &map_layout, NULL, ops, NULL);
    persimm_map_init(&pruned, &map_layout, NULL, ops, NULL);

    for (int i = 0; i < n; i++) {
        entry_t entry = { i, i };
        persimm_map_assoc(&forward, &entry, false);
    }
    for (int i = n - 1; i >= 0; i--) {
        entry_t entry = { i, i };
        persimm_map_assoc(&backward, &entry, false);
    }
    for (int i = 0; i < n * 2; i++) {
        entry_t entry = { i, i };
        persimm_map_assoc(&pruned, &entry, false);
    }
    for (int i = n; i < n * 2; i++) {
        persimm_map_dissoc(&pruned, &i, false);
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
    persimm_map_init(&base, &map_layout, NULL, ops, NULL);
    for (int i = 0; i < n; i++) {
        entry_t entry = { i, i };
        persimm_map_assoc(&base, &entry, false);
    }

    persimm_map_t added;
    persimm_map_t replaced;
    persimm_map_t dropped;
    persimm_map_clone(&base, &added);
    persimm_map_clone(&base, &replaced);
    persimm_map_clone(&base, &dropped);

    entry_t fresh = { n + 100, 7 };
    persimm_map_assoc(&added, &fresh, true);

    CHECK(base.count == (size_t)n, "%s: the original's count moved to %zu", label, base.count);
    CHECK(NULL == persimm_map_ref(&base, &fresh.key), "%s: the original gained a key", label);
    CHECK(added.count == (size_t)n + 1, "%s: the copy did not grow", label);

    int victim = n / 2;
    if (n > 0) {
        entry_t over = { victim, -1 };
        persimm_map_assoc(&replaced, &over, true);
        persimm_map_dissoc(&dropped, &victim, true);

        CHECK(victim == *(int *)persimm_map_ref(&base, &victim), "%s: the original changed", label);
        CHECK(-1 == *(int *)persimm_map_ref(&replaced, &victim), "%s: the copy did not", label);
        CHECK(NULL == persimm_map_ref(&dropped, &victim), "%s: the copy kept a dropped key", label);
        CHECK(replaced.count == (size_t)n, "%s: replacing on a copy grew it", label);
        CHECK(dropped.count == (size_t)n - 1, "%s: dropping on a copy did not shrink it", label);
    }

    for (int i = 0; i < n; i++) {
        CHECK(NULL != persimm_map_ref(&base, &i), "%s: the original lost %d", label, i);
        CHECK(NULL != persimm_map_ref(&added, &i), "%s: the copy lost %d", label, i);
    }

    persimm_map_deinit(&added);
    persimm_map_deinit(&replaced);
    persimm_map_deinit(&dropped);

    /* Whatever the copies shared has outlived every one of them. */
    for (int i = 0; i < n; i++) {
        CHECK(NULL != persimm_map_ref(&base, &i), "%s: a copy took %d with it", label, i);
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
    persimm_set_init(&set, sizeof(int), NULL, ops, NULL);

    for (int i = 0; i < n; i++) {
        CHECK(PERSIMM_OK == persimm_set_conj(&set, &i, false), "%s: conj %d", label, i);
    }
    CHECK(set.count == (size_t)n, "%s: count is %zu, wanted %d", label, set.count, n);

    for (int i = 0; i < n; i++) persimm_set_conj(&set, &i, false);
    CHECK(set.count == (size_t)n, "%s: duplicates grew the count to %zu", label, set.count);

    for (int i = 0; i < n; i++) CHECK(persimm_set_has(&set, &i), "%s: lost %d", label, i);
    int absent = n + 1;
    CHECK(!persimm_set_has(&set, &absent), "%s: found an element it does not hold", label);

    for (int i = 0; i < n; i += 2) persimm_set_disj(&set, &i, false);
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

static void rc_retain(void *slot, void *ctx) {
    (void) ctx;
    live[*(int *)slot]++;
}

static void rc_release(void *slot, void *ctx) {
    (void) ctx;
    if (--live[*(int *)slot] < 0) rc_underflows++;
}

static const persimm_elem_ops rc_ops = { rc_retain, rc_release, NULL };

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

    persimm_map_t base;
    persimm_map_init(&base, &map_layout, &rc_ops, ops, NULL);
    for (int i = 0; i < n; i++) {
        entry_t entry = { i, RC_VALUE_BASE + i };
        persimm_map_assoc(&base, &entry, false);
    }
    check_live(label, "once built", 0, n);

    /* A copy that grows, changes and shrinks shares a great deal with what it
       was copied from, and must leave every one of those elements held once. */
    persimm_map_t copy;
    persimm_map_clone(&base, &copy);
    for (int i = n; i < n + 50; i++) {
        entry_t entry = { i, RC_VALUE_BASE + i };
        persimm_map_assoc(&copy, &entry, true);
    }
    for (int i = 0; i < n; i += 3) {
        persimm_map_dissoc(&copy, &i, true);
    }
    for (int i = 1; i < n; i += 7) {
        entry_t entry = { i, RC_VALUE_BASE + i };
        persimm_map_assoc(&copy, &entry, true);
    }

    for (int i = 0; i < n; i++) {
        CHECK(live[i] >= 1, "%s: key %d was released while the original held it", label, i);
        int *value = (int *)persimm_map_ref(&base, &i);
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

    persimm_set_t base;
    persimm_set_init(&base, sizeof(int), &rc_ops, ops, NULL);
    for (int i = 0; i < n; i++) persimm_set_conj(&base, &i, false);

    /* A set's entries have no value, so nothing above the elements is held. */
    int wrong = 0;
    for (int i = 0; i < RC_SPACE; i++) {
        if (live[i] != ((i < n) ? 1 : 0)) wrong++;
    }
    CHECK(0 == wrong, "%s: %d elements at the wrong count once built", label, wrong);

    persimm_set_t copy;
    persimm_set_clone(&base, &copy);
    for (int i = 0; i < n; i += 2) persimm_set_disj(&copy, &i, true);
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
    persimm_map_init(&map, &map_layout, NULL, NULL, NULL);

    for (int i = 0; i < 500; i++) {
        entry_t entry = { i, i * 2 };
        persimm_map_assoc(&map, &entry, false);
    }
    CHECK(500 == map.count, "defaults: count is %zu", map.count);
    for (int i = 0; i < 500; i++) {
        int *value = (int *)persimm_map_ref(&map, &i);
        CHECK(NULL != value && i * 2 == *value, "defaults: ref %d", i);
    }

    persimm_map_deinit(&map);
}

static void test_rejects_a_bad_layout(void) {
    persimm_map_t map;

    persimm_entry_layout no_key = { sizeof(entry_t), 0, 4, 4 };
    CHECK(PERSIMM_ERR_INVALID == persimm_map_init(&map, &no_key, NULL, NULL, NULL),
          "layout: a key of no size was accepted");
    persimm_map_deinit(&map);

    persimm_entry_layout overlapping = { 8, 4, 2, 4 };
    CHECK(PERSIMM_ERR_INVALID == persimm_map_init(&map, &overlapping, NULL, NULL, NULL),
          "layout: a value inside the key was accepted");
    persimm_map_deinit(&map);

    persimm_entry_layout overrunning = { 8, 4, 4, 8 };
    CHECK(PERSIMM_ERR_INVALID == persimm_map_init(&map, &overrunning, NULL, NULL, NULL),
          "layout: a value past the end of the entry was accepted");
    persimm_map_deinit(&map);

    persimm_set_t set;
    CHECK(PERSIMM_ERR_INVALID == persimm_set_init(&set, 0, NULL, NULL, NULL),
          "layout: an element of no size was accepted");
    persimm_set_deinit(&set);
}

/* Defensive Edges */

static void test_extreme_indexes(void) {
    size_t index = 99;
    int value = 7;

    persimm_vector_t vector;
    CHECK(PERSIMM_OK == persimm_vector_init(&vector, sizeof(value), NULL, NULL),
          "index: vector init failed");
    CHECK(PERSIMM_OK == persimm_vector_push(&vector, &value, false),
          "index: vector push failed");
    CHECK(!persimm_vector_index(&vector, INT64_MAX, &index),
          "index: vector accepted INT64_MAX");
    CHECK(!persimm_vector_index(&vector, INT64_MIN, &index),
          "index: vector accepted INT64_MIN");
    CHECK(persimm_vector_index(&vector, -1, &index) && 0 == index,
          "index: vector rejected -1");
    persimm_vector_deinit(&vector);

    persimm_list_t list;
    CHECK(PERSIMM_OK == persimm_list_init(&list, sizeof(value), NULL, NULL),
          "index: list init failed");
    CHECK(PERSIMM_OK == persimm_list_cons(&list, &value), "index: list cons failed");
    CHECK(!persimm_list_index(&list, INT64_MAX, &index), "index: list accepted INT64_MAX");
    CHECK(!persimm_list_index(&list, INT64_MIN, &index), "index: list accepted INT64_MIN");
    CHECK(persimm_list_index(&list, -1, &index) && 0 == index, "index: list rejected -1");
    persimm_list_deinit(&list);
}

static void test_cursor_survives_same_count_change(void) {
    persimm_list_t list;
    persimm_list_cursor_t cursor;
    persimm_list_init(&list, sizeof(int), NULL, NULL);
    persimm_list_cursor_reset(&cursor);

    int tail = 1;
    int head = 2;
    int replacement = 3;
    persimm_list_cons(&list, &tail);
    persimm_list_cons(&list, &head);

    int *seen = (int *)persimm_list_ref_from(&list, &cursor, 0);
    CHECK(NULL != seen && 2 == *seen, "cursor: initial head was wrong");

    persimm_list_rest(&list);
    persimm_list_cons(&list, &replacement);
    seen = (int *)persimm_list_ref_from(&list, &cursor, 0);
    CHECK(NULL != seen && 3 == *seen, "cursor: reused a stale cell after rest and cons");
    seen = (int *)persimm_list_ref_from(&list, &cursor, 1);
    CHECK(NULL != seen && 1 == *seen, "cursor: stale traversal lost the tail");

    persimm_list_deinit(&list);
}

static void test_replacement_may_alias_storage(void) {
    memset(live, 0, sizeof(live));
    rc_underflows = 0;

    persimm_vector_t vector;
    int value = 17;
    persimm_vector_init(&vector, sizeof(value), &rc_ops, NULL);
    persimm_vector_push(&vector, &value, false);
    void *stored = persimm_vector_ref(&vector, 0);
    CHECK(PERSIMM_OK == persimm_vector_update(&vector, 0, stored, true),
          "alias: vector rejected its stored element");
    CHECK(1 == live[value] && 0 == rc_underflows,
          "alias: vector released its replacement before retaining it");
    persimm_vector_deinit(&vector);

    persimm_map_t map;
    entry_t entry = { 23, RC_VALUE_BASE + 23 };
    persimm_map_init(&map, &map_layout, &rc_ops, &spread_ops, NULL);
    persimm_map_assoc(&map, &entry, false);
    stored = persimm_map_ref_entry(&map, &entry.key);
    CHECK(PERSIMM_OK == persimm_map_assoc(&map, stored, false),
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
    CHECK(PERSIMM_ERR_ALLOC == persimm_list_cons(&list, &value),
          "allocation: list size overflow was accepted");
    persimm_list_deinit(&list);

    persimm_entry_layout huge = { SIZE_MAX, 1, 0, 0 };
    persimm_map_t map;
    CHECK(PERSIMM_OK == persimm_map_init(&map, &huge, NULL, &zero_hash_ops, NULL),
          "allocation: huge map layout was rejected before use");
    CHECK(PERSIMM_ERR_ALLOC == persimm_map_assoc(&map, &value, false),
          "allocation: map size overflow was accepted");
    persimm_map_deinit(&map);

    persimm_set_t set;
    CHECK(PERSIMM_OK == persimm_set_init(&set, SIZE_MAX, NULL, &zero_hash_ops, NULL),
          "allocation: huge set layout was rejected before use");
    CHECK(PERSIMM_ERR_ALLOC == persimm_set_conj(&set, &value, false),
          "allocation: set size overflow was accepted");
    persimm_set_deinit(&set);
}

#if defined(PERSIMM_TEST_ALLOC)
static void build_fault_map(persimm_map_t *map, const persimm_key_ops *ops) {
    persimm_map_init(map, &map_layout, NULL, ops, NULL);
    for (int i = 0; i < 64; i++) {
        entry_t entry = { i, i };
        CHECK(PERSIMM_OK == persimm_map_assoc(map, &entry, false),
              "allocation: could not build fault-test map");
    }
}

static void test_assoc_allocation_failures(void) {
    bool reached_success = false;
    for (int fail = 0; fail < 32 && !reached_success; fail++) {
        persimm_map_t base;
        persimm_map_t copy;
        build_fault_map(&base, &spread_ops);
        persimm_map_clone(&base, &copy);

        entry_t fresh = { 1000, 1000 };
        fail_allocation_after(fail);
        persimm_status status = persimm_map_assoc(&copy, &fresh, true);
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
        persimm_map_clone(&base, &copy);

        int victim = 17;
        fail_allocation_after(fail);
        persimm_status status = persimm_map_dissoc(&copy, &victim, true);
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
        persimm_map_init(&base, &map_layout, NULL, &crowded_ops, NULL);
        entry_t first = { 0, 0 };
        entry_t second = { 4, 4 };
        persimm_map_assoc(&base, &first, false);
        persimm_map_assoc(&base, &second, false);
        persimm_map_clone(&base, &copy);

        entry_t fresh = { 1, 1 };
        fail_allocation_after(fail);
        persimm_status status = persimm_map_assoc(&copy, &fresh, true);
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
    test_rejects_a_bad_layout();
    test_extreme_indexes();
    test_cursor_survives_same_count_change();
    test_replacement_may_alias_storage();
    test_rejects_overflowing_allocations();
#if defined(PERSIMM_TEST_ALLOC)
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
