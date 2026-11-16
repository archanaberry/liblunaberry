# Architecture

LunaBerry is intentionally layered so each tier can be removed without
touching the others.

```
+--------------------------------------------------------------+
|  CLIs: lunavm/lunaberry, lunac, lunajit, lunaaot              |
+----------------------+-------------------+-------------------+
|  LunaJIT             |  LunaAOT          |  Berry emitter    |
|  luna-jit.c          |  luna-llvm.c      |  luna-berry.c     |
+----------------------+-------------------+-------------------+
|  Host plumbing: luna-common.c (files, paths, processes)      |
+--------------------------------------------------------------+
|  Core: luna.c (allocator, load/run/compile, JIT host bridge) |
+--------------------------------------------------------------+
|  Lua 5.5.1: vendor/lua                                        |
+--------------------------------------------------------------+
```

## Core (`luna.c`)

`LunaState` owns a `lua_State` and a tracking allocator.  The allocator records
current and peak usage and enforces an optional soft limit.  The core exposes:

- source and bytecode loading (`luna_dofile`, `luna_dostring`,
  `luna_dobytecode`);
- compilation to `.lbc` (`luna_compile`, `luna_compile_file`);
- result inspection helpers;
- `luna_jit_host_run`, the stable bridge called by generated JIT shims.

The core does not know about compilers, caches, BeASM, or Berry.  That keeps the
static library small and makes the AOT/JIT layers optional.

## Host plumbing (`luna-common.c`)

Shared by the tools, never by the engine:

- file read/write, directory creation;
- path handling (`basename`, `stem`, `dirname`, `join`, executable directory);
- process execution with captured output;
- tool discovery (`luna_find_tool`) for cc/gcc/clang/strip;
- temp-directory and include-root discovery for generated code;
- MinGW sysroot discovery for clang's GNU target;
- C string/array emission for generated sources;
- FNV-1a hashing for JIT cache keys.

On Windows, `luna_run_cmd` uses `CreateProcessA` with an inherited pipe instead
of `popen`.  This avoids `cmd.exe` and works even when the shell `PATH` is
unusual.  Tool discovery checks the common Scoop, MSYS2, and LLVM install
locations.

## LunaJIT (`luna-jit.c`)

1. The input source is compiled to `.lbc` by the core.
2. A tiny C shim is generated:

   ```c
   extern double luna_jit_host_run(const char *, size_t, char *, size_t);
   static const unsigned char luna_bc[] = { ... };
   __declspec(dllexport) double luna_jit_entry(void) { ... }
   ```

3. The shim is compiled to a shared object (`gcc -shared` or
   `clang -shared`) and linked against `liblunaberry.a`.  The engine is linked
   into the shim, so no DLL search path is required.
4. The shared object is loaded with `LoadLibraryA`/`dlopen` and
   `luna_jit_entry` is called.
5. Results are cached in memory and optionally on disk, keyed by bytecode,
   compiler flags, and engine version.

The shim calls `luna_jit_host_run`, which executes the embedded chunk in a
dedicated persistent Lua state.  This is the same "CC JIT" shape used by
libmochiberry: the compiler is paid once and the native artifact is reused.

## LunaAOT (`luna-llvm.c`)

1. The source is compiled to `.lbc`.
2. A C wrapper embeds the bytecode and calls `luna_dobytecode`.
3. `clang` compiles the wrapper and links `liblunaberry.a` into a standalone
   executable.
4. `--emit-ir` keeps the textual LLVM IR (`.ll`).
5. `--target both` also writes the `.lbc` for `lunavm`.

On Windows, clang is invoked with `--target=x86_64-w64-windows-gnu` and the
MinGW sysroot include directory, so the resulting COFF executable links
cleanly with the MinGW-built static library.

## Berry backend (`luna-berry.c` + `platform/berry`)

The Berry backend writes four artifacts:

| File | Purpose |
| --- | --- |
| `<stem>.lbc` | Lua bytecode, runs on `lunavm` |
| `<stem>.basm` | readable BeASM module with the blob and runtime bridge |
| `<stem>.bex` | Berry native binary produced by `beasm` |
| `<stem>.host` | host executable proving the blob runs on a normal OS |

The BeASM module imports `luna_berry_init`, `luna_berry_eval_binary`,
`luna_berry_loop`, and `luna_berry_destroy` from `platform/berry`.  The host
verifier is a normal C executable linked against `liblunaberry.a`.

## Static and shared libraries

- `build/liblunaberry.a` is the primary embedding target and is also linked
  into JIT shims and AOT executables.
- `build/lunaberry.dll` (or `liblunaberry.so`) exports the `luna_*` API and
  `luna_jit_host_run` for hosts that prefer dynamic loading.

The shared library is optional; the JIT and AOT tiers do not depend on it.
