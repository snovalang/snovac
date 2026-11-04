#include "check.h"
#include "check_internal.h"

#include <string.h>

#include "builtins.h"

/* ── the big expression switch ───────────────────────────────────────────── */

SnTypeRep *sn_check_expr(SnChecker *c, SnScope *local, SnExpr *e) {
    SnTypeRep *result;

    /* A recovered parse leaves holes: `a +` produces an SN_EXPR_BINARY whose
     * rhs never materialised, and the parser has already reported why. Those
     * nodes still reach here whenever anything type-checks a unit that did
     * not parse cleanly, so every operand slot is nullable by contract, not
     * by accident. Answering `error` keeps the walk going without inventing a
     * second diagnostic for a hole the parser already owns. */
    if (!e) {
        return sn_type_error(c->types);
    }

    if (e->kind == SN_EXPR_IF || e->kind == SN_EXPR_MATCH ||
        e->kind == SN_EXPR_STRUCT_LIT) {
        result = check_expr_control(c, local, e);
        e->resolved_type = result;
        return result;
    }

    switch (e->kind) {
    case SN_EXPR_INT:
        result = sn_type_int(c->types);
        break;
    case SN_EXPR_LONG:
        result = sn_type_long(c->types);
        break;
    case SN_EXPR_DOUBLE:
        result = sn_type_double(c->types);
        break;
    case SN_EXPR_DECIMAL:
        result = sn_type_decimal(c->types);
        break;
    case SN_EXPR_STRING:
        /* Interpolation contents aren't statically typed — see check.h. */
        result = sn_type_string(c->types);
        break;
    case SN_EXPR_CHAR:
        result = sn_type_char(c->types);
        break;
    case SN_EXPR_BOOL:
        result = sn_type_bool(c->types);
        break;

    case SN_EXPR_THIS: {
        SnSymbol *self_sym =
            c->enclosing_type ? sn_check_find_type_symbol_for_decl(c->resolver, c->enclosing_type)
                              : NULL;
        result = self_sym ? sn_type_named(c->types, self_sym, NULL, 0)
                          : sn_type_error(c->types);
        break;
    }

    case SN_EXPR_IDENT:
    case SN_EXPR_MEMBER:
        resolve_value_symbol(c, local, e);
        result = e->resolved_type;
        if (e->kind == SN_EXPR_IDENT && e->resolved_symbol &&
            e->resolved_symbol->is_moved) {
            sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_USE_AFTER_MOVE, e->span,
                         "`%s` was moved and cannot be used before reinitialization",
                         e->text ? e->text : "<value>");
            result = sn_type_error(c->types);
        }
        break;

    case SN_EXPR_MOVE: {
        if (!e->lhs || e->lhs->kind != SN_EXPR_IDENT || !e->lhs->text) {
            sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_INVALID_MOVE, e->span,
                         "`move` requires a local variable or parameter name");
            result = sn_type_error(c->types);
            break;
        }
        SnSymbol *source = resolve_value_symbol(c, local, e->lhs);
        e->resolved_symbol = source;
        if (!source || (source->kind != SN_SYM_LOCAL && source->kind != SN_SYM_PARAM)) {
            sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_INVALID_MOVE, e->span,
                         "`move` can only consume a local variable or parameter");
            result = sn_type_error(c->types);
        } else if (source->is_moved) {
            sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_USE_AFTER_MOVE, e->span,
                         "`%s` was already moved", source->name);
            result = sn_type_error(c->types);
        } else if (source->is_borrowed) {
            sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_UNSUPPORTED_BORROW, e->span,
                         "borrowed parameter `%s` cannot be moved",
                         source->name);
            result = sn_type_error(c->types);
        } else if (source->is_captured) {
            sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_INVALID_MOVE, e->span,
                         "`%s` is captured by a closure and cannot be moved while that closure may use it",
                         source->name);
            result = sn_type_error(c->types);
        } else {
            source->is_moved = 1;
            result = e->lhs->resolved_type;
        }
        break;
    }

    case SN_EXPR_CALL:
        result = check_expr_call(c, local, e);
        break;

    case SN_EXPR_INDEX: {
        SnTypeRep *base_ty = sn_check_expr(c, local, e->lhs);
        SnTypeRep *idx_ty = sn_check_expr(c, local, e->rhs);
        (void)idx_ty; /* not enforced to be `int` in this pass — documented gap */
        if (sn_check_is_error(base_ty)) {
            result = sn_type_error(c->types);
            break;
        }
        SnTypeRep *elem = sn_builtin_index_result(c->types, c->intern, base_ty);
        if (elem) {
            result = elem;
            break;
        }
        sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_UNARY_TYPE_MISMATCH, e->span,
                    "type is not indexable");
        result = sn_type_error(c->types);
        break;
    }

    case SN_EXPR_UNARY: {
        SnTypeRep *operand = sn_check_expr(c, local, e->lhs);
        result = check_unary(c, e, operand);
        break;
    }

    case SN_EXPR_BINARY: {
        if (e->op == SN_TOK_QUESTION) {
            /* Ternary `cond ? then : else`. parse_expr.c's parse_expr()
             * encodes this as a BINARY QUESTION node whose rhs is itself a
             * BINARY COLON node pairing the two branches — reusing the
             * binary slots rather than a dedicated AST node (see its
             * comment: "Modelled as a one-armed match-free conditional
             * using the binary slots"). QUESTION/COLON are therefore never
             * real operators here and must not reach check_binary() below,
             * which has no case for either and would report a spurious
             * "operator `:` cannot be applied" (this was, until now, true
             * of every ternary in the corpus, single- or multi-line).
             * SN_EXPR_UNARY with op SN_TOK_QUESTION is the unrelated
             * postfix error-propagation form, handled by check_unary(). */
            SnTypeRep *cond_ty = sn_check_expr(c, local, e->lhs);
            if (!sn_check_is_error(cond_ty) && !sn_type_is_any(cond_ty) &&
                cond_ty->tag != SN_T_BOOL) {
                sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_CONDITION_NOT_BOOL, e->lhs->span,
                            "condition must be `bool`");
            }
            SnExpr *pair = e->rhs;
            if (!pair) {
                result = sn_type_error(c->types);
                break;
            }
            /* The branches are mutually exclusive. Analyze each from the
             * same incoming move state, then join their outgoing states.
             * Checking them sequentially would incorrectly make a move in
             * the true branch visible while checking the false branch. */
            SnMoveSnapshot *moves = sn_check_move_snapshot_new(c, local);
            SnTypeRep *then_ty = sn_check_expr(c, local, pair->lhs);
            sn_check_move_snapshot_accumulate(moves);
            sn_check_move_snapshot_restore(moves);
            SnTypeRep *else_ty = sn_check_expr(c, local, pair->rhs);
            sn_check_move_snapshot_accumulate(moves);
            sn_check_move_snapshot_apply(moves);
            pair->resolved_type = sn_type_error(c->types);
            if (sn_check_is_error(then_ty) || sn_check_is_error(else_ty)) {
                result = sn_type_error(c->types);
            } else if (sn_type_is_any(then_ty)) {
                result = else_ty;
            } else if (sn_type_is_any(else_ty)) {
                result = then_ty;
            } else if (then_ty == else_ty) {
                result = then_ty;
            } else if (!sn_check_types_clash(c, then_ty, else_ty)) {
                result = else_ty; /* then_ty is a subtype of else_ty */
            } else if (!sn_check_types_clash(c, else_ty, then_ty)) {
                result = then_ty; /* else_ty is a subtype of then_ty */
            } else {
                sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_BINARY_TYPE_MISMATCH, e->span,
                            "ternary branches have incompatible types (no implicit "
                            "numeric promotion -- branches must match, or one must be "
                            "a subtype of the other)");
                result = sn_type_error(c->types);
            }
            break;
        }
        SnTypeRep *lt = sn_check_expr(c, local, e->lhs);
        SnTypeRep *rt = sn_check_expr(c, local, e->rhs);
        result = check_binary(c, e, lt, rt);
        break;
    }

    case SN_EXPR_ASSIGN: {
        SnSymbol *target_sym = resolve_value_symbol(c, local, e->lhs);
        SnTypeRep *target_ty = e->lhs->resolved_type;
        /* Indexed l-values are not resolved as symbols. Walk them explicitly
         * so a moved receiver cannot be mutated through `value[index] = ...`. */
        if (!target_ty) target_ty = sn_check_expr(c, local, e->lhs);
        SnTypeRep *value_ty = sn_check_expr(c, local, e->rhs);
        value_ty = sn_check_adopt_literal_type(c, e->rhs, value_ty, target_ty);

        if (target_sym && !is_mutable_symbol(c, target_sym)) {
            sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_IMMUTABLE_REASSIGN, e->span,
                        "`%s` is an immutable `let` binding and cannot be reassigned",
                        e->lhs->text ? e->lhs->text : "<expr>");
        } else if (sn_check_types_clash(c, value_ty, target_ty)) {
            sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_ARG_TYPE_MISMATCH, e->span,
                        "cannot assign this value's type to `%s`",
                        e->lhs->text ? e->lhs->text : "<expr>");
        } else {
            int stores_outside_local =
                (target_sym && target_sym->kind != SN_SYM_LOCAL) ||
                (!target_sym && e->lhs && e->lhs->kind != SN_EXPR_IDENT);
            if (e->op == SN_TOK_ASSIGN && stores_outside_local &&
                sn_check_type_is_owned(value_ty) &&
                sn_check_expr_has_borrowed_origin(e->rhs)) {
                sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_UNSUPPORTED_BORROW, e->rhs->span,
                             "borrowed value cannot escape into a field or container");
            } else if (e->op == SN_TOK_ASSIGN &&
                       ((target_sym && is_mutable_symbol(c, target_sym)) ||
                        stores_outside_local)) {
                sn_check_mark_expr_consumed(c, e->rhs, value_ty);
            }
            if (target_sym && target_sym->is_moved && target_sym->is_mutable) {
                target_sym->is_moved = 0;
            }
        }
        result = target_ty;
        break;
    }

    case SN_EXPR_CAST:
        sn_check_expr(c, local, e->lhs);
        result = sn_check_resolve_type(c, e->type); /* trusts the annotation */
        break;

    case SN_EXPR_IS:
        sn_check_expr(c, local, e->lhs);
        sn_check_resolve_type(c, e->type); /* validated to exist; result is always bool */
        result = sn_type_bool(c->types);
        break;

    case SN_EXPR_ARRAY: {
        if (e->args.len == 0) {
            sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_UNTYPED_VARIABLE, e->span,
                        "an empty array literal carries no element type; annotate the "
                        "binding (e.g. `var xs: Array<T> = []`)");
            result = sn_type_error(c->types);
            break;
        }
        SnExpr *first = SN_LIST_AT(e->args, SnExpr, 0);
        SnTypeRep *elem_ty = sn_check_expr_consume(c, local, first);
        for (size_t i = 1; i < e->args.len; i++) {
            SnExpr *item = SN_LIST_AT(e->args, SnExpr, i);
            SnTypeRep *item_ty = sn_check_expr_consume(c, local, item);
            item_ty = sn_check_adopt_literal_type(c, item, item_ty, elem_ty);
            if (sn_check_types_clash(c, item_ty, elem_ty)) {
                sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_ARG_TYPE_MISMATCH,
                             item->span,
                             "array element type does not match the first element");
            }
        }
        result = sn_check_is_error(elem_ty) ? sn_type_error(c->types)
                                   : sn_type_array(c->types, elem_ty);
        break;
    }

    case SN_EXPR_AWAIT: {
        if (!c->in_async_body) {
            sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_AWAIT_OUTSIDE_ASYNC, e->span,
                        "`await` is only valid inside an `async func`/`async method`");
        }
        SnTypeRep *inner = sn_check_expr(c, local, e->lhs);
        if (!sn_check_is_error(inner) && inner->tag == SN_T_NAMED && inner->decl &&
            inner->decl->name == sn_intern_cstr(c->intern, "Task") && inner->nargs >= 1) {
            result = inner->args[0];
        } else {
            result = inner; /* pass-through best-effort; see check.h */
        }
        break;
    }

    case SN_EXPR_LAMBDA: {
        SnTypeRep *expected_fn = c->expected_lambda_type;
        c->expected_lambda_type = NULL;
        int all_annotated = 1;
        for (size_t i = 0; i < e->params.len; i++) {
            SnParam *p = SN_LIST_AT(e->params, SnParam, i);
            if (!p->type) {
                all_annotated = 0;
                break;
            }
        }
        if (!all_annotated && (!expected_fn || expected_fn->tag != SN_T_FUNC ||
                               expected_fn->nargs != e->params.len)) {
            sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_UNTYPED_VARIABLE, e->span,
                         "cannot infer lambda parameter types without an expected function type");
            result = sn_type_error(c->types);
            break;
        }
        SnScope lambda_scope;
        sn_scope_init(&lambda_scope, c->arena, local);
        SnTypeRep **params =
            e->params.len
                ? (SnTypeRep **)sn_arena_alloc(c->arena, e->params.len * sizeof(SnTypeRep *))
                : NULL;
        for (size_t i = 0; i < e->params.len; i++) {
            SnParam *p = SN_LIST_AT(e->params, SnParam, i);
            SnTypeRep *pty = p->type ? sn_check_resolve_type(c, p->type)
                                     : expected_fn->args[i];
            params[i] = pty;
            SnTypeRep *borrow_target = sn_check_supported_borrow_target(pty);
            if (!sn_check_is_error(pty) &&
                sn_check_type_contains_reference(pty) && !borrow_target) {
                sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_UNSUPPORTED_BORROW,
                             p->span,
                             "lambda parameter `%s` uses an unsupported borrow or raw pointer type",
                             p->name ? p->name : "<parameter>");
            }
            SnSymbol *sym = sn_scope_define(&lambda_scope, sn_intern_cstr(c->intern, p->name),
                                            SN_SYM_PARAM, NULL, p->span);
            if (sym) {
                sym->value_type = borrow_target ? borrow_target : pty;
                sym->is_borrowed = borrow_target != NULL;
            }
        }
        SnScope *previous_lambda_scope = c->lambda_scope;
        c->lambda_scope = &lambda_scope;
        SnTypeRep *ret;
        if (e->value) {
            ret = sn_check_expr(c, &lambda_scope, e->value);
        } else if (e->body) {
            sn_check_stmt(c, &lambda_scope, e->body);
            ret = e->type ? sn_check_resolve_type(c, e->type) : sn_type_unit(c->types);
        } else {
            ret = sn_type_error(c->types);
        }
        if (!sn_check_is_error(ret) && sn_check_type_contains_reference(ret)) {
            sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_UNSUPPORTED_BORROW,
                         e->span,
                         "lambda cannot return a borrow or raw pointer; references cannot escape their call");
        }
        c->lambda_scope = previous_lambda_scope;
        if (expected_fn && expected_fn->tag == SN_T_FUNC &&
            !sn_check_is_error(ret) && sn_check_types_clash(c, ret, expected_fn->ret)) {
            sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_RETURN_TYPE_MISMATCH, e->span,
                         "lambda result does not match its expected return type");
        }
        result = sn_type_func(c->types, params, (uint32_t)e->params.len, ret);
        break;
    }

    default:
        result = sn_type_error(c->types);
        break;
    }

    e->resolved_type = result;
    return result;
}
