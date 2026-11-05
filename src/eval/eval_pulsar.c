/* Pulsar jobs run with a private interpreter; only value captures cross the
 * worker boundary. The source AST and strings stay alive until pool_wait. */
#include "eval_internal.h"

#include "lex.h"
#include "parse.h"

#define SN_PULSAR_NAME_MAX 64

static atomic_int g_eval_pulsar_jobs;

void sn_eval_pulsar_jobs_reset(void) {
    atomic_store(&g_eval_pulsar_jobs, 0);
}

int sn_eval_pulsar_jobs(void) {
    return atomic_load(&g_eval_pulsar_jobs);
}

typedef struct {
    const char *name;
    SnEvalValue value;
} SnPulsarCapture;

typedef struct {
    const SnUnit *unit;
    SnDiagFile source;
    const SnStmt *body;
    const SnExpr *expr_body;
    const SnDecl *fn;
    SnPulsarPool *pool;
    atomic_int *failed;
    SnPulsarCapture *captures;
    size_t capture_count;
    size_t capture_cap;
} SnEvalPulsarJob;

static void release_capture(SnEvalValue v) {
    if (v.kind == V_STRING) {
        free((void *)v.as.s);
    } else if (v.kind == V_ARRAY && v.as.arr) {
        for (size_t i = 0; i < v.as.arr->items.len; i++) {
            SnEvalValue *item = (SnEvalValue *)v.as.arr->items.items[i];
            if (item) { release_capture(*item); free(item); }
        }
        free(v.as.arr->items.items);
        free(v.as.arr);
    }
}

static int capture_value(SnEvalValue src, SnEvalValue *dst, int depth) {
    *dst = v_unit();
    if (depth > 32) return 0;
    if (src.kind == V_STRING) {
        const char *s = src.as.s ? src.as.s : "";
        size_t n = strlen(s);
        char *copy = (char *)malloc(n + 1);
        if (!copy) return 0;
        memcpy(copy, s, n + 1);
        *dst = src;
        dst->as.s = copy;
        return 1;
    }
    if (src.kind == V_ARRAY && src.as.arr) {
        ArrayVal *copy = (ArrayVal *)calloc(1, sizeof(*copy));
        if (!copy) return 0;
        *dst = src;
        dst->as.arr = copy;
        size_t n = src.as.arr->items.len;
        if (n > SIZE_MAX / sizeof(*copy->items.items)) return 0;
        copy->items.items = (void **)calloc(n ? n : 1, sizeof(void *));
        if (!copy->items.items) return 0;
        for (size_t i = 0; i < n; i++) {
            SnEvalValue *item = (SnEvalValue *)malloc(sizeof(*item));
            if (!item) return 0;
            copy->items.items[copy->items.len++] = item;
            if (!capture_value(*(SnEvalValue *)src.as.arr->items.items[i],
                               item, depth + 1)) return 0;
        }
        return 1;
    }
    if (src.kind == V_UNIT || src.kind == V_INT || src.kind == V_DOUBLE ||
        src.kind == V_BOOL || src.kind == V_CHAR) {
        *dst = src;
        return 1;
    }
    return 0;
}

static SnEvalValue restore_value(SnEvalInterp *in, SnEvalValue src) {
    if (src.kind != V_ARRAY || !src.as.arr) return src;
    SnEvalValue dst = src;
    dst.as.arr = (ArrayVal *)sn_rt_alloc(in, sizeof(ArrayVal), 0);
    memset(dst.as.arr, 0, sizeof(ArrayVal));
    for (size_t i = 0; i < src.as.arr->items.len; i++) {
        SnEvalValue *item = (SnEvalValue *)src.as.arr->items.items[i];
        sn_rt_array_push(in, dst.as.arr, restore_value(in, *item));
    }
    return dst;
}

static int name_has(const char **names, size_t n, const char *name) {
    if (!name) return 1;
    for (size_t i = 0; i < n; i++) {
        if (names[i] && strcmp(names[i], name) == 0) return 1;
    }
    return 0;
}

static int name_add(const char **names, size_t *n, const char *name) {
    if (!name || name_has(names, *n, name)) return 1;
    if (*n >= SN_PULSAR_NAME_MAX) return 0;
    names[(*n)++] = name;
    return 1;
}

static int collect_stmt(const SnStmt *s, const char **bound, size_t nbound,
                        const char **free_names, size_t *nfree, SnArena *persist);

static int collect_pattern(const SnPattern *p, const char **bound, size_t *nbound) {
    if (!p) return 1;
    if (p->kind == SN_PAT_BINDING && p->name) {
        return name_add(bound, nbound, p->name);
    }
    for (size_t i = 0; i < p->subs.len; i++) {
        if (!collect_pattern((const SnPattern *)p->subs.items[i], bound, nbound))
            return 0;
    }
    return 1;
}

static int collect_expr(const SnExpr *e, const char **bound, size_t nbound,
                        const char **free_names, size_t *nfree, SnArena *persist) {
    if (!e) return 1;
    if (e->kind == SN_EXPR_IDENT) {
        return name_has(bound, nbound, e->text)
            ? 1 : name_add(free_names, nfree, e->text);
    }
    if (e->kind == SN_EXPR_THIS) {
        return name_has(bound, nbound, "this")
            ? 1 : name_add(free_names, nfree, "this");
    }
    if (e->kind == SN_EXPR_STRING && e->interpolated && e->text) {
        const char *raw = e->text;
        size_t n = strlen(raw);
        if (n >= 2 && raw[0] == '"') {
            raw++;
            n -= 2;
        }
        for (size_t i = 0; i < n; i++) {
            if (raw[i] == '\\' && i + 1 < n) {
                i++;
                continue;
            }
            if (raw[i] == '$' && i + 1 < n && raw[i + 1] == '$') {
                i++;
                continue;
            }
            if (raw[i] == '$' && i + 1 < n && raw[i + 1] == '{') {
                size_t start = i + 2;
                int depth = 1;
                size_t j = start;
                for (; j < n && depth > 0; j++) {
                    if (raw[j] == '{') depth++;
                    else if (raw[j] == '}') depth--;
                    if (depth == 0) break;
                }
                char *buf = (char *)malloc(j - start + 1);
                if (!buf) return 0;
                memcpy(buf, raw + start, j - start);
                buf[j - start] = '\0';
                SnArena arena;
                sn_arena_init(&arena, 0);
                SnDiagSink diag;
                sn_diag_init(&diag, "", buf, j - start);
                SnTokenVec toks;
                int ok = 1;
                const char *saved_names[SN_PULSAR_NAME_MAX];
                size_t nsaved = 0;
                if (sn_lex(&arena, &diag, buf, j - start, &toks) == 0) {
                    SnExpr *interp = sn_parse_expr_only(&arena, &diag, &toks);
                    if (interp && !collect_expr(interp, bound, nbound, saved_names, &nsaved, &arena))
                        ok = 0;
                }
                if (ok) {
                    for (size_t k = 0; k < nsaved; k++) {
                        const char *copy = sn_arena_strndup(persist, saved_names[k], strlen(saved_names[k]));
                        if (!copy || !name_add(free_names, nfree, copy)) { ok = 0; break; }
                    }
                }
                sn_arena_free(&arena);
                free(buf);
                if (!ok) return 0;
                i = j;
            }
        }
    }
    if (e->kind == SN_EXPR_LAMBDA) {
        const char *inner[SN_PULSAR_NAME_MAX];
        size_t ni = nbound > SN_PULSAR_NAME_MAX ? SN_PULSAR_NAME_MAX : nbound;
        memcpy(inner, bound, ni * sizeof(*inner));
        for (size_t i = 0; i < e->params.len; i++) {
            const SnParam *p = (const SnParam *)e->params.items[i];
            if (p) {
                if (p->def && !collect_expr(p->def, bound, nbound, free_names, nfree, persist))
                    return 0;
                if (!name_add(inner, &ni, p->name)) return 0;
            }
        }
        return collect_expr(e->value, inner, ni, free_names, nfree, persist) &&
               collect_stmt(e->body, inner, ni, free_names, nfree, persist);
    }
    if (!collect_expr(e->lhs, bound, nbound, free_names, nfree, persist) ||
        !collect_expr(e->rhs, bound, nbound, free_names, nfree, persist) ||
        !collect_expr(e->value, bound, nbound, free_names, nfree, persist) ||
        !collect_stmt(e->body, bound, nbound, free_names, nfree, persist) ||
        !collect_stmt(e->else_body, bound, nbound, free_names, nfree, persist)) {
        return 0;
    }
    for (size_t i = 0; i < e->args.len; i++) {
        if (!collect_expr((const SnExpr *)e->args.items[i], bound, nbound,
                          free_names, nfree, persist)) return 0;
    }
    for (size_t i = 0; i < e->arms.len; i++) {
        const SnMatchArm *arm = (const SnMatchArm *)e->arms.items[i];
        if (!arm) continue;
        const char *arm_bound[SN_PULSAR_NAME_MAX];
        size_t narm = nbound;
        memcpy(arm_bound, bound, nbound * sizeof(*bound));
        if (!collect_pattern(arm->pattern, arm_bound, &narm) ||
            !collect_expr(arm->guard, arm_bound, narm, free_names, nfree, persist) ||
            !collect_expr(arm->value, arm_bound, narm, free_names, nfree, persist) ||
            !collect_stmt(arm->body, arm_bound, narm, free_names, nfree, persist)) return 0;
    }
    return 1;
}

static int collect_stmt(const SnStmt *s, const char **bound, size_t nbound,
                        const char **free_names, size_t *nfree, SnArena *persist) {
    if (!s) return 1;
    const char *inner[SN_PULSAR_NAME_MAX];
    size_t ni = nbound > SN_PULSAR_NAME_MAX ? SN_PULSAR_NAME_MAX : nbound;
    memcpy(inner, bound, ni * sizeof(*inner));
    if (!collect_expr(s->expr, inner, ni, free_names, nfree, persist)) return 0;
    if ((s->kind == SN_STMT_LET || s->kind == SN_STMT_VAR ||
         s->kind == SN_STMT_FOR) && s->name) {
        if (!name_add(inner, &ni, s->name)) return 0;
    }
    for (size_t i = 0; i < s->extra_names.len; i++) {
        if (!name_add(inner, &ni, (const char *)s->extra_names.items[i])) return 0;
    }
    if (!collect_stmt(s->then_br, inner, ni, free_names, nfree, persist) ||
        !collect_stmt(s->else_br, inner, ni, free_names, nfree, persist) ||
        !collect_stmt(s->finally_br, inner, ni, free_names, nfree, persist)) return 0;
    for (size_t i = 0; i < s->stmts.len; i++) {
        const SnStmt *child = (const SnStmt *)s->stmts.items[i];
        if (!collect_stmt(child, inner, ni, free_names, nfree, persist)) return 0;
        if (child && (child->kind == SN_STMT_LET || child->kind == SN_STMT_VAR) &&
            child->name && !name_add(inner, &ni, child->name)) return 0;
    }
    for (size_t i = 0; i < s->arms.len; i++) {
        const SnMatchArm *arm = (const SnMatchArm *)s->arms.items[i];
        if (!arm) continue;
        const char *arm_bound[SN_PULSAR_NAME_MAX];
        size_t narm = ni;
        memcpy(arm_bound, inner, ni * sizeof(*inner));
        if (!collect_pattern(arm->pattern, arm_bound, &narm) ||
            !collect_expr(arm->guard, arm_bound, narm, free_names, nfree, persist) ||
            !collect_expr(arm->value, arm_bound, narm, free_names, nfree, persist) ||
            !collect_stmt(arm->body, arm_bound, narm, free_names, nfree, persist)) return 0;
    }
    for (size_t i = 0; i < s->catches.len; i++) {
        const SnStmt *handler = (const SnStmt *)s->catches.items[i];
        const char *catch_bound[SN_PULSAR_NAME_MAX];
        size_t ncatch = ni;
        memcpy(catch_bound, inner, ni * sizeof(*inner));
        if (handler && handler->name && !name_add(catch_bound, &ncatch, handler->name))
            return 0;
        if (!collect_stmt(handler, catch_bound, ncatch, free_names, nfree, persist)) return 0;
    }
    return 1;
}

static void pulsar_job_free(void *arg) {
    SnEvalPulsarJob *job = (SnEvalPulsarJob *)arg;
    for (size_t i = 0; i < job->capture_count; i++) {
        free((void *)job->captures[i].name);
        release_capture(job->captures[i].value);
    }
    free(job->captures);
    free(job);
}

static void pulsar_job_run(void *arg) {
    SnEvalPulsarJob *job = (SnEvalPulsarJob *)arg;
    SnArena arena;
    sn_arena_init(&arena, 0);
    SnDiagSink diag;
    sn_diag_init(&diag, job->source.path, job->source.src, job->source.src_len);
    SnEvalInterp in = {0};
    in.arena = &arena;
    in.diag = &diag;
    in.unit = job->unit;
    in.ret = v_unit();
    in.thrown = v_unit();
    in.pulsar_pool = job->pool;
    in.pulsar_failed = job->failed;
    sn_rt_mem_reset(&in);
    sn_rt_frame_push(&in);
    in.globals = sn_eval_env_new(&in, NULL);
    SnEvalEnv *local = sn_eval_env_new(&in, NULL);
    for (size_t i = job->capture_count; i > 0; i--) {
        sn_eval_env_define(&in, local, job->captures[i - 1].name,
                           restore_value(&in, job->captures[i - 1].value));
    }
    SnEvalValue result = v_unit();
    if (job->body) {
        result = sn_rt_finish_call(&in, local, NULL, job->body);
    } else if (job->expr_body) {
        result = sn_eval_expr(&in, local, job->expr_body);
    }
    if (in.flow == FLOW_THROW && !in.failed) {
        SnSpan span = job->fn ? job->fn->span
                              : (job->body ? job->body->span : job->expr_body->span);
        sn_rt_error(&in, SNOVA_TYPE_ERROR, span,
                    "uncaught thrown value in pulsar task");
    }
    sn_rt_frame_pop(&in, NULL, result);
    if (in.failed) atomic_store(job->failed, 1);
    sn_arena_free(&arena);
}

static int job_has_name(const SnEvalPulsarJob *job, const char *name) {
    for (size_t i = 0; i < job->capture_count; i++) {
        if (job->captures[i].name && name &&
            strcmp(job->captures[i].name, name) == 0) return 1;
    }
    return 0;
}

static int capture_one(SnEvalInterp *in, const SnStmt *s, SnEvalPulsarJob *job,
                       const char *name, SnEvalValue value) {
    if (job_has_name(job, name)) return 1;
    if (job->capture_count >= job->capture_cap) return 0;
    size_t slot = job->capture_count;
    size_t name_len = name ? strlen(name) : 0;
    char *name_copy = (char *)malloc(name_len + 1);
    if (!name_copy) {
        sn_rt_error(in, SNOVA_OUT_OF_MEMORY, s->span,
                    "out of memory capturing pulsar name");
        return 0;
    }
    if (name) memcpy(name_copy, name, name_len + 1);
    else name_copy[0] = '\0';
    job->captures[slot].name = name_copy;
    job->capture_count++;
    if (!capture_value(value, &job->captures[slot].value, 0)) {
        sn_rt_error(in, SNOVA_TYPE_ERROR, s->span,
                    "pulsar capture cannot cross the worker boundary");
        return 0;
    }
    return 1;
}

static int capture_params(SnEvalInterp *in, SnEvalEnv *env, const SnStmt *s,
                          const SnList *params, const SnList *args,
                          SnEvalPulsarJob *job) {
    for (size_t i = 0; i < params->len; i++) {
        const SnParam *p = (const SnParam *)params->items[i];
        SnEvalValue value = (args && i < args->len)
            ? sn_rt_eval_arg(in, env, (const SnExpr *)args->items[i])
            : p->def ? sn_eval_expr(in, env, p->def)
                     : sn_eval_default_for(p->type);
        if (in->failed || in->flow == FLOW_THROW) return 0;
        if (!capture_one(in, s, job, p->name, value)) return 0;
    }
    return 1;
}

static int capture_used_env(SnEvalInterp *in, SnEvalEnv *env, const SnStmt *s,
                            const SnStmt *body, const SnExpr *expr,
                            const SnList *bound_params, SnEvalPulsarJob *job) {
    const char *bound[SN_PULSAR_NAME_MAX];
    size_t nbound = 0;
    const char *free_names[SN_PULSAR_NAME_MAX];
    size_t nfree = 0;
    if (bound_params) {
        for (size_t i = 0; i < bound_params->len; i++) {
            const SnParam *p = (const SnParam *)bound_params->items[i];
            if (p && !name_add(bound, &nbound, p->name)) return 0;
        }
    }
    if (!collect_stmt(body, bound, nbound, free_names, &nfree, in->arena) ||
        !collect_expr(expr, bound, nbound, free_names, &nfree, in->arena)) {
        sn_rt_error(in, SNOVA_OUT_OF_MEMORY, s->span,
                    "pulsar capture list is too large");
        return 0;
    }
    for (size_t i = 0; i < nfree; i++) {
        SnEvalValue *slot = sn_eval_env_lookup(env, free_names[i]);
        if (!slot) continue;
        if (!capture_one(in, s, job, free_names[i], *slot)) return 0;
    }
    return 1;
}

int sn_eval_pulsar_submit(SnEvalInterp *in, SnEvalEnv *env, const SnStmt *s) {
    if (!in->pulsar_pool) {
        sn_rt_error(in, SNOVA_UNSUPPORTED, s->span,
                    "Pulsar worker pool is not running");
        return 0;
    }
    const SnExpr *call = s->expr;
    const SnDecl *fn = NULL;
    const SnExpr *lambda = NULL;
    if (call) {
        if (call->kind != SN_EXPR_CALL || !call->lhs) {
            sn_rt_error(in, SNOVA_UNSUPPORTED, s->span,
                        "pulsar launch requires a function call");
            return 0;
        }
        if (call->lhs->kind == SN_EXPR_IDENT && call->lhs->text) {
            fn = sn_eval_find_top(in, call->lhs->text, SN_DECL_FUNC);
            if (!fn || !fn->body) {
                sn_rt_error(in, SNOVA_UNDEFINED_NAME, s->span,
                            "unknown pulsar function `%s`", call->lhs->text);
                return 0;
            }
        } else if (call->lhs->kind == SN_EXPR_LAMBDA && call->lhs->body) {
            lambda = call->lhs;
        } else {
            sn_rt_error(in, SNOVA_UNSUPPORTED, s->span,
                        "pulsar launch requires a named or immediately-called function");
            return 0;
        }
    } else if (!s->then_br) {
        sn_rt_error(in, SNOVA_UNSUPPORTED, s->span,
                    "pulsar launch requires a function body");
        return 0;
    }
    SnEvalPulsarJob *job = (SnEvalPulsarJob *)calloc(1, sizeof(*job));
    if (!job) {
        sn_rt_error(in, SNOVA_OUT_OF_MEMORY, s->span,
                    "out of memory scheduling pulsar task");
        return 0;
    }
    job->unit = in->unit;
    job->source = in->diag->file;
    job->body = fn ? fn->body : (lambda ? lambda->body : s->then_br);
    job->expr_body = lambda ? lambda->value : NULL;
    job->fn = fn;
    job->pool = in->pulsar_pool;
    job->failed = in->pulsar_failed;
    job->capture_cap = SN_PULSAR_NAME_MAX;
    job->captures = (SnPulsarCapture *)calloc(job->capture_cap,
                                              sizeof(*job->captures));
    if (!job->captures) {
        pulsar_job_free(job);
        sn_rt_error(in, SNOVA_OUT_OF_MEMORY, s->span,
                    "out of memory capturing pulsar values");
        return 0;
    }
    int ok = 1;
    if (fn) {
        ok = capture_params(in, env, s, &fn->params, &call->args, job);
    } else if (lambda) {
        ok = capture_params(in, env, s, &lambda->params, &call->args, job) &&
             capture_used_env(in, env, s, lambda->body, lambda->value,
                              &lambda->params, job);
    } else {
        ok = capture_used_env(in, env, s, s->then_br, NULL, NULL, job);
    }
    if (!ok || in->failed || in->flow == FLOW_THROW) {
        pulsar_job_free(job);
        return 0;
    }
    if (!sn_pulsar_pool_submit_owned(in->pulsar_pool, pulsar_job_run,
                                     job, pulsar_job_free)) {
        sn_rt_error(in, SNOVA_UNSUPPORTED, s->span,
                    "Pulsar worker queue is unavailable or full");
        return 0;
    }
    atomic_fetch_add(&g_eval_pulsar_jobs, 1);
    return 1;
}
