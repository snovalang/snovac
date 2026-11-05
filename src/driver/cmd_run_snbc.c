/* cmd_run_snbc.c — load a canonical image and execute it.
 * The executor is the generated switch in the native backend. There is no
 * in-process bytecode VM. A missing file, a short file, or a bad magic
 * exits non-zero. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "cmd_run_snbc.h"
#include "driver_utils.h"
#include "native_backend.h"
#include "snbc.h"
#include "target.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

static int make_temp_bin(char *out, size_t out_sz) {
#ifdef _WIN32
    char dir[MAX_PATH];
    if (GetTempPathA((DWORD)sizeof(dir), dir) == 0) {
        return 0;
    }
    if (GetTempFileNameA(dir, "snb", 0, out) == 0) {
        return 0;
    }
    if (strlen(out) + 1 > out_sz) {
        remove(out);
        return 0;
    }
    return 1;
#else
    char tmpl[] = "/tmp/snbc-run-XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd < 0) {
        return 0;
    }
    close(fd);
    if (strlen(tmpl) + 1 > out_sz) {
        remove(tmpl);
        return 0;
    }
    memcpy(out, tmpl, strlen(tmpl) + 1);
    return 1;
#endif
}

int sn_cmd_run_snbc(const char *path) {
    if (!path || path[0] == '\0') {
        fprintf(stderr, "error: run-snbc needs a file.snbc\n");
        return 2;
    }

    SnBCUnit unit;
    sn_bcunit_init(&unit);
    if (!sn_bcunit_load_canonical(&unit, path)) {
        fprintf(stderr, "error: cannot read SnBC image '%s'\n", path);
        sn_bcunit_free(&unit);
        return 1;
    }

    char out_path[SNOVAC_PATH_MAX];
    if (!make_temp_bin(out_path, sizeof(out_path))) {
        fprintf(stderr, "error: cannot create a temporary runner\n");
        sn_bcunit_free(&unit);
        return 1;
    }

    SnTargetInfo target;
    sn_target_init_default(&target);
    int compiled = sn_native_compile(&unit, &target, out_path);
    sn_bcunit_free(&unit);
    if (!compiled) {
        remove(out_path);
        return 1;
    }

    char *argv[2];
    argv[0] = out_path;
    argv[1] = NULL;
    int status = sn_driver_execv(argv);
    remove(out_path);
    if (status < 0) {
        return 1;
    }
    return status;
}
