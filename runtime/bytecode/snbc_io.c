#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "snbc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void sn_bc_oom(void) {
    fputs("snovac bytecode: out of memory\n", stderr);
    abort();
}

static void *sn_bc_malloc(size_t size) {
    void *ptr = malloc(size ? size : 1u);
    if (!ptr) sn_bc_oom();
    return ptr;
}

static size_t sn_bc_string_allocation_size(uint32_t length) {
    size_t size = (size_t)length;
    if (size == SIZE_MAX) {
        fputs("snovac bytecode: string length overflow\n", stderr);
        abort();
    }
    return size + 1u;
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

static int sn_bcunit_read_file_into(SnBCUnit *unit, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;

    uint32_t magic = 0, version = 0, main_idx = 0;
    if (fread(&magic, sizeof(uint32_t), 1, f) != 1 || magic != SNBC_MAGIC) {
        fclose(f);
        return 0;
    }
    if (fread(&version, sizeof(uint32_t), 1, f) != 1 || version != SNBC_VERSION) {
        fclose(f);
        return 0;
    }
    if (fread(&main_idx, sizeof(uint32_t), 1, f) != 1) {
        fclose(f);
        return 0;
    }
    unit->main_func_idx = main_idx;

    uint32_t str_count = 0;
    if (fread(&str_count, sizeof(uint32_t), 1, f) != 1) {
        fclose(f);
        return 0;
    }
    for (uint32_t i = 0; i < str_count; i++) {
        uint32_t slen = 0;
        if (fread(&slen, sizeof(uint32_t), 1, f) != 1) {
            fclose(f);
            return 0;
        }
        char *buf = (char *)sn_bc_malloc(sn_bc_string_allocation_size(slen));
        if (slen > 0) {
            if (fread(buf, 1, slen, f) != slen) {
                free(buf);
                fclose(f);
                return 0;
            }
        }
        buf[slen] = '\0';
        sn_bcunit_add_string(unit, buf);
        free(buf);
    }

    uint32_t fn_count = 0;
    if (fread(&fn_count, sizeof(uint32_t), 1, f) != 1) {
        fclose(f);
        return 0;
    }
    for (uint32_t i = 0; i < fn_count; i++) {
        uint32_t nlen = 0;
        if (fread(&nlen, sizeof(uint32_t), 1, f) != 1) {
            fclose(f);
            return 0;
        }
        char *name = NULL;
        if (nlen > 0) {
            name = (char *)sn_bc_malloc(sn_bc_string_allocation_size(nlen));
            if (fread(name, 1, nlen, f) != nlen) {
                free(name);
                fclose(f);
                return 0;
            }
            name[nlen] = '\0';
        }
        uint32_t arity = 0, local_count = 0, code_len = 0;
        if (fread(&arity, sizeof(uint32_t), 1, f) != 1 ||
            fread(&local_count, sizeof(uint32_t), 1, f) != 1 ||
            fread(&code_len, sizeof(uint32_t), 1, f) != 1) {
            free(name);
            fclose(f);
            return 0;
        }
        uint32_t fidx = sn_bcunit_add_function(unit, name, arity);
        unit->functions[fidx]->local_count = local_count;
        if (code_len > 0) {
            uint8_t *code = (uint8_t *)sn_bc_malloc((size_t)code_len);
            if (fread(code, 1, code_len, f) != code_len) {
                free(code);
                free(name);
                fclose(f);
                return 0;
            }
            for (uint32_t ci = 0; ci < code_len; ci++) {
                sn_chunk_write(&unit->functions[fidx]->chunk, code[ci], 1);
            }
            free(code);
        }
        free(name);
    }

    fclose(f);
    return 1;
}

int sn_bcunit_read_file(SnBCUnit *unit, const char *path) {
    if (!unit) return 0;
    SnBCUnit loaded;
    sn_bcunit_init(&loaded);
    if (!sn_bcunit_read_file_into(&loaded, path)) {
        sn_bcunit_free(&loaded);
        return 0;
    }
    sn_bcunit_free(unit);
    *unit = loaded;
    return 1;
}

