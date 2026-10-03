/* eval_internal.h — shared state for the tree-walking interpreter.
 *
 * Split by concern to keep every translation unit small:
 *   eval.c         entry point, environments, object model, calls
 *   eval_expr.c    expression evaluation
 *   eval_stmt.c    statement execution
 *   eval_string.c  literal decoding and `${...}` interpolation
 *
 * This interpreter is the P1 smoke path — `snovac run` — not the P3 VM. It
 * exists so parser output can be executed end to end while the bytecode
 * backend is still ahead of us.
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

typedef struct Object Object;
typedef struct VariantVal VariantVal;
typedef struct LambdaVal LambdaVal;
typedef struct ArrayVal ArrayVal;
typedef struct RefVal RefVal;

struct ArrayVal {
    SnList items; /* Value* */
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
        Object *o;
        VariantVal *vt;
        LambdaVal *lam;
        ArrayVal *arr;
        RefVal *ref;
    } as;
} Value;

/* Fat when `fat` is set: `len` is the array length carried with the address. */
struct RefVal {
    Value *slot;
    size_t len;
    int is_null;
    int fat;
};

struct Object {
    const SnDecl *cls;
    SnList names;  /* const char* */
    SnList slots;  /* Value* */
};

/* A constructed enum variant — `Some(x)`, `Ok(v)`, `Err(e)`, `None`, or a
 * variant of a user-declared enum. `name` is the bare variant name; `payload`
 * holds the constructor arguments in order (Value*). */
struct VariantVal {
    const char *name;
    SnList payload; /* Value* */
};

typedef enum {
    FLOW_NORMAL, FLOW_RETURN, FLOW_BREAK, FLOW_CONTINUE, FLOW_THROW
} Flow;

typedef struct MemFrame MemFrame;

typedef struct Env {
    struct Env *parent;
    SnList names; /* const char* */
    SnList slots; /* Value* — boxed so assignment is visible to inner scopes */
} Env;

typedef struct DeferCall {
    const SnExpr *call;
    Env *env;
    Value *args;
    size_t nargs;
    struct DeferCall *next;
} DeferCall;

/* A first-class `(x) -> ...` value: the lambda expression plus the environment
 * it closed over. */
struct LambdaVal {
    const SnExpr *expr; /* SN_EXPR_LAMBDA node (params, value/body) */
    Env *env;           /* captured lexical environment */
};

typedef struct {
    SnArena *arena;
    SnDiagSink *diag;
    const SnUnit *unit;
    Env *globals;
    Value ret;
    Flow flow;
    int failed;
    MemFrame *mem;
    DeferCall *defers;
} Interp;

/* ── value constructors ───────────────────────────────────────────────────── */

static inline Value v_unit(void)         { Value v; v.kind = V_UNIT; v.iwidth = 0; v.as.i = 0; return v; }
static inline Value v_int(long long i)   { Value v; v.kind = V_INT; v.iwidth = 64; v.as.i = i; return v; }
static inline Value v_int_w(long long i, unsigned char w) {
    Value v = v_int(i);
    v.iwidth = w ? w : 64;
    return v;
}
static inline Value v_char(long long code) { Value v; v.kind = V_CHAR; v.iwidth = 0; v.as.i = code; return v; }
static inline Value v_double(double d)   { Value v; v.kind = V_DOUBLE; v.iwidth = 0; v.as.d = d; return v; }
static inline Value v_bool(int b)        { Value v; v.kind = V_BOOL; v.iwidth = 0; v.as.b = b != 0; return v; }
static inline Value v_str(const char *s) { Value v; v.kind = V_STRING; v.iwidth = 0; v.as.s = s; return v; }

/* ── eval.c ───────────────────────────────────────────────────────────────── */

void rt_error(Interp *in, int code, SnSpan span, const char *fmt, ...);

Env *env_new(Interp *in, Env *parent);
Value *env_lookup(Env *e, const char *name);
Value *env_define(Interp *in, Env *e, const char *name, Value v);

char *arena_sprintf(Interp *in, const char *fmt, ...);
char *str_concat(Interp *in, const char *a, const char *b);
const char *to_string(Interp *in, Value v, SnSpan span);

const SnDecl *find_member(const SnDecl *cls, const char *name);
const SnDecl *find_member_inherited(const Interp *in, const SnDecl *cls, const char *name);
/* Same name, distinct parameter types. Falls back to the first member when
 * the arguments do not distinguish a candidate. */
const SnDecl *find_overload(Interp *in, Env *env, const SnDecl *cls, const char *name,
                            const SnList *args);
const SnDecl *find_top(const Interp *in, const char *name, SnDeclKind k);
const SnDecl *find_type(const Interp *in, const char *name);

Value default_for(const SnType *t);
Object *instantiate(Interp *in, const SnDecl *cls, SnList *args, SnList *names,
                    Env *env, SnSpan span);
Value *object_field(Object *o, const char *name);

Value call_function(Interp *in, const SnDecl *fn, SnList *args, Env *caller,
                    Object *self, SnSpan span);
Value call_method(Interp *in, Object *self, const SnDecl *m, SnList *args,
                  Env *caller, SnSpan span);
int sn_native_try_dispatch(Interp *in, const SnDecl *fn, SnList *args, Env *caller,
                           Object *self, SnSpan span, Value *out);

/* ── variants, lambdas and pattern matching ───────────────────────────────── */

Value v_variant(Interp *in, const char *name, SnList payload);
Value call_lambda(Interp *in, const LambdaVal *lam, SnList *args, Env *caller,
                  SnSpan span);
int value_equals(Value a, Value b);
/* Attempts to match `subject` against `pat`; bindings are defined in `env`.
 * Returns 1 on match. Bindings from a failed partial match may remain in
 * `env` — callers pass a fresh child env per arm. */
int pattern_match_bind(Interp *in, Env *env, const SnPattern *pat,
                       Value subject);
/* Shared match driver: finds the first matching arm (patterns + guard) and
 * returns it, with `*arm_env` set to the env holding its bindings. NULL when
 * no arm matches. */
const SnMatchArm *match_select_arm(Interp *in, Env *env, const SnList *arms,
                                   Value subject, Env **arm_env);

/* ── eval_expr.c ──────────────────────────────────────────────────────────── */

long long as_int(Interp *in, Value v, SnSpan span);
int truthy(Interp *in, Value v, SnSpan span);
Value eval_expr(Interp *in, Env *env, const SnExpr *e);
/* `Some`/`Ok`/`Err`/`None`, or a variant of a declared enum. Shared with
 * pattern_match_bind(): a bare pattern name is only a wildcard bind when it
 * is NOT one of these — see the parser's note in parse_pattern(). */
int is_variant_constructor(Interp *in, const char *name);

/* ── eval_stmt.c ──────────────────────────────────────────────────────────── */

Flow exec_stmt(Interp *in, Env *env, const SnStmt *s);

/* ── eval_string.c ────────────────────────────────────────────────────────── */

/* Decodes a string literal token: strips the quotes, resolves escapes, and
 * evaluates `${...}` interpolation by parsing the inner expression source.
 * `$$` is a literal `$` and never starts interpolation. */
const char *decode_string(Interp *in, Env *env, const SnExpr *e);

/* ── ownership runtime (rt_mem.c / rt_ptr.c / rt_defer.c) ─────────────────── */

void rt_mem_reset(Interp *in);
void rt_frame_push(Interp *in);
void rt_frame_pop(Interp *in, Env *keep_env, Value extra);
void *rt_alloc(Interp *in, size_t nbytes, int heap);
void rt_list_push(Interp *in, SnList *l, void *item, int heap);
int rt_heap_allocs(void);
int rt_heap_live(void);
int rt_stack_allocs(void);
int rt_bounds_checks(void);
void rt_bounds_hit(void);
int rt_probe_name(const char *name);
long long rt_probe_value(const char *name);

Value rt_eval_arg(Interp *in, Env *env, const SnExpr *e);
Value rt_take_ref(Interp *in, Env *env, const SnExpr *e);
Value rt_deref(Interp *in, Value v, SnSpan span);
Value rt_autoderef(Interp *in, Value v, SnSpan span);
Value rt_null(Interp *in);
Value rt_new_array(Interp *in, Env *env, const SnExpr *e);
Value rt_eval_index(Interp *in, Env *env, const SnExpr *e);
int rt_index_slot(Interp *in, Env *env, const SnExpr *index, Value **slot_out);

void rt_defer_schedule(Interp *in, Env *env, const SnExpr *call);
void rt_run_defers(Interp *in);
Value rt_finish_call(Interp *in, Env *local, Env *caller, const SnStmt *body);

#endif /* SNOVAC_EVAL_INTERNAL_H */
