# LunaBerry

A small, modular **Lua 5.5.1+** engine with two native tiers:

- **LunaJIT** — compile-on-first-use native shims (CC or LLVM/clang), cached
  and loaded at runtime.
- **LunaAOT** — ahead-of-time standalone executables and Berry/BeASM modules.

The engine core is the upstream Lua implementation.  The LunaBerry layers add a
stable C API, a bytecode compiler, a runner/REPL, a JIT host bridge, an
LLVM/clang AOT connector, and a Berry/BeASM artifact backend.

```
lunavm / lunaberry   runner and REPL
lunac                bytecode compiler (.lbc)
lunajit              LunaJIT driver (cc | llvm | berry)
lunaaot              LunaAOT driver (llvm | berry)
liblunaberry.a       static engine library
lunaberry.dll/.so    shared engine library (optional embedding target)
```

## Quick start

### Windows (MinGW + Git Bash)

```sh
make
./build/lunavm.exe examples/hello.lua
./build/lunac.exe -o hello.lbc examples/hello.lua
./build/lunavm.exe hello.lbc
```

### Unix

```sh
make
./build/lunavm examples/hello.lua
```

`make` builds everything: the Lua objects, `liblunaberry.a`, the shared library,
all CLIs, and `beasm`.  No third-party packages are required.

The REPL uses `lunaberry > ` / `lunaberry | ` prompts. Multi-line input is
explicit by default: `/begin`, then `/run` (one-shot buffer) or `/break`.
`.help` lists commands; `.reset` gives a fresh scope; `.load FILE` runs a file.
Use `--autoline` for automatic multi-line input and `--autoclean` to rebuild
the context after each evaluation. See [docs/cli.md](docs/cli.md#repl).

## Native tiers

### LunaJIT

```sh
# compile a native shim at runtime and execute it
./build/lunajit.exe --backend cc   --target lunavm --run examples/fib.lua
./build/lunajit.exe --backend llvm --target lunavm --run examples/fib.lua

# produce a portable JIT launcher (it JIT-compiles on first run)
./build/lunajit.exe --backend llvm --target exe -o dist/fib examples/fib.lua
./dist/fib.exe
```

The launcher embeds the bytecode, finds `liblunaberry.a` next to itself, and
uses the selected compiler at runtime.  Copy the launcher together with
`liblunaberry.a` if you move it.

`-O0`..`-O10` selects the shared optimization ladder (Raw..The Stripper):
O4+ adds loop unrolling, O8+ native tuning, and O10 strips the binary
implicitly and writes "dead assembly" with `--emit-s`.

The ladder also applies to the engine itself — the VM is native code, so its
level is a build-time property:

```sh
make VM_LADDER=7     # build the engine with the level-7 flag set
make bench-ladder    # time every tier at every level
```

### LunaAOT

```sh
# standalone native executable + optional LLVM IR + .lbc
./build/lunaaot.exe --backend llvm --target both --emit-ir --o3 examples/fib.lua

# Berry/BeASM module and host verification executable
./build/lunaaot.exe --backend berry --target both -o dist/berry/fib examples/fib.lua
./dist/berry/fib.host.exe
```

`--target exe` builds only the executable, `--target lunavm` writes the `.lbc`
for `lunavm`, and `--target both` does both.

## Layout

```
luna.h / luna.c            public API, allocator, source/bytecode loader
luna-common.[ch]           files, paths, process runner, C emission, hashing
luna-jit.[ch]              LunaJIT: shim generation, cache, CC/LLVM backends
luna-llvm.[ch]             LunaAOT: C wrapper -> clang -> native executable
luna-berry.[ch]            Berry artifacts: .lbc, .basm, .bex, host verifier
platform/berry/            Berry runtime ABI (luna_berry_*)
luna-vm.c                  lunavm / lunaberry runner and REPL
lunac.c                    bytecode compiler
luna-jit-cli.c             lunajit
luna-aot-cli.c             lunaaot
tools/beasm/               vendored BeASM assembler source
vendor/lua/                Lua 5.5.1 sources
scripts/                   convenience build scripts
examples/                  small Lua programs
tests/                     C API, interpreter, JIT, AOT, Berry tests
docs/                      design and CLI reference
```

## C API

```c
#include "luna.h"

LunaOptions o = { 0 };
LunaState *s = luna_new(&o);
if (luna_dofile(s, "app.lua") != 0)
    fprintf(stderr, "%s\n", luna_last_error(s));
luna_free(s);
```

Compile and link:

```sh
cc -I. app.c build/liblunaberry.a -lm
```

See [docs/api.md](docs/api.md) for the complete API.

## Build targets

| Target | Description |
| --- | --- |
| `make` | engine, shared library, all CLIs, `beasm` |
| `make lib` | `build/liblunaberry.a` |
| `make shared` | `build/lunaberry.dll` / `liblunaberry.so` |
| `make test` | C API, interpreter, compiler, JIT, AOT, Berry smoke tests |
| `make jit` | build and run the LLVM JIT demo |
| `make native` | AOT-compile `examples/app_lunaberry.lua` |
| `make berry` | Berry-compile `examples/app_lunaberry.lua` |
| `make dist` | clean build, tests, and self-contained `dist/` tree |
| `make install` | install to `PREFIX` (default `/usr/local`) |
| `make clean` | remove `build/` and `dist/` |

## Documentation

- [Architecture](docs/architecture.md)
- [CLI reference](docs/cli.md)
- [LunaJIT](docs/jit.md)
- [LunaAOT](docs/aot.md)
- [C API](docs/api.md)
- [Berry backend](docs/berry.md)
- [Inline language (`luna.compile` / `luna.asm`)](docs/inline.md)
- [Building and troubleshooting](docs/build.md)

## License

LunaBerry is distributed under the MIT license.  The vendored Lua sources keep
their upstream license; see [LICENSE](LICENSE) and `vendor/lua/`.
