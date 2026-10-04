/* rt_mem.c — stack pool and heap for language values. Freed when the owner
 * frame drops. The compiler arena is not used for these blocks. */
#include "eval_internal.h"

#define RT_POOL (1u << 20)

typedef struct MemBlock {
    struct MemBlock *next;
    size_t size;
    int heap;
    int marked;
} MemBlock;

struct SnEvalMemFrame {
    SnEvalMemFrame *parent;
    MemBlock *blocks;
};

static char g_pool[RT_POOL];
static size_t g_pool_used;
static MemBlock *g_free_stack;
static int g_heap_allocs;
static int g_heap_live;
static int g_stack_allocs;
static int g_bounds;

static MemBlock *payload_block(SnEvalMemFrame *frame, const void *p) {
    for (MemBlock *b = frame ? frame->blocks : NULL; b; b = b->next) {
        if ((char *)(b + 1) == (const char *)p) {
            return b;
        }
    }
    return NULL;
}

void sn_rt_mem_reset(SnEvalInterp *in) {
    g_pool_used = 0;
    g_free_stack = NULL;
    g_heap_allocs = 0;
    g_heap_live = 0;
    g_stack_allocs = 0;
    g_bounds = 0;
    in->mem = NULL;
    in->defers = NULL;
}

void sn_rt_frame_push(SnEvalInterp *in) {
    SnEvalMemFrame *f = (SnEvalMemFrame *)malloc(sizeof(SnEvalMemFrame));
    f->parent = in->mem;
    f->blocks = NULL;
    in->mem = f;
}

static void mark_ptr(SnEvalMemFrame *frame, const void *p);

static void mark_value(SnEvalMemFrame *frame, SnEvalValue v) {
    if (v.kind == V_ARRAY && v.as.arr) {
        mark_ptr(frame, v.as.arr);
        mark_ptr(frame, v.as.arr->items.items);
        for (size_t i = 0; i < v.as.arr->items.len; i++) {
            SnEvalValue *slot = (SnEvalValue *)v.as.arr->items.items[i];
            mark_ptr(frame, slot);
            if (slot) {
                mark_value(frame, *slot);
            }
        }
    } else if (v.kind == V_OBJECT && v.as.o) {
        mark_ptr(frame, v.as.o);
        for (size_t i = 0; i < v.as.o->slots.len; i++) {
            SnEvalValue *slot = (SnEvalValue *)v.as.o->slots.items[i];
            if (slot) {
                mark_value(frame, *slot);
            }
        }
    } else if (v.kind == V_REF && v.as.ref) {
        mark_ptr(frame, v.as.ref);
        if (v.as.ref->slot && !v.as.ref->is_null) {
            mark_ptr(frame, v.as.ref->slot);
            mark_value(frame, *v.as.ref->slot);
        }
    } else if (v.kind == V_STRING) {
        mark_ptr(frame, v.as.s);
    }
}

static void mark_ptr(SnEvalMemFrame *frame, const void *p) {
    MemBlock *b = payload_block(frame, p);
    if (b) {
        b->marked = 1;
    }
}

static void mark_env(SnEvalMemFrame *frame, SnEvalEnv *env) {
    for (; env; env = env->parent) {
        for (size_t i = 0; i < env->slots.len; i++) {
            SnEvalValue *slot = (SnEvalValue *)env->slots.items[i];
            if (slot) {
                mark_value(frame, *slot);
            }
        }
    }
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
    mark_env(frame, keep_env);
    mark_env(frame, in->globals);
    mark_value(frame, extra);
    mark_value(frame, in->ret);
    MemBlock *b = frame->blocks;
    while (b) {
        MemBlock *next = b->next;
        if (b->marked && frame->parent) {
            b->marked = 0;
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
            if (align + bytes <= RT_POOL) {
                b = (MemBlock *)(void *)(g_pool + align);
                g_pool_used = align + bytes;
            } else {
                heap = 1;
            }
        }
    }
    if (heap) {
        b = (MemBlock *)malloc(bytes);
        g_heap_allocs++;
        g_heap_live++;
    } else if (b) {
        g_stack_allocs++;
    }
    if (!b) {
        return NULL;
    }
    b->size = nbytes;
    b->heap = heap;
    b->marked = 0;
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
        size_t ncap = l->cap ? l->cap * 2 : 4;
        void **nd = (void **)sn_rt_alloc(in, ncap * sizeof(void *), heap);
        if (l->items && nd) {
            memcpy(nd, l->items, l->len * sizeof(void *));
        }
        l->items = nd;
        l->cap = ncap;
    }
    if (l->items) {
        l->items[l->len++] = item;
    }
}

int sn_rt_heap_allocs(void) { return g_heap_allocs; }
int sn_rt_heap_live(void) { return g_heap_live; }
int sn_rt_stack_allocs(void) { return g_stack_allocs; }
int sn_rt_bounds_checks(void) { return g_bounds; }
void sn_rt_bounds_hit(void) { g_bounds++; }

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
