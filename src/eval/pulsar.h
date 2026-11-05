/* pulsar.h — Pulsar platform thread multi-parallelism runtime. */
#ifndef SNOVAC_PULSAR_H
#define SNOVAC_PULSAR_H

#include <stddef.h>
#include <stdint.h>

typedef void (*SnPulsarFn)(void *arg);
typedef void (*SnPulsarCleanupFn)(void *arg);

typedef struct SnPulsarTask {
    SnPulsarFn fn;
    void *arg;
    SnPulsarCleanupFn cleanup;
    struct SnPulsarTask *next;
} SnPulsarTask;

typedef struct SnPulsarPool SnPulsarPool;

SnPulsarPool *sn_pulsar_pool_create(size_t num_threads);
int sn_pulsar_pool_submit(SnPulsarPool *pool, SnPulsarFn fn, void *arg);
/* Transfers arg ownership to the pool. cleanup runs exactly once after fn,
 * or before returning when the task cannot be queued. */
int sn_pulsar_pool_submit_owned(SnPulsarPool *pool, SnPulsarFn fn, void *arg,
                               SnPulsarCleanupFn cleanup);
void sn_pulsar_pool_wait(SnPulsarPool *pool);
void sn_pulsar_pool_destroy(SnPulsarPool *pool);

#endif /* SNOVAC_PULSAR_H */
