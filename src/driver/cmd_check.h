/* cmd_check.h — single-file and project-wide check commands. */
#ifndef SNOVAC_CMD_CHECK_H
#define SNOVAC_CMD_CHECK_H

#include "arena.h"
#include "ast.h"
#include "check.h"
#include "diag.h"
#include "package.h"
#include "resolve.h"

typedef struct {
    const char *own_prefix;
} SnBodyCheckScope;

SnDiagFile sn_cmd_begin_symbol_file(SnDiagSink *diag, const SnSymbol *sym);
void sn_cmd_check_all_bodies(SnChecker *c, SnResolver *resolver, SnPackageGraph *graph,
                     SnArena *arena, const SnBodyCheckScope *scope);
void sn_cmd_report_import_cycle(SnDiagSink *diag, SnPackageGraph *graph, const SnList *cycle);

int sn_cmd_check(const char *path, int dump);
/* Type-check before execution. Warnings stay off the success path so a
 * clean run's stdout/stderr is only the program. Errors are printed. */
int sn_cmd_check_for_exec(const char *path);
int sn_cmd_check_project(const char *path, int typecheck_bodies);
int sn_cmd_check_parse_project(const char *path, int dump);

#endif /* SNOVAC_CMD_CHECK_H */
