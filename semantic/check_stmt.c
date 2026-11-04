#include "check_internal.h"

#include "builtins.h"

/* ── statements ───────────────────────────────────────────────────────────── */

void sn_check_bind_pattern_names(SnChecker *c, SnScope *scope, const SnPattern *pat,
                                SnTypeRep *subject_type,
                                int subject_is_borrowed) {
    if (!pat) {
        return;
    }
    switch (pat->kind) {
    case SN_PAT_BINDING: {
        SnSymbol *sym = sn_scope_define(scope, sn_intern_cstr(c->intern, pat->name),
                                        SN_SYM_LOCAL, NULL, pat->span);
        if (sym) {
            /* Real pattern-to-payload typing (matching a variant's declared
             * field types) isn't implemented — documented gap. */
            sym->value_type = subject_type ? subject_type : sn_type_error(c->types);
            sym->is_borrowed = subject_is_borrowed != 0;
        }
        break;
    }
    case SN_PAT_VARIANT:
        for (size_t i = 0; i < pat->subs.len; i++) {
            sn_check_bind_pattern_names(c, scope, SN_LIST_AT(pat->subs, SnPattern, i),
                                        NULL, subject_is_borrowed);
        }
        break;
    case SN_PAT_WILDCARD:
    case SN_PAT_LITERAL:
        break;
    }
}

void sn_check_stmt(SnChecker *c, SnScope *local, SnStmt *s) {
    if (sn_check_control_stmt(c, local, s)) return;
    switch (s->kind) {
    case SN_STMT_LET:
    case SN_STMT_VAR: {
        SnTypeRep *declared = s->type ? sn_check_resolve_type(c, s->type) : NULL;
        if (declared && !sn_check_is_error(declared) &&
            sn_check_type_contains_reference(declared)) {
            sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_UNSUPPORTED_BORROW, s->span,
                         "borrow and raw pointer types cannot be stored in local variables");
        }
        SnTypeRep *init_ty;
        SnTypeRep *final_ty;

        /* `var ids: Array<string> = []` — the annotation IS the element-type
         * source SNOVA100 asks for, so an empty literal under an annotated
         * array binding is not the untyped case the code reports
         * (tests/compile-pass/ArrayLiteralDeclaredType.snova; the genuinely
         * untyped form still fails in
         * tests/compile-fail/array_literal_untyped.snova). Handled here rather
         * than inside SN_EXPR_ARRAY because only the binding knows the
         * annotation — sn_check_expr has no expected-type channel. */
        if (declared && declared->tag == SN_T_ARRAY && s->expr &&
            s->expr->kind == SN_EXPR_ARRAY && s->expr->args.len == 0) {
            s->expr->resolved_type = declared;
            init_ty = declared;
        } else {
            init_ty = s->expr ? sn_check_expr(c, local, s->expr) : NULL;
        }

        if (declared) {
            init_ty = sn_check_adopt_literal_type(c, s->expr, init_ty, declared);
            if (sn_check_types_clash(c, init_ty, declared)) {
                sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_LET_TYPE_MISMATCH, s->span,
                            "`%s`'s declared type does not match its initializer",
                            s->name);
            }
            final_ty = declared;
        } else if (init_ty) {
            if (sn_type_is_any(init_ty)) {
                sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_ANY_IMPLICITLY_INFERRED, s->span,
                             "type `any` cannot be implicitly inferred for `%s`; add an explicit type annotation `let %s: any = ...`",
                             s->name, s->name);
                final_ty = sn_type_error(c->types);
            } else {
                final_ty = init_ty;
            }
        } else {
            sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_UNTYPED_VARIABLE, s->span,
                        "variable '%s' has no determinable type; add an explicit type "
                        "annotation",
                        s->name);
            final_ty = sn_type_error(c->types);
        }

        if (s->expr && s->extra_names.len == 0) {
            sn_check_mark_expr_consumed(c, s->expr, final_ty);
        }

        SnSymbol *sym = sn_scope_define(local, sn_intern_cstr(c->intern, s->name),
                                        SN_SYM_LOCAL, NULL, s->span);
        if (sym) {
            sym->value_type = final_ty;
            sym->is_mutable = (s->kind == SN_STMT_VAR);
        }
        for (size_t i = 0; i < s->extra_names.len; i++) {
            const char *extra = SN_LIST_AT(s->extra_names, const char, i);
            SnSymbol *esym = sn_scope_define(local, sn_intern_cstr(c->intern, extra),
                                             SN_SYM_LOCAL, NULL, s->span);
            if (esym) {
                esym->value_type = sn_type_error(c->types); /* `<~` multi-target: not modeled */
                esym->is_mutable = (s->kind == SN_STMT_VAR);
            }
        }
        break;
    }

    case SN_STMT_EXPR:
        if (s->expr) {
            sn_check_expr(c, local, s->expr);
        }
        break;

    case SN_STMT_RETURN: {
        SnTypeRep *ret_ty = s->expr ? sn_check_expr(c, local, s->expr) : sn_type_unit(c->types);
        if (sn_check_expr_has_borrowed_origin(s->expr) && ret_ty &&
            (ret_ty->tag == SN_T_STRING || ret_ty->tag == SN_T_ARRAY ||
             ret_ty->tag == SN_T_NAMED)) {
            sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_UNSUPPORTED_BORROW, s->expr->span,
                         "borrowed value cannot escape through a function return");
        }
        ret_ty = sn_check_adopt_literal_type(c, s->expr, ret_ty, c->current_return_type);
        if (sn_check_types_clash(c, ret_ty, c->current_return_type)) {
            sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_RETURN_TYPE_MISMATCH, s->span,
                        "returned type does not match the function's declared return type");
        }
        /* Returning an owned local transfers it to the caller. Keep the
         * checker state aligned with OP_MOVE_LOCAL and the native backend's
         * ownership transfer so a later read cannot reuse the returned owner. */
        if (!sn_check_expr_has_borrowed_origin(s->expr))
            sn_check_mark_expr_consumed(c, s->expr, ret_ty);
        break;
    }

    case SN_STMT_BLOCK:
        sn_check_block(c, local, s);
        break;

    case SN_STMT_BREAK:
        if (c->loop_depth == 0) {
            sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_BREAK_OUTSIDE_LOOP, s->span,
                        "`break` used outside a loop");
        }
        break;
    case SN_STMT_CONTINUE:
        if (c->loop_depth == 0) {
            sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_CONTINUE_OUTSIDE_LOOP, s->span,
                        "`continue` used outside a loop");
        }
        break;

    case SN_STMT_THROW:
        if (s->expr) {
            sn_check_expr(c, local, s->expr);
        }
        break;

    case SN_STMT_DEFER:
        if (s->then_br) {
            sn_check_stmt(c, local, s->then_br);
        }
        break;

    default:
        /* Control-flow statements were handled by sn_check_control_stmt. */
        break;
    }
}

void sn_check_block(SnChecker *c, SnScope *parent, SnStmt *block) {
    SnScope scope;
    sn_scope_init(&scope, c->arena, parent);
    for (size_t i = 0; i < block->stmts.len; i++) {
        sn_check_stmt(c, &scope, SN_LIST_AT(block->stmts, SnStmt, i));
    }
}

