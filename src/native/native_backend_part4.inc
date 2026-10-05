int sn_native_compile_runtime(const SnPackageGraph *graph,
                              const SnTargetInfo *target,
                              const char *output_path) {
  if (!graph || !target || !output_path)
    return 0;

  sn_driver_ensure_parent_dir(output_path);

  char c_source_path[1024];
  snprintf(c_source_path, sizeof(c_source_path), "%s.rt.c", output_path);

  FILE *f = fopen(c_source_path, "w");
  if (!f) {
    fprintf(stderr, "error: cannot create temporary runtime source %s\n",
            c_source_path);
    return 0;
  }

  fprintf(f, "/* Auto-generated standalone binary with embedded Snovalang "
             "runtime */\n");
  fprintf(f, "#define _POSIX_C_SOURCE 200809L\n");
  fprintf(f, "#define _DEFAULT_SOURCE\n");
  fprintf(f, "#include <stdio.h>\n");
  fprintf(f, "#include <stdlib.h>\n");
  fprintf(f, "#include <string.h>\n");
  fprintf(f, "#include \"arena.h\"\n");
  fprintf(f, "#include \"diag.h\"\n");
  fprintf(f, "#include \"eval.h\"\n");
  fprintf(f, "#include \"lex.h\"\n");
  fprintf(f, "#include \"parse.h\"\n\n");

  size_t file_count = 0;
  for (SnPackageNode *node = graph->nodes; node; node = node->next) {
    for (SnPackageFile *pf = node->files; pf; pf = pf->next) {
      file_count++;
    }
  }

  size_t file_idx = 0;
  for (SnPackageNode *node = graph->nodes; node; node = node->next) {
    for (SnPackageFile *pf = node->files; pf; pf = pf->next, file_idx++) {
      fprintf(f, "static const char g_file_src_%zu[] = {", file_idx);
      for (size_t ci = 0; ci < pf->src_len; ci++) {
        if (ci % 20 == 0)
          fprintf(f, "\n  ");
        fprintf(f, "%d, ", (unsigned char)pf->src[ci]);
      }
      fprintf(f, "0\n};\n\n");
    }
  }

  fprintf(f, "typedef struct { const char *path; const char *src; size_t len; "
             "} EmbeddedFile;\n");
  fprintf(f, "static const EmbeddedFile g_embedded_files[%zu] = {\n",
          file_count ? file_count : 1);
  file_idx = 0;
  for (SnPackageNode *node = graph->nodes; node; node = node->next) {
    for (SnPackageFile *pf = node->files; pf; pf = pf->next, file_idx++) {
      fprintf(f, "  { \"%s\", g_file_src_%zu, %zu },\n",
              pf->path ? pf->path : "", file_idx, pf->src_len);
    }
  }
  if (file_count == 0) {
    fprintf(f, "  { \"\", \"\", 0 }\n");
  }
  fprintf(f, "};\n\n");

  fprintf(f, "int main(int argc, char **argv) {\n");
  fprintf(f, "  (void)argc; (void)argv;\n");
  fprintf(f, "  SnArena arena;\n");
  fprintf(f, "  sn_arena_init(&arena, 8 * 1024 * 1024);\n");
  fprintf(f, "  SnDiagSink diag;\n");
  fprintf(f, "  sn_diag_init(&diag, \"main\", \"\", 0);\n\n");
  fprintf(f, "  SnUnit merged;\n");
  fprintf(f, "  memset(&merged, 0, sizeof(merged));\n\n");
  fprintf(f, "  for (size_t i = 0; i < %zu; i++) {\n", file_count);
  fprintf(f, "    SnDiagFile self = { g_embedded_files[i].path, "
             "g_embedded_files[i].src, g_embedded_files[i].len };\n");
  fprintf(f, "    SnDiagFile outer = sn_diag_set_file(&diag, self);\n");
  fprintf(f, "    SnTokenVec toks;\n");
  fprintf(f, "    sn_lex(&arena, &diag, g_embedded_files[i].src, "
             "g_embedded_files[i].len, &toks);\n");
  fprintf(f, "    SnUnit unit;\n");
  fprintf(f, "    sn_parse(&arena, &diag, &toks, &unit);\n");
  fprintf(f, "    for (size_t di = 0; di < unit.decls.len; di++) {\n");
  fprintf(f, "      SnDecl *d = SN_LIST_AT(unit.decls, SnDecl, di);\n");
  fprintf(f, "      if (d && d->kind == SN_DECL_FUNC && strcmp(d->name, \"main\") == 0) {\n");
  fprintf(f, "        if (strstr(g_embedded_files[i].path, \".snovalang/deps\") != NULL ||\n");
  fprintf(f, "            strstr(g_embedded_files[i].path, \"snova-std\") != NULL) {\n");
  fprintf(f, "          continue; /* Skip library dependency's test main() */\n");
  fprintf(f, "        }\n");
  fprintf(f, "      }\n");
  fprintf(f, "      sn_list_push(&arena, &merged.decls, unit.decls.items[di]);\n");
  fprintf(f, "    }\n");
  fprintf(f, "    sn_diag_set_file(&diag, outer);\n");
  fprintf(f, "  }\n\n");
  fprintf(f, "  sn_eval_merge_extensions(&arena, &merged);\n");
  fprintf(f, "  int rc = sn_eval_run(&arena, &diag, &merged);\n");
  fprintf(f, "  sn_arena_free(&arena);\n");
  fprintf(f, "  return (rc < 0) ? 1 : rc;\n");
  fprintf(f, "}\n");

  fclose(f);

  /* Locate the runtime static lib + headers next to the running binary,
   * trying every layout snovac can plausibly be found in:
   *   - installed:    <prefix>/bin/snl, <prefix>/lib/libsnovart.a,
   *                   <prefix>/include/ (see "make install")
   *   - dev build:    snovac/build/snovac, snovac/build/libsnovart.a,
   *                   headers directly under snovac/
   *   - cwd fallback: used only if sn_driver_exe_dir could not be determined at all
   *                   (see sn_driver_set_exe_dir in driver_utils.c). */
  char inc_dir[1024];
  char lib_path[1024];

  const char *env_home = getenv("SNOVA_HOME");
  if (env_home && env_home[0]) {
    snprintf(inc_dir, sizeof(inc_dir), "%s/include", env_home);
    snprintf(lib_path, sizeof(lib_path), "%s/lib/libsnovart.a", env_home);
  } else if (sn_driver_exe_dir[0]) {
    /* Check sibling snovart directory first for local development */
    snprintf(inc_dir, sizeof(inc_dir), "%s/../../snovart/include", sn_driver_exe_dir);
    snprintf(lib_path, sizeof(lib_path), "%s/../../snovart/build/libsnovart.a", sn_driver_exe_dir);

    if (!sn_driver_path_is_file(lib_path) || !sn_driver_path_is_dir(inc_dir)) {
      /* Installed layout: <prefix>/bin/snl -> <prefix>/include, <prefix>/lib/libsnovart.a */
      snprintf(inc_dir, sizeof(inc_dir), "%s/../include", sn_driver_exe_dir);
      snprintf(lib_path, sizeof(lib_path), "%s/../lib/libsnovart.a", sn_driver_exe_dir);

      if (!sn_driver_path_is_file(lib_path) || !sn_driver_path_is_dir(inc_dir)) {
        /* Dev build layout: exe sits in snovac/build/ */
        snprintf(inc_dir, sizeof(inc_dir), "%s/..", sn_driver_exe_dir);
        snprintf(lib_path, sizeof(lib_path), "%s/libsnovart.a", sn_driver_exe_dir);
        if (!sn_driver_path_is_file(lib_path)) {
          snprintf(lib_path, sizeof(lib_path), "%s/build/libsnovart.a", inc_dir);
        }
      }
    }
  } else {
    snprintf(inc_dir, sizeof(inc_dir), "../snovart/include");
    snprintf(lib_path, sizeof(lib_path), "../snovart/build/libsnovart.a");
    if (!sn_driver_path_is_file(lib_path)) {
      snprintf(inc_dir, sizeof(inc_dir), ".");
      snprintf(lib_path, sizeof(lib_path), "build/libsnovart.a");
    }
  }

  int status = sn_native_spawn_cc(target, output_path, c_source_path, inc_dir,
                                 lib_path);
  remove(c_source_path);
  if (status != 0) {
    fprintf(stderr,
            "error: compilation of standalone runtime binary failed with exit "
            "status %d\n",
            status);
    return 0;
  }

#ifndef _WIN32
  chmod(output_path, 0755);
#endif
  return 1;
}
