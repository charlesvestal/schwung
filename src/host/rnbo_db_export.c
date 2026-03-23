/* rnbo_db_export.c — Export RNBO Runner sets from sqlite DB as pack directories.
 *
 * Reads the RNBO Runner sqlite DB and generates pack directories so its
 * graphs appear as chainable modules via the existing scan_packs pipeline. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "sqlite3.h"
#include "rnbo_db_export.h"

#define RNBO_RUNNER_DB "/data/UserData/Documents/rnbo/oscqueryrunner.sqlite"
#define RNBO_CACHE_SO  "/data/UserData/Documents/rnbo/cache/so"
#define RNBO_CACHE_SRC "/data/UserData/Documents/rnbo/cache/src"

/* Sanitize a set name into a safe directory name (spaces/slashes -> dashes) */
static void sanitize_dirname(const char *name, char *out, size_t out_len) {
    size_t i = 0, o = 0;
    while (name[i] && o < out_len - 1) {
        char c = name[i++];
        if (c == ' ' || c == '/' || c == '\\' || c == ':' || c == '*' ||
            c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
            c = '-';
        out[o++] = c;
    }
    out[o] = '\0';
}

/* Helper: write a string to a file, creating parent dirs as needed */
static int write_file_with_dirs(const char *path, const char *content) {
    char dir[1024];
    strncpy(dir, path, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        char cmd[1100];
        snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", dir);
        system(cmd);
    }
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fputs(content, f);
    fclose(f);
    return 0;
}

static void append_json_escaped(char **buf, size_t *len, size_t *cap,
                                const char *str) {
    while (*str) {
        if (*len + 8 > *cap) {
            *cap = (*cap) * 2;
            *buf = realloc(*buf, *cap);
        }
        char c = *str++;
        if (c == '"' || c == '\\') {
            (*buf)[(*len)++] = '\\';
            (*buf)[(*len)++] = c;
        } else if (c == '\n') {
            (*buf)[(*len)++] = '\\';
            (*buf)[(*len)++] = 'n';
        } else if (c == '\r') {
            (*buf)[(*len)++] = '\\';
            (*buf)[(*len)++] = 'r';
        } else if (c == '\t') {
            (*buf)[(*len)++] = '\\';
            (*buf)[(*len)++] = 't';
        } else {
            (*buf)[(*len)++] = c;
        }
    }
}

static void append_raw(char **buf, size_t *len, size_t *cap, const char *str) {
    size_t slen = strlen(str);
    while (*len + slen + 1 > *cap) {
        *cap = (*cap) * 2;
        *buf = realloc(*buf, *cap);
    }
    memcpy(*buf + *len, str, slen);
    *len += slen;
}

void export_rnbo_runner_sets(const char *packs_path) {
    printf("mm: export_rnbo_runner_sets called for '%s'\n", packs_path);
    struct stat db_st;
    if (stat(RNBO_RUNNER_DB, &db_st) != 0) {
        printf("mm: RNBO Runner DB not found, skipping\n");
        return;
    }
    printf("mm: RNBO Runner DB found, mtime=%ld\n", (long)db_st.st_mtime);

    sqlite3 *db = NULL;
    if (sqlite3_open_v2(RNBO_RUNNER_DB, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return;
    }

    {
        char cmd[1100];
        snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", packs_path);
        system(cmd);
    }

    sqlite3_stmt *sets_stmt = NULL;
    if (sqlite3_prepare_v2(db, "SELECT id, name FROM sets", -1, &sets_stmt, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return;
    }

    while (sqlite3_step(sets_stmt) == SQLITE_ROW) {
        int set_id = sqlite3_column_int(sets_stmt, 0);
        const char *set_name = (const char *)sqlite3_column_text(sets_stmt, 1);
        if (!set_name || !set_name[0]) continue;

        char safe_name[128];
        sanitize_dirname(set_name, safe_name, sizeof(safe_name));

        char info_path[1024];
        snprintf(info_path, sizeof(info_path), "%s/%s/info.json", packs_path, safe_name);
        struct stat info_st;
        if (stat(info_path, &info_st) == 0 && info_st.st_mtime >= db_st.st_mtime) {
            continue;
        }

        printf("mm: exporting RNBO Runner set '%s'\n", set_name);

        sqlite3_stmt *inst_stmt = NULL;
        if (sqlite3_prepare_v2(db,
                "SELECT si.set_instance_index, si.config, p.name, p.so_path, p.config_path "
                "FROM sets_patcher_instances si "
                "JOIN patchers p ON si.patcher_id = p.id "
                "WHERE si.set_id = ? "
                "ORDER BY si.set_instance_index",
                -1, &inst_stmt, NULL) != SQLITE_OK) continue;
        sqlite3_bind_int(inst_stmt, 1, set_id);

        #define MAX_INST 32
        struct {
            int instance_index;
            char patcher_name[128];
            char so_path[256];
            char config_path[256];
        } instances[MAX_INST];
        char *config_copies[MAX_INST];
        int inst_count = 0;
        char seen_so[MAX_INST][256];
        int seen_count = 0;

        while (sqlite3_step(inst_stmt) == SQLITE_ROW && inst_count < MAX_INST) {
            int idx = inst_count;
            instances[idx].instance_index = sqlite3_column_int(inst_stmt, 0);
            const char *cfg = (const char *)sqlite3_column_text(inst_stmt, 1);
            config_copies[idx] = cfg ? strdup(cfg) : strdup("{}");
            const char *pname = (const char *)sqlite3_column_text(inst_stmt, 2);
            const char *so = (const char *)sqlite3_column_text(inst_stmt, 3);
            const char *cfgp = (const char *)sqlite3_column_text(inst_stmt, 4);
            strncpy(instances[idx].patcher_name, pname ? pname : "", 127);
            strncpy(instances[idx].so_path, so ? so : "", 255);
            strncpy(instances[idx].config_path, cfgp ? cfgp : "", 255);
            inst_count++;
        }
        sqlite3_finalize(inst_stmt);

        if (inst_count == 0) {
            for (int i = 0; i < inst_count; i++) free(config_copies[i]);
            continue;
        }

        int any_so_exists = 0;
        for (int i = 0; i < inst_count; i++) {
            char so_full[512];
            snprintf(so_full, sizeof(so_full), "%s/%s", RNBO_CACHE_SO, instances[i].so_path);
            struct stat so_st;
            if (stat(so_full, &so_st) == 0) { any_so_exists = 1; break; }
        }
        if (!any_so_exists) {
            printf("mm: skipping set '%s' - no .so files found in cache\n", set_name);
            for (int i = 0; i < inst_count; i++) free(config_copies[i]);
            continue;
        }

        size_t cap = 4096, len = 0;
        char *buf = malloc(cap);

        append_raw(&buf, &len, &cap, "{\n  \"name\": \"");
        append_json_escaped(&buf, &len, &cap, set_name);
        append_raw(&buf, &len, &cap, "\",\n  \"runner_db\": true,\n  \"patchers\": [\n");

        int first_patcher = 1;
        for (int i = 0; i < inst_count; i++) {
            int dup = 0;
            for (int j = 0; j < seen_count; j++) {
                if (strcmp(seen_so[j], instances[i].so_path) == 0) { dup = 1; break; }
            }
            if (dup) continue;
            strncpy(seen_so[seen_count++], instances[i].so_path, 255);

            if (!first_patcher) append_raw(&buf, &len, &cap, ",\n");
            first_patcher = 0;

            char tmp[1024];
            snprintf(tmp, sizeof(tmp),
                "    {\n"
                "      \"name\": \"%s\",\n"
                "      \"binaries\": {\n"
                "        \"aarch64-Linux-GNU-11.4.0\": \"%s/%s\"\n"
                "      },\n"
                "      \"config\": \"%s/%s\"\n"
                "    }",
                instances[i].patcher_name,
                RNBO_CACHE_SO, instances[i].so_path,
                RNBO_CACHE_SRC, instances[i].config_path);
            append_raw(&buf, &len, &cap, tmp);
        }

        {
            char tmp[512];
            snprintf(tmp, sizeof(tmp),
                "\n  ],\n"
                "  \"sets\": [\n"
                "    { \"name\": \"%s\", \"location\": \"sets/%s.json\" }\n"
                "  ],\n"
                "  \"targets\": {\n"
                "    \"aarch64-Linux-GNU-11.4.0\": { \"system_processor\": \"aarch64\" }\n"
                "  }\n"
                "}",
                set_name, safe_name);
            append_raw(&buf, &len, &cap, tmp);
        }

        buf[len] = '\0';
        write_file_with_dirs(info_path, buf);

        len = 0;
        append_raw(&buf, &len, &cap, "{\n  \"name\": \"");
        append_json_escaped(&buf, &len, &cap, set_name);
        append_raw(&buf, &len, &cap, "\",\n  \"instances\": [\n");

        for (int i = 0; i < inst_count; i++) {
            if (i > 0) append_raw(&buf, &len, &cap, ",\n");
            char tmp[256];
            snprintf(tmp, sizeof(tmp),
                "    { \"instance_index\": %d, \"patcher_name\": \"%s\", \"config\": ",
                instances[i].instance_index, instances[i].patcher_name);
            append_raw(&buf, &len, &cap, tmp);
            append_raw(&buf, &len, &cap, config_copies[i]);
            append_raw(&buf, &len, &cap, " }");
        }

        append_raw(&buf, &len, &cap, "\n  ],\n  \"connections\": [\n");

        sqlite3_stmt *conn_stmt = NULL;
        if (sqlite3_prepare_v2(db,
                "SELECT source_name, source_instance_index, source_port_name, "
                "sink_name, sink_instance_index, sink_port_name "
                "FROM sets_connections WHERE set_id = ?",
                -1, &conn_stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_int(conn_stmt, 1, set_id);
            int first_conn = 1;
            while (sqlite3_step(conn_stmt) == SQLITE_ROW) {
                if (!first_conn) append_raw(&buf, &len, &cap, ",\n");
                first_conn = 0;
                const char *sn = (const char *)sqlite3_column_text(conn_stmt, 0);
                int si = sqlite3_column_int(conn_stmt, 1);
                const char *sp = (const char *)sqlite3_column_text(conn_stmt, 2);
                const char *dn = (const char *)sqlite3_column_text(conn_stmt, 3);
                int di = sqlite3_column_int(conn_stmt, 4);
                const char *dp = (const char *)sqlite3_column_text(conn_stmt, 5);
                char tmp[512];
                snprintf(tmp, sizeof(tmp),
                    "    { \"source_name\": \"%s\", \"source_instance_index\": %d, "
                    "\"source_port_name\": \"%s\", "
                    "\"sink_name\": \"%s\", \"sink_instance_index\": %d, "
                    "\"sink_port_name\": \"%s\" }",
                    sn ? sn : "", si, sp ? sp : "",
                    dn ? dn : "", di, dp ? dp : "");
                append_raw(&buf, &len, &cap, tmp);
            }
            sqlite3_finalize(conn_stmt);
        }

        append_raw(&buf, &len, &cap, "\n  ],\n  \"presets\": {\n    \"initial\": [\n");

        sqlite3_stmt *preset_stmt = NULL;
        if (sqlite3_prepare_v2(db,
                "SELECT sp.preset_index, sp.content, p.name, sp.set_instance_index "
                "FROM sets_presets sp "
                "JOIN patchers p ON sp.patcher_id = p.id "
                "WHERE sp.set_id = ?",
                -1, &preset_stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_int(preset_stmt, 1, set_id);
            int first_preset = 1;
            while (sqlite3_step(preset_stmt) == SQLITE_ROW) {
                if (!first_preset) append_raw(&buf, &len, &cap, ",\n");
                first_preset = 0;
                int pidx = sqlite3_column_int(preset_stmt, 0);
                const char *content = (const char *)sqlite3_column_text(preset_stmt, 1);
                const char *pname = (const char *)sqlite3_column_text(preset_stmt, 2);
                int inst_idx = sqlite3_column_int(preset_stmt, 3);
                char tmp[256];
                snprintf(tmp, sizeof(tmp),
                    "      { \"instanceindex\": %d, \"patchername\": \"%s\", "
                    "\"presetindex\": %d, \"content\": ",
                    inst_idx, pname ? pname : "", pidx);
                append_raw(&buf, &len, &cap, tmp);
                append_raw(&buf, &len, &cap, content ? content : "{}");
                append_raw(&buf, &len, &cap, " }");
            }
            sqlite3_finalize(preset_stmt);
        }

        append_raw(&buf, &len, &cap, "\n    ]\n  }\n}");

        buf[len] = '\0';
        char set_json_path[1024];
        snprintf(set_json_path, sizeof(set_json_path), "%s/%s/sets/%s.json",
                 packs_path, safe_name, safe_name);
        write_file_with_dirs(set_json_path, buf);

        free(buf);
        for (int i = 0; i < inst_count; i++) free(config_copies[i]);
    }

    sqlite3_finalize(sets_stmt);
    sqlite3_close(db);
}
