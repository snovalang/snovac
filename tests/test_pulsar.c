#include "pulsar.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sched.h>
#endif

static void yield_cpu(void) {
#ifdef _WIN32
    Sleep(0);
#else
    sched_yield();
#endif
}

typedef struct {
    atomic_uint *cleanups;
    atomic_int *started;
    atomic_int *release;
    int blocks;
} Job;

static void run_job(void *data) {
    Job *job = (Job *)data;
    if (job->blocks) {
        atomic_store(job->started, 1);
        while (!atomic_load(job->release)) yield_cpu();
    }
}

static void cleanup_job(void *data) {
    Job *job = (Job *)data;
    atomic_fetch_add(job->cleanups, 1);
    free(job);
}

static Job *new_job(atomic_uint *cleanups, atomic_int *started,
                    atomic_int *release, int blocks) {
    Job *job = (Job *)calloc(1, sizeof(*job));
    if (job) {
        job->cleanups = cleanups;
        job->started = started;
        job->release = release;
        job->blocks = blocks;
    }
    return job;
}

int main(void) {
    atomic_uint cleanups = 0;
    atomic_int started = 0;
    atomic_int release = 0;
    if (sn_pulsar_pool_create(65) != NULL) return 1;
    SnPulsarPool *pool = sn_pulsar_pool_create(1);
    if (!pool) return 2;

    Job *first = new_job(&cleanups, &started, &release, 1);
    if (!first || !sn_pulsar_pool_submit_owned(pool, run_job, first, cleanup_job)) return 2;
    while (!atomic_load(&started)) yield_cpu();

    for (unsigned i = 0; i < 1024; i++) {
        Job *job = new_job(&cleanups, &started, &release, 0);
        if (!job || !sn_pulsar_pool_submit_owned(pool, run_job, job, cleanup_job)) return 2;
    }
    Job *rejected = new_job(&cleanups, &started, &release, 0);
    if (!rejected || sn_pulsar_pool_submit_owned(pool, run_job, rejected, cleanup_job)) return 1;
    if (atomic_load(&cleanups) != 1) return 1;

    atomic_store(&release, 1);
    sn_pulsar_pool_wait(pool);
    sn_pulsar_pool_destroy(pool);
    if (atomic_load(&cleanups) != 1026) return 1;
    puts("PASS: Pulsar queue rejects task 1025 and cleans every context");
    return 0;
}
