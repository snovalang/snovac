/* runtime_api.h — stable C ABI for host services used by Snovalang runtimes. */
#ifndef SNOVAC_RUNTIME_API_H
#define SNOVAC_RUNTIME_API_H

#include <stddef.h>
#include "async.h"
#include "pulsar.h"
#include "sn_socket.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Status values are stable and independent of host errno values. */
typedef enum {
    SN_RUNTIME_OK = 0,
    SN_RUNTIME_INVALID_ARGUMENT,
    SN_RUNTIME_NOT_FOUND,
    SN_RUNTIME_PERMISSION_DENIED,
    SN_RUNTIME_IO_ERROR,
    SN_RUNTIME_OUT_OF_MEMORY,
    SN_RUNTIME_END_OF_INPUT
} SnRuntimeStatus;

typedef enum {
    SN_RUNTIME_CALL_READ_LINE = 1,
    SN_RUNTIME_CALL_FILE_EXISTS,
    SN_RUNTIME_CALL_READ_TEXT,
    SN_RUNTIME_CALL_WRITE_TEXT,
    SN_RUNTIME_CALL_ENVIRONMENT,
    SN_RUNTIME_CALL_ARGUMENT_COUNT,
    SN_RUNTIME_CALL_ARGUMENT,
    SN_RUNTIME_CALL_PATH_EXISTS
} SnRuntimeCall;

/* Returned buffers are NUL-terminated and owned by the caller. */
void sn_runtime_free(void *buffer);
SnRuntimeStatus sn_runtime_stdin_line(char **line_out);
SnRuntimeStatus sn_runtime_stdin_all(char **text_out, size_t *length_out);
SnRuntimeStatus sn_runtime_file_read(const char *path, char **text_out,
                                     size_t *length_out);
SnRuntimeStatus sn_runtime_file_write(const char *path, const char *bytes,
                                      size_t length);
SnRuntimeStatus sn_runtime_file_append(const char *path, const char *bytes,
                                       size_t length);
SnRuntimeStatus sn_runtime_file_exists(const char *path, int *exists_out);
SnRuntimeStatus sn_runtime_path_exists(const char *path, int *exists_out);
SnRuntimeStatus sn_runtime_directory_create(const char *path);
SnRuntimeStatus sn_runtime_path_remove(const char *path);

/* Process context is set by the embedding host. Arguments exclude argv[0].
 * The runtime copies the pointers, not the strings; they must remain valid
 * until sn_runtime_process_clear(). */
SnRuntimeStatus sn_runtime_process_init(int argc, const char *const argv[]);
void sn_runtime_process_clear(void);
size_t sn_runtime_args_count(void);
const char *sn_runtime_args_get(size_t index);
const char *sn_runtime_env_get(const char *name);

/* Host-thread mutexes. Handles are opaque integer tokens owned by the runtime. */
int sn_runtime_mutex_create(void);
int sn_runtime_mutex_lock(int handle);
int sn_runtime_mutex_try_lock(int handle);
int sn_runtime_mutex_unlock(int handle);
int sn_runtime_mutex_destroy(int handle);

const char *sn_runtime_status_name(SnRuntimeStatus status);

#ifdef __cplusplus
}
#endif
#endif
