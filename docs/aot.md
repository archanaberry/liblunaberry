# LunaAOT

LunaAOT produces artifacts that do not need a compiler at runtime.
`lunavm --aot[=llvm|berry|lbc]` uses the same connectors on the fly, writing
temporary artifacts under `TMPDIR`/`TEMP` and running them.

## LLVM backend

```
source.lua --luna_compile--> chunk.lbc
                              |
                              v
                 generated C wrapper (embeds chunk)
                              |
                         clang -O2/-O3
                              |
                              v
                 standalone executable
```

The generated wrapper is ordinary C:

```c
#include "luna.h"
static const unsigned char luna_bc[] = { ... };
int main(void)
{
    LunaOptions o = { 0 };
    LunaState *s = luna_new(&o);
    if (luna_dobytecode(s, luna_bc, luna_bc_len, "@aot") != 0)
        return 1;
    luna_free(s);
    return 0;
}
```

It is linked with `liblunaberry.a`, so the executable is self-contained and does
not need `lunaberry.dll`.

### Windows details

clang defaults to the MSVC target on Windows and may not find MinGW headers.
LunaAOT automatically adds:

```
--target=x86_64-w64-windows-gnu
-isystem <mingw>/x86_64-w64-mingw32/include
```

The MinGW include directory is discovered from `MINGW_PREFIX`,
`MSYSTEM_PREFIX`, or `gcc -print-file-name=include`.  The result is a COFF
executable that links with the MinGW-built static library.

### Options

| Option | Effect |
| --- | --- |
| `--o3` | `-O3` instead of `-O2` |
| `--lto` | add `-flto` |
| `--native` | add `-march=native -mtune=native` |
| `--emit-ir` | keep `<out>.ll` |
| `--strip` | strip the executable |
| `--target lunavm` | write only the `.lbc` for `lunavm` |
| `--target both` | executable and `.lbc` |

## Berry backend

The Berry backend emits a BeASM module plus a host verifier:

| File | Description |
| --- | --- |
| `<stem>.lbc` | Lua bytecode |
| `<stem>.basm` | BeASM source with `BERRY_IMPORT`/`BERRY_EXPORT` |
| `<stem>.bex` | Berry native binary from `beasm` |
| `<stem>.host` | normal OS executable that runs the same blob |

```sh
lunaaot --backend berry --target both -o dist/berry/app examples/app_lunaberry.lua
dist/berry/app.host.exe
```

The BeASM module imports the runtime bridge from `platform/berry`:

```
BERRY_IMPORT luna_berry_init
BERRY_IMPORT luna_berry_eval_binary
BERRY_IMPORT luna_berry_loop
BERRY_IMPORT luna_berry_destroy
BERRY_EXPORT luna_app_entry, 1
```

`beasm` is built from `tools/beasm/beasm.c` into `build/beasm`.  If it is
missing, `.lbc` and `.basm` are still produced and the emitter prints a warning.

## Portability

- LLVM executables are standalone; copy the executable only.
- JIT launchers need `liblunaberry.a` next to the launcher.
- Berry `.bex` modules require the Berry runtime that provides
  `luna_berry_*`.
- `.lbc` files run on any `lunavm` with a compatible Lua bytecode version.
