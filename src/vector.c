#include "persimmon.h"

/* Forward declarations */

static void print_vector(const char *name, persimm_vector *vector);

/* Utilities */

static bool persimm_vector_oob(persimm_vector *vector, size_t index) {
    return index >= vector->count;
}

/* Deinitialising */

static persimm_error_code persimm_vector_free_node(persimm_vector_node *node, size_t height) {
    if (NULL == node) return PERSIMM_ERROR_NONE;

    if (node->ref_count > 1) {
        node->ref_count--;
        return PERSIMM_ERROR_NONE;
    }

    /* size_t num_nodes = PERSIMM_VECTOR_WIDTH * height; */
    size_t num_nodes = height;
    int top = 0;

    persimm_vector_node **nodes = malloc(num_nodes * sizeof(persimm_vector_node*));
    if (NULL == nodes) return PERSIMM_ERROR_MEMORY;
    nodes[top] = node;

    size_t *indexes = malloc(num_nodes * sizeof(size_t));
    if (NULL == indexes) return PERSIMM_ERROR_MEMORY;
    indexes[top] = 0;

    while (top >= 0) {
        persimm_vector_node *curr_node = nodes[top];
        size_t curr_index = indexes[top];
        if (curr_index == PERSIMM_VECTOR_WIDTH || curr_node->kind == PERSIMM_NODE_LEAF) {
            free(curr_node);
            top--;
        } else {
            indexes[top] = curr_index + 1;
            persimm_vector_node *next_node = curr_node->items[curr_index];
            if (NULL == next_node) {
                /* do nothing */
            } else if (next_node->ref_count > 1) {
                next_node->ref_count--;
            } else {
                if (top + 1 ==  num_nodes) {
                    nodes = realloc(nodes, num_nodes + 1);
                    if (NULL == nodes) return PERSIMM_ERROR_MEMORY;
                    indexes = realloc(indexes, num_nodes + 1);
                    if (NULL == indexes) return PERSIMM_ERROR_MEMORY;
                }

                top++;
                nodes[top] = next_node;
                indexes[top] = 0;
                curr_node->items[curr_index] = NULL;
            }
        }
    }
    /* while (top >= 0) { */
    /*     current = nodes[top--]; */
    /*     if (current->ref_count > 1) { */
    /*         current->ref_count--; */
    /*     } else { */
    /*         if (current->kind == PERSIMM_NODE_INNER) { */
    /*             if ((top + PERSIMM_VECTOR_WIDTH) > num_nodes) { */
    /*                 num_nodes += PERSIMM_VECTOR_WIDTH; */
    /*                 nodes = realloc(nodes, num_nodes * sizeof(persimm_vector_node*)); */
    /*                 if (NULL == nodes) return PERSIMM_ERROR_MEMORY; */
    /*             } */
    /*             for (int i = PERSIMM_VECTOR_WIDTH - 1; i >= 0; i--) { */
    /*                 if (NULL == current->items[i]) continue; */
    /*                 nodes[++top] = current->items[i]; */
    /*                 current->items[i] = NULL; */
    /*             } */
    /*         } */
    /*         free(current); */
    /*     } */
    /* } */

    free(nodes);
    free(indexes);

    return PERSIMM_ERROR_NONE;
}

persimm_error_code persimm_vector_deinit(persimm_vector *vector) {
    persimm_error_code err = 0;

    err = persimm_vector_free_node(vector->root, (vector->shift / PERSIMM_VECTOR_BITS) + 1);
    if (err) return err;
    err = persimm_vector_free_node(vector->tail, 1);
    if (err) return err;

    return PERSIMM_ERROR_NONE;
}

/* Initialising  */

static persimm_vector_node *persimm_vector_new_node(persimm_node_type kind) {
    persimm_vector_node *node = malloc(sizeof(persimm_vector_node));
    if (NULL == node) return NULL;

    node->kind = kind;
    node->ref_count = 1;
    for (size_t i = 0; i < PERSIMM_VECTOR_WIDTH; i++) node->items[i] = NULL;
    return node;
}

persimm_error_code persimm_vector_init(persimm_vector *vector) {
    vector->shift = 0;
    vector->count = 0;
    vector->tail_count = 0;
    vector->root = NULL;
    vector->tail = persimm_vector_new_node(PERSIMM_NODE_LEAF);
    if (NULL == vector->tail) return PERSIMM_ERROR_MEMORY;
    return PERSIMM_ERROR_NONE;
}

/* Copying */

static persimm_vector_node *persimm_vector_copy_node(persimm_vector_node *node) {
    if (NULL == node) return NULL;

    persimm_vector_node *copy = malloc(sizeof(persimm_vector_node));
    if (NULL == copy) return NULL;

    copy->kind = node->kind;
    copy->ref_count = 1;

    for (size_t i = 0; i < PERSIMM_VECTOR_WIDTH; i++) {
        copy->items[i] = node->items[i];
        if (copy->kind == PERSIMM_NODE_INNER && NULL != copy->items[i]) {
            ((persimm_vector_node *)copy->items[i])->ref_count++;
        }
    }

    return copy;
}

static persimm_vector *persimm_vector_copy(persimm_vector *vector) {
    persimm_vector *copy = malloc(sizeof(persimm_vector));
    if (NULL == copy) return NULL;

    copy->shift = vector->shift;
    copy->count = vector->count;
    copy->tail_count = vector->tail_count;
    copy->root = vector->root;
    if (NULL != copy->root) copy->root->ref_count++;
    copy->tail = vector->tail;
    if (NULL != copy->tail) copy->tail->ref_count++;

    return copy;
}

/* Cloning */

static persimm_vector_node *persimm_vector_clone_leaf(persimm_vector_node *node) {
    persimm_vector_node *clone = persimm_vector_new_node(node->kind);
    if (NULL == clone) return NULL;

    for (size_t i = 0; i < PERSIMM_VECTOR_WIDTH; i++) {
        if (NULL == node->items[i]) continue;
        clone->items[i] = node->items[i];
    }

    return clone;
}

static persimm_vector *persimm_vector_clone(persimm_vector *vector, size_t index) {
    persimm_vector *clone = malloc(sizeof(persimm_vector));
    if (NULL == clone) return NULL;

    clone->shift = vector->shift;
    clone->count = vector->count;
    clone->tail_count = vector->tail_count;

    clone->root = NULL;
    clone->tail = persimm_vector_clone_leaf(vector->tail);
    if (NULL == clone->tail) return NULL;

    /* Step 1: Check if index is in tail */
    if (index >= (vector->count - vector->tail_count)) {
        if (NULL != vector->root) {
            clone->root = persimm_vector_copy_node(vector->root);
            if (clone->root == NULL) return NULL;
        }
    } else {
        /* Step 2: Check if root is a leaf */
        if (vector->root->kind == PERSIMM_NODE_LEAF) {
            clone->root = persimm_vector_clone_leaf(vector->root);
            if (NULL == clone->root) return NULL;
        } else {
            /* Step 3: Copy subtrie */
            size_t level = clone->shift;
            size_t num_nodes = (level / PERSIMM_VECTOR_BITS) + 1;
            int top = 0;

            clone->root = persimm_vector_copy_node(vector->root);
            if (NULL == clone->root) return NULL;

            persimm_vector_node **nodes = malloc(num_nodes * sizeof(persimm_vector_node*));
            if (NULL == nodes) return NULL;
            nodes[top] = clone->root;

            size_t *indexes = malloc(num_nodes * sizeof(size_t));
            if (NULL == indexes) return NULL;
            indexes[top] = (index >> level) & PERSIMM_VECTOR_MASK;

            while (top >= 0) {
                persimm_vector_node *curr_node = nodes[top];
                size_t curr_index = indexes[top];
                if (curr_index == PERSIMM_VECTOR_WIDTH || curr_node->kind == PERSIMM_NODE_LEAF) {
                    level += PERSIMM_VECTOR_BITS;
                    top--;
                } else {
                    indexes[top] = curr_index + 1;
                    persimm_vector_node *next_node = curr_node->items[curr_index];
                    if (NULL == next_node) continue;

                    next_node->ref_count--;
                    next_node = persimm_vector_copy_node(next_node);
                    if (NULL == next_node) return NULL;
                    curr_node->items[curr_index] = next_node;

                    if (top + 1 ==  num_nodes) {
                        nodes = realloc(nodes, num_nodes + 1);
                        if (NULL == nodes) return NULL;
                        indexes = realloc(indexes, num_nodes + 1);
                        if (NULL == indexes) return NULL;
                    }

                    level -= PERSIMM_VECTOR_BITS;
                    top++;
                    nodes[top] = next_node;
                    indexes[top] = (index >> level) & PERSIMM_VECTOR_MASK;
                }
            }
        }
    }


    return clone;
}

/* Accessing */

persimm_error_code persimm_vector_get(persimm_vector *vector, size_t index, void **result) {
    if (persimm_vector_oob(vector, index)) return PERSIMM_ERROR_BOUNDS;

    size_t tail_start = vector->count - vector->tail_count;
    if (index >= tail_start) {
        *result = vector->tail->items[index - tail_start];
    } else {
        persimm_vector_node *node = vector->root;
        if (NULL == node) return PERSIMM_ERROR_MISSING;
        for (size_t level = vector->shift; level > 0; level -= PERSIMM_VECTOR_BITS) {
            size_t curr_index = (index >> level) & PERSIMM_VECTOR_MASK;
            node = node->items[curr_index];
            if (NULL == node) return PERSIMM_ERROR_MISSING;
        }
        if (node->kind == PERSIMM_NODE_INNER) return PERSIMM_ERROR_MALFORM;
        *result = node->items[index & PERSIMM_VECTOR_MASK];
    }

    return PERSIMM_ERROR_NONE;
}

/* Adding */

persimm_error_code persimm_vector_push(persimm_vector *old, persimm_vector **new, void *item) {
    bool immutable = false;
    persimm_vector *vector = old;

    if (NULL != new) {
        *new = persimm_vector_copy(old);
        if (NULL == new) return PERSIMM_ERROR_MEMORY;
        vector = *new;
        immutable = true;
    }

    /* Step 1: Check if space in tail */
    if (vector->tail_count < PERSIMM_VECTOR_WIDTH) {
        if (immutable) {
            vector->tail->ref_count--;
            vector->tail = persimm_vector_copy_node(vector->tail);
            if (NULL == vector->tail) return PERSIMM_ERROR_MEMORY;
        }
    } else {
        /* Step 2: Check if all items in tail */
        if (vector->count == PERSIMM_VECTOR_WIDTH) {
            vector->root = vector->tail;
        } else {
            /* Step 3: Check if space in trie */
            /* TODO: Not sure if this test correct */
            if ((vector->count >> PERSIMM_VECTOR_BITS) < (size_t)(1 << vector->shift)) {
                if (immutable) {
                    vector->root->ref_count--;
                    vector->root = persimm_vector_copy_node(vector->root);
                    if (NULL == vector->root) return PERSIMM_ERROR_MEMORY;
                }
            } else {
                persimm_vector_node *new_root = persimm_vector_new_node(PERSIMM_NODE_INNER);
                if (NULL == new_root) return PERSIMM_ERROR_MEMORY;

                new_root->items[0] = vector->root;
                vector->root = new_root;
                vector->shift += PERSIMM_VECTOR_BITS;
            }

            /* Step 4: Descend trie */
            persimm_vector_node *node = vector->root;
            size_t index = vector->count - PERSIMM_VECTOR_WIDTH;
            for (size_t level = vector->shift; level > PERSIMM_VECTOR_BITS; level -= PERSIMM_VECTOR_BITS) {
                size_t curr_index = (index >> level) & PERSIMM_VECTOR_MASK;
                persimm_vector_node *child = node->items[curr_index];

                if (NULL == child) {
                    child = persimm_vector_new_node(PERSIMM_NODE_INNER);
                    if (NULL == child) return PERSIMM_ERROR_MEMORY;
                    node->items[curr_index] = child;
                } else if (immutable) {
                    child->ref_count--;
                    child = persimm_vector_copy_node(child);
                    if (NULL == child) return PERSIMM_ERROR_MEMORY;
                    node->items[curr_index] = child;
                }

                node = child;
            }

            /* Step 5: Put tail in trie */
            node->items[(index >> PERSIMM_VECTOR_BITS) & PERSIMM_VECTOR_MASK] = vector->tail;
        }

        /* Step 6: Create new tail */
        persimm_vector_node *new_tail = persimm_vector_new_node(PERSIMM_NODE_LEAF);
        if (new_tail == NULL) return PERSIMM_ERROR_MEMORY;
        vector->tail = new_tail;
        vector->tail_count = 0;
    }

    /* Step 7: Append item to tail */
    vector->tail->items[vector->tail_count] = item;
    vector->tail_count++;
    vector->count++;
    return PERSIMM_ERROR_NONE;
}

persimm_error_code persimm_vector_update(persimm_vector *old, persimm_vector **new, void *item, size_t index) {
    if (persimm_vector_oob(old, index)) return PERSIMM_ERROR_BOUNDS;

    bool immutable = false;
    persimm_vector *vector = old;

    if (NULL != new) {
        *new = persimm_vector_copy(old);
        if (NULL == new) return PERSIMM_ERROR_MEMORY;
        vector = *new;
        immutable = true;
    }

    size_t tail_start = vector->count - vector->tail_count;
    if (index >= tail_start) {
        if (immutable) {
            vector->tail->ref_count--;
            vector->tail = persimm_vector_copy_node(vector->tail);
            if (NULL == vector->tail) return PERSIMM_ERROR_MEMORY;
        }
        vector->tail->items[index - tail_start] = item;
    } else {
        if (immutable) {
            vector->root->ref_count--;
            vector->root = persimm_vector_copy_node(vector->root);
            if (NULL == vector->root) return PERSIMM_ERROR_MEMORY;
        }

        persimm_vector_node *node = vector->root;
        for (size_t level = vector->shift; level > 0; level -= PERSIMM_VECTOR_BITS) {
            size_t curr_index = (index >> level) & PERSIMM_VECTOR_MASK;
            persimm_vector_node *child = node->items[curr_index];
            if (NULL == child) return PERSIMM_ERROR_MISSING;

            if (immutable) {
                child->ref_count--;
                child = persimm_vector_copy_node(child);
                if (NULL == child) return PERSIMM_ERROR_MEMORY;
                node->items[curr_index] = child;
            }

            node = child;
        }

        node->items[index & PERSIMM_VECTOR_MASK] = item;
    }

    return PERSIMM_ERROR_NONE;
}

persimm_error_code persimm_vector_insert(persimm_vector *old, persimm_vector **new, void *item, size_t index) {
    if (persimm_vector_oob(old, index)) return PERSIMM_ERROR_BOUNDS;

    persimm_error_code err = 0;
    persimm_vector *vector = old;

    if (NULL != new) {
        *new = persimm_vector_clone(old, 2);
        if (NULL == new) return PERSIMM_ERROR_MEMORY;
        vector = *new;
    }

    void *last = NULL;
    err = persimm_vector_get(vector, vector->count - 1, &last);
    if (err) return err;
    err = persimm_vector_push(vector, NULL, last);
    if (err) return err;

    for (size_t i = vector->count - 2; i >= index; i--) {
        void *curr_item = NULL;
        err = persimm_vector_get(vector, i, &curr_item);
        if (err) return err;
        err = persimm_vector_update(vector, NULL, curr_item, i + 1);
        if (err) return err;
    }

    err = persimm_vector_update(vector, NULL, item, index);
    if (err) return err;

    return PERSIMM_ERROR_NONE;
}


/* Removing */

persimm_error_code persimm_vector_pop(persimm_vector *old, persimm_vector **new, void **result) {
    if (old->count == 0) return PERSIMM_ERROR_EMPTY;

    persimm_error_code err = 0;
    bool immutable = false;
    persimm_vector *vector = old;

    if (NULL != new) {
        *new = persimm_vector_copy(old);
        if (NULL == new) return PERSIMM_ERROR_MEMORY;
        vector = *new;
        immutable = true;
    }

    /* Step 1: Set result to last item in tail */
    vector->count--;
    vector->tail_count--;
    *result = vector->tail->items[vector->tail_count];

    /* Step 2: Check if items left in tail */
    if (vector->tail_count > 0) {
        if (immutable) {
            vector->tail->ref_count--;
            vector->tail = persimm_vector_copy_node(vector->tail);
            if (NULL == vector->tail) return PERSIMM_ERROR_MEMORY;
        }

        vector->tail->items[vector->tail_count] = NULL;
    } else {
        /* Step 3: Remove tail */
        err = persimm_vector_free_node(vector->tail, 1);
        if (err) return err;

        /* Step 4: Check if all remaining items in root */
        if (vector->count == PERSIMM_VECTOR_WIDTH) {
            vector->tail = vector->root;
            vector->root = NULL;
        } else {
            if (immutable) {
                vector->root->ref_count--;
                vector->root = persimm_vector_copy_node(vector->root);
                if (NULL == vector->root) return PERSIMM_ERROR_MEMORY;
            }

            /* Step 5: Descend trie */
            /* TODO: Does this descend to the correct level? */
            persimm_vector_node *node = vector->root;
            persimm_vector_node *empty_parent = NULL;
            persimm_vector_node *empty_node = NULL;
            size_t index = vector->count - 1;
            for (size_t level = vector->shift; level > 0; level -= PERSIMM_VECTOR_BITS) {
                size_t curr_index = (index >> level) & PERSIMM_VECTOR_MASK;
                persimm_vector_node *child = node->items[curr_index];
                if (NULL == child) return PERSIMM_ERROR_MALFORM;

                if (immutable) {
                    child->ref_count--;
                    child = persimm_vector_copy_node(child);
                    if (NULL == child) return PERSIMM_ERROR_MEMORY;
                    node->items[curr_index] = child;
                }

                if (curr_index > 0) {
                    empty_node = child;
                    empty_parent = node;
                }

                node = child;
            }

            if (NULL == empty_node) return PERSIMM_ERROR_MALFORM;

            /* Step 6: Remove empty ancestors */
            for (size_t i = 0; i < PERSIMM_VECTOR_WIDTH; i++) {
                if (empty_parent->items[i] == empty_node) {
                    node->ref_count++;
                    err = persimm_vector_free_node(empty_node, (vector->shift / PERSIMM_VECTOR_BITS) + 1);
                    if (err) return err;
                    empty_parent->items[i] = NULL;
                    break;
                }
            }

            /* Step 7: Remove root if only one item */
            if (NULL == vector->root->items[1]) {
                persimm_vector_node *new_root = vector->root->items[0];
                new_root->ref_count++;
                err = persimm_vector_free_node(vector->root, 1);
                if (err) return err;
                vector->root = new_root;
                vector->shift -= PERSIMM_VECTOR_BITS;
            }

            /* Step 8: Move node to tail */
            vector->tail_count = PERSIMM_VECTOR_WIDTH;
            vector->tail = node;
        }
    }

    return PERSIMM_ERROR_NONE;
}

/* Testing */

static void print_vector(const char *name, persimm_vector *vector) {
    printf("---- %s ----\n", name);
    printf("Number of Items: %ld\n", vector->count);
    printf("Number of Tail Items: %ld\n", vector->tail_count);
    printf("Contents: [");

    void *result = NULL;
    for (size_t i = 0; i < vector->count; i++) {
        persimm_error_code err = persimm_vector_get(vector, i, &result);
        if (err) {
            printf(" <index %ld: error>", i);
        } else {
            printf(" %d", *(int *)result);
        }
    }

    printf(" ]\n");
}

int main() {
    persimm_vector *vector = malloc(sizeof(persimm_vector));
    persimm_error_code err = 0;

    persimm_vector_init(vector);
    printf("%s\n\n", "Vector initialised");

    int int_array[] = { 1, 2, 3, 4, 5 };

    printf("Array: [");
    for (size_t i = 0; i < 5; i++) {
        printf(" %d", int_array[i]);
        err = persimm_vector_push(vector, NULL, &int_array[i]);
        if (err) return 1;
    }
    printf(" ]\n");

    printf("\nAfter pushing\n");
    print_vector("vector", vector);

    int lucky = 37;
    persimm_vector *other_vector;
    err = persimm_vector_update(vector, &other_vector, &lucky, 0);
    if (err) return 1;

    printf("\nAfter updating\n");
    print_vector("vector", vector);
    print_vector("other_vector", other_vector);

    void *result = NULL;
    err = persimm_vector_pop(vector, NULL, &result);
    if (err) return 1;

    printf("\nAfter popping\n");
    print_vector("vector", vector);
    print_vector("other_vector", other_vector);

    int unlucky = 13;
    persimm_vector *another_vector;
    err = persimm_vector_insert(vector, &another_vector, &unlucky, 2);
    if (err) return 1;

    printf("\nAfter inserting\n");
    print_vector("vector", vector);
    print_vector("other_vector", other_vector);
    print_vector("another_vector", another_vector);

    persimm_vector_deinit(vector);
    persimm_vector_deinit(other_vector);
    persimm_vector_deinit(another_vector);
    printf("\nVectors deinitialised\n");

    free(vector);
    free(other_vector);
    free(another_vector);

    printf("Vectors freed\n");

    return 0;
}
