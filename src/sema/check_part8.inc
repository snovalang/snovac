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
        SnSymbol *sym = sn_scope_define(&params_scope, sn_intern_cstr(c->intern, p->name),
                                        SN_SYM_PARAM, NULL, p->span);
        if (sym) {
            sym->value_type = pty;
            sym->is_mutable = 0;
        }
    }

    int is_new_constructor = (decl->name && strcmp(decl->name, "new") == 0 && c->enclosing_type != NULL);
    if (is_new_constructor) {
        SnSymbol *self_sym = find_type_symbol_for_decl(c->resolver, c->enclosing_type);
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
    sn_borrow_func(c, decl);
    if (!c->in_constructor && c->current_return_type && !is_error(c->current_return_type) &&
        c->current_return_type->tag != SN_T_UNIT &&
        !stmt_always_returns(decl->body)) {
        sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_MISSING_RETURN, decl->span,
                    "`%s` is declared to return a value on every path but doesn't",
                    decl->name ? decl->name : "<fn>");
    }
    c->in_constructor = 0;
    c->in_async_body = 0;
    c->current_return_type = NULL;
    c->type_params = NULL;
}

static void bind_signature_type_params(SnChecker *c, SnScope *scope, const SnDecl *decl) {
    sn_scope_init(scope, c->arena, NULL);
    if (c->enclosing_type) {
        define_type_params(c, scope, c->enclosing_type);
    }
    if (decl) {
        define_type_params(c, scope, decl);
    }
    c->type_params = scope;
}

void sn_check_field_type(SnChecker *c, const SnDecl *field) {
    if (!c || !field || !field->type || field->init) {
        return;
    }
    SnScope type_params_scope;
    bind_signature_type_params(c, &type_params_scope, NULL);
    sn_check_resolve_type(c, field->type);
    c->type_params = NULL;
}

void sn_check_decl_signature(SnChecker *c, const SnDecl *decl) {
    if (!c || !decl || decl->body) {
        return;
    }
    if (decl->kind != SN_DECL_FUNC && decl->kind != SN_DECL_METHOD) {
        return;
    }
    SnScope type_params_scope;
    bind_signature_type_params(c, &type_params_scope, decl);
    for (size_t i = 0; i < decl->params.len; i++) {
        SnParam *p = SN_LIST_AT(decl->params, SnParam, i);
        if (p && p->type) {
            sn_check_resolve_type(c, p->type);
        }
    }
    if (decl->ret) {
        sn_check_resolve_type(c, decl->ret);
    }
    c->type_params = NULL;
}

void sn_check_field_initializer(SnChecker *c, SnScope *local, const SnDecl *field) {
    if (!c || !field || !field->init || !field->type) {
        return;
    }
    SnTypeRep *expected = sn_check_resolve_type(c, field->type);
    SnTypeRep *got = sn_check_expr(c, local, field->init);
    got = adopt_literal_type(c, field->init, got, expected);
    if (types_clash(c, got, expected)) {
        sn_diag_emit(c->diag, SN_DIAG_ERROR, SNOVA_LET_TYPE_MISMATCH, field->span,
                    "field `%s` initializer has the wrong type",
                    field->name ? field->name : "<field>");
    }
}
