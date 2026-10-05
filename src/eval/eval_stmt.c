/* eval_stmt.c — statement execution and control flow. */
#include "eval_internal.h"
#include "pulsar.h"

typedef struct {
    SnEvalInterp *in;
    SnEvalEnv *env;
    const SnExpr *expr;
} SnPulsarJob;

static void sn_eval_pulsar_job(void *arg) {
    SnPulsarJob *job = (SnPulsarJob *)arg;
    if (job->expr) {
        sn_eval_expr(job->in, job->env, job->expr);
    }
}

/* A runaway loop in a smoke-path interpreter should report, not hang. */
#define SN_LOOP_GUARD 100000000

static SnEvalFlow exec_block(SnEvalInterp *in, SnEvalEnv *env, const SnStmt *s) {
    SnEvalEnv *inner = sn_eval_env_new(in, env);
    for (size_t i = 0; i < s->stmts.len; i++) {
        SnEvalFlow f = sn_eval_exec_stmt(in, inner, (const SnStmt *)s->stmts.items[i]);
        if (f != FLOW_NORMAL) {
            return f;
        }
    }
    return FLOW_NORMAL;
}

static SnEvalFlow exec_while(SnEvalInterp *in, SnEvalEnv *env, const SnStmt *s) {
    int guard = 0;
    while (!in->failed && in->flow != FLOW_THROW &&
           sn_eval_truthy(in, sn_eval_expr(in, env, s->expr), s->span)) {
        sn_rt_frame_push(in);
        SnEvalFlow f = sn_eval_exec_stmt(in, env, s->then_br);
        sn_rt_frame_pop(in, env, in->ret);
        if (f == FLOW_RETURN || f == FLOW_THROW) {
            return f;
        }
        if (f == FLOW_BREAK) {
            break;
        }
        if (++guard > SN_LOOP_GUARD) {
            sn_rt_error(in, SNOVA_UNSUPPORTED, s->span,
                     "loop exceeded iteration guard");
            break;
        }
    }
    return FLOW_NORMAL;
}

/* Bare variant name of a possibly qualified pattern path: `Option.Some` and
 * `Some` both match a variant constructed as `Some`. */
static const char *pattern_variant_name(const char *path) {
    const char *dot = strrchr(path, '.');
    return dot ? dot + 1 : path;
}

int sn_eval_pattern_match(SnEvalInterp *in, SnEvalEnv *env, const SnPattern *pat,
                       SnEvalValue subject) {
    switch (pat->kind) {
    case SN_PAT_WILDCARD:
        return 1;
    case SN_PAT_BINDING: {
        const char *vname = pattern_variant_name(pat->name);
        if (sn_eval_is_variant_ctor(in, vname)) {
            return subject.kind == V_VARIANT &&
                  subject.as.vt->payload.len == 0 &&
                  strcmp(subject.as.vt->name, vname) == 0;
        }
        sn_eval_env_define(in, env, pat->name, subject);
        return 1;
    }
    case SN_PAT_LITERAL: {
        SnEvalValue lit = sn_eval_expr(in, env, pat->literal);
        return !in->failed && sn_eval_value_equals(lit, subject);
    }
    case SN_PAT_VARIANT: {
        if (subject.kind != V_VARIANT) {
            return 0;
        }
        if (strcmp(pattern_variant_name(pat->name), subject.as.vt->name) != 0) {
            return 0;
        }
        if (pat->subs.len > subject.as.vt->payload.len) {
            return 0;
        }
        for (size_t i = 0; i < pat->subs.len; i++) {
            const SnEvalValue *payload = (const SnEvalValue *)subject.as.vt->payload.items[i];
            if (!sn_eval_pattern_match(in, env, (const SnPattern *)pat->subs.items[i],
                                    *payload)) {
                return 0;
            }
        }
        return 1;
    }
    }
    return 0;
}

const SnMatchArm *sn_eval_match_arm(SnEvalInterp *in, SnEvalEnv *env, const SnList *arms,
                                   SnEvalValue subject, SnEvalEnv **arm_env) {
    for (size_t i = 0; i < arms->len && !in->failed; i++) {
        const SnMatchArm *arm = (const SnMatchArm *)arms->items[i];
        SnEvalEnv *candidate = sn_eval_env_new(in, env);
        if (!sn_eval_pattern_match(in, candidate, arm->pattern, subject)) {
            continue;
        }
        if (arm->guard &&
            !sn_eval_truthy(in, sn_eval_expr(in, candidate, arm->guard), arm->span)) {
            continue;
        }
        *arm_env = candidate;
        return arm;
    }
    *arm_env = NULL;
    return NULL;
}

static SnEvalFlow exec_match(SnEvalInterp *in, SnEvalEnv *env, const SnStmt *s) {
    SnEvalValue subject = sn_eval_expr(in, env, s->expr);
    if (in->failed) {
        return FLOW_RETURN;
    }
    SnEvalEnv *arm_env = NULL;
    const SnMatchArm *arm = sn_eval_match_arm(in, env, &s->arms, subject, &arm_env);
    if (!arm) {
        /* A statement-position match with no matching arm is a no-op, mirroring
         * an `if` chain whose conditions are all false. */
        return in->failed ? FLOW_RETURN : FLOW_NORMAL;
    }
    if (arm->body) {
        return sn_eval_exec_stmt(in, arm_env, arm->body);
    }
    if (arm->value) {
        sn_eval_expr(in, arm_env, arm->value);
        return in->failed ? FLOW_RETURN : FLOW_NORMAL;
    }
    return FLOW_NORMAL;
}

static SnEvalFlow exec_for(SnEvalInterp *in, SnEvalEnv *env, const SnStmt *s) {
    if (!s->expr) return FLOW_NORMAL;
    SnEvalValue iterable = sn_eval_expr(in, env, s->expr);
    if (in->failed) return FLOW_RETURN;

    if (iterable.kind == V_INT) {
        long long limit = iterable.as.i;
        int guard = 0;
        for (long long i = 0; i < limit && !in->failed; i++) {
            SnEvalEnv *loop_env = sn_eval_env_new(in, env);
            if (s->name) sn_eval_env_define(in, loop_env, s->name, v_int(i));
            sn_rt_frame_push(in);
            SnEvalFlow f = sn_eval_exec_stmt(in, loop_env, s->then_br);
            sn_rt_frame_pop(in, env, in->ret);
            if (f == FLOW_RETURN || f == FLOW_THROW) return f;
            if (f == FLOW_BREAK) break;
            if (++guard > SN_LOOP_GUARD) {
                sn_rt_error(in, SNOVA_UNSUPPORTED, s->span, "loop exceeded iteration guard");
                break;
            }
        }
    } else if (iterable.kind == V_ARRAY) {
        int guard = 0;
        for (size_t i = 0; i < iterable.as.arr->items.len && !in->failed; i++) {
            SnEvalValue *item = (SnEvalValue *)iterable.as.arr->items.items[i];
            SnEvalEnv *loop_env = sn_eval_env_new(in, env);
            if (s->name) sn_eval_env_define(in, loop_env, s->name, *item);
            sn_rt_frame_push(in);
            SnEvalFlow f = sn_eval_exec_stmt(in, loop_env, s->then_br);
            sn_rt_frame_pop(in, env, in->ret);
            if (f == FLOW_RETURN || f == FLOW_THROW) return f;
            if (f == FLOW_BREAK) break;
            if (++guard > SN_LOOP_GUARD) {
                sn_rt_error(in, SNOVA_UNSUPPORTED, s->span, "loop exceeded iteration guard");
                break;
            }
        }
    }
    return FLOW_NORMAL;
}

SnEvalFlow sn_eval_exec_stmt(SnEvalInterp *in, SnEvalEnv *env, const SnStmt *s) {
    if (!s || in->failed) {
        return in->failed ? FLOW_RETURN : FLOW_NORMAL;
    }

    switch (s->kind) {
    case SN_STMT_BLOCK:
        return exec_block(in, env, s);

    case SN_STMT_LET:
    case SN_STMT_VAR: {
        SnEvalValue v = s->expr ? sn_eval_expr(in, env, s->expr) : sn_eval_default_for(s->type);
        if (v.kind == V_INT && s->type && s->type->name) {
            const char *n = s->type->name;
            if (strcmp(n, "int8") == 0) v.iwidth = 8;
            else if (strcmp(n, "int16") == 0) v.iwidth = 16;
            else if (strcmp(n, "int32") == 0) v.iwidth = 32;
            else if (strcmp(n, "byte") == 0) v.iwidth = 8;
            else if (strcmp(n, "int128") == 0) v.iwidth = 128;
            else if (strcmp(n, "int") == 0 || strcmp(n, "int64") == 0 ||
                     strcmp(n, "long") == 0) v.iwidth = 64;
        }
        sn_eval_env_define(in, env, s->name, v);
        return FLOW_NORMAL;
    }
    case SN_STMT_EXPR:
        sn_eval_expr(in, env, s->expr);
        if (in->flow == FLOW_THROW) {
            return FLOW_THROW;
        }
        return in->failed ? FLOW_RETURN : FLOW_NORMAL;

    case SN_STMT_RETURN:
        in->ret = s->expr ? sn_eval_expr(in, env, s->expr) : v_unit();
        return FLOW_RETURN;

    case SN_STMT_IF:
        if (sn_eval_truthy(in, sn_eval_expr(in, env, s->expr), s->span)) {
            return sn_eval_exec_stmt(in, env, s->then_br);
        }
        return s->else_br ? sn_eval_exec_stmt(in, env, s->else_br) : FLOW_NORMAL;

    case SN_STMT_WHILE:
        return exec_while(in, env, s);

    case SN_STMT_FOR:
        return exec_for(in, env, s);

    case SN_STMT_MATCH:
        return exec_match(in, env, s);

    case SN_STMT_BREAK:    return FLOW_BREAK;
    case SN_STMT_CONTINUE: return FLOW_CONTINUE;

    case SN_STMT_PULSAR: {
        SnPulsarPool *pool = sn_pulsar_pool_create(1);
        SnPulsarJob job;
        job.in = in;
        job.env = env;
        job.expr = s->expr;
        if (pool && sn_pulsar_pool_submit(pool, sn_eval_pulsar_job, &job)) {
            sn_pulsar_pool_wait(pool);
            sn_pulsar_pool_destroy(pool);
        } else {
            if (pool) {
                sn_pulsar_pool_destroy(pool);
            }
            if (s->expr) {
                sn_eval_expr(in, env, s->expr);
            }
        }
        return in->flow == FLOW_THROW ? FLOW_THROW
             : in->failed ? FLOW_RETURN : FLOW_NORMAL;
    }

    case SN_STMT_DEFER:
        sn_rt_defer_schedule(in, env, s->expr);
        return in->flow == FLOW_THROW ? FLOW_THROW : FLOW_NORMAL;

    case SN_STMT_THROW:
        in->flow = FLOW_THROW;
        return FLOW_THROW;

    case SN_STMT_TRY: {
        SnEvalFlow f = s->then_br ? sn_eval_exec_stmt(in, env, s->then_br) : FLOW_NORMAL;
        if (f == FLOW_THROW) {
            in->flow = FLOW_NORMAL;
            f = FLOW_NORMAL;
            if (s->catches.len > 0) {
                f = sn_eval_exec_stmt(in, env, (const SnStmt *)s->catches.items[0]);
            }
        }
        if (s->finally_br) {
            SnEvalFlow ff = sn_eval_exec_stmt(in, env, s->finally_br);
            if (ff == FLOW_RETURN || ff == FLOW_THROW) {
                f = ff;
            }
        }
        return f;
    }

    default:
        sn_rt_error(in, SNOVA_UNSUPPORTED, s->span,
                 "this statement form is not executable yet");
        return FLOW_RETURN;
    }
}
