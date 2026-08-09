#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "persimmon.h"

/*
 * A small, portable throughput benchmark for the core API. It uses C99's
 * clock rather than a platform-specific wall clock so that the same source
 * builds on every target Persimmon supports. Run it more than once and compare
 * results on an otherwise idle machine; it is a regression gauge, not a claim
 * about absolute performance.
 */

typedef struct {
    int key;
    int value;
} entry_t;

static const persimm_entry_layout entry_layout = {
    sizeof(entry_t),
    sizeof(int),
    offsetof(entry_t, value),
    sizeof(int)
};

static volatile uint64_t sink = 0;

static uint32_t int_hash(const void *key, size_t key_size, void *ctx) {
    (void)key_size;
    (void)ctx;
    uint32_t hash = (uint32_t)*(const int *)key;
    hash *= 2654435761u;
    return hash ^ (hash >> 16);
}

static bool int_equals(const void *a, const void *b, size_t key_size, void *ctx) {
    (void)key_size;
    (void)ctx;
    return *(const int *)a == *(const int *)b;
}

static const persimm_key_ops int_key_ops = { int_hash, int_equals };

static void check(persimm_status status, const char *operation) {
    if (PERSIMM_OK == status) return;
    fprintf(stderr, "%s failed: %s\n", operation, persimm_status_string(status));
    exit(1);
}

static double seconds_since(clock_t start) {
    return (double)(clock() - start) / (double)CLOCKS_PER_SEC;
}

static void report(const char *name, size_t operations, double seconds) {
    double ns = seconds * 1000000000.0 / (double)operations;
    double rate = (double)operations / seconds / 1000000.0;
    printf("%-30s %10zu ops  %9.2f ns/op  %8.2f Mops/s\n", name, operations, ns, rate);
}

static size_t scaled(size_t base) {
    const char *input = getenv("PERSIMMON_BENCH_SCALE");
    if (NULL == input || '\0' == *input) return base;

    char *end = NULL;
    unsigned long scale = strtoul(input, &end, 10);
    if ('\0' != *end || 0 == scale || scale > 100) {
        fprintf(stderr, "PERSIMMON_BENCH_SCALE must be an integer from 1 to 100\n");
        exit(2);
    }
    if (base > SIZE_MAX / (size_t)scale) {
        fprintf(stderr, "benchmark size overflow\n");
        exit(2);
    }
    return base * (size_t)scale;
}

static void benchmark_list(void) {
    size_t count = scaled(500000);
    persimm_list_t list;
    check(persimm_list_init(&list, sizeof(int), NULL, NULL), "list init");

    clock_t start = clock();
    for (size_t i = 0; i < count; i++) {
        int value = (int)i;
        check(persimm_list_cons(&list, &value), "list cons");
    }
    report("list cons", count, seconds_since(start));

    persimm_list_cursor_t cursor;
    persimm_list_cursor_reset(&cursor);
    start = clock();
    for (size_t i = 0; i < count; i++) {
        int *value = (int *)persimm_list_ref_from(&list, &cursor, i);
        if (NULL == value) {
            fprintf(stderr, "list cursor returned NULL\n");
            exit(1);
        }
        sink += (uint32_t)*value;
    }
    report("list cursor traversal", count, seconds_since(start));

    start = clock();
    for (size_t i = 0; i < count; i++) check(persimm_list_rest(&list), "list rest");
    report("list rest", count, seconds_since(start));
    persimm_list_deinit(&list);
}

static void benchmark_vector(void) {
    size_t count = scaled(500000);
    persimm_vector_t vector;
    check(persimm_vector_init(&vector, sizeof(int), NULL, NULL), "vector init");

    clock_t start = clock();
    for (size_t i = 0; i < count; i++) {
        int value = (int)i;
        check(persimm_vector_push(&vector, &value, false), "mutable vector push");
    }
    report("vector push (builder)", count, seconds_since(start));

    start = clock();
    for (size_t i = 0; i < count; i++) {
        int *value = (int *)persimm_vector_ref(&vector, i);
        if (NULL == value) {
            fprintf(stderr, "vector ref returned NULL\n");
            exit(1);
        }
        sink += (uint32_t)*value;
    }
    report("vector sequential ref", count, seconds_since(start));
    persimm_vector_deinit(&vector);

    count = scaled(150000);
    check(persimm_vector_init(&vector, sizeof(int), NULL, NULL), "vector init");
    start = clock();
    for (size_t i = 0; i < count; i++) {
        persimm_vector_t next;
        int value = (int)i;
        persimm_vector_clone(&vector, &next);
        check(persimm_vector_push(&next, &value, true), "persistent vector push");
        persimm_vector_deinit(&vector);
        vector = next;
    }
    report("vector push (persistent)", count, seconds_since(start));
    sink += vector.count;
    persimm_vector_deinit(&vector);
}

static void benchmark_map(void) {
    size_t count = scaled(150000);
    persimm_map_t map;
    check(persimm_map_init(&map, &entry_layout, NULL, &int_key_ops, NULL), "map init");

    clock_t start = clock();
    for (size_t i = 0; i < count; i++) {
        entry_t entry = { (int)i, (int)(i * 3) };
        check(persimm_map_assoc(&map, &entry, false), "mutable map assoc");
    }
    report("map assoc (builder)", count, seconds_since(start));

    start = clock();
    for (size_t i = 0; i < count; i++) {
        int key = (int)i;
        int *value = (int *)persimm_map_ref(&map, &key);
        if (NULL == value) {
            fprintf(stderr, "map ref returned NULL\n");
            exit(1);
        }
        sink += (uint32_t)*value;
    }
    report("map sequential ref", count, seconds_since(start));
    persimm_map_deinit(&map);

    count = scaled(75000);
    check(persimm_map_init(&map, &entry_layout, NULL, &int_key_ops, NULL), "map init");
    start = clock();
    for (size_t i = 0; i < count; i++) {
        persimm_map_t next;
        entry_t entry = { (int)i, (int)(i * 3) };
        persimm_map_clone(&map, &next);
        check(persimm_map_assoc(&next, &entry, true), "persistent map assoc");
        persimm_map_deinit(&map);
        map = next;
    }
    report("map assoc (persistent)", count, seconds_since(start));

    start = clock();
    for (size_t i = 0; i < count; i++) {
        persimm_map_t next;
        int key = (int)i;
        persimm_map_clone(&map, &next);
        check(persimm_map_dissoc(&next, &key, true), "persistent map dissoc");
        persimm_map_deinit(&map);
        map = next;
    }
    report("map dissoc (persistent)", count, seconds_since(start));
    persimm_map_deinit(&map);
}

int main(void) {
    printf("Persimmon core benchmark\n");
    printf("list handle: %zu bytes; cursor: %zu bytes\n\n",
           sizeof(persimm_list_t), sizeof(persimm_list_cursor_t));

    benchmark_list();
    benchmark_vector();
    benchmark_map();

    printf("\nchecksum: %" PRIu64 "\n", sink);
    return 0;
}
