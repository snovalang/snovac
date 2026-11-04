#include "../arena.h"
#include "../diag.h"
#include "../intern.h"
#include "../package.h"
#include "test_package_corpus.h"
#include <stdio.h>
#include <string.h>
int report_real_corpus(SnInternTable *it, SnArena *a) {

    SnDiagSink diag;
    sn_diag_init(&diag, "<corpus-scan>", "", 0);
    diag.out = fopen("/dev/null", "w"); /* the point here is the counters, not the text */
    if (!diag.out) {
        diag.out = stderr;
    }

    SnPackageGraph g;
    sn_pkggraph_init(&g, a, it, &diag);
    size_t f1 = sn_pkggraph_scan_root(&g, "../compiler/src");
    size_t f2 = sn_pkggraph_scan_root(&g, "../builtin");
    sn_pkggraph_link(&g);

    SnList cycle;
    memset(&cycle, 0, sizeof(cycle));
    int has_cycle = sn_pkggraph_find_cycle(&g, &cycle);

    printf("\n[real corpus] compiler/src+builtin: %zu+%zu=%zu files scanned, "
           "%zu packages, %zu link errors (known pre-existing corpus "
           "mismatches, out of snovac's module scope — see file header), "
           "cycle=%d\n",
           f1, f2, f1 + f2, g.node_count, (size_t)diag.error_count, has_cycle);

    int ok = has_cycle == 0;

    if (diag.out != stderr) {
        fclose(diag.out);
    }
    return ok;
}
