/*
 * luna.h — LunaBerry public C API.
 *
 * LunaBerry is a small, modular Lua 5.5.1+ engine with two optional native
 * tiers:
 *
 *   LunaJIT  — compile-on-first-use native shims (CC or LLVM/clang backend)
 *              plus a Berry/BeASM artifact backend.
 *   LunaAOT  — ahead-of-time standalone executables (LLVM/clang backend)
 *              plus Berry/BeASM native modules.
 *
 * The engine core is the upstream Lua implementation (vendor/lua).  This
 * header is deliberately tiny and stable so that AOT-generated C files can
 * include it without pulling in Lua internals.
 */

#ifndef LUNA_H
#define LUNA_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Identity                                                            */
/* ------------------------------------------------------------------ */

#define LUNA_VERSION_MAJOR   5
#define LUNA_VERSION_MINOR   5
#define LUNA_VERSION_RELEASE 1
#define LUNA_VERSION_STRING  "5.5.1"

#define LUNA_PACKAGE_ORG      "my.berrylabfoundation"
#define LUNA_PACKAGE_NAME     "my.berrylabfoundation.lunaberry"
#define LUNA_PACKAGE_ARTIFACT "lunaberry"
#define LUNA_PACKAGE_LIBRARY  "liblunaberry"
#define LUNA_ENGINE_NAME      "LunaBerry"
#define LUNA_LUA_LEVEL        505

#if defined(_WIN32)
#  if defined(LUNA_BUILD_SHARED)
#    define LUNA_API __declspec(dllexport)
#  elif defined(LUNA_USE_SHARED)
#    define LUNA_API __declspec(dllimport)
#  else
#    define LUNA_API
#  endif
#elif defined(__GNUC__)
#  define LUNA_API __attribute__((visibility("default")))
#else
#  define LUNA_API
#endif

/* ------------------------------------------------------------------ */
/* Core engine                                                         */
/* ------------------------------------------------------------------ */

struct lua_State;
typedef struct lua_State lua_State;

typedef struct LunaState LunaState;

typedef struct LunaOptions {
    size_t mem_limit;   /* soft Lua allocator budget in bytes; 0 = unlimited */
    int open_libs;      /* open the standard libraries (default 1)           */
    const char *name;   /* chunk name used for errors                        */
} LunaOptions;

/* Create/destroy an engine. Returns NULL on allocation failure. */
LUNA_API LunaState *luna_new(const LunaOptions *opts);
LUNA_API void luna_free(LunaState *s);

/* Run a source file / source string / precompiled bytecode.
 * Returns 0 on success, -1 on error (see luna_last_error). */
LUNA_API int luna_dofile(LunaState *s, const char *path);
LUNA_API int luna_dostring(LunaState *s, const char *src, size_t len,
                           const char *name);
LUNA_API int luna_dobytecode(LunaState *s, const unsigned char *bc, size_t len,
                             const char *name);

/* Compile source to Luna bytecode (.lbc). On success *out is a malloc'd
 * buffer owned by the caller (free with free()). Returns 0 / -1. */
LUNA_API int luna_compile(LunaState *s, const char *src, size_t len,
                          const char *name, unsigned char **out,
                          size_t *out_len, int strip);
LUNA_API int luna_compile_file(LunaState *s, const char *path,
                               unsigned char **out, size_t *out_len,
                               int strip);

/* Introspection. */
LUNA_API const char *luna_version(void);
LUNA_API const char *luna_engine_name(void);
LUNA_API const char *luna_package_name(void);
LUNA_API const char *luna_last_error(const LunaState *s);
LUNA_API lua_State *luna_raw(LunaState *s);
LUNA_API void luna_set_mem_limit(LunaState *s, size_t limit);
LUNA_API size_t luna_mem_used(const LunaState *s);
LUNA_API size_t luna_mem_peak(const LunaState *s);

/* Results left on the Lua stack after a successful run. */
LUNA_API int luna_result_count(LunaState *s);
LUNA_API int luna_result_type(LunaState *s, int idx); /* 0 nil, 1 number,
                                                         2 string, 3 bool,
                                                         4 other */
LUNA_API double luna_result_number(LunaState *s, int idx);
LUNA_API const char *luna_result_string(LunaState *s, int idx);
LUNA_API int luna_result_boolean(LunaState *s, int idx);

/* ------------------------------------------------------------------ */
/* JIT host bridge                                                     */
/* ------------------------------------------------------------------ */

/* Entry point exported by liblunaberry for native JIT shims. It runs a
 * precompiled chunk in a dedicated persistent state and returns the first
 * result coerced to a double. `err` receives a NUL-terminated message on
 * failure. This symbol is intentionally stable: generated shims only need
 * this one function. */
LUNA_API double luna_jit_host_run(const char *bc, size_t len,
                                  char *err, size_t errlen);

#ifdef __cplusplus
}
#endif

#endif /* LUNA_H */
