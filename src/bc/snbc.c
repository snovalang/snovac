#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "snbc.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void sn_chunk_init(SnChunk *chunk) {
    chunk->code = NULL;
    chunk->lines = NULL;
    chunk->count = 0;
    chunk->capacity = 0;
}

void sn_chunk_free(SnChunk *chunk) {
    free(chunk->code);
    free(chunk->lines);
    sn_chunk_init(chunk);
}

void sn_chunk_write(SnChunk *chunk, uint8_t byte, uint32_t line) {
    if (chunk->capacity < chunk->count + 1) {
        size_t old_cap = chunk->capacity;
        chunk->capacity = old_cap < 8 ? 8 : old_cap * 2;
        chunk->code = (uint8_t *)realloc(chunk->code, chunk->capacity);
        chunk->lines = (uint32_t *)realloc(chunk->lines, chunk->capacity * sizeof(uint32_t));
    }
    chunk->code[chunk->count] = byte;
    chunk->lines[chunk->count] = line;
    chunk->count++;
}

void sn_chunk_write_u32(SnChunk *chunk, uint32_t val, uint32_t line) {
    for (int i = 0; i < 4; i++) {
        sn_chunk_write(chunk, (uint8_t)((val >> (i * 8)) & 0xFF), line);
    }
}

void sn_chunk_write_i64(SnChunk *chunk, int64_t val, uint32_t line) {
    uint64_t u = (uint64_t)val;
    for (int i = 0; i < 8; i++) {
        sn_chunk_write(chunk, (uint8_t)((u >> (i * 8)) & 0xFF), line);
    }
}

void sn_chunk_write_double(SnChunk *chunk, double val, uint32_t line) {
    union {
        double d;
        uint64_t u;
    } cvt;
    cvt.d = val;
    for (int i = 0; i < 8; i++) {
        sn_chunk_write(chunk, (uint8_t)((cvt.u >> (i * 8)) & 0xFF), line);
    }
}

void sn_bcunit_init(SnBCUnit *unit) {
    unit->string_pool.strings = NULL;
    unit->string_pool.count = 0;
    unit->string_pool.capacity = 0;
    unit->functions = NULL;
    unit->function_count = 0;
    unit->function_capacity = 0;
    unit->main_func_idx = 0;
}

void sn_bcunit_free(SnBCUnit *unit) {
    for (size_t i = 0; i < unit->string_pool.count; i++) {
        free(unit->string_pool.strings[i]);
    }
    free(unit->string_pool.strings);
    for (size_t i = 0; i < unit->function_count; i++) {
        sn_chunk_free(&unit->functions[i]->chunk);
        free(unit->functions[i]);
    }
    free(unit->functions);
    sn_bcunit_init(unit);
}

uint32_t sn_bcunit_add_string(SnBCUnit *unit, const char *str) {
    for (size_t i = 0; i < unit->string_pool.count; i++) {
        if (strcmp(unit->string_pool.strings[i], str) == 0) {
            return (uint32_t)i;
        }
    }
    if (unit->string_pool.capacity < unit->string_pool.count + 1) {
        size_t old_cap = unit->string_pool.capacity;
        unit->string_pool.capacity = old_cap < 8 ? 8 : old_cap * 2;
        unit->string_pool.strings = (char **)realloc(unit->string_pool.strings,
                                                    unit->string_pool.capacity * sizeof(char *));
    }
    uint32_t idx = (uint32_t)unit->string_pool.count;
    unit->string_pool.strings[idx] = strdup(str);
    unit->string_pool.count++;
    return idx;
}

uint32_t sn_bcunit_add_function(SnBCUnit *unit, const char *name, uint32_t arity) {
    if (unit->function_capacity < unit->function_count + 1) {
        size_t old_cap = unit->function_capacity;
        unit->function_capacity = old_cap < 8 ? 8 : old_cap * 2;
        unit->functions = (SnFunctionChunk **)realloc(unit->functions,
                                                     unit->function_capacity * sizeof(SnFunctionChunk *));
    }
    SnFunctionChunk *fn = (SnFunctionChunk *)malloc(sizeof(SnFunctionChunk));
    fn->name = name ? strdup(name) : NULL;
    fn->arity = arity;
    fn->local_count = arity;
    sn_chunk_init(&fn->chunk);

    uint32_t idx = (uint32_t)unit->function_count;
    unit->functions[idx] = fn;
    unit->function_count++;
    return idx;
}

int sn_bcunit_write_file(const SnBCUnit *unit, const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return 0;

    uint32_t magic = SNBC_MAGIC;
    uint32_t version = SNBC_VERSION;
    uint32_t main_idx = unit->main_func_idx;

    fwrite(&magic, sizeof(uint32_t), 1, f);
    fwrite(&version, sizeof(uint32_t), 1, f);
    fwrite(&main_idx, sizeof(uint32_t), 1, f);

    uint32_t str_count = (uint32_t)unit->string_pool.count;
    fwrite(&str_count, sizeof(uint32_t), 1, f);
    for (uint32_t i = 0; i < str_count; i++) {
        const char *s = unit->string_pool.strings[i];
        uint32_t slen = (uint32_t)(s ? strlen(s) : 0);
        fwrite(&slen, sizeof(uint32_t), 1, f);
        if (slen > 0) {
            fwrite(s, 1, slen, f);
        }
    }

    uint32_t fn_count = (uint32_t)unit->function_count;
    fwrite(&fn_count, sizeof(uint32_t), 1, f);
    for (uint32_t i = 0; i < fn_count; i++) {
        SnFunctionChunk *fn = unit->functions[i];
        uint32_t nlen = (uint32_t)(fn->name ? strlen(fn->name) : 0);
        fwrite(&nlen, sizeof(uint32_t), 1, f);
        if (nlen > 0) {
            fwrite(fn->name, 1, nlen, f);
        }
        fwrite(&fn->arity, sizeof(uint32_t), 1, f);
        fwrite(&fn->local_count, sizeof(uint32_t), 1, f);
        uint32_t code_len = (uint32_t)fn->chunk.count;
        fwrite(&code_len, sizeof(uint32_t), 1, f);
        if (code_len > 0) {
            fwrite(fn->chunk.code, 1, code_len, f);
        }
    }

    fclose(f);
    return 1;
}

void sn_write_u32_le(uint8_t *out, uint32_t v) {
    out[0] = (uint8_t)(v & 0xffu);
    out[1] = (uint8_t)((v >> 8) & 0xffu);
    out[2] = (uint8_t)((v >> 16) & 0xffu);
    out[3] = (uint8_t)((v >> 24) & 0xffu);
}

typedef enum {
    OP_OPERAND_NONE = 0,
    OP_OPERAND_U8,
    OP_OPERAND_U32,
    OP_OPERAND_I32,
    OP_OPERAND_I64,
    OP_OPERAND_F64,
    OP_OPERAND_U32_U32
} OpOperand;

typedef struct {
    const char *name;
    OpOperand operand;
} OpInfo;

/* Operand widths are the comments on SnOpcode. The table names every opcode. */
static const OpInfo OP_INFO[] = {
    {"OP_NOP", OP_OPERAND_NONE},
    {"OP_CONST_INT", OP_OPERAND_I64},
    {"OP_CONST_DOUBLE", OP_OPERAND_F64},
    {"OP_CONST_STRING", OP_OPERAND_U32},
    {"OP_CONST_BOOL", OP_OPERAND_U8},
    {"OP_CONST_UNIT", OP_OPERAND_NONE},
    {"OP_POP", OP_OPERAND_NONE},
    {"OP_DUP", OP_OPERAND_NONE},
    {"OP_GET_LOCAL", OP_OPERAND_U32},
    {"OP_SET_LOCAL", OP_OPERAND_U32},
    {"OP_GET_GLOBAL", OP_OPERAND_U32},
    {"OP_SET_GLOBAL", OP_OPERAND_U32},
    {"OP_ADD", OP_OPERAND_NONE},
    {"OP_SUB", OP_OPERAND_NONE},
    {"OP_MUL", OP_OPERAND_NONE},
    {"OP_DIV", OP_OPERAND_NONE},
    {"OP_MOD", OP_OPERAND_NONE},
    {"OP_NEG", OP_OPERAND_NONE},
    {"OP_NOT", OP_OPERAND_NONE},
    {"OP_BIT_AND", OP_OPERAND_NONE},
    {"OP_BIT_OR", OP_OPERAND_NONE},
    {"OP_BIT_XOR", OP_OPERAND_NONE},
    {"OP_SHL", OP_OPERAND_NONE},
    {"OP_SHR", OP_OPERAND_NONE},
    {"OP_EQ", OP_OPERAND_NONE},
    {"OP_NE", OP_OPERAND_NONE},
    {"OP_LT", OP_OPERAND_NONE},
    {"OP_LE", OP_OPERAND_NONE},
    {"OP_GT", OP_OPERAND_NONE},
    {"OP_GE", OP_OPERAND_NONE},
    {"OP_JUMP", OP_OPERAND_I32},
    {"OP_JUMP_IF_FALSE", OP_OPERAND_I32},
    {"OP_JUMP_IF_TRUE", OP_OPERAND_I32},
    {"OP_CALL", OP_OPERAND_U32_U32},
    {"OP_INVOKE", OP_OPERAND_U32_U32},
    {"OP_RETURN", OP_OPERAND_NONE},
    {"OP_NEW_OBJ", OP_OPERAND_U32_U32},
    {"OP_GET_FIELD", OP_OPERAND_U32},
    {"OP_SET_FIELD", OP_OPERAND_U32},
    {"OP_NEW_ARRAY", OP_OPERAND_U32},
    {"OP_GET_INDEX", OP_OPERAND_NONE},
    {"OP_SET_INDEX", OP_OPERAND_NONE},
    {"OP_VARIANT", OP_OPERAND_U32_U32},
    {"OP_IS_VARIANT", OP_OPERAND_U32},
    {"OP_UNWRAP_VARIANT", OP_OPERAND_U32},
    {"OP_PRINT", OP_OPERAND_U8},
    {"OP_HALT", OP_OPERAND_NONE},
};

_Static_assert(sizeof(OP_INFO) / sizeof(OP_INFO[0]) == (size_t)OP_HALT + 1,
               "every SnOpcode is named");

static int fits_u32(size_t n) {
    return n <= (size_t)UINT32_MAX;
}

static int put_u32(FILE *f, uint32_t v) {
    uint8_t bytes[4];
    sn_write_u32_le(bytes, v);
    return fwrite(bytes, 1, 4, f) == 4;
}

static int put_bytes(FILE *f, const void *bytes, size_t n) {
    if (n == 0) {
        return 1;
    }
    return fwrite(bytes, 1, n, f) == n;
}

static uint32_t read_u32_le(const uint8_t *p) {
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static int32_t read_i32_le(const uint8_t *p) {
    return (int32_t)read_u32_le(p);
}

static int64_t read_i64_le(const uint8_t *p) {
    uint64_t u = 0;
    for (int i = 0; i < 8; i++) {
        u |= (uint64_t)p[i] << (i * 8);
    }
    return (int64_t)u;
}

static double read_f64_le(const uint8_t *p) {
    union {
        uint64_t u;
        double d;
    } cvt;
    cvt.u = 0;
    for (int i = 0; i < 8; i++) {
        cvt.u |= (uint64_t)p[i] << (i * 8);
    }
    return cvt.d;
}

static size_t operand_size(OpOperand operand) {
    switch (operand) {
    case OP_OPERAND_U8:
        return 1;
    case OP_OPERAND_U32:
    case OP_OPERAND_I32:
        return 4;
    case OP_OPERAND_I64:
    case OP_OPERAND_F64:
    case OP_OPERAND_U32_U32:
        return 8;
    case OP_OPERAND_NONE:
        return 0;
    }
    return 0;
}

static void write_decimal_double(FILE *f, double value) {
    char buf[64];
    if (value != value) {
        fputs(" nan", f);
        return;
    }
    if (value != 0.0 && value == value * 2.0) {
        fputs(value < 0.0 ? " -inf" : " inf", f);
        return;
    }
    snprintf(buf, sizeof(buf), "%.17g", value);
    for (char *p = buf; *p != '\0'; p++) {
        if (*p == ',') {
            *p = '.';
        }
    }
    fprintf(f, " %s", buf);
}

static void write_quoted(FILE *f, const char *s, size_t n) {
    fputc('"', f);
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\\' || c == '"') {
            fputc('\\', f);
            fputc(c, f);
        } else if (c < 0x20u || c > 0x7eu) {
            fprintf(f, "\\x%02x", c);
        } else {
            fputc(c, f);
        }
    }
    fputc('"', f);
}

static int finish_file(FILE *f, const char *path, int ok) {
    if (fclose(f) != 0) {
        ok = 0;
    }
    if (!ok) {
        remove(path);
    }
    return ok;
}

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

static int write_instruction(FILE *f, const uint8_t *code, size_t count, size_t *offset) {
    size_t off = *offset;
    if (off >= count) {
        return 0;
    }
    uint8_t op = code[off];
    if (op > OP_HALT) {
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
