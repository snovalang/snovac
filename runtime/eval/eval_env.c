/* eval_env.c — lexical environments and binding ownership. */
#include "eval_internal.h"

Env *env_new(Interp *in, Env *parent) {
    Env *env = (Env *)sn_arena_calloc(in->arena, sizeof(*env));
    env->parent = parent;
    env->owner_region = in->current_region;
    return env;
}

Value *env_lookup_owned(Env *env, const char *name,
                        EvalRegion **owner_region) {
    for (; env; env = env->parent) {
        for (size_t i = env->names.len; i > 0; --i) {
            if (strcmp((const char *)env->names.items[i - 1u], name) == 0) {
                if (owner_region) *owner_region = env->owner_region;
                return (Value *)env->slots.items[i - 1u];
            }
        }
    }
    if (owner_region) *owner_region = NULL;
    return NULL;
}

Value *env_lookup(Env *env, const char *name) {
    return env_lookup_owned(env, name, NULL);
}

int eval_value_is_heap_owned(Value value) {
    return value.kind == V_STRING || value.kind == V_OBJECT ||
           value.kind == V_VARIANT || value.kind == V_LAMBDA ||
           value.kind == V_ARRAY;
}

Value *env_define(Interp *in, Env *env, const char *name, Value value) {
    if (in->current_region && env->owner_region != in->current_region) {
        eval_region_mark_current(in);
        if (eval_value_is_heap_owned(value))
            eval_value_mark_escaping(in, value);
    }
    Value *slot = (Value *)sn_arena_alloc(in->arena, sizeof(*slot));
    *slot = value;
    sn_list_push(in->arena, &env->names, (void *)name);
    sn_list_push(in->arena, &env->slots, slot);
    return slot;
}
