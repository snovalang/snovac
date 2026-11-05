/* runtime_api.c — portable process, console, and filesystem host services. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "runtime_api.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#define sn_mkdir(path) _mkdir(path)
#define sn_unlink(path) _unlink(path)
#define sn_rmdir(path) _rmdir(path)
#else
#include <unistd.h>
#define sn_mkdir(path) mkdir((path), 0777)
#define sn_unlink(path) unlink(path)
#define sn_rmdir(path) rmdir(path)
#endif

static const char **g_args;
static size_t g_arg_count;

#define SN_RUNTIME_MUTEX_CAPACITY 1024u
#define SN_RUNTIME_MUTEX_INDEX_BITS 10u
#define SN_RUNTIME_MUTEX_INDEX_MASK ((1u << SN_RUNTIME_MUTEX_INDEX_BITS) - 1u)
#define SN_RUNTIME_MUTEX_GENERATION_MAX ((1u << (31u - SN_RUNTIME_MUTEX_INDEX_BITS)) - 1u)
typedef struct {
    pthread_mutex_t value;
    unsigned generation;
    int active;
} SnRuntimeMutexSlot;
static SnRuntimeMutexSlot g_mutexes[SN_RUNTIME_MUTEX_CAPACITY];
static pthread_mutex_t g_mutex_table_lock = PTHREAD_MUTEX_INITIALIZER;

static SnRuntimeMutexSlot *mutex_slot(int handle) {
    if (handle < 0) return NULL;
    unsigned token = (unsigned)handle;
    unsigned index = token & SN_RUNTIME_MUTEX_INDEX_MASK;
    unsigned generation = token >> SN_RUNTIME_MUTEX_INDEX_BITS;
    if (generation == 0) return NULL;
    SnRuntimeMutexSlot *slot = &g_mutexes[index];
    return slot->active && slot->generation == generation ? slot : NULL;
}

int sn_runtime_mutex_create(void) {
    pthread_mutex_lock(&g_mutex_table_lock);
    for (unsigned i = 0; i < SN_RUNTIME_MUTEX_CAPACITY; i++) {
        SnRuntimeMutexSlot *slot = &g_mutexes[i];
        if (slot->active || slot->generation >= SN_RUNTIME_MUTEX_GENERATION_MAX) continue;
        if (pthread_mutex_init(&slot->value, NULL) != 0) {
            pthread_mutex_unlock(&g_mutex_table_lock);
            return -1;
        }
        slot->generation++;
        slot->active = 1;
        unsigned token = (slot->generation << SN_RUNTIME_MUTEX_INDEX_BITS) | i;
        pthread_mutex_unlock(&g_mutex_table_lock);
        return (int)token;
    }
    pthread_mutex_unlock(&g_mutex_table_lock);
    return -1;
}

int sn_runtime_mutex_lock(int handle) {
    pthread_mutex_lock(&g_mutex_table_lock);
    SnRuntimeMutexSlot *slot = mutex_slot(handle);
    pthread_mutex_unlock(&g_mutex_table_lock);
    return slot ? pthread_mutex_lock(&slot->value) : -1;
}

int sn_runtime_mutex_try_lock(int handle) {
    pthread_mutex_lock(&g_mutex_table_lock);
    SnRuntimeMutexSlot *slot = mutex_slot(handle);
    pthread_mutex_unlock(&g_mutex_table_lock);
    if (!slot) return -1;
    int result = pthread_mutex_trylock(&slot->value);
    return result == 0 ? 1 : result == EBUSY ? 0 : -1;
}

int sn_runtime_mutex_unlock(int handle) {
    pthread_mutex_lock(&g_mutex_table_lock);
    SnRuntimeMutexSlot *slot = mutex_slot(handle);
    pthread_mutex_unlock(&g_mutex_table_lock);
    return slot ? pthread_mutex_unlock(&slot->value) : -1;
}

int sn_runtime_mutex_destroy(int handle) {
    pthread_mutex_lock(&g_mutex_table_lock);
    SnRuntimeMutexSlot *slot = mutex_slot(handle);
    if (!slot || pthread_mutex_destroy(&slot->value) != 0) {
        pthread_mutex_unlock(&g_mutex_table_lock);
        return -1;
    }
    slot->active = 0;
    pthread_mutex_unlock(&g_mutex_table_lock);
    return 0;
}

static SnRuntimeStatus status_from_errno(void) {
    if (errno == ENOENT || errno == ENOTDIR) return SN_RUNTIME_NOT_FOUND;
    if (errno == EACCES || errno == EPERM) return SN_RUNTIME_PERMISSION_DENIED;
    if (errno == ENOMEM) return SN_RUNTIME_OUT_OF_MEMORY;
    return SN_RUNTIME_IO_ERROR;
}

void sn_runtime_free(void *buffer) { free(buffer); }

static SnRuntimeStatus read_stream(FILE *stream, char **out, size_t *length) {
    size_t used = 0, cap = 4096;
    char *buf = (char *)malloc(cap + 1);
    if (!buf) return SN_RUNTIME_OUT_OF_MEMORY;
    for (;;) {
        if (used == cap) {
            if (cap > ((size_t)-1 - 1) / 2) { free(buf); return SN_RUNTIME_OUT_OF_MEMORY; }
            size_t next = cap * 2;
            char *grown = (char *)realloc(buf, next + 1);
            if (!grown) { free(buf); return SN_RUNTIME_OUT_OF_MEMORY; }
            buf = grown;
            cap = next;
        }
        size_t n = fread(buf + used, 1, cap - used, stream);
        used += n;
        if (n == 0) {
            if (ferror(stream)) { free(buf); return status_from_errno(); }
            break;
        }
    }
    buf[used] = '\0';
    *out = buf;
    if (length) *length = used;
    return SN_RUNTIME_OK;
}

SnRuntimeStatus sn_runtime_stdin_line(char **line_out) {
    if (!line_out) return SN_RUNTIME_INVALID_ARGUMENT;
    *line_out = NULL;
    size_t used = 0, cap = 128;
    char *line = (char *)malloc(cap);
    if (!line) return SN_RUNTIME_OUT_OF_MEMORY;
    int c;
    while ((c = fgetc(stdin)) != EOF) {
        if (c == '\n') break;
        if (used + 1 >= cap) {
            if (cap > ((size_t)-1) / 2) { free(line); return SN_RUNTIME_OUT_OF_MEMORY; }
            size_t next = cap * 2;
            char *grown = (char *)realloc(line, next);
            if (!grown) { free(line); return SN_RUNTIME_OUT_OF_MEMORY; }
            line = grown; cap = next;
        }
        line[used++] = (char)c;
    }
    if (c == EOF && used == 0) { free(line); return ferror(stdin) ? status_from_errno() : SN_RUNTIME_END_OF_INPUT; }
    if (used && line[used - 1] == '\r') used--;
    line[used] = '\0';
    *line_out = line;
    return SN_RUNTIME_OK;
}

SnRuntimeStatus sn_runtime_stdin_all(char **text_out, size_t *length_out) {
    if (!text_out) return SN_RUNTIME_INVALID_ARGUMENT;
    *text_out = NULL;
    return read_stream(stdin, text_out, length_out);
}

SnRuntimeStatus sn_runtime_file_read(const char *path, char **text_out,
                                     size_t *length_out) {
    if (!path || !path[0] || !text_out) return SN_RUNTIME_INVALID_ARGUMENT;
    *text_out = NULL;
    FILE *f = fopen(path, "rb");
    if (!f) return status_from_errno();
    SnRuntimeStatus s = read_stream(f, text_out, length_out);
    if (fclose(f) != 0 && s == SN_RUNTIME_OK) { free(*text_out); *text_out = NULL; return status_from_errno(); }
    return s;
}

static SnRuntimeStatus write_file(const char *path, const char *bytes,
                                  size_t length, const char *mode) {
    if (!path || !path[0] || (!bytes && length)) return SN_RUNTIME_INVALID_ARGUMENT;
    FILE *f = fopen(path, mode);
    if (!f) return status_from_errno();
    size_t n = length ? fwrite(bytes, 1, length, f) : 0;
    int bad = n != length || fflush(f) != 0;
    if (fclose(f) != 0) bad = 1;
    return bad ? status_from_errno() : SN_RUNTIME_OK;
}

SnRuntimeStatus sn_runtime_file_write(const char *p, const char *b, size_t n) { return write_file(p, b, n, "wb"); }
SnRuntimeStatus sn_runtime_file_append(const char *p, const char *b, size_t n) { return write_file(p, b, n, "ab"); }

SnRuntimeStatus sn_runtime_path_exists(const char *path, int *exists_out) {
    if (!path || !path[0] || !exists_out) return SN_RUNTIME_INVALID_ARGUMENT;
    struct stat st;
    if (stat(path, &st) == 0) { *exists_out = 1; return SN_RUNTIME_OK; }
    if (errno == ENOENT || errno == ENOTDIR) { *exists_out = 0; return SN_RUNTIME_OK; }
    return status_from_errno();
}

SnRuntimeStatus sn_runtime_file_exists(const char *path, int *exists_out) {
    SnRuntimeStatus s = sn_runtime_path_exists(path, exists_out);
    if (s != SN_RUNTIME_OK || !*exists_out) return s;
    struct stat st;
    if (stat(path, &st) != 0) return status_from_errno();
    *exists_out = S_ISREG(st.st_mode) != 0;
    return SN_RUNTIME_OK;
}

SnRuntimeStatus sn_runtime_directory_create(const char *path) {
    if (!path || !path[0]) return SN_RUNTIME_INVALID_ARGUMENT;
    if (sn_mkdir(path) == 0) return SN_RUNTIME_OK;
    if (errno == EEXIST) {
        struct stat st;
        if (stat(path, &st) == 0 && (st.st_mode & S_IFDIR) != 0) return SN_RUNTIME_OK;
    }
    return status_from_errno();
}

SnRuntimeStatus sn_runtime_path_remove(const char *path) {
    if (!path || !path[0]) return SN_RUNTIME_INVALID_ARGUMENT;
    if (sn_unlink(path) == 0 || sn_rmdir(path) == 0) return SN_RUNTIME_OK;
    return status_from_errno();
}

SnRuntimeStatus sn_runtime_process_init(int argc, const char *const argv[]) {
    if (argc < 0 || (argc && !argv)) return SN_RUNTIME_INVALID_ARGUMENT;
    const char **copy = argc ? (const char **)malloc((size_t)argc * sizeof(*copy)) : NULL;
    if (argc && !copy) return SN_RUNTIME_OUT_OF_MEMORY;
    for (int i = 0; i < argc; i++) {
        if (!argv[i]) { free(copy); return SN_RUNTIME_INVALID_ARGUMENT; }
        copy[i] = argv[i];
    }
    free(g_args); g_args = copy; g_arg_count = (size_t)argc;
    return SN_RUNTIME_OK;
}

void sn_runtime_process_clear(void) { free(g_args); g_args = NULL; g_arg_count = 0; }
size_t sn_runtime_args_count(void) { return g_arg_count; }
const char *sn_runtime_args_get(size_t i) { return i < g_arg_count ? g_args[i] : NULL; }
const char *sn_runtime_env_get(const char *name) { return name && name[0] ? getenv(name) : NULL; }

const char *sn_runtime_status_name(SnRuntimeStatus s) {
    static const char *const names[] = {"ok", "invalid_argument", "not_found", "permission_denied", "io_error", "out_of_memory", "end_of_input"};
    return (unsigned)s < sizeof(names) / sizeof(names[0]) ? names[s] : "unknown";
}
