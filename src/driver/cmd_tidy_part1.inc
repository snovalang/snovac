/* cmd_tidy.c — automated dependency pruning and mod.sns manifest generator. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "cmd_tidy.h"
#include "arena.h"
#include "diag.h"
#include "driver_utils.h"
#include "intern.h"
#include "lex.h"
#include "package.h"
#include "parse.h"
#include "project.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "cmd_get.h"

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

static int has_snova_extension(const char *path) {
    return sn_driver_path_is_source(path) || sn_driver_path_is_script(path);
}

static void extract_package_from_file(const char *path, char *out_pkg, size_t out_sz) {
    out_pkg[0] = '\0';
    FILE *f = fopen(path, "r");
    if (!f) return;

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, "package ", 8) == 0) {
            p += 8;
            while (*p == ' ' || *p == '\t') p++;
            size_t len = strlen(p);
            while (len > 0 && (p[len - 1] == '\n' || p[len - 1] == '\r' || p[len - 1] == ' ' || p[len - 1] == '\t' || p[len - 1] == ';')) {
                p[--len] = '\0';
            }
            if (len > 0) {
                snprintf(out_pkg, out_sz, "%s", p);
            }
            break;
        }
    }
    fclose(f);
}

static void scan_module_files_recursive(const char *dir, DiscoveredModule *mod) {
    DIR *d = opendir(dir);
    if (!d) return;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }

        char path[SNOVAC_PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);

        struct stat st;
        if (stat(path, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            scan_module_files_recursive(path, mod);
        } else if (S_ISREG(st.st_mode) && has_snova_extension(path)) {
            char pkg[128];
            extract_package_from_file(path, pkg, sizeof(pkg));
            if (pkg[0] != '\0') {
                int already = 0;
                for (size_t i = 0; i < mod->package_count; i++) {
                    if (strcmp(mod->provided_packages[i], pkg) == 0) {
                        already = 1;
                        break;
                    }
                }
                if (!already && mod->package_count < MAX_PKGS_PER_MODULE) {
                    snprintf(mod->provided_packages[mod->package_count++], sizeof(mod->provided_packages[0]), "%s", pkg);
                }
            }
        }
    }
    closedir(d);
}

static const char *clean_module_id(const char *id) {
    if (!id) return "";
    if (strncmp(id, "https://", 8) == 0) return id + 8;
    if (strncmp(id, "http://", 7) == 0) return id + 7;
    return id;
}

static void parse_module_manifest(const char *manifest_path, DiscoveredModule *mod) {
    FILE *f = fopen(manifest_path, "r");
    if (!f) return;

    char line[512];
    int in_direct = 0;
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;

        if (strncmp(p, "module ", 7) == 0) {
            p += 7;
            while (*p == ' ' || *p == '\t') p++;
            char *url_start = strchr(p, ' ');
            if (!url_start) url_start = strchr(p, '\t');
            if (url_start) {
                *url_start = '\0';
                url_start++;
                while (*url_start == ' ' || *url_start == '\t') url_start++;
                size_t ulen = strlen(url_start);
                while (ulen > 0 && (url_start[ulen - 1] == '\n' || url_start[ulen - 1] == '\r' || url_start[ulen - 1] == ' ' || url_start[ulen - 1] == '\t')) {
                    url_start[--ulen] = '\0';
                }
                if (ulen > 0) {
                    snprintf(mod->module_id, sizeof(mod->module_id), "%s", clean_module_id(url_start));
                }
            } else {
                size_t len = strlen(p);
                while (len > 0 && (p[len - 1] == '\n' || p[len - 1] == '\r' || p[len - 1] == ' ' || p[len - 1] == '\t')) {
                    p[--len] = '\0';
                }
                if (len > 0 && mod->module_id[0] == '\0') {
                    snprintf(mod->module_id, sizeof(mod->module_id), "%s", clean_module_id(p));
                }
            }
        } else if (strncmp(p, "snova ", 6) == 0) {
            p += 6;
            while (*p == ' ' || *p == '\t' || *p == '"' || *p == '=') p++;
            size_t len = strlen(p);
            while (len > 0 && (p[len - 1] == '\n' || p[len - 1] == '\r' || p[len - 1] == ' ' || p[len - 1] == '\t' || p[len - 1] == '"')) {
                p[--len] = '\0';
            }
            if (len > 0) snprintf(mod->version, sizeof(mod->version), "%s", p);
        } else if (strstr(p, "direct = [") || strstr(p, "direct=[")) {
            in_direct = 1;
        } else if (in_direct && strchr(p, ']')) {
            in_direct = 0;
        } else if (in_direct) {
            char *quote_start = strchr(p, '"');
            if (quote_start) {
                quote_start++;
                char *quote_end = strchr(quote_start, '"');
                if (quote_end) {
                    *quote_end = '\0';
                    char *at = strchr(quote_start, '@');
                    if (at) *at = '\0';
                    if (mod->sub_dep_count < MAX_SUBDEPS_PER_MODULE) {
                        snprintf(mod->sub_deps[mod->sub_dep_count++], sizeof(mod->sub_deps[0]), "%s", clean_module_id(quote_start));
                    }
                }
            }
        }
    }
    fclose(f);
}

static void discover_deps_recursive(const char *deps_root, const char *current_dir,
                                    DiscoveredModule *mods, size_t *mod_count) {
    DIR *d = opendir(current_dir);
    if (!d) return;

    char manifest[SNOVAC_PATH_MAX];
    struct stat mst;
    snprintf(manifest, sizeof(manifest), "%s/mod.sns", current_dir);
    if (stat(manifest, &mst) != 0) {
        snprintf(manifest, sizeof(manifest), "%s/snova.sns", current_dir);
    }
    int has_manifest = (stat(manifest, &mst) == 0);

    if (has_manifest && strcmp(current_dir, deps_root) != 0) {
        if (*mod_count < MAX_DISCOVERED_MODULES) {
            DiscoveredModule *m = &mods[*mod_count];
            memset(m, 0, sizeof(*m));
            snprintf(m->module_dir, sizeof(m->module_dir), "%s", current_dir);
            snprintf(m->version, sizeof(m->version), "1.0.0");

            /* Default module_id from relative path if not overridden by mod.sns */
            if (strlen(current_dir) > strlen(deps_root) + 1) {
                snprintf(m->module_id, sizeof(m->module_id), "%s", current_dir + strlen(deps_root) + 1);
            }

            parse_module_manifest(manifest, m);
            scan_module_files_recursive(current_dir, m);
            (*mod_count)++;
        }
        closedir(d);
        return;
    }

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        char sub[SNOVAC_PATH_MAX];
        snprintf(sub, sizeof(sub), "%s/%s", current_dir, ent->d_name);

        struct stat st;
        if (stat(sub, &st) == 0 && S_ISDIR(st.st_mode)) {
            discover_deps_recursive(deps_root, sub, mods, mod_count);
        }
    }
    closedir(d);
}

static void read_module_header(const char *manifest_path, char *mod_name, size_t mod_sz,
                               char *mod_url, size_t url_sz, char *snova_ver, size_t ver_sz, int *is_root) {
    snprintf(mod_name, mod_sz, "app");
    if (mod_url && url_sz > 0) mod_url[0] = '\0';
    snprintf(snova_ver, ver_sz, "1.0.0");
    if (is_root) *is_root = 0;

    FILE *f = fopen(manifest_path, "r");
    if (!f) return;

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, "module ", 7) == 0) {
            p += 7;
            while (*p == ' ' || *p == '\t') p++;
            char *url_start = strchr(p, ' ');
            if (!url_start) url_start = strchr(p, '\t');
            
            if (url_start) {
                *url_start = '\0';
                url_start++;
                while (*url_start == ' ' || *url_start == '\t') url_start++;
                size_t len = strlen(url_start);
                while (len > 0 && (url_start[len - 1] == '\n' || url_start[len - 1] == '\r' || url_start[len - 1] == ' ' || url_start[len - 1] == '\t')) {
                    url_start[--len] = '\0';
                }
                if (mod_url && url_sz > 0 && len > 0) {
                    snprintf(mod_url, url_sz, "%s", url_start);
                }
            }
            
            size_t len = strlen(p);
            while (len > 0 && (p[len - 1] == '\n' || p[len - 1] == '\r' || p[len - 1] == ' ' || p[len - 1] == '\t')) {
                p[--len] = '\0';
            }
            if (len > 0) snprintf(mod_name, mod_sz, "%s", p);
        } else if (strncmp(p, "root", 4) == 0 && (p[4] == ' ' || p[4] == '\t' || p[4] == '=' || p[4] == '\0' || p[4] == '\n' || p[4] == '\r')) {
            p += 4;
            while (*p == ' ' || *p == '\t' || *p == '=') p++;
            if (*p == '\0' || *p == '\n' || *p == '\r' || strncmp(p, "true", 4) == 0 || strncmp(p, "1", 1) == 0) {
                if (is_root) *is_root = 1;
            }
        } else if (strncmp(p, "is_root", 7) == 0 && (p[7] == ' ' || p[7] == '\t' || p[7] == '=' || p[7] == '\0')) {
            p += 7;
            while (*p == ' ' || *p == '\t' || *p == '=') p++;
            if (*p == '\0' || *p == '\n' || *p == '\r' || strncmp(p, "true", 4) == 0 || strncmp(p, "1", 1) == 0) {
                if (is_root) *is_root = 1;
            }
        } else if (strncmp(p, "snova ", 6) == 0) {
            p += 6;
            while (*p == ' ' || *p == '\t' || *p == '"' || *p == '=') p++;
            size_t len = strlen(p);
            while (len > 0 && (p[len - 1] == '\n' || p[len - 1] == '\r' || p[len - 1] == ' ' || p[len - 1] == '\t' || p[len - 1] == '"')) {
                p[--len] = '\0';
            }
            if (len > 0) snprintf(snova_ver, ver_sz, "%s", p);
        }
    }
    fclose(f);
}

