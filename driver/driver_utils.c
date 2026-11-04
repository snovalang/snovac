/* driver_utils.c — shared utilities for snovac CLI driver. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "driver_utils.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#if defined(_WIN32)
#include <windows.h>
#include <direct.h>
#include <io.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#elif defined(__linux__)
#include <unistd.h>
#elif defined(__FreeBSD__)
#include <sys/sysctl.h>
#include <sys/types.h>
#endif

char *read_file(const char *path, size_t *out_len) {
  FILE *f = fopen(path, "rb");
  if (!f) {
    return NULL;
  }
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return NULL;
  }
  long n = ftell(f);
  if (n < 0) {
    fclose(f);
    return NULL;
  }
  rewind(f);
  char *buf = (char *)malloc((size_t)n + 1u);
  if (!buf) {
    fclose(f);
    return NULL;
  }
  size_t got = fread(buf, 1, (size_t)n, f);
  fclose(f);
  buf[got] = '\0';
  *out_len = got;
  return buf;
}

void usage(FILE *out) {
  fprintf(
      out,
      "sncli %s - Snovalang Toolchain & Compiler\n"
      "\n"
      "usage:\n"
      "  sncli --version\n"
      "  sncli --target-info                print detected OS/arch target and "
      "environment overrides\n"
      "  sncli --emit=tokens <file.snova>   dump the token stream\n"
      "  sncli --check-lex   <file.snova>   lex only; exit non-zero on error\n"
      "  sncli --emit=ast    <file.snova>   dump the parse tree\n"
      "  sncli --check-parse <file.snova>   lex+parse; exit non-zero on "
      "error\n"
      "  sncli run           <file.snova>   parse and execute\n"
      "  sncli build         <file.snova> [-o output] [--target=triple]\n"
      "                                       compile to standalone native "
      "executable\n"
      "  sncli check         <file.snova>   resolve + type-check (see llm.md: "
      "coverage is partial - no generics substitution yet)\n"
      "\n"
      "dependencies and package management:\n"
      "  sncli get           [<repo-url>] [--version=<ver>] [--project=<path>]\n"
      "                                       fetch and resolve transitive dependencies\n"
      "                                       into .snovalang/deps, update mod.sno\n"
      "  sncli tidy          [--project] [<path>]\n"
      "                                       prune unused dependencies and sync mod.sno\n"
      "\n"
      "project-wide (every file the program is built from, not just the\n"
      "entry file - the source root is the nearest manifest's src/, its\n"
      "own directory otherwise, plus .snovalang/deps):\n"
      "  sncli --check-parse-project <path>  lex+parse the whole project\n"
      "  sncli check --project       <path>  + resolve and type-check\n"
      "  sncli check --project --no-typecheck <path>\n"
      "                                       everything except body type\n"
      "                                       checking (P4 generics are not\n"
      "                                       substituted yet)\n"
      "  sncli run --project         <path>  parse + execute across every\n"
      "                                       file in the project's own\n"
      "                                       source root (P3 preview; no\n"
      "                                       builtin/deps evaluation)\n"
      "  sncli build --project       <path> [-o output] [--target=triple] [--runtime]\n"
      "                                       compile project into standalone native binary\n"
      "\n"
      "environment variables:\n"
      "  SNOVA_TARGET_OS   target OS override (linux, darwin, windows, "
      "freebsd, ...)\n"
      "  SNOVA_TARGET_ARCH target architecture override (x86_64, aarch64, arm, "
      "...)\n"
      "  SNOVA_TARGET      target triple override (e.g. x86_64-linux-gnu)\n",
      SNOVAC_VERSION);
}

void report_errors(const SnDiagSink *diag, const char *path) {
  if (diag->error_count > 0) {
    fprintf(stderr, "%d error%s in %s\n", diag->error_count,
            diag->error_count == 1 ? "" : "s", path);
  }
}
