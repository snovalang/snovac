/* rt_ptr.c — non-null pointers, fat array pointers, deref, indexing. */
#include "eval_internal.h"

SnEvalValue sn_rt_eval_arg(SnEvalInterp *in, SnEvalEnv *env, const SnExpr *e) {
    if (!e) {
        return v_unit();
    }
    if (e->adjust == 2) {
        return sn_rt_take_ref(in, env, e);
    }
    SnEvalValue v = sn_eval_expr(in, env, e);
    if (e->adjust == 1) {
        return sn_rt_deref(in, v, e->span);
    }
    return v;
}

SnEvalValue sn_rt_null(SnEvalInterp *in) {
    RefVal *r = (RefVal *)sn_rt_alloc(in, sizeof(RefVal), 0);
    SnEvalValue v;
    v.kind = V_REF;
    v.as.ref = r;
    if (r) {
        r->is_null = 1;
    }
    return v;
}

SnEvalValue sn_rt_take_ref(SnEvalInterp *in, SnEvalEnv *env, const SnExpr *e) {
    const SnExpr *place = e;
    if (e && e->kind == SN_EXPR_UNARY && e->op == SN_TOK_AMP) {
        place = e->lhs;
    }
    SnEvalValue *slot = NULL;
    if (place && place->kind == SN_EXPR_IDENT && place->text) {
        slot = sn_eval_env_lookup(env, place->text);
    }
    int heap = e && e->storage == 2;
    RefVal *r = (RefVal *)sn_rt_alloc(in, sizeof(RefVal), heap);
    SnEvalValue v;
    v.kind = V_REF;
    v.as.ref = r;
    if (!r) {
        return v_unit();
    }
    if (heap && slot) {
        SnEvalValue *box = (SnEvalValue *)sn_rt_alloc(in, sizeof(SnEvalValue), 1);
        if (box) {
            *box = *slot;
            r->slot = box;
        }
    } else {
        r->slot = slot;
    }
    if (r->slot && r->slot->kind == V_ARRAY && r->slot->as.arr) {
        r->fat = 1;
        r->len = r->slot->as.arr->items.len;
    }
    return v;
}

SnEvalValue sn_rt_deref(SnEvalInterp *in, SnEvalValue v, SnSpan span) {
    if (v.kind != V_REF || !v.as.ref || v.as.ref->is_null || !v.as.ref->slot) {
        sn_rt_error(in, SNOVA_TYPE_ERROR, span, "null pointer dereference");
        return v_unit();
    }
    return *v.as.ref->slot;
}

SnEvalValue sn_rt_autoderef(SnEvalInterp *in, SnEvalValue v, SnSpan span) {
    if (v.kind == V_REF) {
        return sn_rt_deref(in, v, span);
    }
    return v;
}

/* storage 2 is the escape-analysis heap mark. Everything else stays on the
 * stack pool and is freed with the owning frame. */
static int on_heap(const SnExpr *e) { return e && e->storage == 2; }

SnEvalValue sn_rt_new_array(SnEvalInterp *in, SnEvalEnv *env, const SnExpr *e) {
    int heap = on_heap(e);
    ArrayVal *arr = (ArrayVal *)sn_rt_alloc(in, sizeof(ArrayVal), heap);
    if (e) {
        for (size_t i = 0; i < e->args.len; i++) {
            SnEvalValue elem = sn_eval_expr(in, env, (const SnExpr *)e->args.items[i]);
            SnEvalValue *slot = (SnEvalValue *)sn_rt_alloc(in, sizeof(SnEvalValue), heap);
            if (slot) {
                *slot = elem;
            }
            if (arr) {
                sn_rt_list_push(in, &arr->items, slot, heap);
            }
        }
    }
    SnEvalValue v;
    v.kind = V_ARRAY;
    v.as.arr = arr;
    return v;
}

static size_t fat_len(SnEvalValue base) {
    if (base.kind == V_REF && base.as.ref && base.as.ref->fat) {
        return base.as.ref->len;
    }
    if (base.kind == V_ARRAY && base.as.arr) {
        return base.as.arr->items.len;
    }
    return 0;
}

static SnEvalValue *elem_at(SnEvalValue base, size_t idx) {
    if (base.kind == V_REF) {
        base = base.as.ref && base.as.ref->slot ? *base.as.ref->slot : v_unit();
    }
    if (base.kind != V_ARRAY || !base.as.arr) {
        return NULL;
    }
    if (idx >= base.as.arr->items.len) {
        return NULL;
    }
    return (SnEvalValue *)base.as.arr->items.items[idx];
}

SnEvalValue sn_rt_eval_index(SnEvalInterp *in, SnEvalEnv *env, const SnExpr *e) {
    SnEvalValue base = sn_eval_expr(in, env, e->lhs);
    if (base.kind == V_REF) {
        base = sn_rt_deref(in, base, e->span);
    }
    if (base.kind == V_STRING) {
        const char *s = base.as.s ? base.as.s : "";
        long long idx = sn_eval_as_int(in, sn_eval_expr(in, env, e->rhs), e->span);
        size_t len = strlen(s);
        if (idx < 0 || (size_t)idx >= len) {
            sn_rt_error(in, SNOVA_TYPE_ERROR, e->span,
                     "string index %lld out of bounds (len %zu)", idx, len);
            return v_unit();
        }
        return v_char((unsigned char)s[(size_t)idx]);
    }
    if (base.kind != V_ARRAY) {
        sn_rt_error(in, SNOVA_TYPE_ERROR, e->span, "value is not indexable");
        return v_unit();
    }
    long long idx = sn_eval_as_int(in, sn_eval_expr(in, env, e->rhs), e->span);
    size_t len = base.as.arr->items.len;
    if (!e->bounds_proven) {
        sn_rt_bounds_hit();
        if (idx < 0 || (size_t)idx >= len) {
            sn_rt_error(in, SNOVA_TYPE_ERROR, e->span,
                     "array index %lld out of bounds (len %zu)", idx, len);
            return v_unit();
        }
    }
    SnEvalValue *slot = (SnEvalValue *)base.as.arr->items.items[(size_t)idx];
    return slot ? *slot : v_unit();
}

int sn_rt_index_slot(SnEvalInterp *in, SnEvalEnv *env, const SnExpr *index, SnEvalValue **slot_out) {
    SnEvalValue base = sn_eval_expr(in, env, index->lhs);
    if (base.kind == V_REF) {
        base = sn_rt_deref(in, base, index->span);
    }
    if (base.kind != V_ARRAY || !base.as.arr) {
        return 0;
    }
    long long idx = sn_eval_as_int(in, sn_eval_expr(in, env, index->rhs), index->span);
    size_t len = fat_len(base);
    if (!index->bounds_proven) {
        sn_rt_bounds_hit();
    }
    if (idx < 0 || (size_t)idx >= len) {
        sn_rt_error(in, SNOVA_TYPE_ERROR, index->span,
                 "array index %lld out of bounds (len %zu)", idx, len);
        return 0;
    }
    *slot_out = elem_at(base, (size_t)idx);
    return *slot_out != NULL;
}
