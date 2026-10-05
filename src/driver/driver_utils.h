/* driver_utils.h — shared utilities for snovac CLI driver. */
#ifndef SNOVAC_DRIVER_UTILS_H
#define SNOVAC_DRIVER_UTILS_H

#include "diag.h"
#include <stddef.h>
#include <stdio.h>

#ifndef SNOVAC_VERSION
#define SNOVAC_VERSION "0.0.1-p3"
#endif

#define SNOVAC_PATH_MAX 1024

extern char sn_driver_exe_dir[SNOVAC_PATH_MAX];

void sn_driver_set_exe_dir(const char *argv0);
char *sn_driver_read_file(const char *path, size_t *out_len);
void sn_driver_usage(FILE *out);
void sn_driver_report_errors(const SnDiagSink *diag, const char *path);
void sn_driver_dirname_into(const char *path, char *out, size_t out_sz);
void sn_driver_normalize_path(const char *path, char *out, size_t out_sz);
int sn_driver_path_is_dir(const char *path);
int sn_driver_path_is_file(const char *path);
/* Case-sensitive. The suffix is read from the basename after '/' or '\\'. */
int sn_driver_path_is_source(const char *path); /* .snl */
int sn_driver_path_is_script(const char *path); /* .sns, except the manifests mod.sns and snova.sns */
int sn_driver_path_is_manifest(const char *path); /* mod.sns or snova.sns */
void sn_driver_ensure_parent_dir(const char *path);
int sn_driver_find_builtin_root(const char *start_dir, char *out, size_t out_sz);

/* Run argv[0] with argv, no shell. Returns the process exit code, or -1 if
 * the process could not be started. stdio is inherited (compiler errors stay
 * visible). */
int sn_driver_execv(char *const argv[]);
/* Same, but captures stdout into `out` (NUL-terminated, truncated to out_sz)
 * and sends the child's stderr to the platform null device. `out` may be
 * NULL to discard stdout. */
int sn_driver_execv_read(char *const argv[], char *out, size_t out_sz);

#endif /* SNOVAC_DRIVER_UTILS_H */
