#include "check.h"
#include "check_internal.h"

#include <string.h>

#include "builtins.h"

static SnTypeRep *constructor_arg_type(SnChecker *c, SnSymbol *type_sym,
                                       size_t arg_index) {
    const SnDecl *decl = type_sym ? type_sym->decl : NULL;
    if (!decl) return NULL;
    if (decl->params.len) {
        if (arg_index >= decl->params.len) return NULL;
        SnParam *param = SN_LIST_AT(decl->params, SnParam, arg_index);
        return resolve_declared_type(c, type_sym, param->type);
    }
    for (size_t i = 0; i < decl->members.len; ++i) {
        const SnDecl *member = SN_LIST_AT(decl->members, SnDecl, i);
        if (member->kind == SN_DECL_METHOD && member->name &&
            strcmp(member->name, "constructor") == 0) {
            if (arg_index >= member->params.len) return NULL;
            SnParam *param = SN_LIST_AT(member->params, SnParam, arg_index);
            return resolve_declared_type(c, type_sym, param->type);
        }
    }
    size_t field_index = 0;
    for (size_t i = 0; i < decl->members.len; ++i) {
        const SnDecl *member = SN_LIST_AT(decl->members, SnDecl, i);
        if (member->kind != SN_DECL_FIELD) continue;
        if (field_index++ == arg_index) return resolve_declared_type(c, type_sym, member->type);
    }
    return NULL;
}

SnTypeRep *check_expr_call(SnChecker *c, SnScope *local, SnExpr *e) {
    SnTypeRep *result = sn_type_error(c->types);
    do {
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
        if (callee_sym && callee_sym->is_moved &&
            (callee_sym->kind == SN_SYM_LOCAL ||
             callee_sym->kind == SN_SYM_PARAM)) {
            sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_USE_AFTER_MOVE,
                         e->lhs->span,
                         "`%s` was moved and cannot be called before reinitialization",
                         callee_sym->name ? callee_sym->name : "<value>");
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
            SnTypeRep *saved_expected = c->expected_lambda_type;
            c->expected_lambda_type = NULL;
            SnTypeRep *param_type = NULL;
            int consumes_argument = 0;
            if (callee_sym && (callee_sym->kind == SN_SYM_FUNC ||
                               callee_sym->kind == SN_SYM_METHOD ||
                               callee_sym->kind == SN_SYM_VARIANT) &&
                callee_sym->decl && i < callee_sym->decl->params.len) {
                SnParam *param = SN_LIST_AT(callee_sym->decl->params, SnParam, i);
                param_type = resolve_declared_type(c, callee_sym, param->type);
            } else if (callee_sym && callee_sym->kind == SN_SYM_TYPE) {
                param_type = constructor_arg_type(c, callee_sym, i);
            }
            /* Function-typed locals, fields, and parameters have no SnDecl at
             * the call site. Their interned signature still tells us whether
             * passing an argument transfers an owned value. Borrow-qualified
             * parameters remain non-consuming because their tag is distinct. */
            if (!param_type && e->lhs->resolved_type &&
                e->lhs->resolved_type->tag == SN_T_FUNC &&
                i < e->lhs->resolved_type->nargs) {
                param_type = e->lhs->resolved_type->args[i];
            }
            consumes_argument = param_type &&
                (param_type->tag == SN_T_STRING || param_type->tag == SN_T_ARRAY ||
                 param_type->tag == SN_T_NAMED);
            if (arg->kind == SN_EXPR_LAMBDA) {
                if (callee_sym && (callee_sym->kind == SN_SYM_FUNC ||
                                   callee_sym->kind == SN_SYM_METHOD) &&
                    i < callee_sym->decl->params.len) {
                    SnParam *param = SN_LIST_AT(callee_sym->decl->params, SnParam, i);
                    c->expected_lambda_type = resolve_declared_type(c, callee_sym, param->type);
                } else if (e->lhs->resolved_type &&
                           e->lhs->resolved_type->tag == SN_T_FUNC &&
                           i < e->lhs->resolved_type->nargs) {
                    c->expected_lambda_type = e->lhs->resolved_type->args[i];
                }
            }
            arg_tys[i] = sn_check_expr(c, local, arg);
            int generic_consumes = param_type && param_type->tag == SN_T_TYPEVAR &&
                (arg_tys[i]->tag == SN_T_STRING || arg_tys[i]->tag == SN_T_ARRAY ||
                 arg_tys[i]->tag == SN_T_NAMED);
            int stores_in_array = e->lhs && e->lhs->kind == SN_EXPR_MEMBER &&
                e->lhs->text && e->lhs->lhs && e->lhs->lhs->resolved_type &&
                e->lhs->lhs->resolved_type->tag == SN_T_ARRAY &&
                ((strcmp(e->lhs->text, "push") == 0 && i == 0) ||
                 (strcmp(e->lhs->text, "set") == 0 && i == 1));
            int borrowed_origin = sn_check_expr_has_borrowed_origin(arg);
            if (stores_in_array) {
                if (borrowed_origin) {
                    sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_UNSUPPORTED_BORROW,
                                 arg->span,
                                 "borrowed value cannot be stored in an array");
                } else if (arg_tys[i]->tag == SN_T_STRING ||
                           arg_tys[i]->tag == SN_T_ARRAY ||
                           arg_tys[i]->tag == SN_T_NAMED) {
                    sn_check_mark_expr_consumed(c, arg, arg_tys[i]);
                }
            } else if (consumes_argument || generic_consumes) {
                sn_check_mark_expr_consumed(c, arg, arg_tys[i]);
            }
            if (!stores_in_array && param_type && param_type->tag == SN_T_ANY &&
                borrowed_origin) {
                const char *callee_name = e->lhs ? e->lhs->text : NULL;
                int is_output = callee_name &&
                    (strcmp(callee_name, "print") == 0 ||
                     strcmp(callee_name, "println") == 0 ||
                     strcmp(callee_name, "printline") == 0);
                if (!is_output) {
                    sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_UNSUPPORTED_BORROW,
                                 arg->span,
                                 "borrowed value cannot be passed to an `any` parameter that may retain it");
                }
            }
            c->expected_lambda_type = saved_expected;
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
             * is not checked because builtins.c models no parameters. */
            SnTypeRep *callee_ty = e->lhs->resolved_type;
            result = (callee_ty && callee_ty->tag == SN_T_FUNC) ? callee_ty->ret
                                                                : sn_type_error(c->types);
            break;
        }
        if (callee_sym->kind == SN_SYM_FUNC || callee_sym->kind == SN_SYM_METHOD) {
            if (callee_sym->decl->is_pulsar && c->in_async_body) {
                sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_PULSAR_IN_ASYNC, e->span,
                             "cannot call pulsar function `%s` inside an async function",
                             callee_sym->name);
            } else if (callee_sym->decl->is_pulsar && !is_pulsar_launch_call) {
                sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_PULSAR_DIRECT_CALL, e->span,
                            "pulsar function `%s` cannot be called directly -- use `pulsar %s(...)`",
                            callee_sym->name, callee_sym->name);
            }
            if (e->lhs->resolved_type && e->lhs->resolved_type->tag == SN_T_FUNC) {
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
        if (callee_ty && (sn_check_is_error(callee_ty) || sn_type_is_any(callee_ty))) {
            /* Already-failed or deliberately untyped callee: plan.md §5.1
             * silent propagation, no second diagnostic. */
            result = sn_type_error(c->types);
            break;
        }
        sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_NOT_CALLABLE, e->lhs->span,
                    "`%s` is not callable", e->lhs->text ? e->lhs->text : "<expr>");
        result = sn_type_error(c->types);
        break;
        } while (0);
    return result;
}
