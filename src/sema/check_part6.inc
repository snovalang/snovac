    case SN_EXPR_LAMBDA: {
        int all_annotated = 1;
        for (size_t i = 0; i < e->params.len; i++) {
            SnParam *p = SN_LIST_AT(e->params, SnParam, i);
            if (!p->type) {
                all_annotated = 0;
                break;
            }
        }
        if (!all_annotated) {
            /* Bidirectional inference not implemented — documented gap, no
             * diagnostic (this is a checker limitation, not a user error). */
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
            SnTypeRep *pty = sn_check_resolve_type(c, p->type);
            params[i] = pty;
            SnSymbol *sym = sn_scope_define(&lambda_scope, sn_intern_cstr(c->intern, p->name),
                                            SN_SYM_PARAM, NULL, p->span);
            if (sym) {
                sym->value_type = pty;
            }
        }
        SnTypeRep *ret;
        if (e->value) {
            ret = sn_check_expr(c, &lambda_scope, e->value);
        } else if (e->body) {
            sn_check_stmt(c, &lambda_scope, e->body);
            ret = e->type ? sn_check_resolve_type(c, e->type) : sn_type_unit(c->types);
        } else {
            ret = sn_type_error(c->types);
        }
        result = sn_type_func(c->types, params, (uint32_t)e->params.len, ret);
        break;
    }

    case SN_EXPR_IF: {
        if (e->lhs) {
            sn_check_expr(c, local, e->lhs);
        }
        SnTypeRep *then_ty = e->rhs ? sn_check_expr(c, local, e->rhs) : sn_type_unit(c->types);
        SnTypeRep *else_ty = e->value ? sn_check_expr(c, local, e->value) : sn_type_unit(c->types);
        if (e->body) {
            sn_check_stmt(c, local, e->body);
        }
        if (e->else_body) {
            sn_check_stmt(c, local, e->else_body);
        }
        result = !is_error(then_ty) ? then_ty : else_ty;
        break;
    }
    case SN_EXPR_MATCH: {
        SnTypeRep *target_ty = e->lhs ? sn_check_expr(c, local, e->lhs) : NULL;
        check_match_exhaustiveness(c, target_ty, &e->arms, e->span);
        SnTypeRep *inferred = NULL;
        for (size_t i = 0; i < e->arms.len; i++) {
            SnMatchArm *arm = SN_LIST_AT(e->arms, SnMatchArm, i);
            SnScope arm_scope;
            sn_scope_init(&arm_scope, c->arena, local);
            bind_pattern_names(c, &arm_scope, arm->pattern);
            if (arm->guard) {
                sn_check_expr(c, &arm_scope, arm->guard);
            }
            SnTypeRep *arm_ty = NULL;
            if (arm->value) {
                arm_ty = sn_check_expr(c, &arm_scope, arm->value);
            }
            if (arm->body) {
                sn_check_stmt(c, &arm_scope, arm->body);
            }
            if (arm_ty && !is_error(arm_ty) && !inferred) {
                inferred = arm_ty;
            }
        }
        result = inferred ? inferred : sn_type_unit(c->types);
        break;
    }
    case SN_EXPR_STRUCT_LIT: {
        SnSymbol *sym = NULL;
        if (e->lhs && e->lhs->text) {
            /* Scope lookup compares interned pointers. The type name on the
             * literal is the lexer's copy, so it has to be interned first or
             * a known struct is missed and its fields are never checked. */
            sym = find_type_symbol_anywhere(c, c->enclosing_type,
                                            sn_intern_cstr(c->intern, e->lhs->text));
        }
        if (sym && sym->kind == SN_SYM_TYPE) {
            result = sn_type_named(c->types, sym, NULL, 0);
            const SnDecl *td = sym->decl;
            if (td) {
                int saw_named = 0;
                int saw_pos = 0;
                for (size_t i = 0; i < e->field_names.len; i++) {
                    if (SN_LIST_AT(e->field_names, const char, i)) {
                        saw_named = 1;
                    } else {
                        saw_pos = 1;
                    }
                }
                if (saw_named && saw_pos) {
                    sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_ARG_TYPE_MISMATCH, e->span,
                                "struct literal mixes named and positional values");
                    break;
                }
                if (saw_pos) {
                    /* `Message{"olá"}` / `Point{3.0, 4.0}` fill fields in source order. */
                    size_t total = 0;
                    size_t required = 0;
                    for (size_t i = 0; i < td->members.len; i++) {
                        const SnDecl *m = SN_LIST_AT(td->members, SnDecl, i);
                        if (m->kind != SN_DECL_FIELD) {
                            continue;
                        }
                        total++;
                        if (!m->init) {
                            required++;
                        }
                    }
                    if (e->args.len < required || e->args.len > total) {
                        sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_ARITY_MISMATCH, e->span,
                                    "expected %zu argument(s), found %zu", required, e->args.len);
                        break;
                    }
                    size_t ai = 0;
                    for (size_t i = 0; i < td->members.len && ai < e->args.len; i++) {
                        const SnDecl *m = SN_LIST_AT(td->members, SnDecl, i);
                        if (m->kind != SN_DECL_FIELD) {
                            continue;
                        }
                        SnExpr *arg = SN_LIST_AT(e->args, SnExpr, ai);
                        SnTypeRep *decl_ty = resolve_declared_type(c, sym, m->type);
                        SnTypeRep *arg_ty;
                        if (decl_ty && decl_ty->tag == SN_T_ARRAY && arg &&
                            arg->kind == SN_EXPR_ARRAY && arg->args.len == 0) {
                            arg->resolved_type = decl_ty;
                            arg_ty = decl_ty;
                        } else {
                            arg_ty = sn_check_expr(c, local, arg);
                            if (decl_ty) {
                                arg_ty = adopt_literal_type(c, arg, arg_ty, decl_ty);
                            }
                        }
                        if (decl_ty && types_clash(c, arg_ty, decl_ty)) {
                            sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_ARG_TYPE_MISMATCH,
                                         arg ? arg->span : e->span,
                                         "cannot assign incompatible value to field `%s`",
                                         m->name ? m->name : "?");
                        }
                        ai++;
                    }
                    break;
                }
                for (size_t i = 0; i < e->field_names.len; i++) {
                    const char *fname = SN_LIST_AT(e->field_names, const char, i);
                    SnExpr *arg = SN_LIST_AT(e->args, SnExpr, i);
                    SnSymbol *fsym = lookup_member_with_inheritance(c, td, sn_intern_cstr(c->intern, fname));
                    SnTypeRep *decl_ty = fsym ? symbol_type(c, fsym) : NULL;
                    SnTypeRep *arg_ty;
                    /* `field: []` on an `int[]` field uses the field type the
                     * same way `var xs: int[] = []` uses the binding. Checked
                     * before sn_check_expr, which reports an empty literal
                     * that has no expected element type. */
                    if (decl_ty && decl_ty->tag == SN_T_ARRAY && arg &&
                        arg->kind == SN_EXPR_ARRAY && arg->args.len == 0) {
                        arg->resolved_type = decl_ty;
                        arg_ty = decl_ty;
                    } else {
                        arg_ty = sn_check_expr(c, local, arg);
                        if (decl_ty) {
                            arg_ty = adopt_literal_type(c, arg, arg_ty, decl_ty);
                        }
                    }
                    if (!fsym) {
                        sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_UNKNOWN_MEMBER,
                                     arg ? arg->span : e->span,
                                     "type `%s` has no field named `%s`", td->name, fname);
                    } else if (decl_ty && types_clash(c, arg_ty, decl_ty)) {
                        sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_ARG_TYPE_MISMATCH,
                                     arg ? arg->span : e->span,
                                     "cannot assign incompatible value to field `%s`", fname);
                    }
                }
            }
        } else {
            result = sn_type_error(c->types);
        }
        break;
    }

    default:
        result = sn_type_error(c->types);
        break;
    }

    e->resolved_type = result;
    return result;
}

static void check_match_exhaustiveness(SnChecker *c, const SnTypeRep *target_ty,
                                       const SnList *arms, SnSpan span) {
    if (!target_ty || is_error(target_ty) || sn_type_is_any(target_ty) || !arms || arms->len == 0) {
        return;
    }
    for (size_t i = 0; i < arms->len; i++) {
        const SnMatchArm *arm = SN_LIST_AT(*arms, SnMatchArm, i);
        if (arm->guard) continue;
        if (!arm->pattern || arm->pattern->kind == SN_PAT_WILDCARD ||
            arm->pattern->kind == SN_PAT_BINDING) {
            return; /* Catch-all / wildcard covers all remaining branches */
        }
    }
    if (target_ty->tag == SN_T_NAMED && target_ty->decl && target_ty->decl->decl &&
        target_ty->decl->decl->kind == SN_DECL_ENUM) {
        const SnDecl *enum_decl = target_ty->decl->decl;
        size_t total_variants = enum_decl->members.len;
        if (total_variants == 0) return;

        for (size_t vi = 0; vi < total_variants; vi++) {
            const SnDecl *v = SN_LIST_AT(enum_decl->members, SnDecl, vi);
            int covered = 0;
            for (size_t ai = 0; ai < arms->len; ai++) {
                const SnMatchArm *arm = SN_LIST_AT(*arms, SnMatchArm, ai);
                if (arm->guard) continue;
                if (arm->pattern && arm->pattern->kind == SN_PAT_VARIANT &&
                    arm->pattern->name && v->name &&
                    strcmp(arm->pattern->name, v->name) == 0) {
                    covered = 1;
                    break;
                }
            }
            if (!covered) {
                sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_NON_EXHAUSTIVE_MATCH, span,
                             "non-exhaustive match on `%s`: missing variant `%s`",
                             enum_decl->name, v->name);
                break;
            }
        }
    }
}

/* ── loop analysis ───────────────────────────────────────────────────────── */

static int stmt_can_exit_loop(const SnStmt *s) {
    if (!s) return 0;
    switch (s->kind) {
    case SN_STMT_BREAK:
    case SN_STMT_RETURN:
    case SN_STMT_THROW:
        return 1;
    case SN_STMT_BLOCK:
        for (size_t i = 0; i < s->stmts.len; i++) {
            if (stmt_can_exit_loop(SN_LIST_AT(s->stmts, SnStmt, i))) return 1;
        }
        return 0;
    case SN_STMT_IF:
        if (stmt_can_exit_loop(s->then_br)) return 1;
        if (s->else_br && stmt_can_exit_loop(s->else_br)) return 1;
        return 0;
    case SN_STMT_TRY:
        if (stmt_can_exit_loop(s->then_br)) return 1;
        for (size_t i = 0; i < s->catches.len; i++) {
            if (stmt_can_exit_loop(SN_LIST_AT(s->catches, SnStmt, i))) return 1;
        }
        return 0;
    case SN_STMT_MATCH:
        for (size_t i = 0; i < s->arms.len; i++) {
            const SnMatchArm *arm = SN_LIST_AT(s->arms, SnMatchArm, i);
            if (arm->body && stmt_can_exit_loop(arm->body)) return 1;
        }
        return 0;
    default:
        return 0;
    }
}

static int stmt_is_empty(const SnStmt *s) {
    if (!s) return 1;
    if (s->kind == SN_STMT_BLOCK) {
        return s->stmts.len == 0;
    }
    return 0;
}

/* ── statements ───────────────────────────────────────────────────────────── */

static void bind_pattern_names(SnChecker *c, SnScope *scope, const SnPattern *pat) {
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
            sym->value_type = sn_type_error(c->types);
        }
        break;
    }
    case SN_PAT_VARIANT:
        for (size_t i = 0; i < pat->subs.len; i++) {
            bind_pattern_names(c, scope, SN_LIST_AT(pat->subs, SnPattern, i));
        }
        break;
    case SN_PAT_WILDCARD:
    case SN_PAT_LITERAL:
        break;
    }
}

