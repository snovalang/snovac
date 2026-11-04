#ifndef SNOVAC_CMD_TIDY_INTERNAL_H
#define SNOVAC_CMD_TIDY_INTERNAL_H
#include "driver_utils.h"
#define MAX_DISCOVERED_MODULES 128
#define MAX_PKGS_PER_MODULE 64
#define MAX_SUBDEPS_PER_MODULE 64
typedef struct {
    char module_id[256];
    char module_dir[SNOVAC_PATH_MAX];
    char version[64];
    char provided_packages[MAX_PKGS_PER_MODULE][128];
    size_t package_count;
    char sub_deps[MAX_SUBDEPS_PER_MODULE][256];
    size_t sub_dep_count;
} DiscoveredModule;
const char *clean_module_id(const char *id);
void scan_module_files_recursive(const char *, DiscoveredModule *);
void parse_module_manifest(const char *, DiscoveredModule *);
void discover_deps_recursive(const char *, const char *, DiscoveredModule *, size_t *);
void read_module_header(const char *, char *, size_t, char *, size_t, char *, size_t, int *);
#endif
