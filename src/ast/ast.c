/* ast.c — generic AST-list helper.
 *
 * sn_list_push() belongs here, not in parse.c: it is declared in ast.h and
 * used by package.c (P2.2) too, which has no reason to link the parser just
 * to grow an SnList. Moved 2026-07-25 while adding package.c — it was
 * previously defined inside parse.c, coupling every SnList consumer to the
 * whole recursive-descent parser at link time.
 */
#include "ast.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void sn_list_push(SnArena *a, SnList *l, void *item) {
    if (l->len == l->cap) {
        if (l->cap > SIZE_MAX / 2u) {
            fputs("snovac: fatal: list capacity overflow\n", stderr);
            exit(70);
        }
        size_t ncap = l->cap ? l->cap * 2u : 4u;
        if (ncap > SIZE_MAX / sizeof(void *)) {
            fputs("snovac: fatal: list allocation size overflow\n", stderr);
            exit(70);
        }
        void **nd = (void **)sn_arena_alloc(a, ncap * sizeof(void *));
        if (l->items) {
            memcpy(nd, l->items, l->len * sizeof(void *));
        }
        l->items = nd;
        l->cap = ncap;
    }
    l->items[l->len++] = item;
}
