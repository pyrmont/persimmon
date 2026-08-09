#include <stdio.h>
#include <stdlib.h>
#include "persimmon.h"

static void check(persimm_status status, const char *operation) {
    if (PERSIMM_OK == status) return;
    fprintf(stderr, "%s: %s\n", operation, persimm_status_string(status));
    exit(EXIT_FAILURE);
}

int main(void) {
    persimm_vector_t empty;
    persimm_vector_t first;
    int one = 1;

    check(persimm_vector_init(&empty, sizeof(int), NULL, NULL), "initialise vector");
    check(persimm_vector_push(&empty, &one, &first), "append persistently");

    /* A persistent update leaves its source intact. */
    printf("empty: %zu, first: %zu\n", empty.count, first.count);

    persimm_vector_transient_t transient;
    persimm_vector_to_transient(&first, &transient);
    for (int value = 2; value <= 5; value++) {
        check(persimm_vector_transient_push(&transient, &value), "append transiently");
    }

    persimm_vector_t result;
    check(persimm_vector_transient_persist(&transient, &result), "make persistent");

    printf("first: %zu, result:", first.count);
    for (size_t i = 0; i < result.count; i++) {
        printf(" %d", *(const int *)persimm_vector_ref(&result, i));
    }
    printf("\n");

    /* Deinitialising a consumed transient is safe. */
    persimm_vector_transient_deinit(&transient);
    persimm_vector_deinit(&result);
    persimm_vector_deinit(&first);
    persimm_vector_deinit(&empty);
    return EXIT_SUCCESS;
}
