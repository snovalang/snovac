/* cmd_emit_snbc.c — lex and parse one .snl program, then write canonical SnBC.
 * Manifests and .sns scripts are rejected. The emitter is not asked to fail
 * closed; that is a later phase. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "cmd_emit_snbc.h"
#include "arena.h"
#include "diag.h"
#include "driver_utils.h"
#include "emit_bc.h"
#include "lex.h"
#include "parse.h"
#include "snbc.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *listing_path_for(const char *out_path) {
    size_t n = strlen(out_path);
    if (n > SIZE_MAX - 6) {
        return NULL;
    }
    char *listing = (char *)malloc(n + 6);
    if (!listing) {
        return NULL;
    }
    memcpy(listing, out_path, n);
    memcpy(listing + n, ".snbt", 6);
    return listing;
}

int sn_cmd_emit_snbc(const char *path, const char *out_path) {
    if (!path || !sn_driver_path_is_source(path) ||
        sn_driver_path_is_manifest(path) || sn_driver_path_is_script(path)) {
        fprintf(stderr, "error: '%s' is not a Snovalang program (.snl)\n",
                path ? path : "");
        return 2;
    }
    if (!out_path || out_path[0] == '\0') {
        fprintf(stderr, "error: emit-snbc needs -o <file.snbc>\n");
        return 2;
    }

    size_t len = 0;
    char *src = sn_driver_read_file(path, &len);
    if (!src) {
        fprintf(stderr, "error: cannot read '%s'\n", path);
        return 2;
    }

    SnArena arena;
    sn_arena_init(&arena, 256 * 1024);

    SnDiagSink diag;
    sn_diag_init(&diag, path, src, len);

    SnTokenVec toks;
    sn_lex(&arena, &diag, src, len, &toks);

    SnUnit unit;
    sn_parse(&arena, &diag, &toks, &unit);
    sn_driver_report_errors(&diag, path);
    if (diag.error_count > 0) {
        sn_arena_free(&arena);
        free(src);
        return 1;
    }

    SnBCUnit bc;
    int emitted = sn_emit_bytecode(&arena, &diag, &unit, &bc);
    if (!emitted) {
        sn_bcunit_free(&bc);
        sn_arena_free(&arena);
        free(src);
        return 1;
    }

    int wrote = sn_bcunit_write_canonical(&bc, out_path);
    if (wrote) {
        char *listing = listing_path_for(out_path);
        if (!listing) {
            wrote = 0;
        } else {
            wrote = sn_bcunit_write_listing(&bc, listing);
            free(listing);
        }
        if (!wrote) {
            remove(out_path);
        }
    }

    sn_bcunit_free(&bc);
    sn_arena_free(&arena);
    free(src);
    if (!wrote) {
        fprintf(stderr, "error: cannot write '%s'\n", out_path);
        return 1;
    }
    return 0;
}
