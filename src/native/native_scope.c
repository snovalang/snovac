/* native_scope.c — runner support for null, variants, defer, pulsar spawn,
 * and async calls. The generated pulsar queue stays in the runner. */
#include "native_scope.h"
#include "snbc.h"

void sn_native_write_scope_support(FILE *f, int windows) {
    fprintf(f, "typedef struct SnVar { uint32_t tag; Val *payload; uint32_t n; } SnVar;\n");
    fprintf(f, "typedef struct DeferItem { size_t fn; Val args[8]; uint32_t argc; struct DeferItem *next; } DeferItem;\n");
    fprintf(f, "typedef struct AsyncItem { size_t fn; Val args[8]; uint32_t argc; struct AsyncItem *next; } AsyncItem;\n");
    fprintf(f, "static AsyncItem *g_async_q = NULL;\n");
    fprintf(f, "static int g_async_depth = 0;\n");
    fprintf(f, "static void pulsar_drain(void) {\n");
    fprintf(f, "  for (;;) {\n");
    if (windows) {
        fprintf(f, "    EnterCriticalSection(&g_pulsar_mutex);\n");
        fprintf(f, "    int pending = g_pulsar_queue != NULL || g_pulsar_busy;\n");
        fprintf(f, "    if (!pending) { LeaveCriticalSection(&g_pulsar_mutex); break; }\n");
        fprintf(f, "    SleepConditionVariableCS(&g_pulsar_cond, &g_pulsar_mutex, INFINITE);\n");
        fprintf(f, "    LeaveCriticalSection(&g_pulsar_mutex);\n");
    } else {
        fprintf(f, "    pthread_mutex_lock(&g_pulsar_mutex);\n");
        fprintf(f, "    int pending = g_pulsar_queue != NULL || g_pulsar_busy;\n");
        fprintf(f, "    if (!pending) { pthread_mutex_unlock(&g_pulsar_mutex); break; }\n");
        fprintf(f, "    pthread_cond_wait(&g_pulsar_cond, &g_pulsar_mutex);\n");
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
            "      if (argc > 8) { fputs(\"error: pulsar arity\\n\", stderr); exit(1); }\n"
            "      PulsarWork *work = (PulsarWork *)calloc(1, sizeof(PulsarWork));\n"
            "      if (!work) { fputs(\"error: out of memory\\n\", stderr); exit(70); }\n"
            "      work->fn_idx = fni; work->argc = argc;\n"
            "      for (uint32_t ai = argc; ai > 0; ai--) work->args[ai - 1] = pop();\n",
            OP_SPAWN);
    if (windows) {
        fprintf(f,
                "      EnterCriticalSection(&g_pulsar_mutex);\n"
                "      work->next = g_pulsar_queue; g_pulsar_queue = work;\n"
                "      WakeConditionVariable(&g_pulsar_cond);\n"
                "      LeaveCriticalSection(&g_pulsar_mutex);\n");
    } else {
        fprintf(f,
                "      pthread_mutex_lock(&g_pulsar_mutex);\n"
                "      work->next = g_pulsar_queue; g_pulsar_queue = work;\n"
                "      pthread_cond_signal(&g_pulsar_cond);\n"
                "      pthread_mutex_unlock(&g_pulsar_mutex);\n");
    }
    fprintf(f,
            "      pulsar_drain();\n"
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
