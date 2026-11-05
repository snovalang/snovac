/* async.c — async/await asynchronous execution and event loop. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "async.h"
#include <stdlib.h>
#include <string.h>

void sn_async_loop_init(SnAsyncLoop *loop) {
    memset(loop, 0, sizeof(*loop));
    loop->next_id = 1;
}

SnAsyncTask *sn_async_spawn(SnAsyncLoop *loop, SnAsyncStepFn step, void *user_data) {
    return sn_async_spawn_owned(loop, step, user_data, NULL);
}

SnAsyncTask *sn_async_spawn_owned(SnAsyncLoop *loop, SnAsyncStepFn step,
                                  void *user_data, SnAsyncCleanupFn cleanup) {
    if (!loop || !step) {
        if (cleanup) cleanup(user_data);
        return NULL;
    }
    SnAsyncTask *task = (SnAsyncTask *)calloc(1, sizeof(SnAsyncTask));
    if (!task) {
        if (cleanup) cleanup(user_data);
        return NULL;
    }

    task->id = loop->next_id++;
    task->state = SN_TASK_PENDING;
    task->step = step;
    task->user_data = user_data;
    task->cleanup = cleanup;

    if (loop->tail) {
        loop->tail->next = task;
        loop->tail = task;
    } else {
        loop->head = loop->tail = task;
    }
    return task;
}

void sn_async_loop_run(SnAsyncLoop *loop) {
    if (!loop) return;
    for (;;) {
        bool pending = false;
        SnAsyncTask *prev = NULL;
        SnAsyncTask *cur = loop->head;
        while (cur) {
            if (cur->state == SN_TASK_PENDING || cur->state == SN_TASK_RUNNING) {
                cur->state = SN_TASK_RUNNING;
                cur->step(cur);
            }
            SnAsyncTask *next = cur->next;
            if (cur->state == SN_TASK_COMPLETED || cur->state == SN_TASK_FAILED) {
                if (prev) prev->next = next;
                else loop->head = next;
                if (loop->tail == cur) loop->tail = prev;
                if (cur->cleanup) cur->cleanup(cur->user_data);
                free(cur);
            } else {
                pending = true;
                prev = cur;
            }
            cur = next;
        }
        if (!pending) break;
    }
}

void sn_async_loop_cleanup(SnAsyncLoop *loop) {
    if (!loop) return;
    SnAsyncTask *cur = loop->head;
    while (cur) {
        SnAsyncTask *next = cur->next;
        if (cur->cleanup) cur->cleanup(cur->user_data);
        free(cur);
        cur = next;
    }
    loop->head = loop->tail = NULL;
}
