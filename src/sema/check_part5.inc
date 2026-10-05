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
    case SN_EXPR_NULL:
        result = sn_type_ref(c->types, NULL, 1);
        break;

    case SN_EXPR_THIS: {
        SnSymbol *self_sym =
            c->enclosing_type ? find_type_symbol_for_decl(c->resolver, c->enclosing_type)
                              : NULL;
        result = self_sym ? sn_type_named(c->types, self_sym, NULL, 0)
                          : sn_type_error(c->types);
        break;
    }

    case SN_EXPR_IDENT:
    case SN_EXPR_MEMBER:
        resolve_value_symbol(c, local, e);
        result = e->resolved_type;
        break;

    case SN_EXPR_CALL: {
        if (e->lhs && e->lhs->kind == SN_EXPR_IDENT && sn_rt_probe_name(e->lhs->text)) {
            result = sn_type_int(c->types);
            break;
        }
        if (check_byte_io_call(c, local, e, &result)) {
            break;
        }
        /* `Partial<User>(name: "Ada", age: 36)` — construction of an
         * intrinsic generic. No symbol exists for the callee; the type IS the
         * result. Resolved before resolve_value_symbol so the callee is never
         * looked up as a value.
         *
         * Unlike the `Array<Post>.new()` shape (where the postfix loop
         * repurposes the CALL node into an IDENT and its `type_args` land on
         * that same node — see intrinsic_type_ref's header comment), a DIRECT
         * call keeps `e->lhs` as the plain, unmodified callee ident and
         * attaches `<User>` to the CALL node `e` itself (parse_expr.c:
         * `e->type_args = args` in the postfix loop, before `parse_call_args`
         * fills `e->args`). Looking up type args on `e->lhs` here always saw
         * an empty list, so `Partial<T>`/`Array<T>` direct-call construction
         * silently typed as `any` (0 type args) instead of `T`. */
        SnTypeRep *intrinsic_ctor = e->lhs->kind == SN_EXPR_IDENT
            ? intrinsic_generic_ref(c, e->lhs->text, &e->type_args)
            : NULL;
        SnSymbol *callee_sym =
            intrinsic_ctor ? NULL : resolve_value_symbol(c, local, e->lhs);
        if (intrinsic_ctor) {
            e->lhs->resolved_type = intrinsic_ctor;
        }
        /* Consumed here, before argument-checking recurses into anything
         * that might itself be a `pulsar work()` launch — SNOVA124 exempts
         * only the CALL directly named by a `pulsar` statement's own expr,
         * never a call nested inside its arguments or a launch nested
         * elsewhere in the tree. */
        int is_pulsar_launch_call = c->in_pulsar_launch;
        c->in_pulsar_launch = 0;

        SnTypeRep **arg_tys = NULL;
        if (e->args.len) {
            arg_tys =
                (SnTypeRep **)sn_arena_alloc(c->arena, e->args.len * sizeof(SnTypeRep *));
        }
        for (size_t i = 0; i < e->args.len; i++) {
            SnExpr *arg = SN_LIST_AT(e->args, SnExpr, i);
            arg_tys[i] = sn_check_expr(c, local, arg);
        }

        if (intrinsic_ctor) {
            result = intrinsic_ctor; /* arguments not matched — see builtins.h */
            break;
        }
        if (!callee_sym) {
            /* resolve_value_symbol returns NULL both for a real failure (it
             * has already reported one, and left SN_T_ERROR behind) and for an
             * intrinsic member, which has no symbol but does leave a real
             * SN_T_FUNC type behind. The latter is callable; its argument list
             * is not checked because builtins.c models no parameters.
             * An immediate call of an inner or anonymous function is a lambda
             * callee: its parameters are real, so they are checked. */
            SnTypeRep *callee_ty = e->lhs->resolved_type;
            if (e->lhs->kind == SN_EXPR_LAMBDA && callee_ty &&
                callee_ty->tag == SN_T_FUNC) {
                check_call_against_functype(c, e, callee_ty, arg_tys, e->args.len);
                result = callee_ty->ret;
                break;
            }
            result = (callee_ty && callee_ty->tag == SN_T_FUNC) ? callee_ty->ret
                                                                : sn_type_error(c->types);
            break;
        }
        if (callee_sym->kind == SN_SYM_FUNC || callee_sym->kind == SN_SYM_METHOD) {
            int used_overload = 0;
            if (callee_sym->overloads) {
                SnSymbol *chosen = select_overload(c, e, arg_tys, callee_sym);
                if (!chosen) {
                    result = sn_type_error(c->types);
                    break;
                }
                callee_sym = chosen;
                used_overload = 1;
            }
            if (callee_sym->decl->is_pulsar && c->in_async_body) {
                sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_PULSAR_IN_ASYNC, e->span,
                             "cannot call pulsar function `%s` inside an async function",
                             callee_sym->name);
            } else if (callee_sym->decl->is_pulsar && !is_pulsar_launch_call) {
                sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_PULSAR_DIRECT_CALL, e->span,
                            "pulsar function `%s` cannot be called directly -- use `pulsar %s(...)`",
                            callee_sym->name, callee_sym->name);
            }
            if (!used_overload && e->lhs->resolved_type && e->lhs->resolved_type->tag == SN_T_FUNC) {
                check_call_against_functype(c, e, e->lhs->resolved_type, arg_tys, e->args.len);
                result = e->lhs->resolved_type->ret;
            } else {
                check_call_args(c, e, callee_sym, &callee_sym->decl->params, arg_tys, e->args.len);
                result = resolve_declared_type(c, callee_sym, callee_sym->decl->ret);
            }
            if (callee_sym->decl && callee_sym->decl->generics.len > 0 && result) {
                uint32_t ngen = (uint32_t)callee_sym->decl->generics.len;
                const char **param_names = (const char **)sn_arena_alloc(c->arena, ngen * sizeof(const char *));
                SnTypeRep **subst_tys = (SnTypeRep **)sn_arena_alloc(c->arena, ngen * sizeof(SnTypeRep *));
                for (uint32_t gi = 0; gi < ngen; gi++) {
                    param_names[gi] = SN_LIST_AT(callee_sym->decl->generics, const char, gi);
                    subst_tys[gi] = (gi < e->type_args.len)
                        ? sn_check_resolve_type(c, SN_LIST_AT(e->type_args, SnType, gi))
                        : sn_type_any(c->types);
                }
                result = sn_type_subst_names(c->types, result, param_names, subst_tys, ngen);
            }
            break;
        }
        if (callee_sym->kind == SN_SYM_VARIANT) {
            check_call_args(c, e, callee_sym, &callee_sym->decl->params, arg_tys, e->args.len);
            SnSymbol *owner_sym = find_variant_owner_symbol(c->resolver, callee_sym);
            result = owner_sym ? sn_type_named(c->types, owner_sym, NULL, 0)
                               : sn_type_error(c->types);
            break;
        }
        if (callee_sym->kind == SN_SYM_TYPE) {
            check_constructor_call(c, e, callee_sym, arg_tys, e->args.len);
            result = named_with_type_args(c, callee_sym, e);
            break;
        }
        /* A value whose type is a function — a lambda in a local, a
         * function-typed parameter or field. */
        SnTypeRep *callee_ty = e->lhs->resolved_type;
        if (callee_ty && callee_ty->tag == SN_T_FUNC) {
            check_call_against_functype(c, e, callee_ty, arg_tys, e->args.len);
            result = callee_ty->ret;
            break;
        }
        if (callee_ty && (is_error(callee_ty) || sn_type_is_any(callee_ty))) {
            /* Already-failed or deliberately untyped callee: plan.md §5.1
             * silent propagation, no second diagnostic. */
            result = sn_type_error(c->types);
            break;
        }
        sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_NOT_CALLABLE, e->lhs->span,
                    "`%s` is not callable", e->lhs->text ? e->lhs->text : "<expr>");
        result = sn_type_error(c->types);
        break;
    }

    case SN_EXPR_INDEX: {
        SnTypeRep *base_ty = sn_ptr_peel(e->lhs, sn_check_expr(c, local, e->lhs));
        SnTypeRep *idx_ty = sn_check_expr(c, local, e->rhs);
        if (!is_error(idx_ty) && !sn_type_is_any(idx_ty) &&
            sn_type_integer_width(idx_ty) == 0) {
            sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_UNARY_TYPE_MISMATCH, e->span,
                        "index must be an integer type");
            result = sn_type_error(c->types);
            break;
        }
        if (is_error(base_ty)) {
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
            /* Ternary `cond ? then : else`. parse_expr.c's sn_parse_expr()
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
            if (!is_error(cond_ty) && !sn_type_is_any(cond_ty) &&
                cond_ty->tag != SN_T_BOOL) {
                sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_CONDITION_NOT_BOOL, e->lhs->span,
                            "condition must be `bool`");
            }
            SnExpr *pair = e->rhs;
            SnTypeRep *then_ty = sn_check_expr(c, local, pair->lhs);
            SnTypeRep *else_ty = sn_check_expr(c, local, pair->rhs);
            pair->resolved_type = sn_type_error(c->types);
            if (is_error(then_ty) || is_error(else_ty)) {
                result = sn_type_error(c->types);
            } else if (sn_type_is_any(then_ty)) {
                result = else_ty;
            } else if (sn_type_is_any(else_ty)) {
                result = then_ty;
            } else if (then_ty == else_ty) {
                result = then_ty;
            } else if (!types_clash(c, then_ty, else_ty)) {
                result = else_ty; /* then_ty is a subtype of else_ty */
            } else if (!types_clash(c, else_ty, then_ty)) {
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
        SnTypeRep *value_ty = sn_check_expr(c, local, e->rhs);
        value_ty = adopt_literal_type(c, e->rhs, value_ty, target_ty);
        value_ty = sn_ptr_adapt_arg(c, e->rhs, value_ty, target_ty);

        if (target_sym && !is_mutable_symbol(c, target_sym)) {
            sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_IMMUTABLE_REASSIGN, e->span,
                        "`%s` is an immutable `let` binding and cannot be reassigned",
                        e->lhs->text ? e->lhs->text : "<expr>");
        } else if (types_clash(c, value_ty, target_ty)) {
            sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_ARG_TYPE_MISMATCH, e->span,
                        "cannot assign this value's type to `%s`",
                        e->lhs->text ? e->lhs->text : "<expr>");
        }
        result = target_ty;
        break;
    }

    case SN_EXPR_CAST: {
        SnTypeRep *src = sn_check_expr(c, local, e->lhs);
        SnTypeRep *dst = sn_check_resolve_type(c, e->type);
        result = dst;
        if (!src || !dst || is_error(src) || is_error(dst) || sn_type_is_any(src) ||
            sn_type_is_any(dst) || src == dst) {
            break;
        }
        int src_w = sn_type_integer_width(src);
        int dst_w = sn_type_integer_width(dst);
        int numeric_src = src_w > 0 || src->tag == SN_T_FLOAT || src->tag == SN_T_DOUBLE ||
                          src->tag == SN_T_DECIMAL;
        int narrowing = 0;
        if (src_w > 0 && dst_w > 0 && dst_w < src_w) {
            narrowing = 1;
        } else if (dst_w > 0 &&
                   (src->tag == SN_T_FLOAT || src->tag == SN_T_DOUBLE ||
                    src->tag == SN_T_DECIMAL || src->tag == SN_T_STRING ||
                    src->tag == SN_T_CHAR)) {
            narrowing = 1;
        } else if (dst->tag == SN_T_STRING && src->tag == SN_T_CHAR) {
            narrowing = 0; /* one-character widening */
        } else if (numeric_src && dst_w == 0 && dst->tag != SN_T_STRING) {
            /* float as string and similar stay as written; integer targets
             * are the narrowing cases above. */
        }
        if (narrowing) {
            sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_NARROWING_CONVERSION, e->span,
                        "cannot narrow `%s` to `%s`", sn_type_name(src), sn_type_name(dst));
            result = sn_type_error(c->types);
        }
        break;
    }

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
        SnTypeRep *elem_ty = sn_check_expr(c, local, first);
        for (size_t i = 1; i < e->args.len; i++) {
            SnExpr *item = SN_LIST_AT(e->args, SnExpr, i);
            sn_check_expr(c, local, item); /* checked for coverage; not
                                            * cross-validated against elem_ty
                                            * in this pass (documented gap) */
        }
        result = is_error(elem_ty) ? sn_type_error(c->types)
                                   : sn_type_array(c->types, elem_ty);
        break;
    }

    case SN_EXPR_AWAIT: {
        if (!c->in_async_body) {
            sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_AWAIT_OUTSIDE_ASYNC, e->span,
                        "`await` is only valid inside an `async func`/`async method`");
        }
        SnTypeRep *inner = sn_check_expr(c, local, e->lhs);
        if (!is_error(inner) && inner->tag == SN_T_NAMED && inner->decl &&
            inner->decl->name == sn_intern_cstr(c->intern, "Task") && inner->nargs >= 1) {
            result = inner->args[0];
        } else {
            result = inner; /* pass-through best-effort; see check.h */
        }
        break;
    }

