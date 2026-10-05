void compile_expr(Compiler *c, const SnExpr *e) {
    const char *produced = NULL;
    if (!e) {
        emit_unsupported(c, (SnSpan){0}, "missing expression");
        c->expr_type_name = NULL;
        return;
    }
    uint32_t line = e->span.line;
    if (eb_try_expr(c, e)) {
        return;
    }

    switch (e->kind) {
    case SN_EXPR_INT:
    case SN_EXPR_LONG: {
        int64_t val = e->text ? (int64_t)strtoll(e->text, NULL, 0) : 0;
        emit_byte(c, OP_CONST_INT, line);
        emit_i64(c, val, line);
        break;
    }
    case SN_EXPR_DOUBLE:
    case SN_EXPR_DECIMAL: {
        double val = e->text ? strtod(e->text, NULL) : 0.0;
        emit_byte(c, OP_CONST_DOUBLE, line);
        emit_double(c, val, line);
        break;
    }
    case SN_EXPR_STRING:
    case SN_EXPR_CHAR: {
        if (e->interpolated) {
            emit_unsupported(c, e->span, "interpolated string");
            break;
        }
        const char *raw = e->text ? e->text : "";
        char buf[8192];
        size_t n = strlen(raw);
        if (n >= 2 && raw[0] == '"' && raw[n - 1] == '"') {
            raw++;
            n -= 2;
        }
        size_t out_len = 0;
        for (size_t i = 0; i < n && out_len + 1 < sizeof(buf); i++) {
            char ch = raw[i];
            if (ch == '\\' && i + 1 < n) {
                char esc = raw[++i];
                switch (esc) {
                case 'n':  buf[out_len++] = '\n'; break;
                case 't':  buf[out_len++] = '\t'; break;
                case 'r':  buf[out_len++] = '\r'; break;
                case '0':  buf[out_len++] = '\0'; break;
                case '\\': buf[out_len++] = '\\'; break;
                case '"':  buf[out_len++] = '"'; break;
                default:   buf[out_len++] = esc; break;
                }
            } else {
                buf[out_len++] = ch;
            }
        }
        buf[out_len] = '\0';
        uint32_t s_idx = sn_bcunit_add_string(c->bc, buf);
        emit_byte(c, OP_CONST_STRING, line);
        emit_u32(c, s_idx, line);
        break;
    }
    case SN_EXPR_BOOL: {
        uint8_t b = (e->text && strcmp(e->text, "true") == 0) ? 1 : 0;
        emit_byte(c, OP_CONST_BOOL, line);
        emit_byte(c, b, line);
        break;
    }
    case SN_EXPR_IDENT: {
        uint32_t loc_idx;
        if (resolve_local(c, e->text, &loc_idx)) {
            emit_byte(c, OP_GET_LOCAL, line);
            emit_u32(c, loc_idx, line);
            produced = c->locals[loc_idx].type_name;
        } else {
            uint32_t fn_idx;
            if (resolve_func(c, e->text, &fn_idx)) {
                emit_byte(c, OP_CONST_INT, line);
                emit_i64(c, (int64_t)fn_idx, line);
            } else {
                emit_unsupported(c, e->span, "name");
            }
        }
        break;
    }
    case SN_EXPR_ASSIGN: {
        if (e->op != SN_TOK_ASSIGN) {
            emit_unsupported(c, e->span, "assignment");
            break;
        }
        compile_expr(c, e->rhs);
        const char *rhs_ty = c->expr_type_name;
        int stored = 0;
        if (e->lhs && e->lhs->kind == SN_EXPR_IDENT) {
            uint32_t loc_idx;
            if (resolve_local(c, e->lhs->text, &loc_idx)) {
                emit_byte(c, OP_SET_LOCAL, line);
                emit_u32(c, loc_idx, line);
                c->locals[loc_idx].type_name = rhs_ty;
                produced = rhs_ty;
                stored = 1;
            }
        } else if (e->lhs && e->lhs->kind == SN_EXPR_MEMBER && e->lhs->lhs) {
            compile_expr(c, e->lhs->lhs);
            const char *recv_ty = c->expr_type_name;
            uint32_t field_idx = 0;
            const SnDecl *field = NULL;
            if (struct_field_at(find_struct(c, recv_ty), e->lhs->text, &field_idx, &field)) {
                emit_byte(c, OP_SET_FIELD, line);
                emit_u32(c, field_idx, line);
                produced = field_type_name(field);
                stored = 1;
            }
        }
        if (!stored) {
            emit_unsupported(c, e->span, "assignment");
        }
        break;
    }
    case SN_EXPR_BINARY:
        if (e->op == SN_TOK_ANDAND) {
            emit_andand(c, e, line);
            break;
        }
        if (e->op == SN_TOK_OROR) {
            emit_oror(c, e, line);
            break;
        }
        compile_expr(c, e->lhs);
        compile_expr(c, e->rhs);
        switch (e->op) {
        case SN_TOK_PLUS:    emit_byte(c, OP_ADD, line); break;
        case SN_TOK_MINUS:   emit_byte(c, OP_SUB, line); break;
        case SN_TOK_STAR:    emit_byte(c, OP_MUL, line); break;
        case SN_TOK_SLASH:   emit_byte(c, OP_DIV, line); break;
        case SN_TOK_PERCENT: emit_byte(c, OP_MOD, line); break;
        case SN_TOK_AMP:     emit_byte(c, OP_BIT_AND, line); break;
        case SN_TOK_PIPE:    emit_byte(c, OP_BIT_OR, line); break;
        case SN_TOK_CARET:   emit_byte(c, OP_BIT_XOR, line); break;
        case SN_TOK_SHL:     emit_byte(c, OP_SHL, line); break;
        case SN_TOK_SHR:     emit_byte(c, OP_SHR, line); break;
        case SN_TOK_EQ:      emit_byte(c, OP_EQ, line); break;
        case SN_TOK_NE:      emit_byte(c, OP_NE, line); break;
        case SN_TOK_LT:      emit_byte(c, OP_LT, line); break;
        case SN_TOK_LE:      emit_byte(c, OP_LE, line); break;
        case SN_TOK_GT:      emit_byte(c, OP_GT, line); break;
        case SN_TOK_GE:      emit_byte(c, OP_GE, line); break;
        default:
            emit_unsupported(c, e->span, "operator");
            break;
        }
        break;
    case SN_EXPR_UNARY:
        compile_expr(c, e->lhs);
        if (e->op == SN_TOK_MINUS) {
            emit_byte(c, OP_NEG, line);
        } else if (e->op == SN_TOK_BANG) {
            emit_byte(c, OP_NOT, line);
        } else {
            emit_unsupported(c, e->span, "operator");
        }
        break;
    case SN_EXPR_CALL: {
        if (e->lhs && e->lhs->kind == SN_EXPR_MEMBER && e->lhs->text &&
            (strcmp(e->lhs->text, "printline") == 0 || strcmp(e->lhs->text, "println") == 0 || strcmp(e->lhs->text, "print") == 0)) {
            int is_nl = strcmp(e->lhs->text, "print") != 0;
            if (e->args.len > 0) {
                compile_expr(c, SN_LIST_AT(e->args, SnExpr, 0));
            } else {
                uint32_t s_idx = sn_bcunit_add_string(c->bc, "");
                emit_byte(c, OP_CONST_STRING, line);
                emit_u32(c, s_idx, line);
            }
            emit_byte(c, OP_PRINT, line);
            emit_byte(c, (uint8_t)is_nl, line);
            emit_byte(c, OP_CONST_UNIT, line);
            break;
        }
        if (e->lhs && e->lhs->kind == SN_EXPR_IDENT && e->lhs->text &&
            (strcmp(e->lhs->text, "printline") == 0 || strcmp(e->lhs->text, "println") == 0 || strcmp(e->lhs->text, "print") == 0)) {
            int is_nl = strcmp(e->lhs->text, "print") != 0;
            if (e->args.len > 0) {
                compile_expr(c, SN_LIST_AT(e->args, SnExpr, 0));
            } else {
                uint32_t s_idx = sn_bcunit_add_string(c->bc, "");
                emit_byte(c, OP_CONST_STRING, line);
                emit_u32(c, s_idx, line);
            }
            emit_byte(c, OP_PRINT, line);
            emit_byte(c, (uint8_t)is_nl, line);
            emit_byte(c, OP_CONST_UNIT, line);
            break;
        }

        if (e->lhs && e->lhs->kind == SN_EXPR_IDENT && e->lhs->text &&
            strcmp(e->lhs->text, "read_bytes") == 0 && e->args.len == 1) {
            compile_expr(c, SN_LIST_AT(e->args, SnExpr, 0));
            emit_byte(c, OP_READ_BYTES, line);
            break;
        }
        if (e->lhs && e->lhs->kind == SN_EXPR_IDENT && e->lhs->text &&
            strcmp(e->lhs->text, "write_bytes") == 0 && e->args.len == 2) {
            compile_expr(c, SN_LIST_AT(e->args, SnExpr, 0));
            compile_expr(c, SN_LIST_AT(e->args, SnExpr, 1));
            emit_byte(c, OP_WRITE_BYTES, line);
            break;
        }
        if (emit_array_method(c, e, line, &produced)) {
            break;
        }

        uint32_t fn_idx = 0;
        int found = 0;
        if (e->lhs && e->lhs->kind == SN_EXPR_IDENT) {
            found = resolve_func(c, e->lhs->text, &fn_idx);
        }
        for (size_t i = 0; i < e->args.len; i++) {
            compile_expr(c, SN_LIST_AT(e->args, SnExpr, i));
        }
        if (found) {
            emit_byte(c, OP_CALL, line);
            emit_u32(c, fn_idx, line);
            emit_u32(c, (uint32_t)e->args.len, line);
        } else {
            emit_unsupported(c, e->span, "call");
        }
        break;
    }
    case SN_EXPR_ARRAY: {
        for (size_t i = 0; i < e->args.len; i++) {
            compile_expr(c, SN_LIST_AT(e->args, SnExpr, i));
        }
        emit_byte(c, OP_NEW_ARRAY, line);
        emit_u32(c, (uint32_t)e->args.len, line);
        break;
    }
    case SN_EXPR_INDEX: {
        compile_expr(c, e->lhs);
        compile_expr(c, e->rhs);
        emit_byte(c, OP_GET_INDEX, line);
        break;
    }
    case SN_EXPR_STRUCT_LIT: {
        const char *tname = (e->lhs && e->lhs->kind == SN_EXPR_IDENT) ? e->lhs->text : NULL;
        const SnDecl *st = find_struct(c, tname);
        if (!st) {
            emit_unsupported(c, e->span, "struct literal");
            break;
        }
        int positional = 0;
        int named = 0;
        for (size_t j = 0; j < e->field_names.len; j++) {
            const char *fname = (const char *)e->field_names.items[j];
            if (!fname) {
                positional = 1;
                continue;
            }
            named = 1;
            if (!struct_field_at(st, fname, NULL, NULL)) {
                emit_unsupported(c, e->span, "struct field");
                break;
            }
        }
        if (c->failed) {
            break;
        }
        if (positional && named) {
            emit_unsupported(c, e->span, "struct literal");
            break;
        }
        uint32_t nfields = 0;
        size_t argi = 0;
        for (size_t i = 0; i < st->members.len; i++) {
            const SnDecl *m = SN_LIST_AT(st->members, SnDecl, i);
            if (!m || m->kind != SN_DECL_FIELD) {
                continue;
            }
            const SnExpr *val = NULL;
            if (positional) {
                if (argi < e->args.len) {
                    val = SN_LIST_AT(e->args, SnExpr, argi);
                    argi++;
                }
            } else {
                for (size_t j = 0; j < e->field_names.len; j++) {
                    const char *fname = (const char *)e->field_names.items[j];
                    if (m->name && fname && strcmp(fname, m->name) == 0) {
                        val = SN_LIST_AT(e->args, SnExpr, j);
                        break;
                    }
                }
            }
            if (val) {
                compile_expr(c, val);
            } else {
                emit_default_for_type(c, m->type, line);
            }
            if (nfields == UINT32_MAX) {
                emit_unsupported(c, e->span, "struct literal");
                break;
            }
            nfields++;
        }
        if (c->failed) {
            break;
        }
        uint32_t class_idx = sn_bcunit_add_string(c->bc, tname ? tname : "");
        emit_byte(c, OP_NEW_OBJ, line);
        emit_u32(c, class_idx, line);
        emit_u32(c, nfields, line);
        produced = tname;
        break;
    }
    case SN_EXPR_MEMBER: {
        if (!e->lhs) {
            emit_unsupported(c, e->span, "field");
            break;
        }
        compile_expr(c, e->lhs);
        const char *recv_ty = c->expr_type_name;
        uint32_t field_idx = 0;
        const SnDecl *field = NULL;
        if (!struct_field_at(find_struct(c, recv_ty), e->text, &field_idx, &field)) {
            emit_unsupported(c, e->span, "field");
            break;
        }
        emit_byte(c, OP_GET_FIELD, line);
        emit_u32(c, field_idx, line);
        produced = field_type_name(field);
        break;
    }
    default:
        emit_unsupported(c, e->span, "expression");
        break;
    }
    c->expr_type_name = produced;
}

