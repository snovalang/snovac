#include "check_internal.h"

#include <string.h>

#include "builtins.h"

/* SNOVA122's shape rule (docs/snovalang-diagnostics.md, mirrored from
 * crates/snovalang/src/native/selfcheck/pulsar.rs): a pulsar function
 * returns `unit`, `Channel<T>`, or `Select<T>` — nothing else, since a
 * pulsar is fire-and-forget and has no `Task<T>` to hand back a value
 * through. NULL/error types are let through so this never doubles up on a
 * diagnostic sn_check_resolve_type already emitted. */
static int is_allowed_pulsar_return(SnChecker *c, const SnTypeRep *rt) {
    if (!rt || sn_check_is_error(rt)) {
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

void sn_check_validate_type_storage(SnChecker *c, const SnDecl *type_decl) {
    if (!c || !type_decl) return;
    SnScope type_params;
    sn_scope_init(&type_params, c->arena, NULL);
    define_type_params(c, &type_params, type_decl);
    SnScope *previous_type_params = c->type_params;
    const SnDecl *previous_owner = c->enclosing_type;
    c->type_params = &type_params;
    c->enclosing_type = type_decl;
    for (size_t i = 0; i < type_decl->members.len; ++i) {
        const SnDecl *member = SN_LIST_AT(type_decl->members, SnDecl, i);
        if (member->kind != SN_DECL_FIELD || !member->type) continue;
        SnTypeRep *field_type = sn_check_resolve_type(c, member->type);
        if (!sn_check_is_error(field_type) &&
            sn_check_type_contains_reference(field_type)) {
            sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_UNSUPPORTED_BORROW,
                         member->span,
                         "field `%s` cannot store a borrow or raw pointer; references cannot outlive their call",
                         member->name ? member->name : "<field>");
        }
    }
    c->type_params = previous_type_params;
    c->enclosing_type = previous_owner;
}

void sn_check_decl_body(SnChecker *c, const SnDecl *decl) {
    int has_native = 0;
    for (size_t i = 0; i < decl->decorators.len; i++) {
        SnDecorator *dec = SN_LIST_AT(decl->decorators, SnDecorator, i);
        if (dec && dec->name && (strcmp(dec->name, "Native") == 0 || strcmp(dec->name, "native") == 0)) {
            has_native = 1;
            break;
        }
    }
    if (c->enclosing_type) {
        for (size_t i = 0; i < c->enclosing_type->decorators.len; i++) {
            SnDecorator *dec = SN_LIST_AT(c->enclosing_type->decorators, SnDecorator, i);
            if (dec && dec->name && (strcmp(dec->name, "Native") == 0 || strcmp(dec->name, "native") == 0)) {
                has_native = 1;
                break;
            }
        }
    }

    if (has_native) {
        const char *owner_name = c->enclosing_type ? c->enclosing_type->name : decl->name;
        int is_permitted = 0;
        if (owner_name) {
            if (strcmp(owner_name, "NativeNetwork") == 0 ||
                strcmp(owner_name, "NativeIO") == 0 ||
                strcmp(owner_name, "NativeMemory") == 0 ||
                strcmp(owner_name, "NativeThread") == 0 ||
                strcmp(owner_name, "NativeMutex") == 0 ||
                strcmp(owner_name, "NativeProcess") == 0 ||
                strcmp(owner_name, "NativeTime") == 0 ||
                strcmp(owner_name, "NativeAsyncIO") == 0 ||
                strcmp(owner_name, "NativeSocket") == 0 ||
                strcmp(owner_name, "Socket") == 0) {
                is_permitted = 1;
            }
        }
        if (!is_permitted) {
            sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_PROHIBITED_NATIVE, decl->span,
                         "architectural violation: '@Native()' is prohibited on '%s'. Native functions SHALL only be used at the runtime/OS boundary. Implement this in pure Snova.",
                         owner_name ? owner_name : "<unknown>");
            return;
        }
    }

    if (!decl->body) {
        return; /* bare signature or @native on permitted boundary */
    }
    if (decl->kind != SN_DECL_FUNC && decl->kind != SN_DECL_METHOD) {
        return;
    }

    /* Type parameters first: the parameter and return types resolved below
     * may mention them (`func try<T, E>(result: Result<T, E>)`). A method
     * also sees its owning type's — `method map(f: (T) -> U): List<U>` inside
     * `class List<T>` names both. */
    SnScope type_params_scope;
    sn_scope_init(&type_params_scope, c->arena, NULL);
    define_type_params(c, &type_params_scope, decl);
    if (c->enclosing_type) {
        define_type_params(c, &type_params_scope, c->enclosing_type);
    }
    c->type_params = &type_params_scope;

    SnScope params_scope;
    sn_scope_init(&params_scope, c->arena, NULL);
    for (size_t i = 0; i < decl->params.len; i++) {
        SnParam *p = SN_LIST_AT(decl->params, SnParam, i);
        SnTypeRep *pty = sn_check_resolve_type(c, p->type);
        SnTypeRep *borrow_target = sn_check_supported_borrow_target(pty);
        if (pty && !sn_check_is_error(pty) &&
            sn_check_type_contains_reference(pty) && !borrow_target) {
            sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_UNSUPPORTED_BORROW, p->span,
                         "parameter `%s` uses an unsupported borrow or raw pointer type; only immutable scalar and string borrows are supported",
                         p->name ? p->name : "<parameter>");
        }
        SnSymbol *sym = sn_scope_define(&params_scope, sn_intern_cstr(c->intern, p->name),
                                        SN_SYM_PARAM, NULL, p->span);
        if (sym) {
            sym->value_type = borrow_target ? borrow_target : pty;
            sym->is_mutable = 0;
            sym->is_borrowed = borrow_target != NULL;
        }
    }

    int is_new_constructor = (decl->name && strcmp(decl->name, "new") == 0 && c->enclosing_type != NULL);
    if (is_new_constructor) {
        SnSymbol *self_sym = sn_check_find_type_symbol_for_decl(c->resolver, c->enclosing_type);
        SnTypeRep *self_ty = self_sym ? sn_type_named(c->types, self_sym, NULL, 0) : sn_type_error(c->types);
        SnSymbol *this_sym = sn_scope_define(&params_scope, sn_intern_cstr(c->intern, "this"),
                                             SN_SYM_PARAM, NULL, decl->span);
        if (this_sym) {
            this_sym->value_type = self_ty;
            this_sym->is_mutable = 0;
        }
        c->current_return_type = self_ty;
    } else {
        c->current_return_type = sn_check_resolve_type(c, decl->ret);
    }
    if (c->current_return_type && !sn_check_is_error(c->current_return_type) &&
        sn_check_type_contains_reference(c->current_return_type)) {
        sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_UNSUPPORTED_BORROW, decl->span,
                     "function `%s` cannot return a borrow or raw pointer; references cannot escape their call",
                     decl->name ? decl->name : "<function>");
    }
    if (decl->is_async && decl->is_pulsar) {
        sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_PULSAR_IN_ASYNC, decl->span,
                     "function `%s` cannot be declared as both async and pulsar",
                     decl->name ? decl->name : "<fn>");
    }
    if (decl->is_pulsar && !is_allowed_pulsar_return(c, c->current_return_type)) {
        sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_PULSAR_INVALID_RETURN, decl->span,
                    "pulsar function `%s` must return `unit`, `Channel<T>`, or `Select<T>`",
                    decl->name ? decl->name : "<fn>");
    }
    int is_layer3 = (c->current_package &&
                     (strncmp(c->current_package, "builtin", 7) == 0 ||
                      strncmp(c->current_package, "stdlib", 6) == 0));
    if (decl->vis == SN_VIS_PUBLIC && is_layer3) {
        if (c->current_return_type && sn_type_is_any(c->current_return_type)) {
            sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_ANY_IN_PUBLIC_LIBRARY, decl->span,
                         "public declaration `%s` in Layer 3 library cannot use `any` in its signature", decl->name ? decl->name : "<fn>");
        }
        for (size_t i = 0; i < decl->params.len; i++) {
            SnParam *p = SN_LIST_AT(decl->params, SnParam, i);
            SnTypeRep *pty = sn_check_resolve_type(c, p->type);
            if (pty && sn_type_is_any(pty)) {
                sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_ANY_IN_PUBLIC_LIBRARY, p->span,
                             "public parameter `%s` in Layer 3 library cannot use `any` in its signature", p->name);
            }
        }
    }
    c->in_constructor = is_new_constructor ||
        (decl->name && sn_intern_cstr(c->intern, decl->name) ==
                           sn_intern_cstr(c->intern, "constructor"));
    c->in_async_body = decl->is_async;
    sn_check_stmt(c, &params_scope, decl->body);
    if (!c->in_constructor && c->current_return_type && !sn_check_is_error(c->current_return_type) &&
        c->current_return_type->tag != SN_T_UNIT &&
        !sn_check_stmt_always_returns(decl->body)) {
        sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_MISSING_RETURN, decl->span,
                    "`%s` is declared to return a value on every path but doesn't",
                    decl->name ? decl->name : "<fn>");
    }
    c->in_constructor = 0;
    c->in_async_body = 0;
    c->current_return_type = NULL;
    c->type_params = NULL;
}
