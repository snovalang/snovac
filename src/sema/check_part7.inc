void sn_check_stmt(SnChecker *c, SnScope *local, SnStmt *s) {
    switch (s->kind) {
    case SN_STMT_LET:
    case SN_STMT_VAR: {
        SnTypeRep *declared = s->type ? sn_check_resolve_type(c, s->type) : NULL;
        SnTypeRep *init_ty;
        SnTypeRep *final_ty;

        /* `var ids: Array<string> = []` — the annotation IS the element-type
         * source SNOVA100 asks for, so an empty literal under an annotated
         * array binding is not the untyped case the code reports
         * (tests/compile-pass/ArrayLiteralDeclaredType.snl; the genuinely
         * untyped form still fails in
         * tests/compile-fail/array_literal_untyped.snl). Handled here rather
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
            init_ty = adopt_literal_type(c, s->expr, init_ty, declared);
            init_ty = sn_ptr_adapt_arg(c, s->expr, init_ty, declared);
            if (types_clash(c, init_ty, declared)) {
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
        ret_ty = adopt_literal_type(c, s->expr, ret_ty, c->current_return_type);
        if (types_clash(c, ret_ty, c->current_return_type)) {
            sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_RETURN_TYPE_MISMATCH, s->span,
                        "returned type does not match the function's declared return type");
        }
        break;
    }

    case SN_STMT_IF: {
        if (s->expr) {
            SnTypeRep *cond = sn_check_expr(c, local, s->expr);
            if (!is_error(cond) && !sn_type_is_any(cond) && cond->tag != SN_T_BOOL) {
                sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_CONDITION_NOT_BOOL, s->expr->span,
                            "condition must be `bool`");
            }
        }
        if (s->then_br) {
            sn_check_stmt(c, local, s->then_br);
        }
        if (s->else_br) {
            sn_check_stmt(c, local, s->else_br);
        }
        break;
    }

    case SN_STMT_WHILE: {
        int is_always_true = 0;
        if (s->expr) {
            SnTypeRep *cond = sn_check_expr(c, local, s->expr);
            if (!is_error(cond) && !sn_type_is_any(cond) && cond->tag != SN_T_BOOL) {
                sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_CONDITION_NOT_BOOL, s->expr->span,
                            "condition must be `bool`");
            }
            if (s->expr->kind == SN_EXPR_BOOL && s->expr->text && strcmp(s->expr->text, "true") == 0) {
                is_always_true = 1;
            }
        }
        if (stmt_is_empty(s->then_br)) {
            sn_diag_emit(c->diag, SN_DIAG_WARNING, SNOVA_EMPTY_LOOP, s->span,
                         "empty `while` loop body detected (potential busy-wait/dead code)");
        } else if (is_always_true && !stmt_can_exit_loop(s->then_br)) {
            sn_diag_emit(c->diag, SN_DIAG_WARNING, SNOVA_INFINITE_LOOP, s->span,
                         "infinite `while (true)` loop has no `break`, `return`, or `throw` -- potential unintended busy hang");
        }
        if (s->then_br) {
            c->loop_depth++;
            sn_check_stmt(c, local, s->then_br);
            c->loop_depth--;
        }
        break;
    }

    case SN_STMT_FOR: {
        if (s->expr) {
            sn_check_expr(c, local, s->expr); /* iterable — element typing not modeled */
        }
        SnScope body_scope;
        sn_scope_init(&body_scope, c->arena, local);
        if (s->name) {
            SnSymbol *sym = sn_scope_define(&body_scope, sn_intern_cstr(c->intern, s->name),
                                            SN_SYM_LOCAL, NULL, s->span);
            if (sym) {
                sym->value_type = sn_type_error(c->types); /* documented gap */
            }
        }
        if (s->then_br) {
            c->loop_depth++;
            sn_check_stmt(c, &body_scope, s->then_br);
            c->loop_depth--;
        }
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
        if (s->expr) {
            sn_check_expr(c, local, s->expr);
        }
        if (s->then_br) {
            sn_check_stmt(c, local, s->then_br);
        }
        break;

    case SN_STMT_TRY:
        if (s->then_br) {
            sn_check_stmt(c, local, s->then_br);
        }
        for (size_t i = 0; i < s->catches.len; i++) {
            sn_check_stmt(c, local, SN_LIST_AT(s->catches, SnStmt, i));
        }
        if (s->finally_br) {
            sn_check_stmt(c, local, s->finally_br);
        }
        break;

    case SN_STMT_MATCH: {
        SnTypeRep *target_ty = s->expr ? sn_check_expr(c, local, s->expr) : NULL;
        check_match_exhaustiveness(c, target_ty, &s->arms, s->span);
        for (size_t i = 0; i < s->arms.len; i++) {
            SnMatchArm *arm = SN_LIST_AT(s->arms, SnMatchArm, i);
            SnScope arm_scope;
            sn_scope_init(&arm_scope, c->arena, local);
            bind_pattern_names(c, &arm_scope, arm->pattern);
            if (arm->guard) {
                sn_check_expr(c, &arm_scope, arm->guard);
            }
            if (arm->value) {
                sn_check_expr(c, &arm_scope, arm->value);
            }
            if (arm->body) {
                sn_check_stmt(c, &arm_scope, arm->body);
            }
        }
        break;
    }

    case SN_STMT_PULSAR:
        if (c->in_async_body) {
            sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_PULSAR_IN_ASYNC, s->span,
                         "pulsar statements and functions cannot be used inside an async function");
        }
        if (s->expr) {
            /* `pulsar work()` — the one place a direct call to a pulsar
             * function is legal (SNOVA124 forbids it everywhere else).
             * Consumed by the CALL expression's own callee resolution, not
             * cleared here, so it survives exactly one level of `sn_check_expr`
             * recursion into the CALL node itself. */
            c->in_pulsar_launch = 1;
            sn_check_expr(c, local, s->expr);
            c->in_pulsar_launch = 0;
        }
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

/* Conservative "does this statement guarantee a return/throw on every path"
 * check, for SNOVA_MISSING_RETURN. Deliberately narrow: RETURN/THROW, a BLOCK
 * whose last statement qualifies, and an IF whose branches both qualify.
 * Everything else (loops, match/try as statements, ...) is treated as "not
 * guaranteed" even when it may in fact always return — false negatives here
 * only mean a real gap goes unflagged, which is the safe direction; a false
 * positive would reject correct code. */
static int stmt_always_returns(const SnStmt *s) {
    if (!s) {
        return 0;
    }
    switch (s->kind) {
    case SN_STMT_RETURN:
    case SN_STMT_THROW:
        return 1;
    case SN_STMT_BLOCK:
        return s->stmts.len > 0 &&
              stmt_always_returns(SN_LIST_AT(s->stmts, SnStmt, s->stmts.len - 1));
    case SN_STMT_IF:
        return s->else_br && stmt_always_returns(s->then_br) &&
              stmt_always_returns(s->else_br);
    case SN_STMT_MATCH:
        /* Every arm has an explicit `-> return ...`/`-> throw ...` body — an
         * arm written as a bare value (`0 -> "zero"`) is only a return in
         * match-as-EXPRESSION position, which is a different AST node
         * (SN_EXPR_MATCH); here it would be a statement whose value is
         * discarded, so it does not count. */
        if (s->arms.len == 0) {
            return 0;
        }
        for (size_t i = 0; i < s->arms.len; i++) {
            const SnMatchArm *arm = SN_LIST_AT(s->arms, SnMatchArm, i);
            if (!arm->body || !stmt_always_returns(arm->body)) {
                return 0;
            }
        }
        return 1;
    default:
        return 0;
    }
}

/* SNOVA122's shape rule (docs/snovalang-diagnostics.md, mirrored from
 * crates/snovalang/src/native/selfcheck/pulsar.rs): a pulsar function
 * returns `unit`, `Channel<T>`, or `Select<T>` — nothing else, since a
 * pulsar is fire-and-forget and has no `Task<T>` to hand back a value
 * through. NULL/error types are let through so this never doubles up on a
 * diagnostic sn_check_resolve_type already emitted. */
static int is_allowed_pulsar_return(SnChecker *c, const SnTypeRep *rt) {
    if (!rt || is_error(rt)) {
        return 1;
    }
    if (rt->tag == SN_T_UNIT) {
        return 1;
    }
    if (rt->tag == SN_T_NAMED && rt->decl) {
        const char *name = rt->decl->name;
        if (name == sn_intern_cstr(c->intern, "Channel") ||
            name == sn_intern_cstr(c->intern, "Select")) {
            return 1;
        }
    }
    return 0;
}

/* Defines `owner`'s declared type parameters as SN_SYM_TYPE entries in
 * `scope`. A name already present (a method re-declaring its class's `T`)
 * keeps the first binding — shadowing here would only produce a second
 * typevar for the same name, which is indistinguishable in practice. */
static void define_type_params(SnChecker *c, SnScope *scope, const SnDecl *owner) {
    for (size_t i = 0; i < owner->generics.len; i++) {
        const char *name = SN_LIST_AT(owner->generics, const char, i);
        sn_scope_define(scope, sn_intern_cstr(c->intern, name), SN_SYM_TYPE, NULL,
                        owner->span);
    }
}

