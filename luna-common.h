/*
 * luna-common.h — tiny host helpers shared by the LunaBerry CLIs.
 *
 * No engine dependency lives here; this is the "boring plumbing" module
 * (files, paths, processes, C-literal escaping) that keeps the JIT/AOT
 * front-ends small and readable.
 */

#ifndef LUNA_COMMON_H
#define LUNA_COMMON_H

#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Files. */
char *luna_read_file(const char *path, size_t *plen);
int luna_write_file(const char *path, const void *data, size_t len);
int luna_file_exists(const char *path);
int luna_mkdir_p(const char *path);
int luna_ensure_parent(const char *path);

/* Paths. */
const char *luna_basename(const char *path);
void luna_stem(const char *path, char *out, size_t out_size);
void luna_dirname(const char *path, char *out, size_t out_size);
void luna_path_join(char *out, size_t out_size, const char *a, const char *b);
void luna_exe_dir(char *out, size_t out_size);
void luna_tmp_dir(char *out, size_t out_size);
/* Find a directory containing luna.h: the current directory, the
 * installed ../include next to the executable, or the source tree root. */
void luna_find_include_dir(char *out, size_t out_size);

/* Locate a host tool (gcc, clang, beasm, strip, ...).  On Windows this
 * also checks the common Scoop/MSYS2/LLVM install locations because the
 * environment used by popen/CreateProcess may not have a usable PATH.
 * Returns 0 on success, -1 when the tool cannot be found. */
int luna_find_tool(const char *name, char *out, size_t out_size);

/* MinGW sysroot include dir for clang's GNU target on Windows.
 * Returns 0 on success, -1 when it cannot be discovered. */
int luna_mingw_include(char *out, size_t out_size);

/* Processes. Returns the process exit code, or -1 when it could not run.
 * If `out` is non-NULL, captures combined stdout+stderr (truncated). */
int luna_run_cmd(const char *cmd, char *out, size_t out_size);

/* Run a command with the parent's stdout/stderr (no capture). */
int luna_run_cmd_inherit(const char *cmd);

/* C source generation. */
void luna_escape_c_string(const char *src, char *out, size_t out_size);
void luna_emit_c_array(FILE *f, const char *name, const unsigned char *data,
                       size_t len);
void luna_emit_c_string(FILE *f, const char *name, const char *value);

/* FNV-1a 64-bit, used for JIT cache keys. */
unsigned long long luna_hash64(const void *data, size_t len);
unsigned long long luna_hash64_str(const char *s);

#ifdef __cplusplus
}
#endif

#endif /* LUNA_COMMON_H */
