#ifndef SNOVAC_PACKAGE_INTERNAL_H
#define SNOVAC_PACKAGE_INTERNAL_H

#include "package.h"
#include "lex.h"

SnPackageNode *sn_pkggraph_find_or_create(SnPackageGraph *graph,
                                          const char *name);
void sn_pkggraph_scan_header(SnPackageGraph *graph, const char *path,
                             const char *source, size_t source_len);
const char *sn_pkggraph_scan_qualified(SnArena *arena,
                                       const SnTokenVec *tokens, size_t *pos,
                                       SnSpan *span_out);
const char *canonical_import_name(const char *name);
int find_nearest_manifest_rel_pkg(const char *file_path, char *out,
                                  size_t out_size);
int sn_pkggraph_scan_section(SnPackageGraph *graph, const SnDiagFile *file,
                             const SnTokenVec *tokens, size_t *pos);

#endif
