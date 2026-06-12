/*
 * Shared QuickJS scaffolding and JS host bindings — see js_host_common.h.
 *
 * Extracted from schwung_host.c and shadow_ui.c, which had drifted copies.
 * The shadow_ui.c variants were taken as canonical (newer curl config,
 * broader validate_path base, update-staging-aware remove_dir), except
 * host_extract_tar_strip, which keeps schwung_host.c's fork/execvp argv
 * mechanism instead of shadow_ui.c's system() with interpolated paths.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "quickjs.h"
#include "quickjs-libc.h"

#include "js_host_common.h"
#include "unified_log.h"

#define BASE_DIR "/data/UserData"
#define MODULES_DIR "/data/UserData/schwung/modules"
#define CURL_PATH "/data/UserData/schwung/bin/curl"

/* ============================================================================
 * QuickJS scaffolding
 * ============================================================================ */

/* also used to initialize the worker context */
JSContext *JS_NewCustomContext(JSRuntime *rt) {
    JSContext *ctx = JS_NewContext(rt);
    if (!ctx) return NULL;
    /* system modules */
    js_init_module_std(ctx, "std");
    js_init_module_os(ctx, "os");
    return ctx;
}

int eval_buf(JSContext *ctx, const void *buf, int buf_len,
             const char *filename, int eval_flags) {
    JSValue val;
    int ret;
    if ((eval_flags & JS_EVAL_TYPE_MASK) == JS_EVAL_TYPE_MODULE) {
        /* for the modules, we compile then run to be able to set
           import.meta */
        val = JS_Eval(ctx, buf, buf_len, filename, eval_flags | JS_EVAL_FLAG_COMPILE_ONLY);
        if (!JS_IsException(val)) {
            js_module_set_import_meta(ctx, val, 1, 1);
            val = JS_EvalFunction(ctx, val);
        }
        val = js_std_await(ctx, val);
    } else {
        val = JS_Eval(ctx, buf, buf_len, filename, eval_flags);
    }
    if (JS_IsException(val)) {
        js_std_dump_error(ctx);
        ret = -1;
    } else {
        ret = 0;
    }
    JS_FreeValue(ctx, val);
    return ret;
}

int eval_file(JSContext *ctx, const char *filename, int module) {
    uint8_t *buf;
    int ret, eval_flags = JS_EVAL_FLAG_STRICT;
    size_t buf_len;

    buf = js_load_file(ctx, &buf_len, filename);
    if (!buf) {
        perror(filename);
        return -1;
    }

    if (module) eval_flags |= JS_EVAL_TYPE_MODULE;
    ret = eval_buf(ctx, buf, buf_len, filename, eval_flags);
    js_free(ctx, buf);
    return ret;
}

int getGlobalFunction(JSContext *ctx, const char *func_name, JSValue *retFunc) {
    JSValue global_obj = JS_GetGlobalObject(ctx);
    JSValue func = JS_GetPropertyStr(ctx, global_obj, func_name);
    if (!JS_IsFunction(ctx, func)) {
        JS_FreeValue(ctx, func);
        JS_FreeValue(ctx, global_obj);
        return 0;
    }
    *retFunc = func;
    JS_FreeValue(ctx, global_obj);
    return 1;
}

int callGlobalFunction(JSContext *ctx, JSValue *pfunc, unsigned char *data) {
    JSValue ret;
    int is_exception;
    if (data) {
        JSValue arr = JS_NewArray(ctx);
        for (int i = 0; i < 3; i++) {
            JS_SetPropertyUint32(ctx, arr, i, JS_NewInt32(ctx, data[i]));
        }
        JSValue args[1] = { arr };
        ret = JS_Call(ctx, *pfunc, JS_UNDEFINED, 1, args);
        JS_FreeValue(ctx, arr);
    } else {
        ret = JS_Call(ctx, *pfunc, JS_UNDEFINED, 0, 0);
    }
    is_exception = JS_IsException(ret);
    if (is_exception) {
        js_std_dump_error(ctx);
    }
    JS_FreeValue(ctx, ret);
    return is_exception;
}

/* ============================================================================
 * Path / process helpers
 * ============================================================================ */

/* Execute a command safely using fork/execvp instead of system() */
int run_command(const char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return -1;
    }
    if (pid == 0) {
        /* Child: redirect stderr to stdout, exec the command */
        dup2(STDOUT_FILENO, STDERR_FILENO);
        execvp(argv[0], (char *const *)argv);
        _exit(127); /* exec failed */
    }
    /* Parent: wait for child */
    int status;
    if (waitpid(pid, &status, 0) < 0) {
        perror("waitpid");
        return -1;
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    return -1;
}

/* Fire-and-forget: fork + setsid, parent returns immediately.
 * Child detaches from session and redirects stdio to /dev/null. */
static void run_command_background(const char *const argv[]) {
    pid_t pid = fork();
    if (pid != 0) return;          /* parent (or error) */
    /* child */
    setsid();
    int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) {
        dup2(devnull, STDIN_FILENO);
        dup2(devnull, STDOUT_FILENO);
        dup2(devnull, STDERR_FILENO);
        if (devnull > 2) close(devnull);
    }
    execvp(argv[0], (char *const *)argv);
    _exit(127);
}

/* Helper: validate path is within BASE_DIR to prevent directory traversal */
int validate_path(const char *path) {
    if (!path || strlen(path) < strlen(BASE_DIR)) return 0;
    if (strncmp(path, BASE_DIR, strlen(BASE_DIR)) != 0) return 0;
    if (strstr(path, "..") != NULL) return 0;

    /* Resolve symlinks and re-check the resolved path */
    char resolved[PATH_MAX];
    if (realpath(path, resolved) != NULL) {
        if (strncmp(resolved, BASE_DIR, strlen(BASE_DIR)) != 0) return 0;
    }
    /* If realpath fails (e.g. file doesn't exist yet), the basic checks above suffice */
    return 1;
}

/* ============================================================================
 * File bindings
 * ============================================================================ */

/* host_file_exists(path) -> bool */
static JSValue js_host_file_exists(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) {
        return JS_FALSE;
    }

    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) {
        return JS_FALSE;
    }

    if (!validate_path(path)) {
        JS_FreeCString(ctx, path);
        return JS_FALSE;
    }

    struct stat st;
    int exists = (stat(path, &st) == 0);

    JS_FreeCString(ctx, path);
    return exists ? JS_TRUE : JS_FALSE;
}

/* host_read_file(path) -> string or null */
static JSValue js_host_read_file(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) {
        return JS_NULL;
    }

    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) {
        return JS_NULL;
    }

    /* Validate path */
    if (!validate_path(path)) {
        fprintf(stderr, "host_read_file: invalid path: %s\n", path);
        JS_FreeCString(ctx, path);
        return JS_NULL;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        JS_FreeCString(ctx, path);
        return JS_NULL;
    }

    /* Get file size */
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    /* Limit to 4MB for safety (Song.abl can exceed 1MB) */
    if (size > 4 * 1024 * 1024) {
        fprintf(stderr, "host_read_file: file too large: %s\n", path);
        fclose(f);
        JS_FreeCString(ctx, path);
        return JS_NULL;
    }

    char *buf = malloc(size + 1);
    if (!buf) {
        fclose(f);
        JS_FreeCString(ctx, path);
        return JS_NULL;
    }

    size_t bytes_read = fread(buf, 1, size, f);
    buf[bytes_read] = '\0';
    fclose(f);

    JSValue result = JS_NewString(ctx, buf);
    free(buf);
    JS_FreeCString(ctx, path);

    return result;
}

/* Minimal RFC-4648 base64 encoder used by host_read_file_base64 below.
 * Standalone so we don't pull in mbedTLS/OpenSSL just for this one spot. */
static char *js_host_b64_encode(const unsigned char *in, size_t len, size_t *out_len) {
    static const char alpha[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t outlen = 4 * ((len + 2) / 3);
    char *out = (char *)malloc(outlen + 1);
    if (!out) return NULL;
    size_t i, j;
    for (i = 0, j = 0; i + 2 < len; i += 3, j += 4) {
        uint32_t t = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8)
                    | (uint32_t)in[i + 2];
        out[j]     = alpha[(t >> 18) & 0x3F];
        out[j + 1] = alpha[(t >> 12) & 0x3F];
        out[j + 2] = alpha[(t >> 6) & 0x3F];
        out[j + 3] = alpha[t & 0x3F];
    }
    if (i < len) {
        uint32_t t = (uint32_t)in[i] << 16;
        if (i + 1 < len) t |= (uint32_t)in[i + 1] << 8;
        out[j]     = alpha[(t >> 18) & 0x3F];
        out[j + 1] = alpha[(t >> 12) & 0x3F];
        out[j + 2] = (i + 1 < len) ? alpha[(t >> 6) & 0x3F] : '=';
        out[j + 3] = '=';
        j += 4;
    }
    out[j] = '\0';
    if (out_len) *out_len = j;
    return out;
}

/* host_read_file_base64(path) -> string | null
 * Reads the raw bytes of a file and returns a base64-encoded string.
 * Needed for providers like Gemini that take audio inline via JSON as
 * base64 rather than multipart form. Caps input at 16 MB to keep memory
 * usage bounded on the device. */
static JSValue js_host_read_file_base64(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_NULL;
    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_NULL;
    if (!validate_path(path)) {
        JS_FreeCString(ctx, path);
        return JS_NULL;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        JS_FreeCString(ctx, path);
        return JS_NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f); JS_FreeCString(ctx, path); return JS_NULL;
    }
    long size = ftell(f);
    if (size < 0 || size > 16L * 1024 * 1024) {
        fclose(f); JS_FreeCString(ctx, path); return JS_NULL;
    }
    rewind(f);
    unsigned char *buf = (unsigned char *)malloc((size_t)size + 1);
    if (!buf) { fclose(f); JS_FreeCString(ctx, path); return JS_NULL; }
    size_t n = fread(buf, 1, (size_t)size, f);
    fclose(f);
    JS_FreeCString(ctx, path);

    size_t b64_len = 0;
    char *b64 = js_host_b64_encode(buf, n, &b64_len);
    free(buf);
    if (!b64) return JS_NULL;
    JSValue result = JS_NewStringLen(ctx, b64, b64_len);
    free(b64);
    return result;
}

/* host_write_file(path, content) -> bool */
static JSValue js_host_write_file(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 2) {
        return JS_FALSE;
    }

    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) {
        return JS_FALSE;
    }

    const char *content = JS_ToCString(ctx, argv[1]);
    if (!content) {
        JS_FreeCString(ctx, path);
        return JS_FALSE;
    }

    /* Validate path */
    if (!validate_path(path)) {
        fprintf(stderr, "host_write_file: invalid path: %s\n", path);
        JS_FreeCString(ctx, path);
        JS_FreeCString(ctx, content);
        return JS_FALSE;
    }

    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "host_write_file: cannot open file: %s\n", path);
        JS_FreeCString(ctx, path);
        JS_FreeCString(ctx, content);
        return JS_FALSE;
    }

    size_t len = strlen(content);
    size_t written = fwrite(content, 1, len, f);
    fclose(f);

    JS_FreeCString(ctx, path);
    JS_FreeCString(ctx, content);

    return (written == len) ? JS_TRUE : JS_FALSE;
}

/* host_ensure_dir(path) -> bool - creates directory if it doesn't exist */
static JSValue js_host_ensure_dir(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) {
        return JS_FALSE;
    }

    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) {
        return JS_FALSE;
    }

    /* Validate path */
    if (!validate_path(path)) {
        fprintf(stderr, "host_ensure_dir: invalid path: %s\n", path);
        JS_FreeCString(ctx, path);
        return JS_FALSE;
    }

    const char *argv_cmd[] = { "mkdir", "-p", path, NULL };
    int result = run_command(argv_cmd);

    JS_FreeCString(ctx, path);

    return (result == 0) ? JS_TRUE : JS_FALSE;
}

/* host_remove_dir(path) -> bool */
static JSValue js_host_remove_dir(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) {
        return JS_FALSE;
    }

    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) {
        return JS_FALSE;
    }

    /* Validate path - must be within modules directory for safety */
    if (!validate_path(path)) {
        fprintf(stderr, "host_remove_dir: invalid path: %s\n", path);
        JS_FreeCString(ctx, path);
        return JS_FALSE;
    }

    /* Additional safety: must be within base directory (modules, staging, backup, tmp) */
    if (strncmp(path, MODULES_DIR, strlen(MODULES_DIR)) != 0 &&
        strncmp(path, BASE_DIR "/update-staging", strlen(BASE_DIR "/update-staging")) != 0 &&
        strncmp(path, BASE_DIR "/update-backup", strlen(BASE_DIR "/update-backup")) != 0 &&
        strncmp(path, BASE_DIR "/tmp", strlen(BASE_DIR "/tmp")) != 0) {
        fprintf(stderr, "host_remove_dir: path not allowed: %s\n", path);
        JS_FreeCString(ctx, path);
        return JS_FALSE;
    }

    const char *argv_cmd[] = { "rm", "-rf", path, NULL };
    int result = run_command(argv_cmd);

    JS_FreeCString(ctx, path);

    return (result == 0) ? JS_TRUE : JS_FALSE;
}

/* ============================================================================
 * Tar bindings
 * ============================================================================ */

/* host_extract_tar(tar_path, dest_dir) -> bool */
static JSValue js_host_extract_tar(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 2) {
        return JS_FALSE;
    }

    const char *tar_path = JS_ToCString(ctx, argv[0]);
    const char *dest_dir = JS_ToCString(ctx, argv[1]);

    if (!tar_path || !dest_dir) {
        if (tar_path) JS_FreeCString(ctx, tar_path);
        if (dest_dir) JS_FreeCString(ctx, dest_dir);
        return JS_FALSE;
    }

    /* Validate paths */
    if (!validate_path(tar_path) || !validate_path(dest_dir)) {
        fprintf(stderr, "host_extract_tar: invalid path(s)\n");
        JS_FreeCString(ctx, tar_path);
        JS_FreeCString(ctx, dest_dir);
        return JS_FALSE;
    }

    const char *argv_cmd[] = {
        "tar", "-xzf", tar_path, "-C", dest_dir, NULL
    };
    int result = run_command(argv_cmd);

    JS_FreeCString(ctx, tar_path);
    JS_FreeCString(ctx, dest_dir);

    return (result == 0) ? JS_TRUE : JS_FALSE;
}

/* host_extract_tar_strip(tar_path, dest_dir, strip_components) -> bool
 * Like host_extract_tar but with --strip-components for tarballs with a top-level dir */
static JSValue js_host_extract_tar_strip(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 3) {
        return JS_FALSE;
    }

    const char *tar_path = JS_ToCString(ctx, argv[0]);
    const char *dest_dir = JS_ToCString(ctx, argv[1]);
    int strip = 0;
    JS_ToInt32(ctx, &strip, argv[2]);

    if (!tar_path || !dest_dir) {
        if (tar_path) JS_FreeCString(ctx, tar_path);
        if (dest_dir) JS_FreeCString(ctx, dest_dir);
        return JS_FALSE;
    }

    /* Validate paths */
    if (!validate_path(tar_path) || !validate_path(dest_dir)) {
        fprintf(stderr, "host_extract_tar_strip: invalid path(s)\n");
        JS_FreeCString(ctx, tar_path);
        JS_FreeCString(ctx, dest_dir);
        return JS_FALSE;
    }

    /* Validate strip range */
    if (strip < 0 || strip > 5) {
        fprintf(stderr, "host_extract_tar_strip: invalid strip value: %d\n", strip);
        JS_FreeCString(ctx, tar_path);
        JS_FreeCString(ctx, dest_dir);
        return JS_FALSE;
    }

    char strip_arg[32];
    snprintf(strip_arg, sizeof(strip_arg), "--strip-components=%d", strip);

    /* fork/execvp argv (no shell) — never interpolate paths into a shell
     * string; quoted-system() here was injectable via crafted filenames. */
    const char *argv_cmd[] = {
        "tar", "-xzf", tar_path, "-C", dest_dir, strip_arg, NULL
    };
    int result = run_command(argv_cmd);

    JS_FreeCString(ctx, tar_path);
    JS_FreeCString(ctx, dest_dir);

    return (result == 0) ? JS_TRUE : JS_FALSE;
}

/* ============================================================================
 * HTTP bindings (bundled curl)
 * ============================================================================ */

/* host_http_download(url, dest_path) -> bool */
static JSValue js_host_http_download(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv) {
    (void)this_val;
    unified_log("js_host", LOG_LEVEL_DEBUG, "host_http_download: called");
    if (argc < 2) {
        unified_log("js_host", LOG_LEVEL_DEBUG, "host_http_download: argc < 2");
        return JS_FALSE;
    }

    const char *url = JS_ToCString(ctx, argv[0]);
    const char *dest_path = JS_ToCString(ctx, argv[1]);

    if (!url || !dest_path) {
        unified_log("js_host", LOG_LEVEL_DEBUG, "host_http_download: null url or dest_path");
        if (url) JS_FreeCString(ctx, url);
        if (dest_path) JS_FreeCString(ctx, dest_path);
        return JS_FALSE;
    }

    unified_log("js_host", LOG_LEVEL_DEBUG, "host_http_download: %s -> %s", url, dest_path);

    /* Validate URL scheme - only allow https:// and http:// */
    if (strncmp(url, "https://", 8) != 0 && strncmp(url, "http://", 7) != 0) {
        unified_log("js_host", LOG_LEVEL_DEBUG, "host_http_download: invalid URL scheme");
        fprintf(stderr, "host_http_download: invalid URL scheme: %s\n", url);
        JS_FreeCString(ctx, url);
        JS_FreeCString(ctx, dest_path);
        return JS_FALSE;
    }

    /* Validate destination path */
    if (!validate_path(dest_path)) {
        unified_log("js_host", LOG_LEVEL_DEBUG, "host_http_download: invalid dest path");
        fprintf(stderr, "host_http_download: invalid dest path: %s\n", dest_path);
        JS_FreeCString(ctx, url);
        JS_FreeCString(ctx, dest_path);
        return JS_FALSE;
    }

    unified_log("js_host", LOG_LEVEL_DEBUG, "host_http_download: path validated, running curl");

    const char *argv_cmd[] = {
        CURL_PATH, "-fsSLk", "--connect-timeout", "5", "--max-time", "15",
        "-o", dest_path, url, NULL
    };
    int result = run_command(argv_cmd);

    unified_log("js_host", LOG_LEVEL_DEBUG, "host_http_download: curl returned %d", result);

    JS_FreeCString(ctx, url);
    JS_FreeCString(ctx, dest_path);

    return (result == 0) ? JS_TRUE : JS_FALSE;
}

/* host_http_download_background(url, dest_path) -> void
 * Same as host_http_download but fires curl in background (no waitpid).
 * Returns immediately; curl writes the file independently. */
static JSValue js_host_http_download_background(JSContext *ctx, JSValueConst this_val,
                                                int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 2) return JS_UNDEFINED;

    const char *url = JS_ToCString(ctx, argv[0]);
    const char *dest_path = JS_ToCString(ctx, argv[1]);
    if (!url || !dest_path) {
        if (url) JS_FreeCString(ctx, url);
        if (dest_path) JS_FreeCString(ctx, dest_path);
        return JS_UNDEFINED;
    }

    if (strncmp(url, "https://", 8) != 0 && strncmp(url, "http://", 7) != 0) {
        JS_FreeCString(ctx, url);
        JS_FreeCString(ctx, dest_path);
        return JS_UNDEFINED;
    }
    if (!validate_path(dest_path)) {
        JS_FreeCString(ctx, url);
        JS_FreeCString(ctx, dest_path);
        return JS_UNDEFINED;
    }

    const char *argv_cmd[] = {
        CURL_PATH, "-fsSLk", "--connect-timeout", "10", "--max-time", "60",
        "-o", dest_path, url, NULL
    };
    run_command_background(argv_cmd);

    JS_FreeCString(ctx, url);
    JS_FreeCString(ctx, dest_path);
    return JS_UNDEFINED;
}

/* ----------------------------------------------------------------
 * host_http_request_background
 *
 * General-purpose HTTP client wrapping the bundled curl binary.
 * Supports POST/PUT/etc, custom headers (kept out of `ps` via -K
 * config file with mode 0600), JSON or multipart bodies, and reports
 * completion to JS via a status file.
 *
 * Options object:
 *   url             string  required. http(s) only.
 *   method          string  optional, default "GET".
 *   headers         array   optional, strings like "Authorization: Bearer ...".
 *   body_path       string  optional, path under BASE_DIR with raw body.
 *   body_form       array   optional, multipart parts.
 *                           {name, value} or {name, file, type}.
 *   response_path   string  required, where curl writes the body.
 *   status_path     string  required, written when curl exits with
 *                           {"http_status":N,"curl_exit":N}.
 *   timeout_seconds number  optional, default 60.
 *
 * body_path and body_form are mutually exclusive.
 * Returns true if launched, false on validation failure.
 * ---------------------------------------------------------------- */

/* Write a curl -K config file containing headers, mode 0600.
 * Returns 0 on success, -1 on failure. Empty headers list -> deletes the
 * file if it existed and returns 0. */
static int write_curl_headers_config(const char *path,
                                     char **headers,
                                     int header_count) {
    if (header_count <= 0) {
        unlink(path);
        return 0;
    }
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return -1;
    FILE *f = fdopen(fd, "w");
    if (!f) {
        close(fd);
        return -1;
    }
    for (int i = 0; i < header_count; i++) {
        const char *h = headers[i];
        /* Reject CR/LF to prevent header injection */
        if (strchr(h, '\r') || strchr(h, '\n')) {
            fclose(f);
            unlink(path);
            return -1;
        }
        fputs("header = \"", f);
        for (const char *p = h; *p; p++) {
            if (*p == '\\' || *p == '"') fputc('\\', f);
            fputc(*p, f);
        }
        fputs("\"\n", f);
    }
    fclose(f);
    return 0;
}

/* Fork+exec curl in the background. Reads curl's stdout (the http_code from
 * -w "%{http_code}"), waits for it to exit, then writes a JSON status file.
 * If cleanup_path is non-NULL it is unlinked after curl exits. The runner
 * frees argv_storage and headers_storage on the way out. */
static void run_curl_with_status_background(char **argv_storage,
                                            int argv_owned_count,
                                            char *cleanup_path,
                                            char *status_path) {
    pid_t pid = fork();
    if (pid != 0) {
        /* parent: free our copies */
        for (int i = 0; i < argv_owned_count; i++) free(argv_storage[i]);
        free(argv_storage);
        free(cleanup_path);
        free(status_path);
        return;
    }

    /* child: detach */
    setsid();
    int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) dup2(devnull, STDIN_FILENO);

    int pipe_fd[2] = {-1, -1};
    if (pipe(pipe_fd) != 0) {
        if (status_path) {
            FILE *f = fopen(status_path, "w");
            if (f) { fputs("{\"http_status\":0,\"curl_exit\":-1}", f); fclose(f); }
        }
        _exit(1);
    }

    pid_t curl_pid = fork();
    if (curl_pid == 0) {
        /* grandchild: run curl */
        close(pipe_fd[0]);
        dup2(pipe_fd[1], STDOUT_FILENO);
        if (devnull >= 0) dup2(devnull, STDERR_FILENO);
        close(pipe_fd[1]);
        if (devnull > 2) close(devnull);
        execvp(argv_storage[0], argv_storage);
        _exit(127);
    }

    close(pipe_fd[1]);
    if (devnull >= 0) close(devnull);

    char status_buf[64];
    int total = 0;
    while (total < (int)sizeof(status_buf) - 1) {
        ssize_t n = read(pipe_fd[0], status_buf + total,
                         sizeof(status_buf) - 1 - total);
        if (n <= 0) break;
        total += n;
    }
    status_buf[total] = '\0';
    close(pipe_fd[0]);

    int wstat = 0;
    waitpid(curl_pid, &wstat, 0);
    int exit_code = WIFEXITED(wstat) ? WEXITSTATUS(wstat) : -1;
    int http_code = atoi(status_buf);

    if (cleanup_path) unlink(cleanup_path);

    if (status_path) {
        FILE *f = fopen(status_path, "w");
        if (f) {
            fprintf(f, "{\"http_status\":%d,\"curl_exit\":%d}",
                    http_code, exit_code);
            fclose(f);
        }
    }
    _exit(0);
}

/* Helper: get a string property from a JS object. Caller must JS_FreeCString. */
static const char *js_get_str_prop(JSContext *ctx, JSValueConst obj,
                                   const char *name) {
    JSValue v = JS_GetPropertyStr(ctx, obj, name);
    if (JS_IsUndefined(v) || JS_IsNull(v)) {
        JS_FreeValue(ctx, v);
        return NULL;
    }
    const char *s = JS_ToCString(ctx, v);
    JS_FreeValue(ctx, v);
    return s;
}

/* Append a heap-allocated copy of `s` to argv at index *idx, growing *idx. */
static int argv_push_dup(char ***argv, int *idx, int *cap, const char *s) {
    if (*idx + 1 >= *cap) {
        int new_cap = *cap * 2;
        char **new_argv = realloc(*argv, sizeof(char *) * new_cap);
        if (!new_argv) return -1;
        *argv = new_argv;
        *cap = new_cap;
    }
    char *copy = strdup(s ? s : "");
    if (!copy) return -1;
    (*argv)[(*idx)++] = copy;
    return 0;
}

static JSValue js_host_http_request_background(JSContext *ctx,
                                               JSValueConst this_val,
                                               int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1 || !JS_IsObject(argv[0])) return JS_FALSE;
    JSValueConst opts = argv[0];

    const char *url = js_get_str_prop(ctx, opts, "url");
    const char *response_path = js_get_str_prop(ctx, opts, "response_path");
    const char *status_path = js_get_str_prop(ctx, opts, "status_path");
    const char *method = js_get_str_prop(ctx, opts, "method");
    const char *body_path = js_get_str_prop(ctx, opts, "body_path");

    if (!url || !response_path || !status_path) goto fail;

    if (strncmp(url, "https://", 8) != 0 && strncmp(url, "http://", 7) != 0) {
        fprintf(stderr, "host_http_request: bad URL scheme\n");
        goto fail;
    }
    if (!validate_path(response_path) || !validate_path(status_path)) {
        fprintf(stderr, "host_http_request: bad response/status path\n");
        goto fail;
    }
    if (body_path && !validate_path(body_path)) {
        fprintf(stderr, "host_http_request: bad body_path\n");
        goto fail;
    }

    /* timeout */
    int timeout = 60;
    JSValue tv = JS_GetPropertyStr(ctx, opts, "timeout_seconds");
    if (!JS_IsUndefined(tv) && !JS_IsNull(tv)) {
        int t = 0;
        if (JS_ToInt32(ctx, &t, tv) == 0 && t > 0 && t <= 600) timeout = t;
    }
    JS_FreeValue(ctx, tv);

    /* headers -> write config file alongside status_path */
    JSValue headers_v = JS_GetPropertyStr(ctx, opts, "headers");
    char **header_strs = NULL;
    int header_count = 0;
    int has_headers = 0;
    if (JS_IsArray(ctx, headers_v)) {
        JSValue len_v = JS_GetPropertyStr(ctx, headers_v, "length");
        int n = 0;
        JS_ToInt32(ctx, &n, len_v);
        JS_FreeValue(ctx, len_v);
        if (n > 0) {
            header_strs = calloc(n, sizeof(char *));
            if (!header_strs) { JS_FreeValue(ctx, headers_v); goto fail; }
            for (int i = 0; i < n; i++) {
                JSValue h = JS_GetPropertyUint32(ctx, headers_v, (uint32_t)i);
                const char *hs = JS_ToCString(ctx, h);
                JS_FreeValue(ctx, h);
                if (!hs) {
                    for (int j = 0; j < header_count; j++) free(header_strs[j]);
                    free(header_strs);
                    JS_FreeValue(ctx, headers_v);
                    goto fail;
                }
                header_strs[header_count++] = strdup(hs);
                JS_FreeCString(ctx, hs);
            }
            has_headers = 1;
        }
    }
    JS_FreeValue(ctx, headers_v);

    char *cfg_path = NULL;
    if (has_headers) {
        size_t cl = strlen(status_path) + 5;
        cfg_path = malloc(cl);
        if (!cfg_path) goto fail_headers;
        snprintf(cfg_path, cl, "%s.cfg", status_path);
        if (write_curl_headers_config(cfg_path, header_strs, header_count) != 0) {
            fprintf(stderr, "host_http_request: failed to write headers cfg\n");
            free(cfg_path);
            cfg_path = NULL;
            goto fail_headers;
        }
    }

    /* Free our copies of the header strings now that they are persisted. */
    for (int i = 0; i < header_count; i++) free(header_strs[i]);
    free(header_strs);
    header_strs = NULL;
    header_count = 0;

    /* Build curl argv */
    int cap = 32;
    char **cargv = malloc(sizeof(char *) * cap);
    if (!cargv) goto fail_cfg;
    int ci = 0;

    char timeout_str[16];
    snprintf(timeout_str, sizeof(timeout_str), "%d", timeout);

    if (argv_push_dup(&cargv, &ci, &cap, CURL_PATH) ||
        argv_push_dup(&cargv, &ci, &cap, "-sSk") ||
        argv_push_dup(&cargv, &ci, &cap, "-w") ||
        argv_push_dup(&cargv, &ci, &cap, "%{http_code}") ||
        argv_push_dup(&cargv, &ci, &cap, "--connect-timeout") ||
        argv_push_dup(&cargv, &ci, &cap, "10") ||
        argv_push_dup(&cargv, &ci, &cap, "--max-time") ||
        argv_push_dup(&cargv, &ci, &cap, timeout_str) ||
        argv_push_dup(&cargv, &ci, &cap, "-o") ||
        argv_push_dup(&cargv, &ci, &cap, response_path)) goto fail_argv;

    if (method) {
        if (argv_push_dup(&cargv, &ci, &cap, "-X") ||
            argv_push_dup(&cargv, &ci, &cap, method)) goto fail_argv;
    }
    if (cfg_path) {
        if (argv_push_dup(&cargv, &ci, &cap, "-K") ||
            argv_push_dup(&cargv, &ci, &cap, cfg_path)) goto fail_argv;
    }
    if (body_path) {
        char data_arg[PATH_MAX + 2];
        snprintf(data_arg, sizeof(data_arg), "@%s", body_path);
        if (argv_push_dup(&cargv, &ci, &cap, "--data-binary") ||
            argv_push_dup(&cargv, &ci, &cap, data_arg)) goto fail_argv;
    }

    /* body_form: array of {name, value} or {name, file, type?} */
    JSValue form_v = JS_GetPropertyStr(ctx, opts, "body_form");
    if (JS_IsArray(ctx, form_v)) {
        JSValue len_v = JS_GetPropertyStr(ctx, form_v, "length");
        int fn = 0;
        JS_ToInt32(ctx, &fn, len_v);
        JS_FreeValue(ctx, len_v);
        for (int i = 0; i < fn; i++) {
            JSValue part = JS_GetPropertyUint32(ctx, form_v, (uint32_t)i);
            const char *pname = js_get_str_prop(ctx, part, "name");
            const char *pvalue = js_get_str_prop(ctx, part, "value");
            const char *pfile = js_get_str_prop(ctx, part, "file");
            const char *ptype = js_get_str_prop(ctx, part, "type");
            if (!pname || (!pvalue && !pfile)) {
                if (pname) JS_FreeCString(ctx, pname);
                if (pvalue) JS_FreeCString(ctx, pvalue);
                if (pfile) JS_FreeCString(ctx, pfile);
                if (ptype) JS_FreeCString(ctx, ptype);
                JS_FreeValue(ctx, part);
                JS_FreeValue(ctx, form_v);
                goto fail_argv;
            }
            if (pfile && !validate_path(pfile)) {
                fprintf(stderr, "host_http_request: bad form file path\n");
                JS_FreeCString(ctx, pname);
                if (pvalue) JS_FreeCString(ctx, pvalue);
                JS_FreeCString(ctx, pfile);
                if (ptype) JS_FreeCString(ctx, ptype);
                JS_FreeValue(ctx, part);
                JS_FreeValue(ctx, form_v);
                goto fail_argv;
            }
            char part_buf[PATH_MAX + 256];
            if (pfile) {
                if (ptype && *ptype) {
                    snprintf(part_buf, sizeof(part_buf), "%s=@%s;type=%s",
                             pname, pfile, ptype);
                } else {
                    snprintf(part_buf, sizeof(part_buf), "%s=@%s",
                             pname, pfile);
                }
            } else {
                snprintf(part_buf, sizeof(part_buf), "%s=%s", pname, pvalue);
            }
            JS_FreeCString(ctx, pname);
            if (pvalue) JS_FreeCString(ctx, pvalue);
            if (pfile) JS_FreeCString(ctx, pfile);
            if (ptype) JS_FreeCString(ctx, ptype);
            JS_FreeValue(ctx, part);

            if (argv_push_dup(&cargv, &ci, &cap, "-F") ||
                argv_push_dup(&cargv, &ci, &cap, part_buf)) {
                JS_FreeValue(ctx, form_v);
                goto fail_argv;
            }
        }
    }
    JS_FreeValue(ctx, form_v);

    if (argv_push_dup(&cargv, &ci, &cap, url)) goto fail_argv;

    /* Sentinel NULL */
    if (ci + 1 >= cap) {
        char **na = realloc(cargv, sizeof(char *) * (cap + 1));
        if (!na) goto fail_argv;
        cargv = na;
        cap++;
    }
    cargv[ci] = NULL;

    /* Pre-clear status file so JS poll only sees the new result */
    unlink(status_path);

    /* Hand ownership of cfg_path to the background runner (it'll unlink the
     * file and free the string). NULL our local so the fail_cfg cleanup path
     * doesn't double-free if status_dup fails. */
    char *cleanup_dup = cfg_path;
    cfg_path = NULL;
    char *status_dup = strdup(status_path);
    if (!status_dup) { free(cleanup_dup); goto fail_argv; }

    run_curl_with_status_background(cargv, ci, cleanup_dup, status_dup);

    JS_FreeCString(ctx, url);
    JS_FreeCString(ctx, response_path);
    JS_FreeCString(ctx, status_path);
    if (method) JS_FreeCString(ctx, method);
    if (body_path) JS_FreeCString(ctx, body_path);
    return JS_TRUE;

fail_argv:
    if (cargv) {
        for (int i = 0; i < ci; i++) free(cargv[i]);
        free(cargv);
    }
fail_cfg:
    if (cfg_path) { unlink(cfg_path); free(cfg_path); }
fail_headers:
    if (header_strs) {
        for (int i = 0; i < header_count; i++) free(header_strs[i]);
        free(header_strs);
    }
fail:
    if (url) JS_FreeCString(ctx, url);
    if (response_path) JS_FreeCString(ctx, response_path);
    if (status_path) JS_FreeCString(ctx, status_path);
    if (method) JS_FreeCString(ctx, method);
    if (body_path) JS_FreeCString(ctx, body_path);
    return JS_FALSE;
}

/* ============================================================================
 * Registration
 * ============================================================================ */

void js_host_register_common(JSContext *ctx) {
    JSValue global_obj = JS_GetGlobalObject(ctx);

    JS_SetPropertyStr(ctx, global_obj, "host_file_exists", JS_NewCFunction(ctx, js_host_file_exists, "host_file_exists", 1));
    JS_SetPropertyStr(ctx, global_obj, "host_read_file", JS_NewCFunction(ctx, js_host_read_file, "host_read_file", 1));
    JS_SetPropertyStr(ctx, global_obj, "host_read_file_base64", JS_NewCFunction(ctx, js_host_read_file_base64, "host_read_file_base64", 1));
    JS_SetPropertyStr(ctx, global_obj, "host_write_file", JS_NewCFunction(ctx, js_host_write_file, "host_write_file", 2));
    JS_SetPropertyStr(ctx, global_obj, "host_http_download", JS_NewCFunction(ctx, js_host_http_download, "host_http_download", 2));
    JS_SetPropertyStr(ctx, global_obj, "host_http_download_background", JS_NewCFunction(ctx, js_host_http_download_background, "host_http_download_background", 2));
    JS_SetPropertyStr(ctx, global_obj, "host_http_request_background", JS_NewCFunction(ctx, js_host_http_request_background, "host_http_request_background", 1));
    JS_SetPropertyStr(ctx, global_obj, "host_extract_tar", JS_NewCFunction(ctx, js_host_extract_tar, "host_extract_tar", 2));
    JS_SetPropertyStr(ctx, global_obj, "host_extract_tar_strip", JS_NewCFunction(ctx, js_host_extract_tar_strip, "host_extract_tar_strip", 3));
    JS_SetPropertyStr(ctx, global_obj, "host_ensure_dir", JS_NewCFunction(ctx, js_host_ensure_dir, "host_ensure_dir", 1));
    JS_SetPropertyStr(ctx, global_obj, "host_remove_dir", JS_NewCFunction(ctx, js_host_remove_dir, "host_remove_dir", 1));

    JS_FreeValue(ctx, global_obj);
}
