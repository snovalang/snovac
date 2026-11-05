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
