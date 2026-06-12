/*
 * Shared QuickJS scaffolding and JS host bindings used by both binaries
 * (schwung_host and shadow_ui). Extracted from verbatim-copied (and since
 * drifted) helpers; the shadow_ui.c variants were taken as canonical.
 */
#ifndef JS_HOST_COMMON_H
#define JS_HOST_COMMON_H

#include "quickjs.h"

/* QuickJS scaffolding */
JSContext *JS_NewCustomContext(JSRuntime *rt);
int eval_buf(JSContext *ctx, const void *buf, int buf_len,
             const char *filename, int eval_flags);
int eval_file(JSContext *ctx, const char *filename, int module);
int getGlobalFunction(JSContext *ctx, const char *func_name, JSValue *retFunc);
int callGlobalFunction(JSContext *ctx, JSValue *pfunc, unsigned char *data);

/* Path / process helpers shared by the file and store bindings.
 * validate_path: path must live under /data/UserData, no "..", symlinks
 * re-checked via realpath. run_command: fork/execvp (no shell). */
int validate_path(const char *path);
int run_command(const char *const argv[]);

/*
 * Register the shared JS bindings on the global object. Names are the
 * module-facing API and must not change:
 *   host_file_exists, host_read_file, host_read_file_base64,
 *   host_write_file, host_http_download, host_http_download_background,
 *   host_http_request_background, host_extract_tar, host_extract_tar_strip,
 *   host_ensure_dir, host_remove_dir
 * Each binary still registers its own non-shared bindings separately.
 */
void js_host_register_common(JSContext *ctx);

#endif /* JS_HOST_COMMON_H */
