static void compile_routine(Compiler *c, const SnDecl *d, const SnDecl *owner) {
    char buf[256];
    routine_name(owner, d, buf, sizeof(buf));
    uint32_t fn_idx = 0;
    if (!eb_resolve_func(c, buf, &fn_idx)) {
        eb_emit_unsupported(c, d->span, "function");
        return;
    }
    c->current_fn = c->bc->functions[fn_idx];
    c->local_count = 0;
    c->owner = owner;
    c->loop_top = -1;
    if (owner) {
        uint32_t slot = eb_add_local(c, "this", d->span);
        c->locals[slot].type_name = owner->name;
    }
    for (size_t pi = 0; pi < d->params.len; pi++) {
        const SnParam *p = SN_LIST_AT(d->params, SnParam, pi);
        uint32_t idx = eb_add_local(c, p->name, p->span);
        if (p->type && p->type->kind == SN_TYPE_NAME) {
            c->locals[idx].type_name = p->type->name;
        }
    }
    if (d->body) {
        eb_compile_stmt(c, d->body);
    } else {
        eb_emit_byte(c, OP_TRAP, d->span.line);
    }
    eb_emit_byte(c, OP_CONST_UNIT, d->span.line);
    eb_emit_byte(c, OP_RETURN, d->span.line);
    c->owner = NULL;
}

static void compile_nested(Compiler *c, const SnDecl *d, const SnDecl *owner) {
    if (!d) {
        return;
    }
    if (d->kind == SN_DECL_FUNC || d->kind == SN_DECL_METHOD) {
        compile_routine(c, d, owner);
        return;
    }
    if (d->kind == SN_DECL_CLASS || d->kind == SN_DECL_INTERFACE) {
        for (size_t i = 0; i < d->members.len; i++) {
            const SnDecl *m = SN_LIST_AT(d->members, SnDecl, i);
            if (m && (m->kind == SN_DECL_FUNC || m->kind == SN_DECL_METHOD)) {
                compile_nested(c, m, d);
            }
        }
    }
}

void eb_compile_unit(Compiler *c) {
    for (size_t i = 0; i < c->unit->decls.len; i++) {
        const SnDecl *d = SN_LIST_AT(c->unit->decls, SnDecl, i);
        if (!d) {
            continue;
        }
        if (d->kind == SN_DECL_FUNC || d->kind == SN_DECL_METHOD) {
            compile_routine(c, d, NULL);
        } else if (d->kind == SN_DECL_CLASS || d->kind == SN_DECL_INTERFACE) {
            compile_nested(c, d, NULL);
        }
    }
}
