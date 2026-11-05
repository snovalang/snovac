/* pulsar.c — Pulsar platform thread multi-parallelism runtime. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "pulsar.h"
#include <pthread.h>
#include <stdlib.h>
#include <stdbool.h>

#define SN_PULSAR_QUEUE_CAPACITY 1024u
#define SN_PULSAR_THREAD_CAPACITY 64u

struct SnPulsarPool {
    pthread_t *threads;
    size_t thread_count;
    SnPulsarTask *queue_head;
    SnPulsarTask *queue_tail;
    pthread_mutex_t lock;
    pthread_cond_t notify;
    pthread_cond_t done;
    bool stop;
    size_t active_tasks;
    size_t queued_tasks;
};

static void *pulsar_worker(void *arg) {
    SnPulsarPool *pool = (SnPulsarPool *)arg;
    for (;;) {
        pthread_mutex_lock(&pool->lock);
        while (!pool->queue_head && !pool->stop) {
            pthread_cond_wait(&pool->notify, &pool->lock);
        }
        if (pool->stop && !pool->queue_head) {
            pthread_mutex_unlock(&pool->lock);
            break;
        }
        SnPulsarTask *task = pool->queue_head;
        if (task) {
            pool->queue_head = task->next;
            pool->queued_tasks--;
            if (!pool->queue_head) pool->queue_tail = NULL;
            pool->active_tasks++;
        }
        pthread_mutex_unlock(&pool->lock);

        if (task) {
            task->fn(task->arg);
            if (task->cleanup) task->cleanup(task->arg);
            free(task);

            pthread_mutex_lock(&pool->lock);
            pool->active_tasks--;
            if (pool->active_tasks == 0 && !pool->queue_head) {
                pthread_cond_signal(&pool->done);
            }
            pthread_mutex_unlock(&pool->lock);
        }
    }
    return NULL;
}

SnPulsarPool *sn_pulsar_pool_create(size_t num_threads) {
    if (num_threads == 0) num_threads = 4;
    if (num_threads > SN_PULSAR_THREAD_CAPACITY) return NULL;
    SnPulsarPool *pool = (SnPulsarPool *)calloc(1, sizeof(SnPulsarPool));
    if (!pool) return NULL;

    pool->thread_count = num_threads;
    pool->threads = (pthread_t *)calloc(num_threads, sizeof(pthread_t));
    if (!pool->threads) {
        free(pool);
        return NULL;
    }
    if (pthread_mutex_init(&pool->lock, NULL) != 0) {
        free(pool->threads);
        free(pool);
        return NULL;
    }
    if (pthread_cond_init(&pool->notify, NULL) != 0) {
        pthread_mutex_destroy(&pool->lock);
        free(pool->threads);
        free(pool);
        return NULL;
    }
    if (pthread_cond_init(&pool->done, NULL) != 0) {
        pthread_cond_destroy(&pool->notify);
        pthread_mutex_destroy(&pool->lock);
        free(pool->threads);
        free(pool);
        return NULL;
    }

    for (size_t i = 0; i < num_threads; i++) {
        if (pthread_create(&pool->threads[i], NULL, pulsar_worker, pool) != 0) {
            pthread_mutex_lock(&pool->lock);
            pool->stop = true;
            pthread_cond_broadcast(&pool->notify);
            pthread_mutex_unlock(&pool->lock);
            for (size_t j = 0; j < i; j++) {
                pthread_join(pool->threads[j], NULL);
            }
            free(pool->threads);
            pthread_mutex_destroy(&pool->lock);
            pthread_cond_destroy(&pool->notify);
            pthread_cond_destroy(&pool->done);
            free(pool);
            return NULL;
        }
    }
    return pool;
}

int sn_pulsar_pool_submit(SnPulsarPool *pool, SnPulsarFn fn, void *arg) {
    return sn_pulsar_pool_submit_owned(pool, fn, arg, NULL);
}

int sn_pulsar_pool_submit_owned(SnPulsarPool *pool, SnPulsarFn fn, void *arg,
                                SnPulsarCleanupFn cleanup) {
    if (!pool || !fn) {
        if (cleanup) cleanup(arg);
        return 0;
    }
    pthread_mutex_lock(&pool->lock);
    if (pool->stop) {
        pthread_mutex_unlock(&pool->lock);
        if (cleanup) cleanup(arg);
        return 0;
    }
    if (pool->queued_tasks >= SN_PULSAR_QUEUE_CAPACITY) {
        pthread_mutex_unlock(&pool->lock);
        if (cleanup) cleanup(arg);
        return 0;
    }
    SnPulsarTask *task = (SnPulsarTask *)malloc(sizeof(SnPulsarTask));
    if (!task) {
        pthread_mutex_unlock(&pool->lock);
        if (cleanup) cleanup(arg);
        return 0;
    }
    task->fn = fn;
    task->arg = arg;
    task->cleanup = cleanup;
    task->next = NULL;
    if (pool->queue_tail) {
        pool->queue_tail->next = task;
        pool->queue_tail = task;
    } else {
        pool->queue_head = pool->queue_tail = task;
    }
    pool->queued_tasks++;
    pthread_cond_signal(&pool->notify);
    pthread_mutex_unlock(&pool->lock);
    return 1;
}

void sn_pulsar_pool_wait(SnPulsarPool *pool) {
    if (!pool) return;
    pthread_mutex_lock(&pool->lock);
    while (pool->queue_head || pool->active_tasks > 0) {
        pthread_cond_wait(&pool->done, &pool->lock);
    }
    pthread_mutex_unlock(&pool->lock);
}

void sn_pulsar_pool_destroy(SnPulsarPool *pool) {
    if (!pool) return;
    pthread_mutex_lock(&pool->lock);
    pool->stop = true;
    pthread_cond_broadcast(&pool->notify);
    pthread_mutex_unlock(&pool->lock);

    for (size_t i = 0; i < pool->thread_count; i++) {
        pthread_join(pool->threads[i], NULL);
    }
    free(pool->threads);
    pthread_mutex_destroy(&pool->lock);
    pthread_cond_destroy(&pool->notify);
    pthread_cond_destroy(&pool->done);
    free(pool);
}
