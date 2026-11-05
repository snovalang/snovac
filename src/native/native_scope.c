/* native_scope.c — runner support for null, variants, defer, pulsar spawn,
 * and async calls. The generated pulsar queue stays in the runner. */
#include "native_scope.h"
#include "snbc.h"

void sn_native_write_scope_support(FILE *f, int windows) {
    fprintf(f, "typedef struct SnVar { uint32_t tag; Val *payload; uint32_t n; } SnVar;\n");
    fprintf(f,
            "static Val val_clone(Val v) {\n"
            "  uint32_t i;\n"
            "  if (v.tag == VAL_STRING) {\n"
            "    const char *s = v.as.str ? v.as.str : \"\";\n"
            "    size_t n = strlen(s);\n"
            "    char *copy = (char *)malloc(n + 1);\n"
            "    if (!copy) { fputs(\"error: out of memory\\n\", stderr); exit(70); }\n"
            "    memcpy(copy, s, n + 1);\n"
            "    v.as.str = copy;\n"
            "    return v;\n"
            "  }\n"
            "  if (v.tag == VAL_ARRAY && v.as.ptr) {\n"
            "    SnArray *src = (SnArray *)v.as.ptr;\n"
            "    SnArray *dst = (SnArray *)calloc(1, sizeof(SnArray));\n"
            "    if (!dst) { fputs(\"error: out of memory\\n\", stderr); exit(70); }\n"
            "    dst->count = src->count; dst->cap = src->count;\n"
            "    dst->items = (Val *)calloc(src->count ? src->count : 1u, sizeof(Val));\n"
            "    if (!dst->items) { fputs(\"error: out of memory\\n\", stderr); exit(70); }\n"
            "    for (i = 0; i < src->count; i++) dst->items[i] = val_clone(src->items[i]);\n"
            "    v.as.ptr = dst;\n"
            "    return v;\n"
            "  }\n"
            "  if (v.tag == VAL_OBJ && v.as.ptr) {\n"
            "    SnObj *src = (SnObj *)v.as.ptr;\n"
            "    SnObj *dst = (SnObj *)calloc(1, sizeof(SnObj));\n"
            "    if (!dst) { fputs(\"error: out of memory\\n\", stderr); exit(70); }\n"
            "    dst->count = src->count;\n"
            "    dst->fields = (Val *)calloc(src->count ? src->count : 1u, sizeof(Val));\n"
            "    if (!dst->fields) { fputs(\"error: out of memory\\n\", stderr); exit(70); }\n"
            "    for (i = 0; i < src->count; i++) dst->fields[i] = val_clone(src->fields[i]);\n"
            "    v.as.ptr = dst;\n"
            "    return v;\n"
            "  }\n"
            "  if (v.tag == VAL_VAR && v.as.ptr) {\n"
            "    SnVar *src = (SnVar *)v.as.ptr;\n"
            "    SnVar *dst = (SnVar *)calloc(1, sizeof(SnVar));\n"
            "    if (!dst) { fputs(\"error: out of memory\\n\", stderr); exit(70); }\n"
            "    dst->tag = src->tag; dst->n = src->n;\n"
            "    dst->payload = (Val *)calloc(src->n ? src->n : 1u, sizeof(Val));\n"
            "    if (!dst->payload) { fputs(\"error: out of memory\\n\", stderr); exit(70); }\n"
            "    for (i = 0; i < src->n; i++) dst->payload[i] = val_clone(src->payload[i]);\n"
            "    v.as.ptr = dst;\n"
            "    return v;\n"
            "  }\n"
            "  return v;\n"
            "}\n"
            "static void val_release(Val v) {\n"
            "  uint32_t i;\n"
            "  if (v.tag == VAL_STRING && v.as.str) { free(v.as.str); return; }\n"
            "  if (v.tag == VAL_ARRAY && v.as.ptr) {\n"
            "    SnArray *a = (SnArray *)v.as.ptr;\n"
            "    if (a->items) { for (i = 0; i < a->count; i++) val_release(a->items[i]); free(a->items); }\n"
            "    free(a); return;\n"
            "  }\n"
            "  if (v.tag == VAL_OBJ && v.as.ptr) {\n"
            "    SnObj *o = (SnObj *)v.as.ptr;\n"
            "    if (o->fields) { for (i = 0; i < o->count; i++) val_release(o->fields[i]); free(o->fields); }\n"
            "    free(o); return;\n"
            "  }\n"
            "  if (v.tag == VAL_VAR && v.as.ptr) {\n"
            "    SnVar *var = (SnVar *)v.as.ptr;\n"
            "    if (var->payload) { for (i = 0; i < var->n; i++) val_release(var->payload[i]); free(var->payload); }\n"
            "    free(var);\n"
            "  }\n"
            "}\n");
    fprintf(f, "typedef struct DeferItem { size_t fn; Val args[8]; uint32_t argc; struct DeferItem *next; } DeferItem;\n");
    fprintf(f, "typedef struct AsyncItem { size_t fn; Val args[8]; uint32_t argc; struct AsyncItem *next; } AsyncItem;\n");
    fprintf(f, "static _Thread_local AsyncItem *g_async_q = NULL;\n");
    fprintf(f, "static _Thread_local int g_async_depth = 0;\n");
    fprintf(f, "static void pulsar_drain(void) {\n");
    fprintf(f, "  for (;;) {\n");
    if (windows) {
        fprintf(f, "    EnterCriticalSection(&g_pulsar_mutex);\n");
        fprintf(f, "    int pending = g_pulsar_queue != NULL || g_pulsar_busy;\n");
        fprintf(f, "    if (!pending) { LeaveCriticalSection(&g_pulsar_mutex); break; }\n");
        fprintf(f, "    SleepConditionVariableCS(&g_pulsar_done, &g_pulsar_mutex, INFINITE);\n");
        fprintf(f, "    LeaveCriticalSection(&g_pulsar_mutex);\n");
    } else {
        fprintf(f, "    pthread_mutex_lock(&g_pulsar_mutex);\n");
        fprintf(f, "    int pending = g_pulsar_queue != NULL || g_pulsar_busy;\n");
        fprintf(f, "    if (!pending) { pthread_mutex_unlock(&g_pulsar_mutex); break; }\n");
        fprintf(f, "    pthread_cond_wait(&g_pulsar_done, &g_pulsar_mutex);\n");
        fprintf(f, "    pthread_mutex_unlock(&g_pulsar_mutex);\n");
    }
    fprintf(f, "  }\n}\n");
    fprintf(f, "static void run_defers(DeferItem **head) {\n");
    fprintf(f, "  while (*head) {\n");
    fprintf(f, "    DeferItem *d = *head; *head = d->next;\n");
    fprintf(f, "    Val saved = g_ret;\n");
    fprintf(f, "    run_function(d->fn, d->args, d->argc);\n");
    fprintf(f, "    g_ret = saved;\n");
    fprintf(f, "    free(d);\n");
    fprintf(f, "  }\n}\n");
}

void sn_native_write_scope_ops(FILE *f, int windows) {
    fprintf(f,
            "    case %d: { Val v; memset(&v, 0, sizeof(v)); v.tag = VAL_NULL; push(v); break; }\n",
            OP_CONST_NULL);
    fprintf(f,
            "    case %d: {\n"
            "      fputs(\"error: function has no body and its @native intrinsic is not implemented yet\\n\", stderr);\n"
            "      exit(1);\n"
            "    }\n",
            OP_TRAP);
    fprintf(f,
            "    case %d: {\n"
            "      uint32_t fni = read_u32(&ip); uint32_t argc = read_u32(&ip);\n"
            "      if (argc > 8) { fputs(\"error: defer arity\\n\", stderr); exit(1); }\n"
            "      DeferItem *d = (DeferItem *)calloc(1, sizeof(DeferItem));\n"
            "      if (!d) { fputs(\"error: out of memory\\n\", stderr); exit(70); }\n"
            "      d->fn = fni; d->argc = argc;\n"
            "      for (uint32_t ai = argc; ai > 0; ai--) d->args[ai - 1] = pop();\n"
            "      d->next = defers; defers = d; break;\n"
            "    }\n",
            OP_DEFER);
    fprintf(f,
            "    case %d: {\n"
            "      uint32_t fni = read_u32(&ip); uint32_t argc = read_u32(&ip);\n"
            "      if (argc > 256 || argc > (SIZE_MAX - sizeof(PulsarWork)) / sizeof(Val)) { fputs(\"error: pulsar arity\\n\", stderr); exit(1); }\n"
            "      PulsarWork *work = (PulsarWork *)calloc(1, sizeof(PulsarWork) + (size_t)argc * sizeof(Val));\n"
            "      if (!work) { fputs(\"error: out of memory\\n\", stderr); exit(70); }\n"
            "      work->fn_idx = fni; work->argc = argc;\n"
            "      for (uint32_t ai = argc; ai > 0; ai--) work->args[ai - 1] = val_clone(pop());\n",
            OP_SPAWN);
    if (windows) {
        fprintf(f,
                "      EnterCriticalSection(&g_pulsar_mutex);\n"
                "      if (g_pulsar_queued >= 1024u) { LeaveCriticalSection(&g_pulsar_mutex); { uint32_t ai; for (ai = 0; ai < work->argc; ai++) val_release(work->args[ai]); } free(work); fputs(\"error: pulsar queue capacity exceeded\\n\", stderr); exit(75); }\n"
                "      work->next = g_pulsar_queue; g_pulsar_queue = work;\n"
                "      g_pulsar_queued++;\n"
                "      WakeConditionVariable(&g_pulsar_cond);\n"
                "      LeaveCriticalSection(&g_pulsar_mutex);\n");
    } else {
        fprintf(f,
                "      pthread_mutex_lock(&g_pulsar_mutex);\n"
                "      if (g_pulsar_queued >= 1024u) { pthread_mutex_unlock(&g_pulsar_mutex); { uint32_t ai; for (ai = 0; ai < work->argc; ai++) val_release(work->args[ai]); } free(work); fputs(\"error: pulsar queue capacity exceeded\\n\", stderr); exit(75); }\n"
                "      work->next = g_pulsar_queue; g_pulsar_queue = work;\n"
                "      g_pulsar_queued++;\n"
                "      pthread_cond_signal(&g_pulsar_cond);\n"
                "      pthread_mutex_unlock(&g_pulsar_mutex);\n");
    }
    fprintf(f,
            "      break;\n"
            "    }\n");
    fprintf(f,
            "    case %d: {\n"
            "      uint32_t fni = read_u32(&ip); uint32_t argc = read_u32(&ip);\n"
            "      if (argc > 8) { fputs(\"error: async arity\\n\", stderr); exit(1); }\n"
            "      AsyncItem *task = (AsyncItem *)calloc(1, sizeof(AsyncItem));\n"
            "      if (!task) { fputs(\"error: out of memory\\n\", stderr); exit(70); }\n"
            "      task->fn = fni; task->argc = argc;\n"
            "      for (uint32_t ai = argc; ai > 0; ai--) task->args[ai - 1] = pop();\n"
            "      if (g_async_depth == 0) {\n"
            "        task->next = g_async_q; g_async_q = task;\n"
            "        g_async_depth = 1;\n"
            "        while (g_async_q) {\n"
            "          AsyncItem *t = g_async_q; g_async_q = t->next;\n"
            "          run_function(t->fn, t->args, t->argc);\n"
            "          free(t);\n"
            "        }\n"
            "        g_async_depth = 0;\n"
            "      } else {\n"
            "        run_function(task->fn, task->args, task->argc);\n"
            "        free(task);\n"
            "      }\n"
            "      push(g_ret); break;\n"
            "    }\n",
            OP_CALL_ASYNC);
    fprintf(f,
            "    case %d: {\n"
            "      Val v = pop(); Val r; memset(&r, 0, sizeof(r)); r.tag = VAL_BOOL;\n"
            "      r.as.b = v.tag == VAL_ARRAY; push(r); break;\n"
            "    }\n",
            OP_IS_ARRAY);
    fprintf(f,
            "    case %d: {\n"
            "      uint32_t tag = read_u32(&ip); uint32_t n = read_u32(&ip);\n"
            "      SnVar *var = (SnVar *)calloc(1, sizeof(SnVar));\n"
            "      if (!var) { fputs(\"error: out of memory\\n\", stderr); exit(70); }\n"
            "      var->tag = tag; var->n = n;\n"
            "      var->payload = (Val *)calloc(n ? n : 1, sizeof(Val));\n"
            "      if (!var->payload) { fputs(\"error: out of memory\\n\", stderr); exit(70); }\n"
            "      for (uint32_t i = n; i > 0; i--) var->payload[i - 1] = pop();\n"
            "      Val v; memset(&v, 0, sizeof(v)); v.tag = VAL_VAR; v.as.ptr = var; push(v); break;\n"
            "    }\n",
            OP_VARIANT);
    fprintf(f,
            "    case %d: {\n"
            "      uint32_t tag = read_u32(&ip); Val v = pop();\n"
            "      SnVar *var = (v.tag == VAL_VAR) ? (SnVar *)v.as.ptr : NULL;\n"
            "      int same = var && g_strings[tag] && g_strings[var->tag] &&\n"
            "        strcmp(g_strings[tag], g_strings[var->tag]) == 0;\n"
            "      Val r; memset(&r, 0, sizeof(r)); r.tag = VAL_BOOL; r.as.b = same; push(r); break;\n"
            "    }\n",
            OP_IS_VARIANT);
}
