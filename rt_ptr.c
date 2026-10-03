/* rt_ptr.c — non-null pointers, fat array pointers, deref, indexing. */
#include "eval_internal.h"

Value rt_eval_arg(Interp *in, Env *env, const SnExpr *e) {
    if (!e) {
        return v_unit();
    }
    if (e->adjust == 2) {
        return rt_take_ref(in, env, e);
    }
    Value v = eval_expr(in, env, e);
    if (e->adjust == 1) {
        return rt_deref(in, v, e->span);
    }
    return v;
}

Value rt_null(Interp *in) {
    RefVal *r = (RefVal *)rt_alloc(in, sizeof(RefVal), 0);
    Value v;
    v.kind = V_REF;
    v.as.ref = r;
    if (r) {
        r->is_null = 1;
    }
    return v;
}

Value rt_take_ref(Interp *in, Env *env, const SnExpr *e) {
    const SnExpr *place = e;
    if (e && e->kind == SN_EXPR_UNARY && e->op == SN_TOK_AMP) {
        place = e->lhs;
    }
    Value *slot = NULL;
    if (place && place->kind == SN_EXPR_IDENT && place->text) {
        slot = env_lookup(env, place->text);
    }
    int heap = e && e->storage == 2;
    RefVal *r = (RefVal *)rt_alloc(in, sizeof(RefVal), heap);
    Value v;
    v.kind = V_REF;
    v.as.ref = r;
    if (!r) {
        return v_unit();
    }
    if (heap && slot) {
        Value *box = (Value *)rt_alloc(in, sizeof(Value), 1);
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

Value rt_deref(Interp *in, Value v, SnSpan span) {
    if (v.kind != V_REF || !v.as.ref || v.as.ref->is_null || !v.as.ref->slot) {
        rt_error(in, SNOVA_TYPE_ERROR, span, "null pointer dereference");
        return v_unit();
    }
    return *v.as.ref->slot;
}

Value rt_autoderef(Interp *in, Value v, SnSpan span) {
    if (v.kind == V_REF) {
        return rt_deref(in, v, span);
    }
    return v;
}

/* storage 2 is the escape-analysis heap mark. Everything else stays on the
 * stack pool and is freed with the owning frame. */
static int on_heap(const SnExpr *e) { return e && e->storage == 2; }

Value rt_new_array(Interp *in, Env *env, const SnExpr *e) {
    int heap = on_heap(e);
    ArrayVal *arr = (ArrayVal *)rt_alloc(in, sizeof(ArrayVal), heap);
    if (e) {
        for (size_t i = 0; i < e->args.len; i++) {
            Value elem = eval_expr(in, env, (const SnExpr *)e->args.items[i]);
            Value *slot = (Value *)rt_alloc(in, sizeof(Value), heap);
            if (slot) {
                *slot = elem;
            }
            if (arr) {
                rt_list_push(in, &arr->items, slot, heap);
            }
        }
    }
    Value v;
    v.kind = V_ARRAY;
    v.as.arr = arr;
    return v;
}

static size_t fat_len(Value base) {
    if (base.kind == V_REF && base.as.ref && base.as.ref->fat) {
        return base.as.ref->len;
    }
    if (base.kind == V_ARRAY && base.as.arr) {
        return base.as.arr->items.len;
    }
    return 0;
}

static Value *elem_at(Value base, size_t idx) {
    if (base.kind == V_REF) {
        base = base.as.ref && base.as.ref->slot ? *base.as.ref->slot : v_unit();
    }
    if (base.kind != V_ARRAY || !base.as.arr) {
        return NULL;
    }
    if (idx >= base.as.arr->items.len) {
        return NULL;
    }
    return (Value *)base.as.arr->items.items[idx];
}

Value rt_eval_index(Interp *in, Env *env, const SnExpr *e) {
    Value base = eval_expr(in, env, e->lhs);
    if (base.kind == V_REF) {
        base = rt_deref(in, base, e->span);
    }
    if (base.kind == V_STRING) {
        const char *s = base.as.s ? base.as.s : "";
        long long idx = as_int(in, eval_expr(in, env, e->rhs), e->span);
        size_t len = strlen(s);
        if (idx < 0 || (size_t)idx >= len) {
            rt_error(in, SNOVA_TYPE_ERROR, e->span,
                     "string index %lld out of bounds (len %zu)", idx, len);
            return v_unit();
        }
        return v_char((unsigned char)s[(size_t)idx]);
    }
    if (base.kind != V_ARRAY) {
        rt_error(in, SNOVA_TYPE_ERROR, e->span, "value is not indexable");
        return v_unit();
    }
    long long idx = as_int(in, eval_expr(in, env, e->rhs), e->span);
    size_t len = base.as.arr->items.len;
    if (!e->bounds_proven) {
        rt_bounds_hit();
        if (idx < 0 || (size_t)idx >= len) {
            rt_error(in, SNOVA_TYPE_ERROR, e->span,
                     "array index %lld out of bounds (len %zu)", idx, len);
            return v_unit();
        }
    }
    Value *slot = (Value *)base.as.arr->items.items[(size_t)idx];
    return slot ? *slot : v_unit();
}

int rt_index_slot(Interp *in, Env *env, const SnExpr *index, Value **slot_out) {
    Value base = eval_expr(in, env, index->lhs);
    if (base.kind == V_REF) {
        base = rt_deref(in, base, index->span);
    }
    if (base.kind != V_ARRAY || !base.as.arr) {
        return 0;
    }
    long long idx = as_int(in, eval_expr(in, env, index->rhs), index->span);
    size_t len = fat_len(base);
    if (!index->bounds_proven) {
        rt_bounds_hit();
    }
    if (idx < 0 || (size_t)idx >= len) {
        rt_error(in, SNOVA_TYPE_ERROR, index->span,
                 "array index %lld out of bounds (len %zu)", idx, len);
        return 0;
    }
    *slot_out = elem_at(base, (size_t)idx);
    return *slot_out != NULL;
}
