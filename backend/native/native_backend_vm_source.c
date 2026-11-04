#include "native_backend_internal.h"

#include <stdio.h>

void sn_native_emit_vm_source(FILE *f, const SnBCUnit *bc, const SnTargetInfo *target) {
  sn_native_emit_vm_value_runtime(f);

  /* Emit Pulsar Multi-parallelism Runtime */
  fprintf(f, "/* Pulsar platform thread multi-parallelism */\n");
  fprintf(f, "typedef struct PulsarWork {\n");
  fprintf(f, "  size_t fn_idx;\n");
  fprintf(f, "  struct PulsarWork *next;\n");
  fprintf(f, "} PulsarWork;\n\n");

  if (target->os == SN_OS_WINDOWS) {
    fprintf(f, "static CRITICAL_SECTION g_pulsar_mutex;\n");
    fprintf(f, "static CRITICAL_SECTION g_container_mutex;\n");
    fprintf(f, "static CONDITION_VARIABLE g_pulsar_cond;\n");
    fprintf(f, "static void sn_native_container_lock(void) { EnterCriticalSection(&g_container_mutex); }\n");
    fprintf(f, "static void sn_native_container_unlock(void) { LeaveCriticalSection(&g_container_mutex); }\n");
  } else {
    fprintf(
        f,
        "static pthread_mutex_t g_pulsar_mutex = PTHREAD_MUTEX_INITIALIZER;\n");
    fprintf(f, "static pthread_mutex_t g_container_mutex = PTHREAD_MUTEX_INITIALIZER;\n");
    fprintf(
        f, "static pthread_cond_t g_pulsar_cond = PTHREAD_COND_INITIALIZER;\n");
    fprintf(f, "static void sn_native_container_lock(void) { pthread_mutex_lock(&g_container_mutex); }\n");
    fprintf(f, "static void sn_native_container_unlock(void) { pthread_mutex_unlock(&g_container_mutex); }\n");
  }
  fprintf(f, "static PulsarWork *g_pulsar_queue = NULL;\n");
  fprintf(f, "static bool g_pulsar_stop = false;\n\n");
  fprintf(f, "static atomic_int g_pulsar_worker_available;\n\n");

  fprintf(f, "static Val run_function(size_t fn_idx, size_t argc);\n\n");

  if (target->os == SN_OS_WINDOWS) {
    fprintf(f, "static unsigned __stdcall pulsar_thread_proc(void *arg) {\n");
    fprintf(f, "  (void)arg;\n");
    fprintf(f, "  for (;;) {\n");
    fprintf(f, "    EnterCriticalSection(&g_pulsar_mutex);\n");
    fprintf(f, "    while (!g_pulsar_queue && !g_pulsar_stop) {\n");
    fprintf(f, "      SleepConditionVariableCS(&g_pulsar_cond, "
               "&g_pulsar_mutex, INFINITE);\n");
    fprintf(f, "    }\n");
    fprintf(f, "    if (g_pulsar_stop && !g_pulsar_queue) { "
               "LeaveCriticalSection(&g_pulsar_mutex); break; }\n");
    fprintf(f, "    PulsarWork *work = g_pulsar_queue;\n");
    fprintf(f, "    if (work) g_pulsar_queue = work->next;\n");
    fprintf(f, "    LeaveCriticalSection(&g_pulsar_mutex);\n");
    fprintf(f, "    if (work) { sn_native_val_release(run_function(work->fn_idx, 0)); free(work); }\n");
    fprintf(f, "  }\n  return 0;\n}\n\n");
  } else {
    fprintf(f, "static void *pulsar_thread_proc(void *arg) {\n");
    fprintf(f, "  (void)arg;\n");
    fprintf(f, "  for (;;) {\n");
    fprintf(f, "    pthread_mutex_lock(&g_pulsar_mutex);\n");
    fprintf(f, "    while (!g_pulsar_queue && !g_pulsar_stop) {\n");
    fprintf(f, "      pthread_cond_wait(&g_pulsar_cond, &g_pulsar_mutex);\n");
    fprintf(f, "    }\n");
    fprintf(f, "    if (g_pulsar_stop && !g_pulsar_queue) { "
               "pthread_mutex_unlock(&g_pulsar_mutex); break; }\n");
    fprintf(f, "    PulsarWork *work = g_pulsar_queue;\n");
    fprintf(f, "    if (work) g_pulsar_queue = work->next;\n");
    fprintf(f, "    pthread_mutex_unlock(&g_pulsar_mutex);\n");
    fprintf(f, "    if (work) { sn_native_val_release(run_function(work->fn_idx, 0)); free(work); }\n");
    fprintf(f, "  }\n  return NULL;\n}\n\n");
  }

  fprintf(f, "static void pulsar_spawn(size_t fn_idx) {\n");
  fprintf(f, "  if (!atomic_load_explicit(&g_pulsar_worker_available, memory_order_relaxed)) { fputs(\"runtime error: Pulsar worker is unavailable\\n\", stderr); atomic_store_explicit(&g_runtime_failed, 1, memory_order_relaxed); return; }\n");
  fprintf(f, "  PulsarWork *work = (PulsarWork*)malloc(sizeof(PulsarWork));\n");
  fprintf(f, "  if (!work) { fputs(\"runtime error: out of memory queuing Pulsar task\\n\", stderr); atomic_store_explicit(&g_runtime_failed, 1, memory_order_relaxed); return; }\n");
  fprintf(f, "  work->fn_idx = fn_idx;\n  work->next = NULL;\n");
  if (target->os == SN_OS_WINDOWS) {
    fprintf(f, "  EnterCriticalSection(&g_pulsar_mutex);\n");
    fprintf(f, "  work->next = g_pulsar_queue;\n  g_pulsar_queue = work;\n");
    fprintf(f, "  WakeConditionVariable(&g_pulsar_cond);\n");
    fprintf(f, "  LeaveCriticalSection(&g_pulsar_mutex);\n");
  } else {
    fprintf(f, "  pthread_mutex_lock(&g_pulsar_mutex);\n");
    fprintf(f, "  work->next = g_pulsar_queue;\n  g_pulsar_queue = work;\n");
    fprintf(f, "  pthread_cond_signal(&g_pulsar_cond);\n");
    fprintf(f, "  pthread_mutex_unlock(&g_pulsar_mutex);\n");
  }
  fprintf(f, "}\n\n");

  /* Emit run_function */
  fprintf(f, "static Val run_function(size_t fn_idx, size_t argc) {\n");
  fprintf(f, "  const uint8_t *code = NULL;\n");
  fprintf(f, "  size_t local_count = 0;\n");
  fprintf(f, "  size_t arity = 0;\n");
  for (size_t i = 0; i < bc->function_count; i++) {
    fprintf(f, "  if (fn_idx == %zu) { code = g_fn_code_%zu; local_count = %u; arity = %u; }\n",
            i, i, bc->functions[i]->local_count, bc->functions[i]->arity);
  }
  fprintf(f, "  ensure_stack();\n");
  fprintf(f, "  if (argc > (size_t)(g_sp - g_stack)) { fputs(\"runtime error: invalid call argument stack depth\\n\", stderr); atomic_store_explicit(&g_runtime_failed, 1, memory_order_relaxed); Val v = {0}; return v; }\n");
  fprintf(f, "  Val *slots = g_sp - argc;\n");
  fprintf(f, "  if (!code || argc != arity || local_count < argc || local_count > (size_t)(g_stack + 4096 - slots)) { fputs(\"runtime error: invalid function or call frame\\n\", stderr); atomic_store_explicit(&g_runtime_failed, 1, memory_order_relaxed); while (g_sp > slots) sn_native_val_release(pop()); Val v = {0}; return v; }\n");
  fprintf(f, "  if (g_call_depth >= 256u) { fputs(\"runtime error: maximum call depth exceeded\\n\", stderr); atomic_store_explicit(&g_runtime_failed, 1, memory_order_relaxed); while (g_sp > slots) sn_native_val_release(pop()); Val v = {0}; return v; }\n");
  fprintf(f, "  ++g_call_depth;\n");
  fprintf(f, "  for (size_t i = argc; i < local_count; ++i) push((Val){0});\n");
  fprintf(f, "  Val *locals = slots;\n");
  fprintf(f, "  const uint8_t *ip = code;\n");
  fprintf(f, "  Val *stack_base = g_sp;\n");
  fprintf(f, "  for (;;) {\n");
  fprintf(f, "    uint8_t op = *ip++;\n");
  fprintf(f, "    switch (op) {\n");
  fprintf(f, "    case %d: break;\n", OP_NOP);
  fprintf(f,
          "    case %d: { Val v; v.tag = VAL_INT; v.as.i = read_i64(&ip); "
          "push(v); break; }\n",
          OP_CONST_INT);
  fprintf(f,
          "    case %d: { Val v; v.tag = VAL_DOUBLE; v.as.d = "
          "read_double(&ip); push(v); break; }\n",
          OP_CONST_DOUBLE);
  fprintf(f,
          "    case %d: { Val v; v.tag = VAL_STRING; const char *s = g_strings[read_u32(&ip)]; v.as.str = sn_native_string_copy(s, strlen(s)); push(v); break; }\n",
          OP_CONST_STRING);
  fprintf(f,
          "    case %d: { Val v; v.tag = VAL_BOOL; v.as.b = *ip++ != 0; "
          "push(v); break; }\n",
          OP_CONST_BOOL);
  fprintf(
      f,
      "    case %d: { Val v; v.tag = VAL_UNIT; v.as.i = 0; push(v); break; }\n",
      OP_CONST_UNIT);
  fprintf(f, "    case %d: { uint32_t count = read_u32(&ip); if ((size_t)count > (size_t)(g_sp - stack_base)) abort(); SnNativeArray *a = sn_native_array_new(count); for (size_t i = count; i > 0; --i) a->items[i - 1u] = pop(); Val v = {0}; v.tag = VAL_ARRAY; v.as.ptr = a; push(v); break; }\n", OP_NEW_ARRAY);
  fprintf(f, "    case %d: { Val index = pop(), target = pop(), result = {0}; sn_native_container_lock(); if (target.tag == VAL_ARRAY && target.as.ptr && index.tag == VAL_INT && index.as.i >= 0 && (uint64_t)index.as.i < ((SnNativeArray*)target.as.ptr)->count) { result = ((SnNativeArray*)target.as.ptr)->items[index.as.i]; sn_native_val_retain(result); } sn_native_container_unlock(); sn_native_val_release(target); sn_native_val_release(index); push(result); break; }\n", OP_GET_INDEX);
  fprintf(f, "    case %d: { Val value = pop(), index = pop(), target = pop(), result = value; int cycle = -1; sn_native_container_lock(); if (target.tag == VAL_ARRAY && target.as.ptr && index.tag == VAL_INT && index.as.i >= 0 && (uint64_t)index.as.i < ((SnNativeArray*)target.as.ptr)->count) cycle = sn_native_array_would_cycle((SnNativeArray*)target.as.ptr, value); if (cycle == 0) { SnNativeArray *a = (SnNativeArray*)target.as.ptr; sn_native_val_retain(value); Val previous = a->items[index.as.i]; a->items[index.as.i] = value; sn_native_val_release(previous); } sn_native_container_unlock(); if (cycle != 0) { if (cycle > 0) fputs(\"runtime error: array store would create a strong reference cycle\\n\", stderr); else fputs(\"runtime error: invalid array store or cycle check allocation failure\\n\", stderr); g_runtime_failed = 1; sn_native_val_release(value); sn_native_val_release(target); sn_native_val_release(index); while (g_sp > stack_base) sn_native_val_release(pop()); for (size_t i = 0; i < local_count; ++i) sn_native_val_release(locals[i]); g_sp = slots; --g_call_depth; return (Val){0}; } sn_native_val_release(target); sn_native_val_release(index); push(result); break; }\n", OP_SET_INDEX);
  fprintf(f, "    case %d: { Val value = pop(), target = pop(); int status = -1; size_t length = 0; sn_native_container_lock(); if (target.tag == VAL_ARRAY && target.as.ptr) { SnNativeArray *a = (SnNativeArray*)target.as.ptr; if (a->count >= (size_t)INT64_MAX) status = 2; else { int cycle = sn_native_array_would_cycle(a, value); if (cycle == 0 && a->count < SIZE_MAX && sn_native_array_reserve(a, a->count + 1u)) { a->items[a->count++] = value; memset(&value, 0, sizeof(value)); length = a->count; status = 0; } else status = cycle > 0 ? 1 : -1; } } sn_native_container_unlock(); if (status != 0) { fputs(status == 1 ? \"runtime error: array push would create a strong reference cycle\\n\" : status == 2 ? \"runtime error: array length exceeds the language integer range\\n\" : \"runtime error: array push rejected (invalid target or memory pressure)\\n\", stderr); g_runtime_failed = 1; sn_native_val_release(value); sn_native_val_release(target); while (g_sp > stack_base) sn_native_val_release(pop()); for (size_t i = 0; i < local_count; ++i) sn_native_val_release(locals[i]); g_sp = slots; --g_call_depth; return (Val){0}; } sn_native_val_release(target); Val result = {0}; result.tag = VAL_INT; result.as.i = (int64_t)length; push(result); break; }\n", OP_ARRAY_PUSH);
  fprintf(f, "    case %d: { Val target = pop(), result = {0}; sn_native_container_lock(); if (target.tag == VAL_ARRAY && target.as.ptr) { SnNativeArray *a = (SnNativeArray*)target.as.ptr; if (a->count) { size_t index = --a->count; result = a->items[index]; memset(&a->items[index], 0, sizeof(Val)); if (!a->count) sn_native_array_shrink(a, 0); else if (a->capacity > 4u && a->count <= a->capacity / 4u) { size_t cap = a->capacity / 2u; if (cap < 4u) cap = 4u; sn_native_array_shrink(a, cap); } } } sn_native_container_unlock(); sn_native_val_release(target); push(result); break; }\n", OP_ARRAY_POP);
  fprintf(f, "    case %d: { Val target = pop(); int valid = target.tag == VAL_ARRAY && target.as.ptr; if (valid) { sn_native_container_lock(); sn_native_array_clear((SnNativeArray*)target.as.ptr); sn_native_container_unlock(); } sn_native_val_release(target); if (!valid) { fputs(\"runtime error: array clear rejected (invalid target)\\n\", stderr); g_runtime_failed = 1; while (g_sp > stack_base) sn_native_val_release(pop()); for (size_t i = 0; i < local_count; ++i) sn_native_val_release(locals[i]); g_sp = slots; --g_call_depth; return (Val){0}; } Val result = {0}; result.tag = VAL_INT; result.as.i = 0; push(result); break; }\n", OP_ARRAY_CLEAR);
  fprintf(f, "    case %d: sn_native_val_release(pop()); break;\n", OP_POP);
  fprintf(f, "    case %d: { Val v = peek(0); sn_native_val_retain(v); push(v); break; }\n", OP_DUP);
  fprintf(
      f,
      "    case %d: { uint32_t s = read_u32(&ip); Val v = locals[s]; sn_native_val_retain(v); push(v); break; }\n",
      OP_GET_LOCAL);
  fprintf(f,
          "    case %d: { uint32_t s = read_u32(&ip); Val next = peek(0); sn_native_val_retain(next); sn_native_val_release(locals[s]); locals[s] = next; "
          "break; }\n",
          OP_SET_LOCAL);
  fprintf(f,
          "    case %d: { uint32_t s = read_u32(&ip); Val v = locals[s]; "
          "memset(&locals[s], 0, sizeof(locals[s])); push(v); break; }\n",
          OP_MOVE_LOCAL);
  fprintf(f,
          "    case %d: { uint32_t s = read_u32(&ip); sn_native_val_release(locals[s]); "
          "memset(&locals[s], 0, sizeof(locals[s])); break; }\n",
          OP_CLEAR_LOCAL);
  fprintf(f,
          "    case %d: { Val b = pop(), a = pop(), r;\n"
          "      if (a.tag == VAL_STRING && b.tag == VAL_STRING) {\n"
          "        r.tag = VAL_STRING; r.as.str = sn_native_string_concat(a.as.str, b.as.str);\n"
          "      } else if (a.tag == VAL_DOUBLE || b.tag == VAL_DOUBLE) {\n"
          "        r.tag = VAL_DOUBLE; r.as.d = (a.tag == VAL_DOUBLE ? a.as.d "
          ": a.as.i) + (b.tag == VAL_DOUBLE ? b.as.d : b.as.i);\n"
          "      } else {\n"
          "        r.tag = VAL_INT; r.as.i = a.as.i + b.as.i;\n"
          "      }\n      sn_native_val_release(a); sn_native_val_release(b); push(r); break;\n    }\n",
          OP_ADD);
  fprintf(f,
          "    case %d: { Val b = pop(), a = pop(), r; r.tag = VAL_INT; r.as.i "
          "= a.as.i - b.as.i; sn_native_val_release(a); sn_native_val_release(b); push(r); break; }\n",
          OP_SUB);
  fprintf(f,
          "    case %d: { Val b = pop(), a = pop(), r; r.tag = VAL_INT; r.as.i "
          "= a.as.i * b.as.i; sn_native_val_release(a); sn_native_val_release(b); push(r); break; }\n",
          OP_MUL);
  fprintf(f,
          "    case %d: { Val b = pop(), a = pop(), r; r.tag = VAL_INT; r.as.i "
          "= b.as.i ? a.as.i / b.as.i : 0; sn_native_val_release(a); sn_native_val_release(b); push(r); break; }\n",
          OP_DIV);
  fprintf(f,
          "    case %d: { Val b = pop(), a = pop(), r; r.tag = VAL_INT; r.as.i "
          "= b.as.i ? a.as.i %% b.as.i : 0; sn_native_val_release(a); sn_native_val_release(b); push(r); break; }\n",
          OP_MOD);
  fprintf(f,
          "    case %d: { Val a = pop(), r; r.tag = VAL_INT; r.as.i = -a.as.i; "
          "sn_native_val_release(a); push(r); break; }\n",
          OP_NEG);
  fprintf(f,
          "    case %d: { Val a = pop(), r; r.tag = VAL_BOOL; r.as.b = "
          "!a.as.b; sn_native_val_release(a); push(r); break; }\n",
          OP_NOT);
  fprintf(f,
          "    case %d: { Val b = pop(), a = pop(), r; r.tag = VAL_BOOL; "
          "r.as.b = (a.as.i == b.as.i); sn_native_val_release(a); sn_native_val_release(b); push(r); break; }\n",
          OP_EQ);
  fprintf(f,
          "    case %d: { Val b = pop(), a = pop(), r; r.tag = VAL_BOOL; "
          "r.as.b = (a.as.i != b.as.i); sn_native_val_release(a); sn_native_val_release(b); push(r); break; }\n",
          OP_NE);
  fprintf(f,
          "    case %d: { Val b = pop(), a = pop(), r; r.tag = VAL_BOOL; "
          "r.as.b = (a.as.i < b.as.i); sn_native_val_release(a); sn_native_val_release(b); push(r); break; }\n",
          OP_LT);
  fprintf(f,
          "    case %d: { Val b = pop(), a = pop(), r; r.tag = VAL_BOOL; "
          "r.as.b = (a.as.i <= b.as.i); sn_native_val_release(a); sn_native_val_release(b); push(r); break; }\n",
          OP_LE);
  fprintf(f,
          "    case %d: { Val b = pop(), a = pop(), r; r.tag = VAL_BOOL; "
          "r.as.b = (a.as.i > b.as.i); sn_native_val_release(a); sn_native_val_release(b); push(r); break; }\n",
          OP_GT);
  fprintf(f,
          "    case %d: { Val b = pop(), a = pop(), r; r.tag = VAL_BOOL; "
          "r.as.b = (a.as.i >= b.as.i); sn_native_val_release(a); sn_native_val_release(b); push(r); break; }\n",
          OP_GE);
  fprintf(f,
          "    case %d: { int32_t off = (int32_t)read_u32(&ip); ip += off; "
          "break; }\n",
          OP_JUMP);
  fprintf(f,
          "    case %d: { int32_t off = (int32_t)read_u32(&ip); Val c = pop(); "
          "if (!c.as.b) ip += off; sn_native_val_release(c); break; }\n",
          OP_JUMP_IF_FALSE);
  fprintf(f,
          "    case %d: { int32_t off = (int32_t)read_u32(&ip); Val c = pop(); "
          "if (c.as.b) ip += off; sn_native_val_release(c); break; }\n",
          OP_JUMP_IF_TRUE);
  fprintf(f,
          "    case %d: { uint32_t f = read_u32(&ip); uint32_t argc = read_u32(&ip); "
          "push(run_function(f, argc)); break; }\n",
          OP_CALL);
  fprintf(f,
          "    case %d: { Val r = pop(); while (g_sp > stack_base) sn_native_val_release(pop()); for (size_t i = 0; i < local_count; ++i) sn_native_val_release(locals[i]); g_sp = slots; --g_call_depth; return r; }\n",
          OP_RETURN);
  fprintf(f,
          "    case %d: { uint8_t nl = *ip++; Val v = pop();\n"
          "      if (v.tag == VAL_STRING) printf(\"%%s\", v.as.str);\n"
          "      else if (v.tag == VAL_INT) printf(\"%%ld\", (long)v.as.i);\n"
          "      else if (v.tag == VAL_DOUBLE) printf(\"%%g\", v.as.d);\n"
          "      else if (v.tag == VAL_BOOL) printf(\"%%s\", v.as.b ? \"true\" "
          ": \"false\");\n"
          "      else if (v.tag == VAL_ARRAY) printf(\"[... ]\");\n"
          "      if (nl) printf(\"\\n\");\n      sn_native_val_release(v); break;\n    }\n",
          OP_PRINT);
  fprintf(f, "    case %d: while (g_sp > stack_base) sn_native_val_release(pop()); for (size_t i = 0; i < local_count; ++i) sn_native_val_release(locals[i]); g_sp = slots; --g_call_depth; return (Val){0};\n", OP_HALT);
  fprintf(f, "    default: g_runtime_failed = 1; while (g_sp > stack_base) sn_native_val_release(pop()); for (size_t i = 0; i < local_count; ++i) sn_native_val_release(locals[i]); g_sp = slots; --g_call_depth; return (Val){0};\n");
  fprintf(f, "    }\n  }\n}\n\n");

  /* Main entry */
  fprintf(f, "int main(int argc, char **argv) {\n");
  fprintf(f, "  (void)argc; (void)argv;\n");
  fprintf(f, "  g_memory_stats_enabled = getenv(\"SNOVA_MEMORY_STATS\") != NULL;\n");
  if (target->os == SN_OS_WINDOWS) {
    fprintf(f, "  InitializeCriticalSection(&g_pulsar_mutex);\n");
    fprintf(f, "  InitializeCriticalSection(&g_container_mutex);\n");
    fprintf(f, "  InitializeConditionVariable(&g_pulsar_cond);\n");
    fprintf(f, "  unsigned int worker_id = 0;\n");
    fprintf(f, "  HANDLE worker = (HANDLE)_beginthreadex(NULL, 0, pulsar_thread_proc, NULL, 0, &worker_id);\n");
    fprintf(f, "  if (worker) atomic_store_explicit(&g_pulsar_worker_available, 1, memory_order_relaxed); else { fputs(\"runtime error: cannot start Pulsar worker\\n\", stderr); atomic_store_explicit(&g_runtime_failed, 1, memory_order_relaxed); }\n");
  } else {
    fprintf(f, "  pthread_t pth;\n");
    fprintf(f, "  if (pthread_create(&pth, NULL, pulsar_thread_proc, NULL) == 0) atomic_store_explicit(&g_pulsar_worker_available, 1, memory_order_relaxed); else { fputs(\"runtime error: cannot start Pulsar worker\\n\", stderr); atomic_store_explicit(&g_runtime_failed, 1, memory_order_relaxed); }\n");
  }
  fprintf(f, "  Val result = run_function(%u, 0);\n", bc->main_func_idx);
  fprintf(f, "  sn_native_val_release(result);\n");
  if (target->os == SN_OS_WINDOWS) {
    fprintf(f, "  EnterCriticalSection(&g_pulsar_mutex);\n");
    fprintf(f, "  g_pulsar_stop = true;\n");
    fprintf(f, "  WakeConditionVariable(&g_pulsar_cond);\n");
    fprintf(f, "  LeaveCriticalSection(&g_pulsar_mutex);\n");
    fprintf(f, "  if (worker) { WaitForSingleObject(worker, INFINITE); CloseHandle(worker); }\n");
    fprintf(f, "  DeleteCriticalSection(&g_pulsar_mutex);\n");
    fprintf(f, "  DeleteCriticalSection(&g_container_mutex);\n");
  } else {
    fprintf(f, "  pthread_mutex_lock(&g_pulsar_mutex);\n");
    fprintf(f, "  g_pulsar_stop = true;\n");
    fprintf(f, "  pthread_cond_broadcast(&g_pulsar_cond);\n");
    fprintf(f, "  pthread_mutex_unlock(&g_pulsar_mutex);\n");
    fprintf(f, "  if (atomic_load_explicit(&g_pulsar_worker_available, memory_order_relaxed)) pthread_join(pth, NULL);\n");
  }
  fprintf(f, "  if (g_memory_stats_enabled) fprintf(stderr, \"memory[native-values]: objects=%%zu live=%%zu peak=%%zu\\n\", atomic_load_explicit(&g_managed_live_objects, memory_order_relaxed), atomic_load_explicit(&g_managed_live_bytes, memory_order_relaxed), atomic_load_explicit(&g_managed_peak_bytes, memory_order_relaxed));\n");
  fprintf(f, "  int rc = atomic_load_explicit(&g_runtime_failed, memory_order_relaxed) ? 1 : result.tag == VAL_INT ? (int)result.as.i : 0;\n");
  fprintf(f, "  return rc;\n");
  fprintf(f, "}\n");

}
