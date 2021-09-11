#ifndef PERSIMMON_H
#define PERSIMMON_H

#include <stdlib.h>
#include <stddef.h>
#include <stdbool.h>

/* Put behind a debug flag */
#include <stdio.h>

/* Error Goto */

#define persimm_goto(label, error_code) do { \
    err = error_code; \
    goto label; \
} while (0)

/* Memory Safe Allocation */

void *persimm_realloc(void *memblock, size_t size) {
    void *old_memblock = memblock;
    memblock = realloc(memblock, size);
    if (NULL == memblock) free(old_memblock);
    return memblock;
}

/* Enums */

typedef enum {
    PERSIMM_ERROR_NONE,
    PERSIMM_ERROR_MEMORY,
    PERSIMM_ERROR_EMPTY,
    PERSIMM_ERROR_BOUNDS,
    PERSIMM_ERROR_MISSING,
    PERSIMM_ERROR_MALFORM
} persimm_error_code;

typedef enum {
    PERSIMM_NODE_INNER,
    PERSIMM_NODE_LEAF
} persimm_node_type;

/* Vector */

#define PERSIMM_VECTOR_BITS 5
#define PERSIMM_VECTOR_WIDTH (1 << PERSIMM_VECTOR_BITS) // 2^5 = 32
#define PERSIMM_VECTOR_MASK (PERSIMM_VECTOR_WIDTH - 1) // 31, or 0x1f

typedef struct {
    persimm_node_type kind;
    size_t ref_count;
    void* items[PERSIMM_VECTOR_WIDTH];
} persimm_vector_node;

typedef struct {
    size_t shift;
    size_t count;
    size_t tail_count;
    persimm_vector_node *root;
    persimm_vector_node *tail;
} persimm_vector;


#endif /* end of include guard */
