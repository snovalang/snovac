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
#include <direct.h>
#else
#include <unistd.h>
#endif

int find_builtin_root(const char *start_dir, char *out, size_t out_sz) {
  const char *env_std = getenv("SNOVA_STD_PATH");
  if (env_std && env_std[0] && path_is_dir(env_std)) {
    normalize_path_into(env_std, out, out_sz);
    return 1;
  }

  /* 1. Prefer canonical installed standard library (~/.snovalang/std/src) */
#if defined(_WIN32)
  const char *home = getenv("USERPROFILE");
#else
  const char *home = getenv("HOME");
#endif
  if (home && home[0]) {
    char installed_std[SNOVAC_PATH_MAX + 128];
    snprintf(installed_std, sizeof(installed_std), "%s/.snovalang/std/src", home);
    if (path_is_dir(installed_std)) {
      normalize_path_into(installed_std, out, out_sz);
      return 1;
    }
  }

  /* 2. Check project dependency .snovalang/deps/github.com/supernovalang/snova-std/src */
  if (start_dir && start_dir[0]) {
    char dep_std[SNOVAC_PATH_MAX + 128];
    snprintf(dep_std, sizeof(dep_std), "%s/.snovalang/deps/github.com/supernovalang/snova-std/src", start_dir);
    if (path_is_dir(dep_std)) {
      normalize_path_into(dep_std, out, out_sz);
      return 1;
    }
  }

  /* 3. Sibling/development directory search (normalized, no relative .. in output) */
  char cur[SNOVAC_PATH_MAX + 64];
  normalize_path_into(start_dir, cur, sizeof(cur));
  for (int i = 0; i < 8; i++) {
    char candidate_std[SNOVAC_PATH_MAX + 128];
    snprintf(candidate_std, sizeof(candidate_std), "%s/snova-std/src", cur);
    if (path_is_dir(candidate_std)) {
      normalize_path_into(candidate_std, out, out_sz);
      return 1;
    }

    char candidate_std_direct[SNOVAC_PATH_MAX + 128];
    snprintf(candidate_std_direct, sizeof(candidate_std_direct), "%s/src", cur);
    char test_file[SNOVAC_PATH_MAX + 160];
    snprintf(test_file, sizeof(test_file), "%s/Snova/Std", candidate_std_direct);
    if (path_is_dir(test_file)) {
      normalize_path_into(candidate_std_direct, out, out_sz);
      return 1;
    }

    char candidate[SNOVAC_PATH_MAX + 128];
    snprintf(candidate, sizeof(candidate), "%s/builtin", cur);
    struct stat st;
    if (stat(candidate, &st) == 0 && S_ISDIR(st.st_mode)) {
      normalize_path_into(candidate, out, out_sz);
      return 1;
    }
    char parent[SNOVAC_PATH_MAX + 64];
    snprintf(parent, sizeof(parent), "%s/..", cur);
    normalize_path_into(parent, cur, sizeof(cur));
  }

  return 0;
}

void dirname_into(const char *path, char *out, size_t out_sz) {
  const char *slash = strrchr(path, '/');
#if defined(_WIN32)
  const char *bslash = strrchr(path, '\\');
  if (bslash && (!slash || bslash > slash)) {
    slash = bslash;
  }
#endif
  if (!slash) {
    snprintf(out, out_sz, ".");
    return;
  }
  size_t n = (size_t)(slash - path);
  if (n == 0) {
    n = 1; /* "/" or "\" */
  }
#if defined(_WIN32)
  if (n == 2 && path[1] == ':') {
    n = 3;
  }
#endif
  if (n >= out_sz) {
    n = out_sz - 1;
  }
  memcpy(out, path, n);
  out[n] = '\0';
}

int path_is_dir(const char *path) {
  if (!path || !path[0]) return 0;
  char norm[SNOVAC_PATH_MAX];
  normalize_path_into(path, norm, sizeof(norm));
  struct stat st;
  return stat(norm, &st) == 0 && S_ISDIR(st.st_mode);
}

int path_is_file(const char *path) {
  if (!path || !path[0]) return 0;
  char norm[SNOVAC_PATH_MAX];
  normalize_path_into(path, norm, sizeof(norm));
  struct stat st;
  return stat(norm, &st) == 0 && S_ISREG(st.st_mode);
}

/* Recursively creates `dir` and any missing parent directories (mkdir -p). */
static int mkdir_p(const char *dir) {
  if (!dir || !dir[0] || strcmp(dir, ".") == 0 || strcmp(dir, "/") == 0 ||
      strcmp(dir, "\\") == 0) {
    return 1;
  }
#if defined(_WIN32)
  if (strlen(dir) == 2 && dir[1] == ':') {
    return 1;
  }
  if (strlen(dir) == 3 && dir[1] == ':' && (dir[2] == '/' || dir[2] == '\\')) {
    return 1;
  }
#endif
  struct stat st;
  if (stat(dir, &st) == 0) {
    return S_ISDIR(st.st_mode);
  }

  char parent[SNOVAC_PATH_MAX];
  dirname_into(dir, parent, sizeof(parent));
  if (strcmp(parent, dir) != 0) {
    if (!mkdir_p(parent)) {
      return 0;
    }
  }

#if defined(_WIN32)
  if (_mkdir(dir) != 0 && errno != EEXIST) {
    return 0;
  }
#else
  if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
    return 0;
  }
#endif
  return 1;
}

/* Ensures the parent directory of `path` exists, creating it (and any
 * missing ancestors) if necessary. Used before writing output/temporary
 * build artifacts so callers don't have to pre-create -o's directory. */
void ensure_parent_dir_exists(const char *path) {
  char dir[SNOVAC_PATH_MAX];
  dirname_into(path, dir, sizeof(dir));
  mkdir_p(dir);
}

void normalize_path_into(const char *path, char *out, size_t out_sz) {
  if (!path || !path[0]) {
    if (out && out_sz > 0) out[0] = '\0';
    return;
  }
  char resolved[SNOVAC_PATH_MAX];
#if defined(_WIN32)
  const char *src = path;
  char msys_buf[SNOVAC_PATH_MAX];
  /* Handle MSYS/Git-Bash/Cygwin absolute paths like "/c/Users/..." -> "C:/Users/..." */
  if (path[0] == '/' && isalpha((unsigned char)path[1]) && (path[2] == '/' || path[2] == '\0')) {
    snprintf(msys_buf, sizeof(msys_buf), "%c:%s", (char)toupper((unsigned char)path[1]), path + 2);
    src = msys_buf;
  }
  if (_fullpath(resolved, src, sizeof(resolved)) != NULL) {
    for (char *p = resolved; *p; p++) {
      if (*p == '\\') {
        *p = '/';
      }
    }
    snprintf(out, out_sz, "%s", resolved);
  } else if (out != path) {
    snprintf(out, out_sz, "%s", src);
  }
#else
  if (realpath(path, resolved) != NULL) {
    snprintf(out, out_sz, "%s", resolved);
  } else if (out != path) {
    snprintf(out, out_sz, "%s", path);
  }
#endif
}
