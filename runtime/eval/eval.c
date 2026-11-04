/* eval.c — interpreter entry point, environments, object model and calls.
 * Expressions, statements and string decoding live in their own files; see
 * eval_internal.h for the split. */
#include "eval_internal.h"
#include "sn_native_dispatch.h"

void rt_error(Interp *in, int code, SnSpan span, const char *fmt, ...) {
    if (in->failed) {
        return;
    }
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    sn_diag_emit(in->diag, SN_DIAG_ERROR, code, span, "%s", buf);
    in->failed = 1;
    in->flow = FLOW_RETURN;
}

/* ── environment ──────────────────────────────────────────────────────────── */

Env *env_new(Interp *in, Env *parent) {
    Env *e = (Env *)sn_arena_calloc(in->arena, sizeof(Env));
    e->parent = parent;
    e->owner_region = in->current_region;
    return e;
}

Value *env_lookup_owned(Env *e, const char *name, EvalRegion **owner_region) {
    for (; e; e = e->parent) {
        for (size_t i = e->names.len; i > 0; i--) {
            if (strcmp((const char *)e->names.items[i - 1], name) == 0) {
                if (owner_region) *owner_region = e->owner_region;
                return (Value *)e->slots.items[i - 1];
            }
        }
    }
    if (owner_region) *owner_region = NULL;
    return NULL;
}

Value *env_lookup(Env *e, const char *name) {
    return env_lookup_owned(e, name, NULL);
}

int eval_value_is_heap_owned(Value value) {
    return value.kind == V_STRING || value.kind == V_OBJECT ||
           value.kind == V_VARIANT || value.kind == V_LAMBDA ||
           value.kind == V_ARRAY;
}

Value *env_define(Interp *in, Env *e, const char *name, Value v) {
    if (in->current_region && eval_value_is_heap_owned(v) &&
        e->owner_region != in->current_region)
        eval_region_mark_escaping(in);
    Value *slot = (Value *)sn_arena_alloc(in->arena, sizeof(Value));
    *slot = v;
    sn_list_push(in->arena, &e->names, (void *)name);
    sn_list_push(in->arena, &e->slots, slot);
    return slot;
}

/* ── declaration lookup ───────────────────────────────────────────────────── */

const SnDecl *find_member(const SnDecl *cls, const char *name) {
    if (!cls) {
        return NULL;
    }
    for (size_t i = 0; i < cls->members.len; i++) {
        const SnDecl *m = (const SnDecl *)cls->members.items[i];
        if (m->name && strcmp(m->name, name) == 0) {
            return m;
        }
    }
    return NULL;
}

const SnDecl *find_member_inherited(const Interp *in, const SnDecl *cls, const char *name) {
    if (!cls) return NULL;
    const SnDecl *m = find_member(cls, name);
    if (m) return m;
    for (size_t i = 0; i < cls->supertypes.len; i++) {
        const SnType *st = (const SnType *)cls->supertypes.items[i];
        if (st && st->name) {
            const SnDecl *parent = find_type(in, st->name);
            if (parent) {
                const SnDecl *pm = find_member_inherited(in, parent, name);
                if (pm) return pm;
            }
        }
    }
    return NULL;
}

const SnDecl *find_top(const Interp *in, const char *name, SnDeclKind k) {
    for (size_t i = 0; i < in->unit->decls.len; i++) {
        const SnDecl *d = (const SnDecl *)in->unit->decls.items[i];
        if (d->kind == k && d->name && strcmp(d->name, name) == 0) {
            return d;
        }
    }
    return NULL;
}

const SnDecl *find_type(const Interp *in, const char *name) {
    for (size_t i = 0; i < in->unit->decls.len; i++) {
        const SnDecl *d = (const SnDecl *)in->unit->decls.items[i];
        if ((d->kind == SN_DECL_CLASS || d->kind == SN_DECL_STRUCT) &&
            d->name && strcmp(d->name, name) == 0) {
            return d;
        }
    }
    return NULL;
}

void sn_eval_merge_extensions(SnArena *arena, SnUnit *unit) {
    for (size_t i = 0; i < unit->decls.len; i++) {
        SnDecl *ext = (SnDecl *)unit->decls.items[i];
        if (ext->kind != SN_DECL_EXTENSION || !ext->name) {
            continue;
        }
        for (size_t j = 0; j < unit->decls.len; j++) {
            SnDecl *target = (SnDecl *)unit->decls.items[j];
            if ((target->kind != SN_DECL_CLASS && target->kind != SN_DECL_STRUCT) ||
                !target->name || strcmp(target->name, ext->name) != 0) {
                continue;
            }
            for (size_t k = 0; k < ext->members.len; k++) {
                sn_list_push(arena, &target->members, ext->members.items[k]);
            }
            break;
        }
    }
}

/* ── calls ────────────────────────────────────────────────────────────────── */

EvalRegion *eval_region_begin(Interp *in) {
    EvalRegion *region = (EvalRegion *)calloc(1, sizeof(*region));
    if (!region) {
        fputs("snovac evaluator: out of memory allocating call region\n", stderr);
        abort();
    }
    sn_arena_init(&region->arena, SN_EVAL_ARENA_BLOCK_SIZE);
    region->parent_arena = in->arena;
    region->parent_region = in->current_region;
    in->arena = &region->arena;
    in->current_region = region;
    return region;
}

void eval_region_end(Interp *in, EvalRegion *region) {
    in->arena = region->parent_arena;
    in->current_region = region->parent_region;
    if (region->may_escape) {
        region->parent_arena = NULL;
        region->parent_region = NULL;
        region->next_retained = in->retained_regions;
        in->retained_regions = region;
    } else {
        array_free_storage(in, region->arrays);
        region->arrays = NULL;
        sn_arena_free(&region->arena);
        free(region);
    }
}

void eval_region_mark_escaping(Interp *in) {
    /* A value allocated in a child region may retain its enclosing Env chain
     * (closures are the main case). Keep every owning ancestor alive too. */
    if (!in) return;
    for (EvalRegion *region = in->current_region; region;
         region = region->parent_region) {
        region->may_escape = 1;
    }
}

Value call_function(Interp *in, const SnDecl *fn, SnList *args, Env *caller,
                    Object *self, SnSpan span) {
    Value out_native;
    if (sn_native_try_dispatch(in, fn, args, caller, self, span, &out_native)) {
        return out_native;
    }

    if (!fn->body) {
        rt_error(in, SNOVA_UNKNOWN_INTRINSIC, span,
                 "`%s` has no body and its @native intrinsic is not implemented yet",
                 fn->name ? fn->name : "?");
        return v_unit();
    }

    EvalRegion *region = eval_region_begin(in);
    Env *local = env_new(in, NULL); /* functions do not close over the caller */

    /* `this` and the receiver's fields are visible inside a method body. */
    if (self) {
        Value tv;
        tv.kind = V_OBJECT;
        tv.as.o = self;
        env_define(in, local, "this", tv);
        for (size_t i = 0; i < self->names.len; i++) {
            sn_list_push(in->arena, &local->names, self->names.items[i]);
            sn_list_push(in->arena, &local->slots, self->slots.items[i]);
        }
    }

    for (size_t i = 0; i < fn->params.len; i++) {
        const SnParam *p = (const SnParam *)fn->params.items[i];
        Value v;
        if (args && i < args->len) {
            v = eval_expr(in, caller, (const SnExpr *)args->items[i]);
        } else if (p->def) {
            v = eval_expr(in, caller, p->def);
        } else {
            v = default_for(p->type);
        }
        env_define(in, local, p->name, v);
    }

    Value saved = in->ret;
    in->ret = v_unit();
    Flow f = exec_stmt(in, local, fn->body);
    Value r = (f == FLOW_RETURN) ? in->ret : v_unit();
    if (f == FLOW_RETURN && eval_value_is_heap_owned(r))
        eval_region_mark_escaping(in);
    in->ret = saved;
    in->flow = FLOW_NORMAL;
    eval_region_end(in, region);
    return r;
}

Value call_method(Interp *in, Object *self, const SnDecl *m, SnList *args,
                  Env *caller, SnSpan span) {
    return call_function(in, m, args, caller, self, span);
}

Value call_lambda(Interp *in, const LambdaVal *lam, SnList *args, Env *caller,
                  SnSpan span) {
    (void)span;
    EvalRegion *region = eval_region_begin(in);
    Env *local = env_new(in, lam->env); /* lambdas close over their env */
    for (size_t i = 0; i < lam->expr->params.len; i++) {
        const SnParam *p = (const SnParam *)lam->expr->params.items[i];
        Value v;
        if (args && i < args->len) {
            v = eval_expr(in, caller, (const SnExpr *)args->items[i]);
        } else if (p->def) {
            v = eval_expr(in, caller, p->def);
        } else {
            v = default_for(p->type);
        }
        env_define(in, local, p->name, v);
    }
    Value result = v_unit();
    if (lam->expr->value) result = eval_expr(in, local, lam->expr->value);
    if (lam->expr->body) {
        Value saved = in->ret;
        in->ret = v_unit();
        Flow f = exec_stmt(in, local, lam->expr->body);
        result = (f == FLOW_RETURN) ? in->ret : v_unit();
        in->ret = saved;
        in->flow = FLOW_NORMAL;
    }
    if (eval_value_is_heap_owned(result)) eval_region_mark_escaping(in);
    eval_region_end(in, region);
    return result;
}

/* ── entry point ──────────────────────────────────────────────────────────── */

int sn_eval_run(SnArena *arena, SnDiagSink *diag, const SnUnit *unit) {
    (void)arena; /* The unit's AST is borrowed; runtime allocations are private. */
    if (!sn_runtime_init()) {
        SnSpan z = {0, 0, 1, 1};
        sn_diag_emit(diag, SN_DIAG_ERROR, SNOVA_UNSUPPORTED, z,
                     "could not initialize native runtime subsystems");
        sn_runtime_shutdown();
        return -1;
    }
    SnArena runtime_arena;
    sn_arena_init(&runtime_arena, SN_EVAL_ARENA_BLOCK_SIZE);

    Interp in;
    /* Keep transient environments, objects and generated strings out of the
     * caller's AST/token arena. Source declarations remain borrowed from it. */
    in.arena = &runtime_arena;
    in.diag = diag;
    in.unit = unit;
    in.globals = env_new(&in, NULL);
    in.arrays = NULL;
    in.array_reserved_bytes = 0;
    in.array_used_bytes = 0;
    in.array_peak_bytes = 0;
    in.array_peak_used_bytes = 0;
    in.current_region = NULL;
    in.retained_regions = NULL;
    in.ret = v_unit();
    in.flow = FLOW_NORMAL;
    in.failed = 0;

    const SnDecl *main_fn = find_top(&in, "main", SN_DECL_FUNC);
    if (!main_fn) {
        SnSpan z = {0, 0, 1, 1};
        sn_diag_emit(diag, SN_DIAG_ERROR, SNOVA_NO_MAIN, z,
                     "no `func main()` found in this file");
        if (getenv("SNOVA_MEMORY_STATS"))
            sn_arena_report_stats(stderr, "evaluator-runtime", &runtime_arena);
        sn_arena_free(&runtime_arena);
        sn_runtime_shutdown();
        return -1;
    }

    Env *global = env_new(&in, NULL);
    SnList no_args = {0};
    Value r = call_function(&in, main_fn, &no_args, global, NULL, main_fn->span);

    int result = in.failed ? -1 : (r.kind == V_INT) ? (int)r.as.i : 0;
    array_free_storage(&in, in.arrays);
    in.arrays = NULL;
    EvalRegion *region = in.retained_regions;
    while (region) {
        EvalRegion *next = region->next_retained;
        if (getenv("SNOVA_MEMORY_STATS"))
            sn_arena_report_stats(stderr, "evaluator-call-region", &region->arena);
        array_free_storage(&in, region->arrays);
        region->arrays = NULL;
        sn_arena_free(&region->arena);
        free(region);
        region = next;
    }
    in.retained_regions = NULL;
    if (getenv("SNOVA_MEMORY_STATS")) {
        fprintf(stderr,
                "memory[evaluator-arrays]: used=%zu reserved=%zu slack=%zu peak-used=%zu peak-reserved=%zu\n",
                in.array_used_bytes, in.array_reserved_bytes,
                in.array_reserved_bytes - in.array_used_bytes,
                in.array_peak_used_bytes, in.array_peak_bytes);
        sn_arena_report_stats(stderr, "evaluator-runtime", &runtime_arena);
    }
    sn_arena_free(&runtime_arena);
    sn_runtime_shutdown();
    return result;
}
