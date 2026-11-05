#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif

#include "cmd_get.h"
#include "driver_utils.h"
#include "project.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#if defined(_WIN32)
#include <direct.h>
#include <windows.h>
#endif

/* ── String helpers ──────────────────────────────────────────────────────── */

static void str_trim(char *s) {
    if (!s) return;
    char *p = s;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) {
        s[--len] = '\0';
    }
}

/* ── File and Directory Copying (No Shell) ───────────────────────────────── */

static int copy_file_direct(const char *src, const char *dst) {
    FILE *in = fopen(src, "rb");
    if (!in) return 0;
    FILE *out = fopen(dst, "wb");
    if (!out) {
        fclose(in);
        return 0;
    }
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            fclose(in);
            fclose(out);
            return 0;
        }
    }
    fclose(in);
    fclose(out);
    return 1;
}

static int copy_dir_recursive(const char *src_dir, const char *dst_dir) {
    DIR *d = opendir(src_dir);
    if (!d) return 0;

    sn_driver_ensure_parent_dir(dst_dir);
#if defined(_WIN32)
    mkdir(dst_dir);
#else
    mkdir(dst_dir, 0755);
#endif

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        if (strcmp(ent->d_name, ".git") == 0) continue;

        char src_path[SNOVAC_PATH_MAX];
        char dst_path[SNOVAC_PATH_MAX];
        snprintf(src_path, sizeof(src_path), "%s/%s", src_dir, ent->d_name);
        snprintf(dst_path, sizeof(dst_path), "%s/%s", dst_dir, ent->d_name);

        struct stat st;
        if (stat(src_path, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                copy_dir_recursive(src_path, dst_path);
            } else if (S_ISREG(st.st_mode)) {
                copy_file_direct(src_path, dst_path);
            }
        }
    }
    closedir(d);
    return 1;
}

/* ── Safe Git (No Shell Interpolation) ────────────────────────────────────── */

static int run_git_clone_safe(const char *url, const char *version, const char *dest_dir) {
    char *argv[10];
    int argc = 0;
    argv[argc++] = "git";
    argv[argc++] = "clone";
    argv[argc++] = "-q";
    if (version && version[0]) {
        argv[argc++] = "--branch";
        argv[argc++] = (char *)version;
    }
    argv[argc++] = "--depth";
    argv[argc++] = "1";
    argv[argc++] = (char *)url;
    argv[argc++] = (char *)dest_dir;
    argv[argc] = NULL;

    int status = sn_driver_execv_read(argv, NULL, 0);
    return (status == 0) ? 0 : 1;
}

static int query_latest_remote_tag(const char *url, char *tag_out, size_t tag_sz) {
    char *argv[6];
    argv[0] = "git";
    argv[1] = "ls-remote";
    argv[2] = "--tags";
    argv[3] = "--sort=-v:refname";
    argv[4] = (char *)url;
    argv[5] = NULL;

    char buf[65536];
    if (sn_driver_execv_read(argv, buf, sizeof(buf)) < 0) {
        return 0;
    }

    int found = 0;
    char *line = buf;
    while (line && *line) {
        char *nl = strchr(line, '\n');
        if (nl) {
            *nl = '\0';
        }
        char *ref = strstr(line, "refs/tags/");
        if (ref) {
            ref += 10;
            size_t n = strlen(ref);
            while (n > 0 && (ref[n - 1] == '\r' || ref[n - 1] == ' ' || ref[n - 1] == '\t')) {
                ref[--n] = '\0';
            }
            if (!strstr(ref, "^{}") && n > 0) {
                snprintf(tag_out, tag_sz, "%s", ref);
                found = 1;
                break;
            }
        }
        line = nl ? nl + 1 : NULL;
    }
    return found;
}

static int fetch_dependency(const char *url_or_path, const char *version, const char *target_dir) {
    if (sn_driver_path_is_dir(target_dir)) {
        return 0; /* Already fetched */
    }

    sn_driver_ensure_parent_dir(target_dir);

    /* Check if local filesystem path */
    if (strncmp(url_or_path, "file://", 7) == 0 || sn_driver_path_is_dir(url_or_path) ||
        url_or_path[0] == '/' || url_or_path[0] == '.' || url_or_path[0] == '\\' ||
        (isalpha((unsigned char)url_or_path[0]) && url_or_path[1] == ':')) {
        const char *local_src = (strncmp(url_or_path, "file://", 7) == 0) ? url_or_path + 7 : url_or_path;
        char norm_src[SNOVAC_PATH_MAX];
        sn_driver_normalize_path(local_src, norm_src, sizeof(norm_src));
        if (sn_driver_path_is_dir(norm_src)) {
            if (version && version[0]) {
                char v_tag[128];
                snprintf(v_tag, sizeof(v_tag), "v%s", (version[0] == 'v' || version[0] == 'V') ? version + 1 : version);
                if (run_git_clone_safe(norm_src, v_tag, target_dir) == 0) return 0;
                if (run_git_clone_safe(norm_src, version, target_dir) == 0) return 0;
            }
            if (run_git_clone_safe(norm_src, NULL, target_dir) == 0) return 0;
            if (copy_dir_recursive(norm_src, target_dir)) return 0;
        }
    }

    /* Remote git repo */
    char full_url[1024];
    if (strncmp(url_or_path, "http://", 7) != 0 &&
        strncmp(url_or_path, "https://", 8) != 0 &&
        strncmp(url_or_path, "git@", 4) != 0 &&
        strncmp(url_or_path, "ssh://", 6) != 0) {
        snprintf(full_url, sizeof(full_url), "https://%s", url_or_path);
    } else {
        snprintf(full_url, sizeof(full_url), "%s", url_or_path);
    }

    /* Resolve version: if none provided or 'latest', query latest released tag */
    char resolved_ver[128] = {0};
    if (version && version[0] && strcmp(version, "latest") != 0) {
        snprintf(resolved_ver, sizeof(resolved_ver), "%s", version);
    } else {
        query_latest_remote_tag(full_url, resolved_ver, sizeof(resolved_ver));
    }

    /* 1. Try release tag `v<version>` (e.g. `v1.0.0`) */
    if (resolved_ver[0]) {
        char v_tag[128];
        if (resolved_ver[0] == 'v' || resolved_ver[0] == 'V') {
            snprintf(v_tag, sizeof(v_tag), "%s", resolved_ver);
        } else {
            snprintf(v_tag, sizeof(v_tag), "v%s", resolved_ver);
        }
        if (run_git_clone_safe(full_url, v_tag, target_dir) == 0) {
            return 0;
        }

        /* 2. Try release tag `<version>` (e.g. `1.0.0`) */
        const char *raw_ver = (resolved_ver[0] == 'v' || resolved_ver[0] == 'V') ? resolved_ver + 1 : resolved_ver;
        if (run_git_clone_safe(full_url, raw_ver, target_dir) == 0) {
            return 0;
        }
    }

    /* 3. Fallback: clone default branch / HEAD */
    int rc = run_git_clone_safe(full_url, NULL, target_dir);
    return rc;
}

/* ── Manifest Representation & Parser ────────────────────────────────────── */

typedef struct SnManifestDep {
    char raw[512];
    char from[256];
    char module[256];
    char version[64];
    int is_edge;
    struct SnManifestDep *next;
} SnManifestDep;

typedef struct SnManifest {
    char module_name[256];
    char module_url[512];
    char snova_version[64];
    int is_root;
    SnManifestDep *direct;
    SnManifestDep *indirect;
    int direct_count;
    int indirect_count;
} SnManifest;

static void parse_dep_string(const char *str, SnManifestDep *dep) {
    memset(dep, 0, sizeof(*dep));
    snprintf(dep->raw, sizeof(dep->raw), "%s", str);
    str_trim(dep->raw);

    const char *arrow = strstr(str, "->");
    if (arrow) {
        dep->is_edge = 1;
        size_t from_len = (size_t)(arrow - str);
        char from_buf[256];
        if (from_len >= sizeof(from_buf)) from_len = sizeof(from_buf) - 1;
        memcpy(from_buf, str, from_len);
        from_buf[from_len] = '\0';
        str_trim(from_buf);
        snprintf(dep->from, sizeof(dep->from), "%s", from_buf);

        const char *to_str = arrow + 2;
        char to_buf[256];
        snprintf(to_buf, sizeof(to_buf), "%s", to_str);
        str_trim(to_buf);
        char *at = strchr(to_buf, '@');
        if (at) {
            *at = '\0';
            snprintf(dep->version, sizeof(dep->version), "%s", at + 1);
            str_trim(dep->version);
        }
        str_trim(to_buf);
        snprintf(dep->module, sizeof(dep->module), "%s", to_buf);
    } else {
        dep->is_edge = 0;
        char buf[256];
        snprintf(buf, sizeof(buf), "%s", str);
        str_trim(buf);
        char *at = strchr(buf, '@');
        if (at) {
            *at = '\0';
            snprintf(dep->version, sizeof(dep->version), "%s", at + 1);
            str_trim(dep->version);
        }
        str_trim(buf);
        snprintf(dep->module, sizeof(dep->module), "%s", buf);
    }
}

static int find_dir_manifest(const char *dir, char *out_path, size_t out_sz) {
    const char *names[] = {"mod.sns", "snova.sns", "snova.mod", "snova.toml"};
    for (size_t i = 0; i < sizeof(names)/sizeof(names[0]); i++) {
        char cand[SNOVAC_PATH_MAX + 32];
        snprintf(cand, sizeof(cand), "%s/%s", dir, names[i]);
        if (sn_driver_path_is_file(cand)) {
            snprintf(out_path, out_sz, "%s", cand);
            return 1;
        }
    }
    return 0;
}

