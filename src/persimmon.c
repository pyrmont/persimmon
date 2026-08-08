#include "persimmon_internal.h"

/*
 * The pieces every structure shares. The structures themselves each live in
 * their own translation unit.
 */

/* Status Codes */

const char *persimm_status_string(persimm_status status) {
    switch (status) {
        case PERSIMM_OK: return "ok";
        case PERSIMM_ERR_ALLOC: return "out of memory";
        case PERSIMM_ERR_BOUNDS: return "index out of bounds";
        case PERSIMM_ERR_INVALID: return "invalid argument";
        case PERSIMM_ERR_CORRUPT: return "corrupt structure";
        default: return "unknown error";
    }
}

/* Reference Counting */

bool persimm_has_atomic_refcounts(void) {
    return PERSIMM_RC_ATOMIC ? true : false;
}
