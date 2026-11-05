int sn_bcunit_write_canonical(const SnBCUnit *unit, const char *path) {
    if (!unit || !path) {
        return 0;
    }
    if (!fits_u32(unit->string_pool.count) || !fits_u32(unit->function_count)) {
        return 0;
    }
    FILE *f = fopen(path, "wb");
    if (!f) {
        return 0;
    }

    int ok = put_u32(f, SNBC_MAGIC);
    ok = ok && put_u32(f, SNBC_VERSION);
    ok = ok && put_u32(f, unit->main_func_idx);

    uint32_t str_count = (uint32_t)unit->string_pool.count;
    ok = ok && put_u32(f, str_count);
    for (uint32_t i = 0; ok && i < str_count; i++) {
        const char *s = unit->string_pool.strings ? unit->string_pool.strings[i] : NULL;
        size_t slen = s ? strlen(s) : 0;
        if (!s && unit->string_pool.strings == NULL) {
            ok = 0;
            break;
        }
        if (!fits_u32(slen)) {
            ok = 0;
            break;
        }
        ok = ok && put_u32(f, (uint32_t)slen);
        ok = ok && put_bytes(f, s, slen);
    }

    uint32_t fn_count = (uint32_t)unit->function_count;
    ok = ok && put_u32(f, fn_count);
    for (uint32_t i = 0; ok && i < fn_count; i++) {
        if (!unit->functions || !unit->functions[i]) {
            ok = 0;
            break;
        }
        const SnFunctionChunk *fn = unit->functions[i];
        size_t nlen = fn->name ? strlen(fn->name) : 0;
        if (!fits_u32(nlen) || !fits_u32(fn->chunk.count)) {
            ok = 0;
            break;
        }
        if (fn->chunk.count > 0 && !fn->chunk.code) {
            ok = 0;
            break;
        }
        ok = ok && put_u32(f, (uint32_t)nlen);
        ok = ok && put_bytes(f, fn->name, nlen);
        ok = ok && put_u32(f, fn->arity);
        ok = ok && put_u32(f, fn->local_count);
        uint32_t code_len = (uint32_t)fn->chunk.count;
        ok = ok && put_u32(f, code_len);
        ok = ok && put_bytes(f, fn->chunk.code, code_len);
    }

    return finish_file(f, path, ok);
}

static int take_u32(const uint8_t *buf, size_t n, size_t *at, uint32_t *out) {
    if (*at > n || n - *at < 4) {
        return 0;
    }
    *out = read_u32_le(buf + *at);
    *at += 4;
    return 1;
}

static int take_bytes(const uint8_t *buf, size_t n, size_t *at, uint32_t len,
                      const uint8_t **out) {
    if (*at > n || (size_t)len > n - *at) {
        return 0;
    }
    *out = buf + *at;
    *at += len;
    return 1;
}

static char *copy_text(const uint8_t *bytes, uint32_t len) {
    if ((size_t)len == SIZE_MAX) {
        return NULL;
    }
    char *text = (char *)malloc((size_t)len + 1u);
    if (!text) {
        return NULL;
    }
    if (len > 0) {
        memcpy(text, bytes, len);
    }
    text[len] = '\0';
    return text;
}

int sn_bcunit_load_canonical(SnBCUnit *unit, const char *path) {
    if (!unit || !path) {
        return 0;
    }
    sn_bcunit_init(unit);

    FILE *f = fopen(path, "rb");
    if (!f) {
        return 0;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return 0;
    }
    long sz = ftell(f);
    if (sz < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return 0;
    }
    size_t nbytes = (size_t)sz;
    uint8_t *buf = NULL;
    if (nbytes > 0) {
        buf = (uint8_t *)malloc(nbytes);
        if (!buf || fread(buf, 1, nbytes, f) != nbytes) {
            free(buf);
            fclose(f);
            return 0;
        }
    }
    fclose(f);

    size_t at = 0;
    uint32_t magic = 0;
    uint32_t version = 0;
    uint32_t main_idx = 0;
    uint32_t str_count = 0;
    int ok = take_u32(buf, nbytes, &at, &magic)
          && magic == SNBC_MAGIC
          && take_u32(buf, nbytes, &at, &version)
          && version == SNBC_VERSION
          && take_u32(buf, nbytes, &at, &main_idx)
          && take_u32(buf, nbytes, &at, &str_count);
    unit->main_func_idx = main_idx;

    for (uint32_t i = 0; ok && i < str_count; i++) {
        uint32_t slen = 0;
        const uint8_t *bytes = NULL;
        ok = take_u32(buf, nbytes, &at, &slen) && take_bytes(buf, nbytes, &at, slen, &bytes);
        char *text = ok ? copy_text(bytes, slen) : NULL;
        if (!ok || !text) {
            ok = 0;
            break;
        }
        if (unit->string_pool.capacity < unit->string_pool.count + 1) {
            size_t old_cap = unit->string_pool.capacity;
            size_t cap = old_cap < 8 ? 8 : old_cap * 2;
            char **grown = (char **)realloc(unit->string_pool.strings, cap * sizeof(char *));
            if (!grown) {
                free(text);
                ok = 0;
                break;
            }
            unit->string_pool.strings = grown;
            unit->string_pool.capacity = cap;
        }
        unit->string_pool.strings[unit->string_pool.count++] = text;
    }

    uint32_t fn_count = 0;
    ok = ok && take_u32(buf, nbytes, &at, &fn_count);
    for (uint32_t i = 0; ok && i < fn_count; i++) {
        uint32_t nlen = 0;
        uint32_t arity = 0;
        uint32_t locals = 0;
        uint32_t code_len = 0;
        const uint8_t *name_bytes = NULL;
        const uint8_t *code = NULL;
        ok = take_u32(buf, nbytes, &at, &nlen)
          && take_bytes(buf, nbytes, &at, nlen, &name_bytes)
          && take_u32(buf, nbytes, &at, &arity)
          && take_u32(buf, nbytes, &at, &locals)
          && take_u32(buf, nbytes, &at, &code_len)
          && take_bytes(buf, nbytes, &at, code_len, &code);
        if (!ok) {
            break;
        }
        char *name = copy_text(name_bytes, nlen);
        SnFunctionChunk *fn = name ? (SnFunctionChunk *)malloc(sizeof(SnFunctionChunk)) : NULL;
        uint8_t *code_copy = NULL;
        if (fn && code_len > 0) {
            code_copy = (uint8_t *)malloc(code_len);
        }
        if (!name || !fn || (code_len > 0 && !code_copy)) {
            free(name);
            free(fn);
            free(code_copy);
            ok = 0;
            break;
        }
        sn_chunk_init(&fn->chunk);
        fn->name = name;
        fn->arity = arity;
        fn->local_count = locals;
        fn->chunk.count = code_len;
        fn->chunk.capacity = code_len;
        fn->chunk.code = code_copy;
        if (code_len > 0) {
            memcpy(fn->chunk.code, code, code_len);
        }
        if (unit->function_capacity < unit->function_count + 1) {
            size_t old_cap = unit->function_capacity;
            size_t cap = old_cap < 8 ? 8 : old_cap * 2;
            SnFunctionChunk **grown = (SnFunctionChunk **)realloc(
                unit->functions, cap * sizeof(SnFunctionChunk *));
            if (!grown) {
                sn_chunk_free(&fn->chunk);
                free(name);
                free(fn);
                ok = 0;
                break;
            }
            unit->functions = grown;
            unit->function_capacity = cap;
        }
        unit->functions[unit->function_count++] = fn;
    }

    if (ok && at != nbytes) {
        ok = 0;
    }
    free(buf);
    if (!ok) {
        sn_bcunit_free(unit);
        return 0;
    }
    return 1;
}

static int write_instruction(FILE *f, const uint8_t *code, size_t count, size_t *offset) {
    size_t off = *offset;
    if (off >= count) {
        return 0;
    }
    uint8_t op = code[off];
    if ((size_t)op >= sizeof(OP_INFO) / sizeof(OP_INFO[0])) {
        return 0;
    }
    const OpInfo *info = &OP_INFO[op];
    size_t nbytes = operand_size(info->operand);
    if (off + 1 + nbytes > count) {
        return 0;
    }
    fprintf(f, "%zu %s", off, info->name);
    const uint8_t *p = code + off + 1;
    switch (info->operand) {
    case OP_OPERAND_NONE:
        break;
    case OP_OPERAND_U8:
        fprintf(f, " %u", (unsigned)p[0]);
        break;
    case OP_OPERAND_U32:
        fprintf(f, " %u", (unsigned)read_u32_le(p));
        break;
    case OP_OPERAND_I32:
        fprintf(f, " %ld", (long)read_i32_le(p));
        break;
    case OP_OPERAND_I64:
        fprintf(f, " %" PRId64, read_i64_le(p));
        break;
    case OP_OPERAND_F64:
        write_decimal_double(f, read_f64_le(p));
        break;
    case OP_OPERAND_U32_U32:
        fprintf(f, " %u %u",
                (unsigned)read_u32_le(p),
                (unsigned)read_u32_le(p + 4));
        break;
    }
    fputc('\n', f);
    *offset = off + 1 + nbytes;
    return 1;
}

int sn_bcunit_write_listing(const SnBCUnit *unit, const char *path) {
    if (!unit || !path) {
        return 0;
    }
    if (!fits_u32(unit->string_pool.count) || !fits_u32(unit->function_count)) {
        return 0;
    }
    FILE *f = fopen(path, "wb");
    if (!f) {
        return 0;
    }

    int ok = fprintf(f, "; snbc %u\n", (unsigned)SNBC_VERSION) >= 0;
    ok = ok && fprintf(f, "; main %u\n", (unsigned)unit->main_func_idx) >= 0;

    uint32_t str_count = (uint32_t)unit->string_pool.count;
    for (uint32_t i = 0; ok && i < str_count; i++) {
        const char *s = unit->string_pool.strings ? unit->string_pool.strings[i] : NULL;
        if (!s && unit->string_pool.strings == NULL) {
            ok = 0;
            break;
        }
        size_t slen = s ? strlen(s) : 0;
        fprintf(f, ".string %u ", (unsigned)i);
        write_quoted(f, s, slen);
        fputc('\n', f);
    }

    uint32_t fn_count = (uint32_t)unit->function_count;
    for (uint32_t i = 0; ok && i < fn_count; i++) {
        if (!unit->functions || !unit->functions[i]) {
            ok = 0;
            break;
        }
        const SnFunctionChunk *fn = unit->functions[i];
        const char *name = fn->name ? fn->name : "";
        fprintf(f, ".func %u %s arity=%u locals=%u\n",
                (unsigned)i, name, (unsigned)fn->arity, (unsigned)fn->local_count);
        if (fn->chunk.count > 0 && !fn->chunk.code) {
            ok = 0;
            break;
        }
        size_t offset = 0;
        while (ok && offset < fn->chunk.count) {
            if (!write_instruction(f, fn->chunk.code, fn->chunk.count, &offset)) {
                break;
            }
        }
        fprintf(f, ".endfunc\n");
    }

    if (ferror(f)) {
        ok = 0;
    }
    return finish_file(f, path, ok);
}
