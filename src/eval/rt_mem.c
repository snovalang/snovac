/* rt_mem.c — stack pool and heap for language values. Freed when the owner
 * frame drops. The compiler arena is not used for these blocks. */
#include "eval_internal.h"

#include <limits.h>

#define RT_POOL (1u << 20)

typedef struct MemBlock {
    struct MemBlock *next;
    SnEvalMemFrame *owner;
    size_t size;
    int heap;
    int marked;
    uint64_t visit_epoch;
} MemBlock;

struct SnEvalMemFrame {
    SnEvalMemFrame *parent;
    MemBlock *blocks;
};

static char g_pool[RT_POOL];
static size_t g_pool_used;
static MemBlock *g_free_stack;
static size_t g_heap_allocs;
static size_t g_heap_live;
static size_t g_stack_allocs;
static size_t g_bounds;
static uint64_t g_visit_epoch;

static void count_inc(size_t *counter) {
    if (*counter < SIZE_MAX) (*counter)++;
}

static int count_export(size_t count) {
    return count > (size_t)INT_MAX ? INT_MAX : (int)count;
}

static void runtime_oom(size_t requested) {
    fprintf(stderr, "snovac: fatal: runtime allocation failed for %zu bytes\n",
            requested);
    exit(70);
}

void sn_rt_mem_reset(SnEvalInterp *in) {
    g_pool_used = 0;
    g_free_stack = NULL;
    g_heap_allocs = 0;
    g_heap_live = 0;
    g_stack_allocs = 0;
    g_bounds = 0;
    g_visit_epoch = 0;
    in->mem = NULL;
    in->defers = NULL;
}

void sn_rt_frame_push(SnEvalInterp *in) {
    SnEvalMemFrame *f = (SnEvalMemFrame *)malloc(sizeof(SnEvalMemFrame));
    if (!f) {
        fprintf(stderr, "snovac: fatal: out of memory allocating eval frame\n");
        exit(70);
    }
    f->parent = in->mem;
    f->blocks = NULL;
    in->mem = f;
}

static int mark_ptr(SnEvalMemFrame *frame, const void *p, uint64_t epoch);

#define SN_MARK_INLINE 64u
typedef struct {
    SnEvalValue inline_values[SN_MARK_INLINE];
    SnEvalValue *values;
    size_t len;
    size_t cap;
} SnMarkWalk;

static void mark_push(SnMarkWalk *walk, SnEvalValue value) {
    if (walk->len == walk->cap) {
        if (walk->cap > SIZE_MAX / 2u ||
            walk->cap * 2u > SIZE_MAX / sizeof(*walk->values)) {
            fputs("snovac: fatal: escape graph is too large\n", stderr);
            exit(70);
        }
        size_t cap = walk->cap * 2u;
        SnEvalValue *values;
        if (walk->values == walk->inline_values) {
            values = (SnEvalValue *)malloc(cap * sizeof(*values));
            if (values) memcpy(values, walk->inline_values,
                               walk->len * sizeof(*values));
        } else {
            values = (SnEvalValue *)realloc(walk->values, cap * sizeof(*values));
        }
        if (!values) {
            fputs("snovac: fatal: out of memory traversing escape graph\n", stderr);
            exit(70);
        }
        walk->values = values;
        walk->cap = cap;
    }
    walk->values[walk->len++] = value;
}

static void mark_env(SnMarkWalk *walk, SnEvalEnv *env, uint64_t epoch) {
    for (; env; env = env->parent) {
        if (env->visit_epoch == epoch) return;
        env->visit_epoch = epoch;
        for (size_t i = 0; i < env->slots.len; i++) {
            SnEvalValue *slot = (SnEvalValue *)env->slots.items[i];
            if (slot) mark_push(walk, *slot);
        }
    }
}

static void mark_value(SnMarkWalk *walk, SnEvalMemFrame *frame,
                       SnEvalValue v, uint64_t epoch) {
    if (v.kind == V_ARRAY && v.as.arr) {
        if (!mark_ptr(frame, v.as.arr, epoch)) return;
        (void)mark_ptr(frame, v.as.arr->items.items, epoch);
        for (size_t i = 0; i < v.as.arr->items.len; i++) {
            SnEvalValue *slot = (SnEvalValue *)v.as.arr->items.items[i];
            if (slot && mark_ptr(frame, slot, epoch)) mark_push(walk, *slot);
        }
    } else if (v.kind == V_OBJECT && v.as.o) {
        if (!mark_ptr(frame, v.as.o, epoch)) return;
        /* Object field slots are owned by the evaluation arena. */
        for (size_t i = 0; i < v.as.o->slots.len; i++) {
            SnEvalValue *slot = (SnEvalValue *)v.as.o->slots.items[i];
            if (slot) mark_push(walk, *slot);
        }
    } else if (v.kind == V_REF && v.as.ref) {
        if (!mark_ptr(frame, v.as.ref, epoch)) return;
        if (v.as.ref->slot && !v.as.ref->is_null) {
            if (v.as.ref->slot_owned) {
                (void)mark_ptr(frame, v.as.ref->slot, epoch);
            }
            mark_push(walk, *v.as.ref->slot);
        }
    } else if (v.kind == V_VARIANT && v.as.vt) {
        if (v.as.vt->visit_epoch == epoch) return;
        v.as.vt->visit_epoch = epoch;
        for (size_t i = 0; i < v.as.vt->payload.len; i++) {
            SnEvalValue *slot = (SnEvalValue *)v.as.vt->payload.items[i];
            if (slot) mark_push(walk, *slot);
        }
    } else if (v.kind == V_LAMBDA && v.as.lam) {
        if (v.as.lam->visit_epoch == epoch) return;
        v.as.lam->visit_epoch = epoch;
        mark_env(walk, v.as.lam->env, epoch);
    }
}

static void mark_roots(SnEvalMemFrame *frame, SnEvalEnv *keep_env,
                       SnEvalEnv *globals, SnEvalValue extra,
                       SnEvalValue ret, uint64_t epoch) {
    SnMarkWalk walk = {{0}, NULL, 0, SN_MARK_INLINE};
    walk.values = walk.inline_values;
    mark_env(&walk, keep_env, epoch);
    mark_env(&walk, globals, epoch);
    mark_push(&walk, extra);
    mark_push(&walk, ret);
    while (walk.len) {
        SnEvalValue value = walk.values[--walk.len];
        mark_value(&walk, frame, value, epoch);
    }
    if (walk.values != walk.inline_values) free(walk.values);
}

static int mark_ptr(SnEvalMemFrame *frame, const void *p, uint64_t epoch) {
    if (!p) return 0;
    /* `p` is an exact pointer returned by sn_rt_alloc for these runtime nodes. */
    MemBlock *b = ((MemBlock *)p) - 1;
    SnEvalMemFrame *owner = b->owner;
    SnEvalMemFrame *active;
    for (active = frame; active && active != owner; active = active->parent) {
    }
    if (!active || b->visit_epoch == epoch) return 0;
    b->visit_epoch = epoch;
    if (owner == frame) b->marked = 1;
    return 1;
}

static void free_block(MemBlock *b) {
    if (b->heap) {
        if (g_heap_live > 0) {
            g_heap_live--;
        }
        free(b);
        return;
    }
    b->next = g_free_stack;
    g_free_stack = b;
}

void sn_rt_frame_pop(SnEvalInterp *in, SnEvalEnv *keep_env, SnEvalValue extra) {
    SnEvalMemFrame *frame = in->mem;
    if (!frame) {
        return;
    }
    if (++g_visit_epoch == 0) {
        for (SnEvalMemFrame *f = in->mem; f; f = f->parent) {
            for (MemBlock *block = f->blocks; block; block = block->next) {
                block->visit_epoch = 0;
            }
        }
        g_visit_epoch = 1;
    }
    mark_roots(frame, keep_env, in->globals, extra, in->ret, g_visit_epoch);
    MemBlock *b = frame->blocks;
    while (b) {
        MemBlock *next = b->next;
        if (b->marked && frame->parent) {
            b->marked = 0;
            b->owner = frame->parent;
            b->next = frame->parent->blocks;
            frame->parent->blocks = b;
        } else {
            free_block(b);
        }
        b = next;
    }
    in->mem = frame->parent;
    free(frame);
}

void *sn_rt_alloc(SnEvalInterp *in, size_t nbytes, int heap) {
    if (!in || !in->mem || nbytes > SIZE_MAX - sizeof(MemBlock)) {
        runtime_oom(nbytes);
    }
    size_t bytes = sizeof(MemBlock) + nbytes;
    MemBlock *b = NULL;
    if (!heap) {
        MemBlock **prev = &g_free_stack;
        for (MemBlock *s = g_free_stack; s; prev = &s->next, s = s->next) {
            if (s->size >= nbytes) {
                *prev = s->next;
                b = s;
                break;
            }
        }
        if (!b) {
            size_t align = (g_pool_used + 15u) & ~15u;
            if (align <= RT_POOL && bytes <= RT_POOL - align) {
                b = (MemBlock *)(void *)(g_pool + align);
                g_pool_used = align + bytes;
            } else {
                heap = 1;
            }
        }
    }
    if (heap) {
        b = (MemBlock *)malloc(bytes);
        if (!b) runtime_oom(nbytes);
        count_inc(&g_heap_allocs);
        count_inc(&g_heap_live);
    } else if (b) {
        count_inc(&g_stack_allocs);
    }
    b->size = nbytes;
    b->owner = in->mem;
    b->heap = heap;
    b->marked = 0;
    b->visit_epoch = 0;
    b->next = in->mem ? in->mem->blocks : NULL;
    if (in->mem) {
        in->mem->blocks = b;
    }
    void *payload = b + 1;
    memset(payload, 0, nbytes);
    return payload;
}

void sn_rt_list_push(SnEvalInterp *in, SnList *l, void *item, int heap) {
    if (l->len == l->cap) {
        if (l->cap > SIZE_MAX / 2u) runtime_oom(SIZE_MAX);
        size_t ncap = l->cap ? l->cap * 2 : 4;
        if (ncap > SIZE_MAX / sizeof(void *)) runtime_oom(SIZE_MAX);
        void **nd = (void **)sn_rt_alloc(in, ncap * sizeof(void *), heap);
        if (l->items) {
            memcpy(nd, l->items, l->len * sizeof(void *));
        }
        l->items = nd;
        l->cap = ncap;
    }
    l->items[l->len++] = item;
}

int sn_rt_heap_allocs(void) { return count_export(g_heap_allocs); }
int sn_rt_heap_live(void) { return count_export(g_heap_live); }
int sn_rt_stack_allocs(void) { return count_export(g_stack_allocs); }
int sn_rt_bounds_checks(void) { return count_export(g_bounds); }
void sn_rt_bounds_hit(void) { count_inc(&g_bounds); }

int sn_eval_probe_name(const char *name) {
    if (!name) {
        return 0;
    }
    return strcmp(name, "heapAllocs") == 0 || strcmp(name, "heapLive") == 0 ||
           strcmp(name, "stackAllocs") == 0 || strcmp(name, "boundsChecks") == 0;
}

long long sn_eval_probe_value(const char *name) {
    if (!name) {
        return 0;
    }
    if (strcmp(name, "heapAllocs") == 0) {
        return sn_rt_heap_allocs();
    }
    if (strcmp(name, "heapLive") == 0) {
        return sn_rt_heap_live();
    }
    if (strcmp(name, "stackAllocs") == 0) {
        return sn_rt_stack_allocs();
    }
    if (strcmp(name, "boundsChecks") == 0) {
        return sn_rt_bounds_checks();
    }
    return 0;
}
