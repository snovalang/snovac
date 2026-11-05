/* native_dispatch.c — Minimal OS ABI dispatch for Snovalang runtime (Sockets & OS only) */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "sn_native_dispatch.h"
#include "sn_socket.h"
#include "eval_internal.h"
#include "arena.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

static long long monotonic_nanoseconds(void) {
#ifdef _WIN32
    LARGE_INTEGER ticks, frequency;
    if (!QueryPerformanceCounter(&ticks) || !QueryPerformanceFrequency(&frequency) ||
        frequency.QuadPart <= 0) return 0;
    return (long long)((ticks.QuadPart / frequency.QuadPart) * 1000000000LL +
        (ticks.QuadPart % frequency.QuadPart) * 1000000000LL / frequency.QuadPart);
#else
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    return (long long)now.tv_sec * 1000000000LL + now.tv_nsec;
#endif
}

static void sleep_nanoseconds(long long duration) {
    if (duration <= 0) return;
#ifdef _WIN32
    DWORD milliseconds = (DWORD)((duration + 999999LL) / 1000000LL);
    Sleep(milliseconds);
#else
    struct timespec remaining = {duration / 1000000000LL, duration % 1000000000LL};
    while (nanosleep(&remaining, &remaining) != 0) {}
#endif
}

void sn_runtime_init(void) {
    sn_socket_init();
}

void sn_runtime_shutdown(void) {
    sn_socket_cleanup();
}

/* Dispatch native calls from the evaluator strictly for OS ABI (Socket / NativeNetwork) */
int sn_native_try_dispatch(SnEvalInterp *in, const SnDecl *fn, SnList *args, SnEvalEnv *caller,
                           SnEvalObject *self, SnSpan span, SnEvalValue *out) {
    (void)span;
    /* Only declarations marked as host calls may enter the OS ABI. Wrapper
     * methods such as Net.listen and HttpServer.listen have Snovalang bodies. */
    if (!fn || !fn->name || fn->body) return 0;

    if (!self && strcmp(fn->name, "mutexCreate") == 0) {
        *out = v_int(sn_runtime_mutex_create());
        return 1;
    }
    if (!self && args && args->len >= 1 &&
        (strcmp(fn->name, "mutexLock") == 0 ||
         strcmp(fn->name, "mutexTryLock") == 0 ||
         strcmp(fn->name, "mutexUnlock") == 0 ||
         strcmp(fn->name, "mutexDestroy") == 0)) {
        SnEvalValue vhandle = sn_eval_expr(in, caller, (const SnExpr *)args->items[0]);
        int handle = vhandle.kind == V_INT ? (int)vhandle.as.i : -1;
        if (strcmp(fn->name, "mutexTryLock") == 0) {
            *out = v_bool(sn_runtime_mutex_try_lock(handle) == 1);
        } else {
            int status = strcmp(fn->name, "mutexLock") == 0
                ? sn_runtime_mutex_lock(handle)
                : strcmp(fn->name, "mutexUnlock") == 0
                    ? sn_runtime_mutex_unlock(handle)
                    : sn_runtime_mutex_destroy(handle);
            if (status != 0)
                sn_rt_error(in, SNOVA_TYPE_ERROR, span, "mutex operation failed");
            *out = v_unit();
        }
        return 1;
    }

    /* Time methods are host-boundary operations; the API and value types stay
     * in Snovalang while the platform clock and scheduler sleep live here. */
    if (!self && strcmp(fn->name, "now") == 0) {
        const SnDecl *instant_decl = sn_eval_find_type(in, "Instant");
        if (!instant_decl) return 0;
        SnEvalObject *instant = sn_eval_instantiate(in, instant_decl, NULL, NULL,
                                                    caller, span);
        SnEvalValue *field = sn_eval_object_field(instant, "monotonicNanoseconds");
        if (!field) return 0;
        *field = v_int(monotonic_nanoseconds());
        out->kind = V_OBJECT;
        out->as.o = instant;
        return 1;
    }
    if (!self && strcmp(fn->name, "sleep") == 0 && args && args->len >= 1) {
        SnEvalValue duration = sn_eval_expr(in, caller, (const SnExpr *)args->items[0]);
        if (duration.kind == V_OBJECT && duration.as.o) {
            SnEvalValue *field = sn_eval_object_field(duration.as.o, "nanoseconds");
            if (field && field->kind == V_INT) sleep_nanoseconds(field->as.i);
        }
        *out = v_unit();
        return 1;
    }

    /* Socket / NativeNetwork static methods: minimal OS ABI */
    if (self == NULL || (self->cls && (strcmp(self->cls->name, "Socket") == 0 ||
                                       strcmp(self->cls->name, "NativeNetwork") == 0))) {
        if (strcmp(fn->name, "listen") == 0 && args && args->len >= 2) {
            SnEvalValue vhost = sn_eval_expr(in, caller, (const SnExpr *)args->items[0]);
            SnEvalValue vport = sn_eval_expr(in, caller, (const SnExpr *)args->items[1]);
            const char *host = (vhost.kind == V_STRING && vhost.as.s) ? vhost.as.s : "0.0.0.0";
            int port = (vport.kind == V_INT) ? (int)vport.as.i : 8080;
            int handle = sn_socket_listen(host, port);
            *out = v_int(handle);
            return 1;
        }

        if (strcmp(fn->name, "connect") == 0 && args && args->len >= 2) {
            SnEvalValue vhost = sn_eval_expr(in, caller, (const SnExpr *)args->items[0]);
            SnEvalValue vport = sn_eval_expr(in, caller, (const SnExpr *)args->items[1]);
            const char *host = (vhost.kind == V_STRING && vhost.as.s) ? vhost.as.s : NULL;
            int port = (vport.kind == V_INT) ? (int)vport.as.i : 0;
            *out = v_int(sn_socket_connect(host, port));
            return 1;
        }

        if (strcmp(fn->name, "accept") == 0 && args && args->len >= 1) {
            SnEvalValue vhandle = sn_eval_expr(in, caller, (const SnExpr *)args->items[0]);
            int shandle = (vhandle.kind == V_INT) ? (int)vhandle.as.i : -1;
            char ip[64] = {0};
            int client_handle = sn_socket_accept(shandle, ip, sizeof(ip));
            *out = v_int(client_handle);
            return 1;
        }

        if (strcmp(fn->name, "receive") == 0 && args && args->len >= 2) {
            SnEvalValue vhandle = sn_eval_expr(in, caller, (const SnExpr *)args->items[0]);
            SnEvalValue vlen = sn_eval_expr(in, caller, (const SnExpr *)args->items[1]);
            int handle = (vhandle.kind == V_INT) ? (int)vhandle.as.i : -1;
            size_t max_len = (vlen.kind == V_INT && vlen.as.i > 0) ? (size_t)vlen.as.i : 4096;
            char *buf = (char *)malloc(max_len + 1);
            if (!buf) {
                *out = v_str("");
                return 1;
            }
            int64_t n = sn_socket_receive(handle, buf, max_len);
            if (n <= 0) {
                free(buf);
                *out = v_str("");
                return 1;
            }
            buf[n] = '\0';
            char *interned = sn_arena_strndup(in->arena, buf, (size_t)n);
            free(buf);
            *out = v_str(interned);
            return 1;
        }

        if (strcmp(fn->name, "send") == 0 && args && args->len >= 2) {
            SnEvalValue vhandle = sn_eval_expr(in, caller, (const SnExpr *)args->items[0]);
            SnEvalValue vdata = sn_eval_expr(in, caller, (const SnExpr *)args->items[1]);
            int handle = (vhandle.kind == V_INT) ? (int)vhandle.as.i : -1;
            const char *data = (vdata.kind == V_STRING && vdata.as.s) ? vdata.as.s : "";
            int64_t sent = sn_socket_send(handle, data, strlen(data));
            *out = v_int(sent);
            return 1;
        }

        if (strcmp(fn->name, "udpBind") == 0 && args && args->len >= 2) {
            SnEvalValue vhost = sn_eval_expr(in, caller, (const SnExpr *)args->items[0]);
            SnEvalValue vport = sn_eval_expr(in, caller, (const SnExpr *)args->items[1]);
            const char *host = (vhost.kind == V_STRING && vhost.as.s) ? vhost.as.s : NULL;
            int port = (vport.kind == V_INT) ? (int)vport.as.i : 0;
            *out = v_int(sn_socket_udp_bind(host, port));
            return 1;
        }

        if (strcmp(fn->name, "udpSendTo") == 0 && args && args->len >= 4) {
            SnEvalValue vhandle = sn_eval_expr(in, caller, (const SnExpr *)args->items[0]);
            SnEvalValue vhost = sn_eval_expr(in, caller, (const SnExpr *)args->items[1]);
            SnEvalValue vport = sn_eval_expr(in, caller, (const SnExpr *)args->items[2]);
            SnEvalValue vdata = sn_eval_expr(in, caller, (const SnExpr *)args->items[3]);
            int handle = (vhandle.kind == V_INT) ? (int)vhandle.as.i : -1;
            const char *host = (vhost.kind == V_STRING && vhost.as.s) ? vhost.as.s : NULL;
            int port = (vport.kind == V_INT) ? (int)vport.as.i : 0;
            const char *data = (vdata.kind == V_STRING && vdata.as.s) ? vdata.as.s : "";
            *out = v_int(sn_socket_udp_send_to(handle, host, port, data, strlen(data)));
            return 1;
        }

        if (strcmp(fn->name, "udpReceive") == 0 && args && args->len >= 2) {
            SnEvalValue vhandle = sn_eval_expr(in, caller, (const SnExpr *)args->items[0]);
            SnEvalValue vlen = sn_eval_expr(in, caller, (const SnExpr *)args->items[1]);
            int handle = (vhandle.kind == V_INT) ? (int)vhandle.as.i : -1;
            size_t max_len = (vlen.kind == V_INT && vlen.as.i > 0) ? (size_t)vlen.as.i : 4096;
            char *buffer = (char *)malloc(max_len + 1);
            if (!buffer) { *out = v_str(""); return 1; }
            int64_t n = sn_socket_udp_receive(handle, buffer, max_len);
            if (n <= 0) {
                free(buffer);
                *out = v_str("");
                return 1;
            }
            buffer[n] = '\0';
            const char *copy = sn_arena_strndup(in->arena, buffer, (size_t)n);
            free(buffer);
            *out = v_str(copy);
            return 1;
        }

        if (strcmp(fn->name, "resolve") == 0 && args && args->len >= 1) {
            SnEvalValue vhost = sn_eval_expr(in, caller, (const SnExpr *)args->items[0]);
            char **addresses = NULL;
            size_t count = 0;
            if (vhost.kind == V_STRING &&
                sn_socket_resolve(vhost.as.s, &addresses, &count) == 0) {
                SnEvalValue result;
                result.kind = V_ARRAY;
                result.as.arr = (ArrayVal *)sn_rt_alloc(in, sizeof(ArrayVal), 0);
                memset(result.as.arr, 0, sizeof(ArrayVal));
                for (size_t i = 0; i < count; i++) {
                    sn_rt_array_push(in, result.as.arr,
                        v_str(sn_arena_strndup(in->arena, addresses[i], strlen(addresses[i]))));
                }
                sn_socket_free_addresses(addresses, count);
                *out = result;
            } else {
                SnEvalValue result;
                result.kind = V_ARRAY;
                result.as.arr = (ArrayVal *)sn_rt_alloc(in, sizeof(ArrayVal), 0);
                memset(result.as.arr, 0, sizeof(ArrayVal));
                *out = result;
            }
            return 1;
        }

        if (strcmp(fn->name, "close") == 0 && args && args->len >= 1) {
            SnEvalValue vhandle = sn_eval_expr(in, caller, (const SnExpr *)args->items[0]);
            int handle = (vhandle.kind == V_INT) ? (int)vhandle.as.i : -1;
            sn_socket_close(handle);
            *out = v_unit();
            return 1;
        }
    }

    return 0;
}
