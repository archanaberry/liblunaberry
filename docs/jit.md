# LunaJIT

LunaJIT turns a precompiled chunk into a native shared object at runtime and
calls it through a plain function pointer.  It is deliberately a *compile-once*
JIT, not a tracing JIT: the win is removing interpreter overhead for a whole
chunk while keeping the implementation small and predictable.

## Pipeline

```
source.lua --luna_compile--> chunk.lbc
                              |
                              v
                 generated C shim (embeds chunk)
                              |
                     gcc / clang -shared
                              |
                              v
                 shim.dll / shim.so  --dlopen-->  luna_jit_entry()
                              |
                              v
                    luna_jit_host_run(chunk)
```

The shim contains only:

```c
extern double luna_jit_host_run(const char *, size_t, char *, size_t);
static const unsigned char luna_bc[] = { ... };
double luna_jit_entry(void)
{
    char err[512];
    double r = luna_jit_host_run((const char *)luna_bc, luna_bc_len,
                                 err, sizeof(err));
    if (err[0]) fprintf(stderr, "luna jit: %s\n", err);
    return r;
}
```

The engine (`luna.c` + Lua) is linked into the shim through
`liblunaberry.a`.  This means the shim is self-contained: no DLL search path,
no import library, and no host/guest state mismatch.  The trade-off is that the
first compile links the engine into the shim; the result is cached so it is paid
only once.

## Backends

| Backend | Compiler | Flags | Notes |
| --- | --- | --- | --- |
| `cc` | `gcc` / `cc` | `-O2` | fastest to start, always available |
| `llvm` | `clang` | `-O3` | optional `.ll` emission; GNU target on Windows |
| `berry` | BeASM | — | dispatched to the AOT Berry emitter |

On Windows, `luna_find_tool` locates gcc/clang under Scoop, MSYS2, or the
standard LLVM install directory.  If clang is unavailable, the LLVM backend
falls back to the C compiler and reports it in `luna_jit_last_error`.

## Cache

`luna_jit_new` keeps an in-memory cache of up to 64 shims.  If
`LunaJitOptions.cache_dir` is set, shims are also written to disk with a key
derived from:

- the bytecode bytes;
- the chunk name;
- the Luna version;
- the compiler path and flags.

Disk hits avoid both shim generation and compilation.  The cache is safe to
share between processes; files are written with a temporary name and renamed.

## Statistics

```c
LunaJitStats st = luna_jit_stats(jit);
printf("attempts=%lu ok=%lu failed=%lu native=%lu cache=%lu\n",
       st.compile_attempts, st.compile_ok, st.compile_failed,
       st.native_calls, st.cache_hits);
```

`lunajit --stats` and `lunavm --jit=... --stats` print the same counters.

## Failure model

`luna_jit_call` never aborts the host:

- if compilation fails, it fills `err` and returns `0.0`;
- `lunavm` then falls back to the interpreter;
- `lunajit` reports the error and exits nonzero only when `--run` was requested.

## Copy path and patch (the launcher)

`lunajit --target exe` emits a launcher that uses the *copy path and patch*
technique (the same approach as libmochiberry):

1. **Copy path** — the native shim (`<base>.dll` / `<base>.so`) is pre-built at
   build time, so the toolchain is paid once and never runs at runtime.
2. **Patch** — the launcher embeds the shim's name and resolves it against its
   own executable directory via `luna_jit_run_shim()`, then simply `dlopen`s it.

The launcher therefore never compiles anything on the target machine: startup
is just `dlopen` + one call.  `--compile-at-run` opts back into the older
behavior (compile the shim on first run); the default is copy path and patch.

Measured startup on Windows (best of 5, small chunk):

| launcher mode | ms |
| --- | --- |
| copy path and patch (default) | 231 |
| `--compile-at-run` | 1905 |
| LunaAOT native | 272 |

## Limits

- The JIT compiles whole chunks, not individual hot functions.
- `hot_threshold` is reserved for future hot-function tracking; it does not
  change behavior today.
- The shim runs in its own Lua state.  Values are returned as the first result
  coerced to a `double`; for richer results, use the C API directly.
- A compiler must exist on the target machine.  For compiler-free deployment,
  use LunaAOT.
