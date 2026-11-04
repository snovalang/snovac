#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#include "driver_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

char g_exe_dir[SNOVAC_PATH_MAX] = {0};

/* Resolves the absolute path of the currently running executable using the
 * most reliable OS-specific API available. Returns 1 and fills `out` on
 * success, 0 otherwise (leaving `out` untouched). This does NOT depend on
 * argv[0], which is unreliable: when a binary is found via $PATH (e.g.
 * `snovac build ...` after `make install`), the shell execve()s the
 * resolved path but usually still passes argv[0] as the bare command name
 * typed by the user, with no '/' in it at all. */
static int resolve_exe_path(char *out, size_t out_sz) {
#if defined(_WIN32)
  DWORD n = GetModuleFileNameA(NULL, out, (DWORD)out_sz);
  if (n > 0 && n < out_sz) {
    for (size_t i = 0; i < (size_t)n; i++) {
      if (out[i] == '\\') {
        out[i] = '/';
      }
    }
    return 1;
  }
  return 0;
#elif defined(__APPLE__)
  char tmp[SNOVAC_PATH_MAX];
  uint32_t tmp_size = (uint32_t)sizeof(tmp);
  if (_NSGetExecutablePath(tmp, &tmp_size) != 0) {
    return 0;
  }
  if (realpath(tmp, out) == NULL) {
    snprintf(out, out_sz, "%s", tmp);
  }
  return 1;
#elif defined(__linux__)
  ssize_t n = readlink("/proc/self/exe", out, out_sz - 1);
  if (n < 0) {
    return 0;
  }
  out[n] = '\0';
  return 1;
#elif defined(__FreeBSD__)
  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, -1};
  size_t len = out_sz;
  if (sysctl(mib, 4, out, &len, NULL, 0) != 0) {
    return 0;
  }
  return 1;
#else
  (void)out;
  (void)out_sz;
  return 0;
#endif
}

/* Last-resort fallback for platforms without a "current executable" API:
 * manually search $PATH for argv0, the way a POSIX shell would. */
static int resolve_exe_path_via_path_env(const char *argv0, char *out,
                                         size_t out_sz) {
  if (!argv0 || !argv0[0]) {
    return 0;
  }
  const char *path_env = getenv("PATH");
  if (!path_env) {
    return 0;
  }
  const char *p = path_env;
  while (p && *p) {
#if defined(_WIN32)
    const char *sep = strchr(p, ';');
#else
    const char *sep = strchr(p, ':');
#endif
    size_t len = sep ? (size_t)(sep - p) : strlen(p);
    if (len > 0 && len < SNOVAC_PATH_MAX - 1) {
      char dir[SNOVAC_PATH_MAX];
      memcpy(dir, p, len);
      dir[len] = '\0';
      char candidate[SNOVAC_PATH_MAX];
      snprintf(candidate, sizeof(candidate), "%s/%s", dir, argv0);
      if (path_is_file(candidate)) {
        normalize_path_into(candidate, out, out_sz);
        return 1;
      }
#if defined(_WIN32)
      snprintf(candidate, sizeof(candidate), "%s/%s.exe", dir, argv0);
      if (path_is_file(candidate)) {
        normalize_path_into(candidate, out, out_sz);
        return 1;
      }
#endif
    }
    p = sep ? sep + 1 : NULL;
  }
  return 0;
}

void sn_set_exe_dir(const char *argv0) {
  char exe_path[SNOVAC_PATH_MAX];

  if (resolve_exe_path(exe_path, sizeof(exe_path)) && exe_path[0]) {
    dirname_into(exe_path, g_exe_dir, sizeof(g_exe_dir));
    return;
  }

  if (argv0 && (strchr(argv0, '/') || strchr(argv0, '\\'))) {
    normalize_path_into(argv0, exe_path, sizeof(exe_path));
    dirname_into(exe_path, g_exe_dir, sizeof(g_exe_dir));
    return;
  }

  if (resolve_exe_path_via_path_env(argv0, exe_path, sizeof(exe_path))) {
    dirname_into(exe_path, g_exe_dir, sizeof(g_exe_dir));
    return;
  }
}

