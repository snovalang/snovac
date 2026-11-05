/* eval_env.c — lexical environments and binding ownership. */
#include "eval_internal.h"

typedef struct {
    const char **names;
    size_t count;
    size_t capacity;
    const char *inline_names[16];
    int owns_names;
    int incomplete;
} CaptureNames;

static void capture_name_add(CaptureNames *set, const char *name) {
    if (!name || set->incomplete) return;
    for (size_t i = 0; i < set->count; ++i)
        if (strcmp(set->names[i], name) == 0) return;
    if (set->count == set->capacity) {
        size_t next = set->capacity * 2u;
        if (next < set->capacity || next > SIZE_MAX / sizeof(*set->names)) {
            set->incomplete = 1;
            return;
        }
        const char **grown;
        if (set->owns_names) {
            grown = (const char **)realloc((void *)set->names,
                                           next * sizeof(*set->names));
        } else {
            grown = (const char **)malloc(next * sizeof(*set->names));
            if (grown) memcpy(grown, set->names,
                              set->count * sizeof(*set->names));
        }
        if (!grown) {
            set->incomplete = 1;
            return;
        }
        set->names = grown;
        set->capacity = next;
        set->owns_names = 1;
    }
    set->names[set->count++] = name;
}

static void capture_name_remove(CaptureNames *set, const char *name) {
    if (!name) return;
    for (size_t i = 0; i < set->count; ++i) {
        if (strcmp(set->names[i], name) != 0) continue;
        memmove(&set->names[i], &set->names[i + 1u],
                (set->count - i - 1u) * sizeof(*set->names));
        set->count--;
        return;
    }
}

static void capture_expr(CaptureNames *set, const SnExpr *expr);

static void capture_pattern(CaptureNames *set, const SnPattern *pattern) {
    if (!pattern) return;
    capture_expr(set, pattern->literal);
    for (size_t i = 0; i < pattern->subs.len; ++i)
        capture_pattern(set, SN_LIST_AT(pattern->subs, SnPattern, i));
}

static void capture_stmt(CaptureNames *set, const SnStmt *stmt) {
    if (!stmt || set->incomplete) return;
    capture_expr(set, stmt->expr);
    capture_stmt(set, stmt->then_br);
    capture_stmt(set, stmt->else_br);
    capture_stmt(set, stmt->finally_br);
    for (size_t i = 0; i < stmt->stmts.len; ++i)
        capture_stmt(set, SN_LIST_AT(stmt->stmts, SnStmt, i));
    for (size_t i = 0; i < stmt->catches.len; ++i)
        capture_stmt(set, SN_LIST_AT(stmt->catches, SnStmt, i));
    for (size_t i = 0; i < stmt->arms.len; ++i) {
        const SnMatchArm *arm = SN_LIST_AT(stmt->arms, SnMatchArm, i);
        capture_pattern(set, arm->pattern);
        capture_expr(set, arm->guard);
        capture_expr(set, arm->value);
        capture_stmt(set, arm->body);
    }
}

static void capture_lambda_body(CaptureNames *set, const SnExpr *lambda) {
    if (!lambda || set->incomplete) return;
    capture_expr(set, lambda->value);
    capture_stmt(set, lambda->body);
}

static void capture_nested_lambda(CaptureNames *set, const SnExpr *lambda) {
    CaptureNames nested = {0};
    nested.names = nested.inline_names;
    nested.capacity = sizeof(nested.inline_names) / sizeof(nested.inline_names[0]);
    capture_lambda_body(&nested, lambda);
    for (size_t i = 0; i < lambda->params.len; ++i) {
        const SnParam *param = SN_LIST_AT(lambda->params, SnParam, i);
        capture_name_remove(&nested, param->name);
    }
    if (nested.incomplete) set->incomplete = 1;
    else for (size_t i = 0; i < nested.count; ++i)
        capture_name_add(set, nested.names[i]);
    if (nested.owns_names) free((void *)nested.names);
}

static void capture_expr(CaptureNames *set, const SnExpr *expr) {
    if (!expr || set->incomplete) return;
    if (expr->kind == SN_EXPR_IDENT) capture_name_add(set, expr->text);
    else if (expr->kind == SN_EXPR_THIS) capture_name_add(set, "this");
    else if (expr->kind == SN_EXPR_STRING && expr->interpolated) {
        /* Interpolation is reparsed at runtime; keep a safe full-frame fallback. */
        set->incomplete = 1;
        return;
    } else if (expr->kind == SN_EXPR_LAMBDA) {
        capture_nested_lambda(set, expr);
        return;
    }
    capture_expr(set, expr->lhs);
    capture_expr(set, expr->rhs);
    for (size_t i = 0; i < expr->args.len; ++i)
        capture_expr(set, SN_LIST_AT(expr->args, SnExpr, i));
    capture_expr(set, expr->value);
    capture_stmt(set, expr->body);
    capture_stmt(set, expr->else_body);
    for (size_t i = 0; i < expr->arms.len; ++i) {
        const SnMatchArm *arm = SN_LIST_AT(expr->arms, SnMatchArm, i);
        capture_pattern(set, arm->pattern);
        capture_expr(set, arm->guard);
        capture_expr(set, arm->value);
        capture_stmt(set, arm->body);
    }
}

static void capture_remove_lambda_params(CaptureNames *set,
                                         const SnExpr *lambda) {
    for (size_t i = 0; i < lambda->params.len; ++i) {
        const SnParam *param = SN_LIST_AT(lambda->params, SnParam, i);
        capture_name_remove(set, param->name);
    }
}

Env *env_capture_lambda(Interp *in, Env *source, const SnExpr *lambda) {
    if (in->closure_count < SIZE_MAX) in->closure_count++;
    Env *capture = env_new(in, NULL);
    if (!source) return capture;
    CaptureNames names = {0};
    names.names = names.inline_names;
    names.capacity = sizeof(names.inline_names) / sizeof(names.inline_names[0]);
    capture_lambda_body(&names, lambda);
    capture_remove_lambda_params(&names, lambda);
    if (names.incomplete) {
        if (in->capture_fallback_count < SIZE_MAX) in->capture_fallback_count++;
        /* Keep the complete lexical frame chain if analysis cannot enumerate
         * every runtime reference (for example, interpolation reparsed later). */
        capture->parent = source->parent;
        capture->captured_regions.len = 0;
        for (size_t i = 0; i < source->names.len; ++i) {
            sn_list_push(in->arena, &capture->names, source->names.items[i]);
            sn_list_push(in->arena, &capture->slots, source->slots.items[i]);
            sn_list_push(in->arena, &capture->captured_regions, source->owner_region);
        }
    } else {
        for (size_t i = 0; i < names.count; ++i) {
            EvalRegion *owner_region = NULL;
            Value *slot = env_lookup_owned(source, names.names[i], &owner_region);
            if (!slot) continue;
            sn_list_push(in->arena, &capture->names, (void *)names.names[i]);
            sn_list_push(in->arena, &capture->slots, slot);
            sn_list_push(in->arena, &capture->captured_regions, owner_region);
        }
    }
    if (in->captured_slot_count <= SIZE_MAX - capture->slots.len)
        in->captured_slot_count += capture->slots.len;
    else
        in->captured_slot_count = SIZE_MAX;
    if (names.owns_names) free((void *)names.names);
    return capture;
}

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
    SnArena *env_arena = env->owner_region ? &env->owner_region->arena
                                          : in->root_arena;
    if (in->current_region && env->owner_region != in->current_region) {
        if (eval_value_is_heap_owned(value))
            eval_value_mark_escaping(in, value);
    }
    /* Binding metadata belongs to the environment, not to whichever nested
     * call happens to add the binding. Otherwise a one-slot assignment into
     * an outer environment would force the entire call region to escape. */
    Value *slot = (Value *)sn_arena_alloc(env_arena, sizeof(*slot));
    *slot = value;
    sn_list_push(env_arena, &env->names, (void *)name);
    sn_list_push(env_arena, &env->slots, slot);
    return slot;
}
