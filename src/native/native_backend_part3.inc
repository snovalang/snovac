  /* Emit run_function */
  fprintf(f, "static int run_function(size_t fn_idx, const Val *args, uint32_t argc) {\n");
  fprintf(f, "  const uint8_t *code = NULL;\n");
  for (size_t i = 0; i < bc->function_count; i++) {
    fprintf(f, "  if (fn_idx == %zu) code = g_fn_code_%zu;\n", i, i);
  }
  fprintf(f, "  if (!code) {\n");
  fprintf(f, "    fputs(\"error: unknown function\\n\", stderr);\n");
  fprintf(f, "    exit(1);\n");
  fprintf(f, "  }\n");
  fprintf(f, "  const uint8_t *ip = code;\n");
  fprintf(f, "  Val locals[256]; memset(locals, 0, sizeof(locals));\n");
  fprintf(f, "  if (argc > 256) {\n");
  fprintf(f, "    fputs(\"error: too many arguments\\n\", stderr);\n");
  fprintf(f, "    exit(1);\n");
  fprintf(f, "  }\n");
  fprintf(f, "  if (args) {\n");
  fprintf(f, "    for (uint32_t i = 0; i < argc; i++) locals[i] = args[i];\n");
  fprintf(f, "  }\n");
  fprintf(f, "  g_ret.tag = VAL_UNIT; g_ret.as.i = 0;\n");
  fprintf(f, "  DeferItem *defers = NULL;\n");
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
          "    case %d: { Val v; v.tag = VAL_STRING; v.as.str = "
          "(char*)g_strings[read_u32(&ip)]; push(v); break; }\n",
          OP_CONST_STRING);
  fprintf(f,
          "    case %d: { Val v; v.tag = VAL_BOOL; v.as.b = *ip++ != 0; "
          "push(v); break; }\n",
          OP_CONST_BOOL);
  fprintf(
      f,
      "    case %d: { Val v; v.tag = VAL_UNIT; v.as.i = 0; push(v); break; }\n",
      OP_CONST_UNIT);
  fprintf(f, "    case %d: pop(); break;\n", OP_POP);
  fprintf(f, "    case %d: push(peek(0)); break;\n", OP_DUP);
  fprintf(
      f,
      "    case %d: { uint32_t s = read_u32(&ip); push(locals[s]); break; }\n",
      OP_GET_LOCAL);
  fprintf(f,
          "    case %d: { uint32_t s = read_u32(&ip); locals[s] = peek(0); "
          "break; }\n",
          OP_SET_LOCAL);
  fprintf(f,
          "    case %d: { Val b = pop(), a = pop(), r;\n"
          "      if (a.tag == VAL_STRING && b.tag == VAL_STRING) {\n"
          "        size_t la = strlen(a.as.str), lb = strlen(b.as.str);\n"
          "        if (lb > ((size_t)-1) - la - 1) {\n"
          "          fputs(\"error: string concat overflow\\n\", stderr); exit(70);\n"
          "        }\n"
          "        char *buf = malloc(la + lb + 1);\n"
          "        if (!buf) { fputs(\"error: out of memory\\n\", stderr); exit(70); }\n"
          "        memcpy(buf, a.as.str, la); memcpy(buf + la, b.as.str, lb); buf[la + lb] = 0;\n"
          "        r.tag = VAL_STRING; r.as.str = buf;\n"
          "      } else if (a.tag == VAL_DOUBLE || b.tag == VAL_DOUBLE) {\n"
          "        r.tag = VAL_DOUBLE; r.as.d = (a.tag == VAL_DOUBLE ? a.as.d "
          ": a.as.i) + (b.tag == VAL_DOUBLE ? b.as.d : b.as.i);\n"
          "      } else {\n"
          "        r.tag = VAL_INT; r.as.i = a.as.i + b.as.i;\n"
          "      }\n      push(r); break;\n    }\n",
          OP_ADD);
  fprintf(f,
          "    case %d: { Val b = pop(), a = pop(), r; r.tag = VAL_INT; r.as.i "
          "= a.as.i - b.as.i; push(r); break; }\n",
          OP_SUB);
  fprintf(f,
          "    case %d: { Val b = pop(), a = pop(), r; r.tag = VAL_INT; r.as.i "
          "= a.as.i * b.as.i; push(r); break; }\n",
          OP_MUL);
  fprintf(f,
          "    case %d: { Val b = pop(), a = pop(), r; r.tag = VAL_INT; r.as.i "
          "= b.as.i ? a.as.i / b.as.i : 0; push(r); break; }\n",
          OP_DIV);
  fprintf(f,
          "    case %d: { Val b = pop(), a = pop(), r; r.tag = VAL_INT; r.as.i "
          "= b.as.i ? a.as.i %% b.as.i : 0; push(r); break; }\n",
          OP_MOD);
  fprintf(f,
          "    case %d: { Val a = pop(), r; r.tag = VAL_INT; r.as.i = -a.as.i; "
          "push(r); break; }\n",
          OP_NEG);
  fprintf(f,
          "    case %d: { Val b = pop(), a = pop(), r; memset(&r, 0, sizeof(r)); "
          "r.tag = VAL_INT; r.as.i = a.as.i & b.as.i; push(r); break; }\n",
          OP_BIT_AND);
  fprintf(f,
          "    case %d: { Val b = pop(), a = pop(), r; memset(&r, 0, sizeof(r)); "
          "r.tag = VAL_INT; r.as.i = a.as.i | b.as.i; push(r); break; }\n",
          OP_BIT_OR);
  fprintf(f,
          "    case %d: { Val b = pop(), a = pop(), r; memset(&r, 0, sizeof(r)); "
          "r.tag = VAL_INT; r.as.i = a.as.i ^ b.as.i; push(r); break; }\n",
          OP_BIT_XOR);
  fprintf(f,
          "    case %d: { Val b = pop(), a = pop(), r; memset(&r, 0, sizeof(r)); "
          "r.tag = VAL_INT; r.as.i = (int64_t)((uint64_t)a.as.i << (b.as.i & 63)); "
          "push(r); break; }\n",
          OP_SHL);
  fprintf(f,
          "    case %d: { Val b = pop(), a = pop(), r; memset(&r, 0, sizeof(r)); "
          "r.tag = VAL_INT; r.as.i = a.as.i >> (b.as.i & 63); push(r); break; }\n",
          OP_SHR);
  fprintf(f,
          "    case %d: { Val a = pop(), r; memset(&r, 0, sizeof(r)); r.tag = VAL_BOOL; "
          "r.as.b = !val_truthy(a); push(r); break; }\n",
          OP_NOT);
  fprintf(f,
          "    case %d: { Val b = pop(), a = pop(), r; r.tag = VAL_BOOL; "
          "r.as.b = (a.tag != VAL_NULL && b.tag != VAL_NULL && a.as.i == b.as.i); "
          "push(r); break; }\n",
          OP_EQ);
  fprintf(f,
          "    case %d: { Val b = pop(), a = pop(), r; r.tag = VAL_BOOL; "
          "r.as.b = (a.tag == VAL_NULL || b.tag == VAL_NULL || a.as.i != b.as.i); "
          "push(r); break; }\n",
          OP_NE);
  fprintf(f,
          "    case %d: { Val b = pop(), a = pop(), r; r.tag = VAL_BOOL; "
          "r.as.b = (a.as.i < b.as.i); push(r); break; }\n",
          OP_LT);
  fprintf(f,
          "    case %d: { Val b = pop(), a = pop(), r; r.tag = VAL_BOOL; "
          "r.as.b = (a.as.i <= b.as.i); push(r); break; }\n",
          OP_LE);
  fprintf(f,
          "    case %d: { Val b = pop(), a = pop(), r; r.tag = VAL_BOOL; "
          "r.as.b = (a.as.i > b.as.i); push(r); break; }\n",
          OP_GT);
  fprintf(f,
          "    case %d: { Val b = pop(), a = pop(), r; r.tag = VAL_BOOL; "
          "r.as.b = (a.as.i >= b.as.i); push(r); break; }\n",
          OP_GE);
  fprintf(f,
          "    case %d: { int32_t off = (int32_t)read_u32(&ip); ip += off; "
          "break; }\n",
          OP_JUMP);
  fprintf(f,
          "    case %d: { int32_t off = (int32_t)read_u32(&ip); Val cond = pop(); "
          "if (!val_truthy(cond)) ip += off; break; }\n",
          OP_JUMP_IF_FALSE);
  fprintf(f,
          "    case %d: { int32_t off = (int32_t)read_u32(&ip); Val cond = pop(); "
          "if (val_truthy(cond)) ip += off; break; }\n",
          OP_JUMP_IF_TRUE);
  fprintf(f,
          "    case %d: { uint32_t fni = read_u32(&ip); uint32_t argc = read_u32(&ip);\n"
          "      if (argc > 256) { fputs(\"error: too many arguments\\n\", stderr); exit(1); }\n"
          "      Val args_tmp[256]; memset(args_tmp, 0, sizeof(args_tmp));\n"
          "      for (uint32_t ai = argc; ai > 0; ai--) args_tmp[ai - 1] = pop();\n"
          "      run_function(fni, args_tmp, argc); push(g_ret); break; }\n",
          OP_CALL);
  fprintf(f,
          "    case %d: { Val saved = pop(); run_defers(&defers); g_ret = saved; return 0; }\n",
          OP_RETURN);
  fprintf(f,
          "    case %d: { uint32_t cls = read_u32(&ip); uint32_t n = read_u32(&ip); (void)cls;\n"
          "      SnObj *o = (SnObj *)calloc(1, sizeof(SnObj));\n"
          "      if (!o) { fputs(\"error: out of memory\\n\", stderr); exit(1); }\n"
          "      o->count = n;\n"
          "      o->fields = (Val *)calloc(n ? n : 1, sizeof(Val));\n"
          "      if (!o->fields) { fputs(\"error: out of memory\\n\", stderr); exit(1); }\n"
          "      for (uint32_t i = n; i > 0; i--) o->fields[i - 1] = pop();\n"
          "      Val v; memset(&v, 0, sizeof(v)); v.tag = VAL_OBJ; v.as.ptr = o; push(v); break; }\n",
          OP_NEW_OBJ);
  fprintf(f,
          "    case %d: { uint32_t idx = read_u32(&ip); Val obj = pop();\n"
          "      SnObj *o = (obj.tag == VAL_OBJ) ? (SnObj *)obj.as.ptr : NULL;\n"
          "      if (!o || idx >= o->count) { fputs(\"error: field index out of bounds\\n\", stderr); exit(1); }\n"
          "      push(o->fields[idx]); break; }\n",
          OP_GET_FIELD);
  fprintf(f,
          "    case %d: { uint32_t idx = read_u32(&ip); Val obj = pop(); Val v = peek(0);\n"
          "      SnObj *o = (obj.tag == VAL_OBJ) ? (SnObj *)obj.as.ptr : NULL;\n"
          "      if (!o || idx >= o->count) { fputs(\"error: field index out of bounds\\n\", stderr); exit(1); }\n"
          "      o->fields[idx] = v; break; }\n",
          OP_SET_FIELD);
  fprintf(f,
          "    case %d: { uint32_t n = read_u32(&ip);\n"
          "      SnArray *a = (SnArray *)calloc(1, sizeof(SnArray));\n"
          "      if (!a) { fputs(\"error: out of memory\\n\", stderr); exit(1); }\n"
          "      a->count = n;\n"
          "      a->cap = n;\n"
          "      a->items = (Val *)calloc(n ? n : 1, sizeof(Val));\n"
          "      if (!a->items) { fputs(\"error: out of memory\\n\", stderr); exit(1); }\n"
          "      for (uint32_t i = n; i > 0; i--) a->items[i - 1] = pop();\n"
          "      Val v; memset(&v, 0, sizeof(v)); v.tag = VAL_ARRAY; v.as.ptr = a; push(v); break; }\n",
          OP_NEW_ARRAY);
  fprintf(f,
          "    case %d: { Val idx = pop(); Val arr = pop();\n"
          "      SnArray *a = (arr.tag == VAL_ARRAY) ? (SnArray *)arr.as.ptr : NULL;\n"
          "      if (!a || idx.tag != VAL_INT || idx.as.i < 0 || (uint64_t)idx.as.i >= a->count) {\n"
          "        fputs(\"error: array index out of bounds\\n\", stderr); exit(1);\n"
          "      }\n"
          "      push(a->items[(uint32_t)idx.as.i]); break; }\n",
          OP_GET_INDEX);
  fprintf(f,
          "    case %d: { Val path = pop();\n"
          "      if (path.tag != VAL_STRING) { fputs(\"error: read_bytes expects a string path\\n\", stderr); exit(1); }\n"
          "      push(sn_read_bytes(path.as.str)); break; }\n",
          OP_READ_BYTES);
  fprintf(f,
          "    case %d: { Val data = pop(); Val path = pop();\n"
          "      if (path.tag != VAL_STRING) { fputs(\"error: write_bytes expects a string path\\n\", stderr); exit(1); }\n"
          "      push(sn_write_bytes(path.as.str, data)); break; }\n",
          OP_WRITE_BYTES);
  fprintf(f,
          "    case %d: { push(sn_array_len(pop())); break; }\n",
          OP_ARRAY_LEN);
  fprintf(f,
          "    case %d: { Val item = pop(); Val arr = pop(); push(sn_array_push(arr, item)); break; }\n",
          OP_ARRAY_PUSH);
  fprintf(f,
          "    case %d: { uint8_t nl = *ip++; Val v = pop(); print_val(v, nl); break; }\n",
          OP_PRINT);
  fprintf(f, "    case %d: g_ret.tag = VAL_INT; g_ret.as.i = 0; return 0;\n", OP_HALT);
  sn_native_write_scope_ops(f, target->os == SN_OS_WINDOWS);
  fprintf(f, "    default: fprintf(stderr, \"error: unknown opcode %%u\\n\", (unsigned)op); exit(1);\n");
  fprintf(f, "    }\n  }\n}\n\n");

  /* Main entry */
  fprintf(f, "int main(int argc, char **argv) {\n");
  fprintf(f, "  (void)argc; (void)argv;\n");
  if (target->os == SN_OS_WINDOWS) {
    fprintf(f, "  InitializeCriticalSection(&g_pulsar_mutex);\n");
    fprintf(f, "  InitializeConditionVariable(&g_pulsar_cond);\n");
  } else {
    fprintf(f, "  pthread_t pth;\n");
    fprintf(f, "  pthread_create(&pth, NULL, pulsar_thread_proc, NULL);\n");
  }
  fprintf(f, "  run_function(%u, NULL, 0);\n", bc->main_func_idx);
  fprintf(f, "  int rc = (g_ret.tag == VAL_INT) ? (int)g_ret.as.i : 0;\n");
  if (target->os == SN_OS_WINDOWS) {
    fprintf(f, "  EnterCriticalSection(&g_pulsar_mutex);\n");
    fprintf(f, "  g_pulsar_stop = true;\n");
    fprintf(f, "  WakeConditionVariable(&g_pulsar_cond);\n");
    fprintf(f, "  LeaveCriticalSection(&g_pulsar_mutex);\n");
  } else {
    fprintf(f, "  pthread_mutex_lock(&g_pulsar_mutex);\n");
    fprintf(f, "  g_pulsar_stop = true;\n");
    fprintf(f, "  pthread_cond_broadcast(&g_pulsar_cond);\n");
    fprintf(f, "  pthread_mutex_unlock(&g_pulsar_mutex);\n");
    fprintf(f, "  pthread_join(pth, NULL);\n");
  }
  fprintf(f, "  return rc;\n");
  fprintf(f, "}\n");

  fclose(f);

  /* Compile runner C code to final binary using target compiler & flags */
  int status = sn_native_spawn_cc(target, output_path, c_source_path, NULL, NULL);
  remove(c_source_path);
  if (status != 0) {
    fprintf(stderr,
            "error: compilation to native binary failed with exit status %d\n",
            status);
    return 0;
  }

#ifndef _WIN32
  chmod(output_path, 0755);
#endif
  return 1;
}

