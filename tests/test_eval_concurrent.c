#include "arena.h"
#include "diag.h"
#include "eval.h"
#include "lex.h"
#include "parse.h"

#include <pthread.h>
#include <stdio.h>
#include <stdatomic.h>

#define THREADS 8
#define RUNS_PER_THREAD 40

static const char SOURCE[] =
    "package tests.concurrent\n"
    "func main(): int {\n"
    "  var values = [1, 2, 3]\n"
    "  var i = 0\n"
    "  while i < 8 { values = [i, i + 1, i + 2]; i = i + 1 }\n"
    "  return 42\n"
    "}\n";

static atomic_uint failures;

static void *run_many(void *unused) {
    (void)unused;
    for (unsigned i = 0; i < RUNS_PER_THREAD; i++) {
        SnArena arena;
        SnDiagSink diag;
        SnTokenVec tokens = {0};
        SnUnit unit = {0};
        sn_arena_init(&arena, 4096);
        sn_diag_init(&diag, "concurrent.snl", SOURCE, sizeof(SOURCE) - 1u);
        int rc = sn_lex(&arena, &diag, SOURCE, sizeof(SOURCE) - 1u, &tokens);
        if (rc == 0) rc = sn_parse(&arena, &diag, &tokens, &unit);
        if (rc == 0 && sn_eval_run(&arena, &diag, &unit) != 42) rc = 1;
        if (rc != 0 || diag.error_count != 0) atomic_fetch_add(&failures, 1);
        sn_arena_free(&arena);
    }
    return NULL;
}

int main(void) {
    pthread_t threads[THREADS];
    for (unsigned i = 0; i < THREADS; i++) {
        if (pthread_create(&threads[i], NULL, run_many, NULL) != 0) return 2;
    }
    for (unsigned i = 0; i < THREADS; i++) pthread_join(threads[i], NULL);
    if (atomic_load(&failures)) {
        fprintf(stderr, "FAIL: %u concurrent evaluator runs failed\n",
                atomic_load(&failures));
        return 1;
    }
    puts("PASS: evaluator serializes 320 concurrent sn_eval_run calls safely");
    return 0;
}
