/* test_snbc.c — little-endian SnBC container and its text listing. */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "snbc.h"

static int pass = 0;
static int fail = 0;

#define CHECK(name, cond)                                                    \
    do {                                                                     \
        if (cond) {                                                          \
            pass++;                                                         \
        } else {                                                             \
            fail++;                                                         \
            printf("FAIL %s\n", name);                                      \
        }                                                                    \
    } while (0)

static int read_file(const char *path, uint8_t *buf, size_t cap, size_t *out_n) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return 0;
    }
    size_t n = fread(buf, 1, cap, f);
    int extra = fgetc(f);
    fclose(f);
    *out_n = n;
    return extra == EOF;
}

static void append_u32(uint8_t *buf, size_t *n, uint32_t v) {
    sn_write_u32_le(buf + *n, v);
    *n += 4;
}

static void test_write_u32_le(void) {
    uint8_t bytes[4];
    sn_write_u32_le(bytes, 0x01020304u);
    CHECK("sn_write_u32_le(0x01020304) yields 04 03 02 01",
          bytes[0] == 0x04 && bytes[1] == 0x03 && bytes[2] == 0x02 &&
              bytes[3] == 0x01);
}

static void test_canonical_image(void) {
    SnBCUnit unit;
    sn_bcunit_init(&unit);
    uint32_t fn_idx = sn_bcunit_add_function(&unit, "main", 0);
    unit.main_func_idx = fn_idx;
    SnChunk *chunk = &unit.functions[0]->chunk;
    sn_chunk_write(chunk, (uint8_t)OP_CONST_INT, 1);
    sn_chunk_write_i64(chunk, 0, 1);
    sn_chunk_write(chunk, (uint8_t)OP_RETURN, 1);
    sn_chunk_write(chunk, (uint8_t)OP_CONST_UNIT, 1);
    sn_chunk_write(chunk, (uint8_t)OP_RETURN, 1);

    const char *path = "build/test_snbc_image.snbc";
    const char *listing = "build/test_snbc_image.snbc.snbt";
    CHECK("canonical image writes", sn_bcunit_write_canonical(&unit, path));

    uint8_t expect[128];
    size_t n = 0;
    append_u32(expect, &n, SNBC_MAGIC);
    append_u32(expect, &n, SNBC_VERSION);
    append_u32(expect, &n, unit.main_func_idx);
    append_u32(expect, &n, 0); /* no strings */
    append_u32(expect, &n, 1); /* one function */
    append_u32(expect, &n, 4);
    memcpy(expect + n, "main", 4);
    n += 4;
    append_u32(expect, &n, 0);
    append_u32(expect, &n, 0);
    append_u32(expect, &n, (uint32_t)chunk->count);
    memcpy(expect + n, chunk->code, chunk->count);
    n += chunk->count;

    uint8_t got[128];
    size_t got_n = 0;
    int read_ok = read_file(path, got, sizeof(got), &got_n);
    CHECK("canonical image matches sn_write_u32_le layout",
          read_ok && got_n == n && memcmp(got, expect, n) == 0);
    CHECK("canonical image opens with 53 4e 42 43",
          got_n >= 4 && got[0] == 0x53 && got[1] == 0x4e && got[2] == 0x42 &&
              got[3] == 0x43);

    const char *expect_listing =
        "; snbc 1\n"
        "; main 0\n"
        ".func 0 main arity=0 locals=0\n"
        "0 OP_CONST_INT 0\n"
        "9 OP_RETURN\n"
        "10 OP_CONST_UNIT\n"
        "11 OP_RETURN\n"
        ".endfunc\n";
    CHECK("listing writes", sn_bcunit_write_listing(&unit, listing));
    uint8_t text[512];
    size_t text_n = 0;
    read_ok = read_file(listing, text, sizeof(text) - 1, &text_n);
    text[text_n] = 0;
    CHECK("listing shows the const, both returns, and the trailing unit",
          read_ok && strcmp((char *)text, expect_listing) == 0);

    remove(path);
    remove(listing);
    sn_bcunit_free(&unit);
}

static void test_listing_escapes(void) {
    SnBCUnit unit;
    sn_bcunit_init(&unit);
    char raw[] = {'a', '"', 'b', '\\', 'c', '\n', '\x01', '\0'};
    sn_bcunit_add_string(&unit, raw);
    const char *listing = "build/test_snbc_escape.snbt";
    CHECK("escape listing writes", sn_bcunit_write_listing(&unit, listing));
    uint8_t text[512];
    size_t text_n = 0;
    int read_ok = read_file(listing, text, sizeof(text) - 1, &text_n);
    text[text_n] = 0;
    CHECK("listing escapes quote, backslash, and non-printable bytes",
          read_ok && strstr((char *)text, ".string 0 \"a\\\"b\\\\c\\x0a\\x01\"") != NULL);
    remove(listing);
    sn_bcunit_free(&unit);
}

int main(void) {
    test_write_u32_le();
    test_canonical_image();
    test_listing_escapes();

    printf("\n%d passed, %d failed\n", pass, fail);
    return fail == 0 ? 0 : 1;
}
