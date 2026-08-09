#ifndef PERSIMMON_JANET_WRAPPER_H
#define PERSIMMON_JANET_WRAPPER_H

#include <janet.h>

void persimm_register_type(JanetTable *env);
void persimm_register_functions(JanetTable *env);

#endif /* end of include guard */
