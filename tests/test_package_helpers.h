#ifndef SNOVAC_TEST_PACKAGE_HELPERS_H
#define SNOVAC_TEST_PACKAGE_HELPERS_H
#include <stdio.h>
#include <string.h>
static void write_file(const char *dir, const char *name, const char *content) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "w");
    if (!f) {
        printf("FAIL setup: could not create %s\n", path);
        return;
    }
    fputs(content, f);
    fclose(f);
}

/* Fixtures are written under the OS tmp dir (mkdtemp) and deliberately left
 * in place rather than cleaned up here — they're a few hundred bytes each,
 * and adding a directory walker just to unlink them isn't worth it in a test
 * binary. */

/* ── individual scenarios ────────────────────────────────────────────────── */

#endif
