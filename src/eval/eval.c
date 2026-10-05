/* eval.c — interpreter entry point, environments, object model and calls.
 * Expressions, statements and string decoding live in their own files; see
 * eval_internal.h for the split. */
#include "eval_internal.h"
#include "async.h"

void sn_rt_error(SnEvalInterp *in, int code, SnSpan span, const char *fmt, ...) {
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

SnEvalEnv *sn_eval_env_new(SnEvalInterp *in, SnEvalEnv *parent) {
    SnEvalEnv *e = (SnEvalEnv *)sn_arena_calloc(in->arena, sizeof(SnEvalEnv));
    e->parent = parent;
    return e;
}

SnEvalValue *sn_eval_env_lookup(SnEvalEnv *e, const char *name) {
    for (; e; e = e->parent) {
        for (size_t i = e->names.len; i > 0; i--) {
            if (strcmp((const char *)e->names.items[i - 1], name) == 0) {
                return (SnEvalValue *)e->slots.items[i - 1];
            }
        }
    }
    return NULL;
}

SnEvalValue *sn_eval_env_define(SnEvalInterp *in, SnEvalEnv *e, const char *name, SnEvalValue v) {
    SnEvalValue *slot = (SnEvalValue *)sn_arena_alloc(in->arena, sizeof(SnEvalValue));
    *slot = v;
    sn_list_push(in->arena, &e->names, (void *)name);
    sn_list_push(in->arena, &e->slots, slot);
    return slot;
}

/* ── strings ──────────────────────────────────────────────────────────────── */

char *sn_eval_sprintf(SnEvalInterp *in, const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) {
        n = 0;
    }
    return sn_arena_strndup(in->arena, buf,
                            (size_t)n < sizeof(buf) ? (size_t)n
                                                    : sizeof(buf) - 1);
}

char *sn_eval_str_concat(SnEvalInterp *in, const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    char *out = (char *)sn_arena_alloc(in->arena, la + lb + 1);
    memcpy(out, a, la);
    memcpy(out + la, b, lb);
    out[la + lb] = '\0';
    return out;
}

const char *sn_eval_to_string(SnEvalInterp *in, SnEvalValue v, SnSpan span) {
    switch (v.kind) {
    case V_UNIT:   return "unit";
    case V_INT:    return sn_eval_sprintf(in, "%lld", v.as.i);
    case V_DOUBLE: return sn_eval_sprintf(in, "%g", v.as.d);
    case V_BOOL:   return v.as.b ? "true" : "false";
    case V_STRING: return v.as.s;
    case V_CHAR: {
        char *s = (char *)sn_arena_alloc(in->arena, 2);
        s[0] = (char)v.as.i;
        s[1] = '\0';
        return s;
    }
    case V_OBJECT: {
        /* Printing an object uses its `asString()`, matching the convention the
         * corpus relies on (tests/run-pass/counter.snl). */
        const SnDecl *m = sn_eval_find_member(v.as.o->cls, "asString");
        if (m && m->body) {
            SnList none = {0};
            SnEvalValue s = sn_eval_call_method(in, v.as.o, m, &none, NULL, span);
            if (s.kind == V_STRING) {
                return s.as.s;
            }
        }
        return sn_eval_sprintf(in, "<%s>",
                             v.as.o->cls->name ? v.as.o->cls->name : "object");
    }
    case V_VARIANT: {
        if (v.as.vt->payload.len == 0) {
            return v.as.vt->name;
        }
        if (v.as.vt->payload.len == 1 &&
            (strcmp(v.as.vt->name, "Ok") == 0 || strcmp(v.as.vt->name, "Some") == 0)) {
            return sn_eval_to_string(in, *(const SnEvalValue *)v.as.vt->payload.items[0], span);
        }
        const char *out = sn_eval_sprintf(in, "%s(", v.as.vt->name);
        for (size_t i = 0; i < v.as.vt->payload.len; i++) {
            const SnEvalValue *p = (const SnEvalValue *)v.as.vt->payload.items[i];
            out = sn_eval_str_concat(in, out, sn_eval_to_string(in, *p, span));
            if (i + 1 < v.as.vt->payload.len) {
                out = sn_eval_str_concat(in, out, ", ");
            }
        }
        return sn_eval_str_concat(in, out, ")");
    }
    case V_LAMBDA:
        return "<lambda>";
    case V_ARRAY: {
        const char *out = "[";
        for (size_t i = 0; i < v.as.arr->items.len; i++) {
            const SnEvalValue *item = (const SnEvalValue *)v.as.arr->items.items[i];
            out = sn_eval_str_concat(in, out, sn_eval_to_string(in, *item, span));
            if (i + 1 < v.as.arr->items.len) {
                out = sn_eval_str_concat(in, out, ", ");
            }
        }
        return sn_eval_str_concat(in, out, "]");
    }
    case V_REF:
        return v.as.ref && v.as.ref->is_null ? "null" : "&";
    }
    return "?";
}

/* ── variants and equality ────────────────────────────────────────────────── */

SnEvalValue sn_eval_variant(SnEvalInterp *in, const char *name, SnList payload) {
    VariantVal *vt = (VariantVal *)sn_arena_calloc(in->arena, sizeof(VariantVal));
    vt->name = name;
    vt->payload = payload;
    SnEvalValue v;
    v.kind = V_VARIANT;
    v.as.vt = vt;
    return v;
}

int sn_eval_value_equals(SnEvalValue a, SnEvalValue b) {
    if (a.kind == V_INT && b.kind == V_INT) return a.as.i == b.as.i;
    if (a.kind == V_BOOL && b.kind == V_BOOL) return a.as.b == b.as.b;
    if (a.kind == V_DOUBLE && b.kind == V_DOUBLE) return a.as.d == b.as.d;
    if ((a.kind == V_INT && b.kind == V_DOUBLE) ||
        (a.kind == V_DOUBLE && b.kind == V_INT)) {
        double x = (a.kind == V_DOUBLE) ? a.as.d : (double)a.as.i;
        double y = (b.kind == V_DOUBLE) ? b.as.d : (double)b.as.i;
        return x == y;
    }
    if (a.kind == V_STRING && b.kind == V_STRING) {
        return strcmp(a.as.s, b.as.s) == 0;
    }
    if (a.kind == V_CHAR && b.kind == V_CHAR) {
        return a.as.i == b.as.i;
    }
    if (a.kind == V_UNIT && b.kind == V_UNIT) return 1;
    if (a.kind == V_VARIANT && b.kind == V_VARIANT) {
        if (strcmp(a.as.vt->name, b.as.vt->name) != 0 ||
            a.as.vt->payload.len != b.as.vt->payload.len) {
            return 0;
        }
        for (size_t i = 0; i < a.as.vt->payload.len; i++) {
            if (!sn_eval_value_equals(*(const SnEvalValue *)a.as.vt->payload.items[i],
                              *(const SnEvalValue *)b.as.vt->payload.items[i])) {
                return 0;
            }
        }
        return 1;
    }
    return 0;
}

/* ── declaration lookup ───────────────────────────────────────────────────── */

const SnDecl *sn_eval_find_member(const SnDecl *cls, const char *name) {
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

const SnDecl *sn_eval_find_member_inherited(const SnEvalInterp *in, const SnDecl *cls, const char *name) {
    if (!cls) return NULL;
    const SnDecl *m = sn_eval_find_member(cls, name);
    if (m) return m;
    for (size_t i = 0; i < cls->supertypes.len; i++) {
        const SnType *st = (const SnType *)cls->supertypes.items[i];
        if (st && st->name) {
            const SnDecl *parent = sn_eval_find_type(in, st->name);
            if (parent) {
                const SnDecl *pm = sn_eval_find_member_inherited(in, parent, name);
                if (pm) return pm;
            }
        }
    }
    return NULL;
}

static int param_matches_expr(SnEvalInterp *in, SnEvalEnv *env, const SnType *ty, const SnExpr *arg) {
    if (!ty || ty->kind != SN_TYPE_NAME || !ty->name || !arg) {
        return 1;
    }
    int known = 0;
    ValKind kind = V_UNIT;
    if (arg->kind == SN_EXPR_STRING) {
        kind = V_STRING;
        known = 1;
    } else if (arg->kind == SN_EXPR_CHAR) {
        kind = V_CHAR;
        known = 1;
    } else if (arg->kind == SN_EXPR_INT || arg->kind == SN_EXPR_LONG) {
        kind = V_INT;
        known = 1;
    } else if (arg->kind == SN_EXPR_BOOL) {
        kind = V_BOOL;
        known = 1;
    } else if (arg->kind == SN_EXPR_IDENT && arg->text) {
        SnEvalValue *slot = sn_eval_env_lookup(env, arg->text);
        if (slot) {
            kind = slot->kind;
            known = 1;
        }
    }
    if (!known) {
        return 1;
    }
    if (strcmp(ty->name, "string") == 0) {
        return kind == V_STRING;
    }
    if (strcmp(ty->name, "char") == 0) {
        return kind == V_CHAR;
    }
    if (strcmp(ty->name, "bool") == 0) {
        return kind == V_BOOL;
    }
    if (strcmp(ty->name, "int") == 0 || strcmp(ty->name, "long") == 0 ||
        strcmp(ty->name, "int8") == 0 || strcmp(ty->name, "int16") == 0 ||
        strcmp(ty->name, "int32") == 0 || strcmp(ty->name, "int64") == 0 ||
        strcmp(ty->name, "int128") == 0 || strcmp(ty->name, "byte") == 0) {
        return kind == V_INT;
    }
    (void)in;
    return 1;
}

static int method_accepts(SnEvalInterp *in, SnEvalEnv *env, const SnDecl *m, const SnList *args) {
    size_t nargs = args ? args->len : 0;
    if (!m || m->params.len != nargs) {
        return 0;
    }
    for (size_t i = 0; i < nargs; i++) {
        const SnParam *p = (const SnParam *)m->params.items[i];
        const SnExpr *arg = (const SnExpr *)args->items[i];
        if (!param_matches_expr(in, env, p->type, arg)) {
            return 0;
        }
    }
    return 1;
}

const SnDecl *sn_eval_find_overload(SnEvalInterp *in, SnEvalEnv *env, const SnDecl *cls, const char *name,
                            const SnList *args) {
    const SnDecl *first = NULL;
    const SnDecl *winner = NULL;
    int wins = 0;
    for (const SnDecl *cur = cls; cur; ) {
        for (size_t i = 0; i < cur->members.len; i++) {
            const SnDecl *m = (const SnDecl *)cur->members.items[i];
            if (!m->name || strcmp(m->name, name) != 0 || m->kind != SN_DECL_METHOD) {
                continue;
            }
            if (!first) {
                first = m;
            }
            if (method_accepts(in, env, m, args)) {
                winner = m;
                wins++;
            }
        }
        const SnDecl *parent = NULL;
        if (in && cur->supertypes.len > 0) {
            const SnType *st = (const SnType *)cur->supertypes.items[0];
            if (st && st->name) {
                parent = sn_eval_find_type(in, st->name);
            }
        }
        cur = parent;
    }
    if (wins == 1) {
        return winner;
    }
    return first ? first : sn_eval_find_member_inherited(in, cls, name);
}

const SnDecl *sn_eval_find_top(const SnEvalInterp *in, const char *name, SnDeclKind k) {
    for (size_t i = 0; i < in->unit->decls.len; i++) {
        const SnDecl *d = (const SnDecl *)in->unit->decls.items[i];
        if (d->kind == k && d->name && strcmp(d->name, name) == 0) {
            return d;
        }
    }
    return NULL;
}

const SnDecl *sn_eval_find_type(const SnEvalInterp *in, const char *name) {
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

/* ── objects ──────────────────────────────────────────────────────────────── */

SnEvalValue sn_eval_default_for(const SnType *t) {
    if (t && t->name) {
        if (strcmp(t->name, "int") == 0 || strcmp(t->name, "long") == 0 ||
            strcmp(t->name, "int8") == 0 || strcmp(t->name, "int16") == 0 ||
            strcmp(t->name, "int32") == 0 || strcmp(t->name, "int64") == 0 ||
            strcmp(t->name, "int128") == 0 || strcmp(t->name, "byte") == 0) return v_int(0);
        if (strcmp(t->name, "char") == 0) return v_char(0);
        if (strcmp(t->name, "double") == 0 || strcmp(t->name, "decimal") == 0) return v_double(0);
        if (strcmp(t->name, "bool") == 0) return v_bool(0);
        if (strcmp(t->name, "string") == 0) return v_str("");
    }
    return v_unit();
}

SnEvalObject *sn_eval_instantiate(SnEvalInterp *in, const SnDecl *cls, SnList *args, SnList *names,
                    SnEvalEnv *env, SnSpan span) {
    SnEvalObject *o = (SnEvalObject *)sn_rt_alloc(in, sizeof(SnEvalObject), 1);
    o->cls = cls;

    int named = 0;
    if (names && args && names->len == args->len) {
        for (size_t i = 0; i < names->len; i++) {
            if (names->items[i]) {
                named = 1;
                break;
            }
        }
    }

    /* Fields are declared in order; positional constructor arguments fill them
     * in that same order (`LiveWorkbook(path, frame, false)`). Named arguments
     * (`Snovalang(field: 1)`) select the field by name. */
    size_t argi = 0;
    for (size_t i = 0; i < cls->members.len; i++) {
        const SnDecl *m = (const SnDecl *)cls->members.items[i];
        if (m->kind != SN_DECL_FIELD) {
            continue;
        }
        SnEvalValue v = sn_eval_default_for(m->type);
        int filled = 0;
        if (named && args) {
            for (size_t ai = 0; ai < args->len; ai++) {
                const char *fname = (const char *)names->items[ai];
                if (fname && m->name && strcmp(fname, m->name) == 0) {
                    v = sn_eval_expr(in, env, (const SnExpr *)args->items[ai]);
                    filled = 1;
                    break;
                }
            }
        } else if (args && argi < args->len) {
            v = sn_eval_expr(in, env, (const SnExpr *)args->items[argi]);
            argi++;
            filled = 1;
        }
        if (!filled && m->init) {
            v = sn_eval_expr(in, env, m->init);
        }
        SnEvalValue *slot = (SnEvalValue *)sn_arena_alloc(in->arena, sizeof(SnEvalValue));
        *slot = v;
        sn_list_push(in->arena, &o->names, (void *)m->name);
        sn_list_push(in->arena, &o->slots, slot);
    }
    (void)span;
    return o;
}

SnEvalValue *sn_eval_object_field(SnEvalObject *o, const char *name) {
    for (size_t i = 0; i < o->names.len; i++) {
        if (strcmp((const char *)o->names.items[i], name) == 0) {
            return (SnEvalValue *)o->slots.items[i];
        }
    }
    return NULL;
}

/* ── calls ────────────────────────────────────────────────────────────────── */

typedef struct {
    SnEvalInterp *in;
    SnEvalEnv *local;
    SnEvalEnv *caller;
    const SnStmt *body;
    SnEvalValue result;
} SnAsyncCall;

static void sn_eval_async_step(SnAsyncTask *task) {
    SnAsyncCall *job = (SnAsyncCall *)task->user_data;
    job->result = sn_rt_finish_call(job->in, job->local, job->caller, job->body);
    task->state = SN_TASK_COMPLETED;
}

SnEvalValue sn_eval_call_function(SnEvalInterp *in, const SnDecl *fn, SnList *args, SnEvalEnv *caller,
                    SnEvalObject *self, SnSpan span) {
    SnEvalValue out_native;
    if (sn_native_try_dispatch(in, fn, args, caller, self, span, &out_native)) {
        return out_native;
    }

    if (!fn->body) {
        sn_rt_error(in, SNOVA_UNKNOWN_INTRINSIC, span,
                 "`%s` has no body and its @native intrinsic is not implemented yet",
                 fn->name ? fn->name : "?");
        return v_unit();
    }

    SnEvalEnv *local = sn_eval_env_new(in, NULL); /* functions do not close over the caller */

    /* `this` and the receiver's fields are visible inside a method body. */
    if (self) {
        SnEvalValue tv;
        tv.kind = V_OBJECT;
        tv.as.o = self;
        sn_eval_env_define(in, local, "this", tv);
        for (size_t i = 0; i < self->names.len; i++) {
            sn_list_push(in->arena, &local->names, self->names.items[i]);
            sn_list_push(in->arena, &local->slots, self->slots.items[i]);
        }
    }

    for (size_t i = 0; i < fn->params.len; i++) {
        const SnParam *p = (const SnParam *)fn->params.items[i];
        SnEvalValue v;
        if (args && i < args->len) {
            v = sn_rt_eval_arg(in, caller, (const SnExpr *)args->items[i]);
        } else if (p->def) {
            v = sn_eval_expr(in, caller, p->def);
        } else {
            v = sn_eval_default_for(p->type);
        }
        sn_eval_env_define(in, local, p->name, v);
    }

    if (fn->is_async && in->async_depth == 0) {
        SnAsyncCall job;
        job.in = in;
        job.local = local;
        job.caller = caller;
        job.body = fn->body;
        job.result = v_unit();
        SnAsyncLoop loop;
        sn_async_loop_init(&loop);
        if (sn_async_spawn(&loop, sn_eval_async_step, &job)) {
            in->async_depth = 1;
            sn_async_loop_run(&loop);
            in->async_depth = 0;
            sn_async_loop_cleanup(&loop);
            return job.result;
        }
        sn_async_loop_cleanup(&loop);
    }

    return sn_rt_finish_call(in, local, caller, fn->body);
}

SnEvalValue sn_eval_call_method(SnEvalInterp *in, SnEvalObject *self, const SnDecl *m, SnList *args,
                  SnEvalEnv *caller, SnSpan span) {
    return sn_eval_call_function(in, m, args, caller, self, span);
}

SnEvalValue sn_eval_call_lambda(SnEvalInterp *in, const SnEvalLambda *lam, SnList *args, SnEvalEnv *caller,
                  SnSpan span) {
    (void)span;
    SnEvalEnv *local = sn_eval_env_new(in, lam->env); /* lambdas close over their env */
    for (size_t i = 0; i < lam->expr->params.len; i++) {
        const SnParam *p = (const SnParam *)lam->expr->params.items[i];
        SnEvalValue v;
        if (args && i < args->len) {
            v = sn_rt_eval_arg(in, caller, (const SnExpr *)args->items[i]);
        } else if (p->def) {
            v = sn_eval_expr(in, caller, p->def);
        } else {
            v = sn_eval_default_for(p->type);
        }
        sn_eval_env_define(in, local, p->name, v);
    }
    if (lam->expr->value) {
        return sn_eval_expr(in, local, lam->expr->value);
    }
    if (lam->expr->body) {
        return sn_rt_finish_call(in, local, caller, lam->expr->body);
    }
    return v_unit();
}

/* ── entry point ──────────────────────────────────────────────────────────── */

int sn_eval_run(SnArena *arena, SnDiagSink *diag, const SnUnit *unit) {
    SnEvalInterp in;
    in.arena = arena;
    in.diag = diag;
    in.unit = unit;
    in.globals = sn_eval_env_new(&in, NULL);
    in.ret = v_unit();
    in.flow = FLOW_NORMAL;
    in.failed = 0;
    in.async_depth = 0;
    in.mem = NULL;
    in.defers = NULL;
    sn_rt_mem_reset(&in);
    sn_rt_frame_push(&in);

    const SnDecl *main_fn = sn_eval_find_top(&in, "main", SN_DECL_FUNC);
    if (!main_fn) {
        SnSpan z = {0, 0, 1, 1};
        sn_diag_emit(diag, SN_DIAG_ERROR, SNOVA_NO_MAIN, z,
                     "no `func main()` found in this file");
        return -1;
    }

    SnEvalEnv *global = sn_eval_env_new(&in, NULL);
    SnList no_args = {0};
    SnEvalValue r = sn_eval_call_function(&in, main_fn, &no_args, global, NULL, main_fn->span);
    sn_rt_frame_pop(&in, NULL, r);

    if (in.failed) {
        return -1;
    }
    return (r.kind == V_INT) ? (int)r.as.i : 0;
}
