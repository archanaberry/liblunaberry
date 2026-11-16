# C API

## Core (`luna.h`)

```c
typedef struct LunaOptions {
    size_t mem_limit;   /* soft allocator budget; 0 = unlimited */
    int open_libs;      /* open standard libraries (default 1) */
    const char *name;   /* optional chunk/global name */
} LunaOptions;

LunaState *luna_new(const LunaOptions *opts);
void       luna_free(LunaState *s);
```

`luna_new` returns `NULL` on allocation failure.  A `LunaOptions` zeroed with
`{0}` opens the standard libraries; set `open_libs = 0` for a bare state.

### Running code

```c
int luna_dofile(LunaState *s, const char *path);
int luna_dostring(LunaState *s, const char *src, size_t len,
                  const char *name);
int luna_dobytecode(LunaState *s, const unsigned char *bc, size_t len,
                    const char *name);
```

All return `0` on success and `-1` on error.  The error text is available from
`luna_last_error(s)`.  After a successful run, results are on the Lua stack and
can be inspected with the helpers below.

### Compiling

```c
int luna_compile(LunaState *s, const char *src, size_t len,
                 const char *name, unsigned char **out, size_t *out_len,
                 int strip);
int luna_compile_file(LunaState *s, const char *path,
                      unsigned char **out, size_t *out_len, int strip);
```

`*out` is a `malloc`-owned buffer; release it with `free()`.  `strip` removes
debug information from the chunk.

### Introspection

```c
const char *luna_version(void);        /* "5.5.1" */
const char *luna_engine_name(void);    /* "LunaBerry" */
const char *luna_package_name(void);   /* my.berrylabfoundation.lunaberry */
const char *luna_last_error(const LunaState *s);
lua_State  *luna_raw(LunaState *s);
void        luna_set_mem_limit(LunaState *s, size_t limit);
size_t      luna_mem_used(const LunaState *s);
size_t      luna_mem_peak(const LunaState *s);
```

### Results

```c
int         luna_result_count(LunaState *s);
int         luna_result_type(LunaState *s, int idx);
double      luna_result_number(LunaState *s, int idx);
const char *luna_result_string(LunaState *s, int idx);
int         luna_result_boolean(LunaState *s, int idx);
```

`idx` is 1-based.  Type codes: `0` nil, `1` number, `2` string, `3` boolean,
`4` other.

### JIT bridge

```c
double luna_jit_host_run(const char *bc, size_t len, char *err, size_t errlen);
```

This is the stable symbol used by generated JIT shims.  It runs the chunk in a
dedicated persistent state and returns the first result coerced to `double`.

## LunaJIT (`luna-jit.h`)

```c
typedef enum LunaJitBackend {
    LUNA_JIT_AUTO = 0, LUNA_JIT_NONE, LUNA_JIT_CC,
    LUNA_JIT_LLVM, LUNA_JIT_BERRY
} LunaJitBackend;

typedef struct LunaJitOptions {
    const char *cc;
    const char *cc_flags;
    const char *lib_dir;
    const char *tmp_dir;
    const char *cache_dir;
    unsigned hot_threshold;
} LunaJitOptions;

LunaJit *luna_jit_new(LunaJitBackend backend, const LunaJitOptions *opts);
void     luna_jit_free(LunaJit *jit);
LunaJitFn luna_jit_compile(LunaJit *jit, const unsigned char *bc,
                           size_t len, const char *name);
double   luna_jit_call(LunaJit *jit, const unsigned char *bc, size_t len,
                       const char *name, char *err, size_t errlen);
const char *luna_jit_backend_name(const LunaJit *jit);
const char *luna_jit_last_error(const LunaJit *jit);
const char *luna_jit_last_ir(const LunaJit *jit);
LunaJitStats luna_jit_stats(const LunaJit *jit);
```

`lib_dir` must contain `liblunaberry.a`.  When `NULL`, the executable's
directory is used.  `cache_dir` enables the persistent shim cache.

## LunaAOT (`luna-llvm.h`)

```c
typedef struct LunaLLvmOptions {
    const char *clang;
    const char *flags;
    const char *lib_dir;
    const char *include_dir;
    int emit_ir;
    int verbose;
} LunaLLvmOptions;

int luna_llvm_available(void);
int luna_llvm_aot(const unsigned char *bc, size_t len, const char *out_exe,
                  const char *chunk_name, const LunaLLvmOptions *opts,
                  char *err, size_t errlen);
int luna_llvm_emit_ir(const unsigned char *bc, size_t len, const char *out_ll,
                      const char *chunk_name, const LunaLLvmOptions *opts,
                      char *err, size_t errlen);
```

`lib_dir` must contain `liblunaberry.a`; `include_dir` must contain `luna.h`
(and, for the Berry host, `luna_berry.h` either directly or under
`platform/berry/`).

## Berry emitter (`luna-berry.h`)

```c
typedef struct LunaBerryOptions {
    const char *beasm;
    const char *out_dir;
    const char *include_dir;
    const char *lib_dir;
    int emit_host;
    int verbose;
} LunaBerryOptions;

int luna_berry_emit(const unsigned char *bc, size_t len, const char *stem,
                    const LunaBerryOptions *opts, char *err, size_t errlen);
```

## Berry runtime ABI (`luna_berry.h`, source: `platform/berry/luna_berry.h`)

```c
typedef struct LunaBerryEngine {
    LunaState *state;
    size_t heap_limit;
    size_t stack_limit;
    int ready;
} LunaBerryEngine;

int  luna_berry_init(LunaBerryEngine *e, size_t heap_limit,
                     size_t stack_limit);
int  luna_berry_eval(LunaBerryEngine *e, const char *src, size_t len,
                     const char *name);
int  luna_berry_eval_binary(LunaBerryEngine *e, const uint8_t *buf,
                            size_t len);
int  luna_berry_loop(LunaBerryEngine *e);
void luna_berry_destroy(LunaBerryEngine *e);
```

`luna_berry_loop` is a no-op kept for ABI stability; Lua has no promise queue.
