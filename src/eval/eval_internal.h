/* eval_internal.h — shared state for the tree-walking interpreter.
 *
 * Split by concern to keep every translation unit small:
 *   eval.c         entry point, environments, object model, calls
 *   eval_expr.c    expression evaluation
 *   eval_stmt.c    statement execution
 *   eval_string.c  literal decoding and `${...}` interpolation
 *
 * This interpreter is the execution path for `snl run`. `snl build` emits
 * bytecode and compiles a generated C runner; there is no in-process VM.
 */
#ifndef SNOVAC_EVAL_INTERNAL_H
#define SNOVAC_EVAL_INTERNAL_H

#include "eval.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Runtime band: 0300-0399 */
#define SNOVA_NO_MAIN            300
#define SNOVA_UNDEFINED_NAME     301
#define SNOVA_NOT_CALLABLE       302
#define SNOVA_UNKNOWN_INTRINSIC  303
#define SNOVA_TYPE_ERROR         304
#define SNOVA_UNSUPPORTED        305

typedef enum {
    V_UNIT, V_INT, V_DOUBLE, V_BOOL, V_STRING, V_CHAR, V_OBJECT, V_VARIANT,
    V_LAMBDA, V_ARRAY, V_REF
} ValKind;

typedef struct SnEvalObject SnEvalObject;
typedef struct VariantVal VariantVal;
typedef struct SnEvalLambda SnEvalLambda;
typedef struct ArrayVal ArrayVal;
typedef struct RefVal RefVal;

struct ArrayVal {
    SnList items; /* SnEvalValue* */
};

typedef struct {
    ValKind kind;
    /* Bit width of a V_INT. 64 for an unspecialized `v_int`. 0 on every
     * other kind. Annotated `let`/`var` bindings stamp the declared width. */
    unsigned char iwidth;
    union {
        long long i;
        double d;
        int b;
        const char *s;
        SnEvalObject *o;
        VariantVal *vt;
        SnEvalLambda *lam;
        ArrayVal *arr;
        RefVal *ref;
    } as;
} SnEvalValue;

/* Fat when `fat` is set: `len` is the array length carried with the address. */
struct RefVal {
    SnEvalValue *slot;
    size_t len;
    int is_null;
    int fat;
};

struct SnEvalObject {
    const SnDecl *cls;
    SnList names;  /* const char* */
    SnList slots;  /* SnEvalValue* */
};

/* A constructed enum variant — `Some(x)`, `Ok(v)`, `Err(e)`, `None`, or a
 * variant of a user-declared enum. `name` is the bare variant name; `payload`
 * holds the constructor arguments in order (SnEvalValue*). */
struct VariantVal {
    const char *name;
    SnList payload; /* SnEvalValue* */
};

typedef enum {
    FLOW_NORMAL, FLOW_RETURN, FLOW_BREAK, FLOW_CONTINUE, FLOW_THROW
} SnEvalFlow;

typedef struct SnEvalMemFrame SnEvalMemFrame;

typedef struct SnEvalEnv {
    struct SnEvalEnv *parent;
    SnList names; /* const char* */
    SnList slots; /* SnEvalValue* — boxed so assignment is visible to inner scopes */
} SnEvalEnv;

typedef struct SnEvalDefer {
    const SnExpr *call;
    SnEvalEnv *env;
    SnEvalValue *args;
    size_t nargs;
    struct SnEvalDefer *next;
} SnEvalDefer;

/* A first-class `(x) -> ...` value: the lambda expression plus the environment
 * it closed over. */
struct SnEvalLambda {
    const SnExpr *expr; /* SN_EXPR_LAMBDA node (params, value/body) */
    SnEvalEnv *env;           /* captured lexical environment */
};

typedef struct {
    SnArena *arena;
    SnDiagSink *diag;
    const SnUnit *unit;
    SnEvalEnv *globals;
    SnEvalValue ret;
    SnEvalFlow flow;
    int failed;
    int async_depth;
    SnEvalMemFrame *mem;
    SnEvalDefer *defers;
} SnEvalInterp;

/* ── value constructors ───────────────────────────────────────────────────── */

static inline SnEvalValue v_unit(void)         { SnEvalValue v; v.kind = V_UNIT; v.iwidth = 0; v.as.i = 0; return v; }
static inline SnEvalValue v_int(long long i)   { SnEvalValue v; v.kind = V_INT; v.iwidth = 64; v.as.i = i; return v; }
static inline SnEvalValue v_int_w(long long i, unsigned char w) {
    SnEvalValue v = v_int(i);
    v.iwidth = w ? w : 64;
    return v;
}
static inline SnEvalValue v_char(long long code) { SnEvalValue v; v.kind = V_CHAR; v.iwidth = 0; v.as.i = code; return v; }
static inline SnEvalValue v_double(double d)   { SnEvalValue v; v.kind = V_DOUBLE; v.iwidth = 0; v.as.d = d; return v; }
static inline SnEvalValue v_bool(int b)        { SnEvalValue v; v.kind = V_BOOL; v.iwidth = 0; v.as.b = b != 0; return v; }
static inline SnEvalValue v_str(const char *s) { SnEvalValue v; v.kind = V_STRING; v.iwidth = 0; v.as.s = s; return v; }

/* ── eval.c ───────────────────────────────────────────────────────────────── */

void sn_rt_error(SnEvalInterp *in, int code, SnSpan span, const char *fmt, ...);

SnEvalEnv *sn_eval_env_new(SnEvalInterp *in, SnEvalEnv *parent);
SnEvalValue *sn_eval_env_lookup(SnEvalEnv *e, const char *name);
SnEvalValue *sn_eval_env_define(SnEvalInterp *in, SnEvalEnv *e, const char *name, SnEvalValue v);

char *sn_eval_sprintf(SnEvalInterp *in, const char *fmt, ...);
char *sn_eval_str_concat(SnEvalInterp *in, const char *a, const char *b);
const char *sn_eval_to_string(SnEvalInterp *in, SnEvalValue v, SnSpan span);

const SnDecl *sn_eval_find_member(const SnDecl *cls, const char *name);
const SnDecl *sn_eval_find_member_inherited(const SnEvalInterp *in, const SnDecl *cls, const char *name);
/* Same name, distinct parameter types. Falls back to the first member when
 * the arguments do not distinguish a candidate. */
const SnDecl *sn_eval_find_overload(SnEvalInterp *in, SnEvalEnv *env, const SnDecl *cls, const char *name,
                            const SnList *args);
const SnDecl *sn_eval_find_top(const SnEvalInterp *in, const char *name, SnDeclKind k);
const SnDecl *sn_eval_find_type(const SnEvalInterp *in, const char *name);

SnEvalValue sn_eval_default_for(const SnType *t);
SnEvalObject *sn_eval_instantiate(SnEvalInterp *in, const SnDecl *cls, SnList *args, SnList *names,
                    SnEvalEnv *env, SnSpan span);
SnEvalValue *sn_eval_object_field(SnEvalObject *o, const char *name);

SnEvalValue sn_eval_call_function(SnEvalInterp *in, const SnDecl *fn, SnList *args, SnEvalEnv *caller,
                    SnEvalObject *self, SnSpan span);
SnEvalValue sn_eval_call_method(SnEvalInterp *in, SnEvalObject *self, const SnDecl *m, SnList *args,
                  SnEvalEnv *caller, SnSpan span);
int sn_native_try_dispatch(SnEvalInterp *in, const SnDecl *fn, SnList *args, SnEvalEnv *caller,
                           SnEvalObject *self, SnSpan span, SnEvalValue *out);

/* ── variants, lambdas and pattern matching ───────────────────────────────── */

SnEvalValue sn_eval_variant(SnEvalInterp *in, const char *name, SnList payload);
SnEvalValue sn_eval_call_lambda(SnEvalInterp *in, const SnEvalLambda *lam, SnList *args, SnEvalEnv *caller,
                  SnSpan span);
int sn_eval_value_equals(SnEvalValue a, SnEvalValue b);
/* Attempts to match `subject` against `pat`; bindings are defined in `env`.
 * Returns 1 on match. Bindings from a failed partial match may remain in
 * `env` — callers pass a fresh child env per arm. */
int sn_eval_pattern_match(SnEvalInterp *in, SnEvalEnv *env, const SnPattern *pat,
                       SnEvalValue subject);
/* Shared match driver: finds the first matching arm (patterns + guard) and
 * returns it, with `*arm_env` set to the env holding its bindings. NULL when
 * no arm matches. */
const SnMatchArm *sn_eval_match_arm(SnEvalInterp *in, SnEvalEnv *env, const SnList *arms,
                                   SnEvalValue subject, SnEvalEnv **arm_env);

/* ── eval_expr.c ──────────────────────────────────────────────────────────── */

long long sn_eval_as_int(SnEvalInterp *in, SnEvalValue v, SnSpan span);
int sn_eval_truthy(SnEvalInterp *in, SnEvalValue v, SnSpan span);
SnEvalValue sn_eval_expr(SnEvalInterp *in, SnEvalEnv *env, const SnExpr *e);
/* `Some`/`Ok`/`Err`/`None`, or a variant of a declared enum. Shared with
 * sn_eval_pattern_match(): a bare pattern name is only a wildcard bind when it
 * is NOT one of these — see the parser's note in sn_parse_pattern(). */
int sn_eval_is_variant_ctor(SnEvalInterp *in, const char *name);

/* ── eval_stmt.c ──────────────────────────────────────────────────────────── */

SnEvalFlow sn_eval_exec_stmt(SnEvalInterp *in, SnEvalEnv *env, const SnStmt *s);

/* ── eval_string.c ────────────────────────────────────────────────────────── */

/* Decodes a string literal token: strips the quotes, resolves escapes, and
 * evaluates `${...}` interpolation by parsing the inner expression source.
 * `$$` is a literal `$` and never starts interpolation. */
const char *sn_eval_decode_string(SnEvalInterp *in, SnEvalEnv *env, const SnExpr *e);

/* ── ownership runtime (rt_mem.c / rt_ptr.c / rt_defer.c) ─────────────────── */

void sn_rt_mem_reset(SnEvalInterp *in);
void sn_rt_frame_push(SnEvalInterp *in);
void sn_rt_frame_pop(SnEvalInterp *in, SnEvalEnv *keep_env, SnEvalValue extra);
void *sn_rt_alloc(SnEvalInterp *in, size_t nbytes, int heap);
void sn_rt_list_push(SnEvalInterp *in, SnList *l, void *item, int heap);
int sn_rt_heap_allocs(void);
int sn_rt_heap_live(void);
int sn_rt_stack_allocs(void);
int sn_rt_bounds_checks(void);
void sn_rt_bounds_hit(void);
int sn_eval_probe_name(const char *name);
long long sn_eval_probe_value(const char *name);

SnEvalValue sn_rt_eval_arg(SnEvalInterp *in, SnEvalEnv *env, const SnExpr *e);
SnEvalValue sn_rt_take_ref(SnEvalInterp *in, SnEvalEnv *env, const SnExpr *e);
SnEvalValue sn_rt_deref(SnEvalInterp *in, SnEvalValue v, SnSpan span);
SnEvalValue sn_rt_autoderef(SnEvalInterp *in, SnEvalValue v, SnSpan span);
SnEvalValue sn_rt_null(SnEvalInterp *in);
SnEvalValue sn_rt_new_array(SnEvalInterp *in, SnEvalEnv *env, const SnExpr *e);
SnEvalValue sn_rt_eval_index(SnEvalInterp *in, SnEvalEnv *env, const SnExpr *e);
int sn_rt_index_slot(SnEvalInterp *in, SnEvalEnv *env, const SnExpr *index, SnEvalValue **slot_out);

void sn_rt_defer_schedule(SnEvalInterp *in, SnEvalEnv *env, const SnExpr *call);
void sn_rt_run_defers(SnEvalInterp *in);
SnEvalValue sn_rt_finish_call(SnEvalInterp *in, SnEvalEnv *local, SnEvalEnv *caller, const SnStmt *body);

#endif /* SNOVAC_EVAL_INTERNAL_H */
