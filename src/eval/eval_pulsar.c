/* Pulsar jobs run with a private interpreter; only value captures cross the
 * worker boundary. The source AST and strings stay alive until pool_wait. */
#include "eval_internal.h"

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
    const SnDecl *fn;
    SnPulsarPool *pool;
    atomic_int *failed;
    SnPulsarCapture *captures;
    size_t capture_count;
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

/* Snapshot Send values before the launcher's frame can be released. */
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

static void pulsar_job_free(void *arg) {
    SnEvalPulsarJob *job = (SnEvalPulsarJob *)arg;
    for (size_t i = 0; i < job->capture_count; i++)
        release_capture(job->captures[i].value);
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
    /* Captures were collected from the inner scope outward. Defining them in
     * reverse order keeps the nearest shadowing binding visible. */
    for (size_t i = job->capture_count; i > 0; i--) {
        sn_eval_env_define(&in, local, job->captures[i - 1].name,
                           restore_value(&in, job->captures[i - 1].value));
    }
    SnEvalValue result = sn_rt_finish_call(&in, local, NULL, job->body);
    if (in.flow == FLOW_THROW && !in.failed) {
        sn_rt_error(&in, SNOVA_TYPE_ERROR,
                    job->fn ? job->fn->span : job->body->span,
                    "uncaught thrown value in pulsar task");
    }
    sn_rt_frame_pop(&in, NULL, result);
    if (in.failed) atomic_store(job->failed, 1);
    sn_arena_free(&arena);
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
        job->captures[i].name = p->name;
        job->capture_count++;
        if (!capture_value(value, &job->captures[i].value, 0)) {
            sn_rt_error(in, SNOVA_TYPE_ERROR, s->span,
                        "pulsar capture cannot cross the worker boundary");
            return 0;
        }
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
    job->fn = fn;
    job->pool = in->pulsar_pool;
    job->failed = in->pulsar_failed;
    size_t count = fn ? fn->params.len : (lambda ? lambda->params.len : 0);
    if (!fn && !lambda) {
        for (SnEvalEnv *e = env; e; e = e->parent) count += e->names.len;
    }
    if (count > SIZE_MAX / sizeof(*job->captures)) {
        pulsar_job_free(job);
        sn_rt_error(in, SNOVA_OUT_OF_MEMORY, s->span,
                    "pulsar capture list is too large");
        return 0;
    }
    job->captures = (SnPulsarCapture *)calloc(count ? count : 1,
                                               sizeof(*job->captures));
    if (!job->captures) {
        pulsar_job_free(job);
        sn_rt_error(in, SNOVA_OUT_OF_MEMORY, s->span,
                    "out of memory capturing pulsar values");
        return 0;
    }
    if (fn) {
        if (!capture_params(in, env, s, &fn->params, &call->args, job)) {
            /* capture_params already reported the error. */
        }
    } else if (lambda) {
        if (!capture_params(in, env, s, &lambda->params, &call->args, job)) {
            /* capture_params already reported the error. */
        }
    } else {
        for (SnEvalEnv *e = env; e; e = e->parent) {
            for (size_t i = e->names.len; i > 0; i--) {
                SnEvalValue *slot = (SnEvalValue *)e->slots.items[i - 1];
                job->captures[job->capture_count].name =
                    (const char *)e->names.items[i - 1];
                SnPulsarCapture *capture = &job->captures[job->capture_count];
                job->capture_count++;
                if (!capture_value(*slot, &capture->value, 0)) {
                    sn_rt_error(in, SNOVA_TYPE_ERROR, s->span,
                                "pulsar capture cannot cross the worker boundary");
                    break;
                }
            }
            if (in->failed) break;
        }
    }
    if (in->failed || in->flow == FLOW_THROW) {
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
