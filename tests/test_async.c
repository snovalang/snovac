#include "async.h"

#include <stdio.h>
#include <stdlib.h>

typedef struct {
    unsigned steps;
    unsigned *cleanups;
} TestJob;

static unsigned failures;

static void step(SnAsyncTask *task) {
    TestJob *job = (TestJob *)task->user_data;
    if (++job->steps == 2) task->state = SN_TASK_COMPLETED;
}

static void cleanup(void *data) {
    TestJob *job = (TestJob *)data;
    (*job->cleanups)++;
    free(job);
}

static void check(int ok, const char *message) {
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", message);
        failures++;
    }
}

int main(void) {
    SnAsyncLoop loop;
    unsigned cleanups = 0;
    sn_async_loop_init(&loop);

    for (unsigned i = 0; i < 10000; i++) {
        TestJob *job = (TestJob *)calloc(1, sizeof(*job));
        if (!job) return 2;
        job->cleanups = &cleanups;
        check(sn_async_spawn_owned(&loop, step, job, cleanup) != NULL,
              "task submission succeeds");
        sn_async_loop_run(&loop);
        check(loop.head == NULL && loop.tail == NULL,
              "completed tasks are reaped from a reused loop");
    }

    check(cleanups == 10000, "each completed task is cleaned exactly once");
    sn_async_loop_cleanup(&loop);
    check(cleanups == 10000, "cleanup does not revisit reaped tasks");
    if (failures) return 1;
    puts("PASS: async loop reaps 10000 reused tasks");
    return 0;
}
