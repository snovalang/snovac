/* native_backend.c — Native executable compiler backend for Snovalang. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "native_backend.h"
#include "native_scope.h"
#include "driver_utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Split whitespace-separated flags into argv slots. `storage` is mutated.
 * Returns 0 when the flag list does not fit. */
static int append_ws_args(char *storage, char **argv, int *argc, int max_argc) {
  if (!storage || !storage[0]) {
    return 1;
  }
  for (char *tok = strtok(storage, " \t"); tok; tok = strtok(NULL, " \t")) {
    if (*argc >= max_argc) {
      return 0;
    }
    argv[(*argc)++] = tok;
  }
  return 1;
}

static int sn_native_spawn_cc(const SnTargetInfo *target, const char *output_path,
                              const char *c_source_path, const char *inc_dir,
                              const char *lib_path) {
  char cflags_buf[256];
  char ldflags_buf[256];
  snprintf(cflags_buf, sizeof(cflags_buf), "%s", target->cflags);
  snprintf(ldflags_buf, sizeof(ldflags_buf), "%s", target->ldflags);

  char *argv[96];
  int argc = 0;
  argv[argc++] = (char *)target->c_compiler;
  if (!append_ws_args(cflags_buf, argv, &argc, 80)) {
    fprintf(stderr, "error: compiler flag list is too long\n");
    return -1;
  }
  if (inc_dir && inc_dir[0]) {
    argv[argc++] = "-I";
    argv[argc++] = (char *)inc_dir;
  }
  argv[argc++] = "-o";
  argv[argc++] = (char *)output_path;
  argv[argc++] = (char *)c_source_path;
  if (lib_path && lib_path[0]) {
    argv[argc++] = (char *)lib_path;
  }
  if (!append_ws_args(ldflags_buf, argv, &argc, 94)) {
    fprintf(stderr, "error: linker flag list is too long\n");
    return -1;
  }
  argv[argc] = NULL;
  return sn_driver_execv(argv);
}

