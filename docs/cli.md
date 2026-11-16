# CLI reference

All tools print `-h` / `--help` and `-v` / `--version`.

## `lunavm` / `lunaberry`

Runner and REPL.

```
lunavm [options] [file [args...]]
```

| Option | Meaning |
| --- | --- |
| `-e CODE` | execute a Lua string |
| `-i` | enter the REPL after running a file |
| `-c` | compile the input to bytecode |
| `-o FILE` | output path for `-c` (default `<stem>.lbc`) |
| `-s` | strip debug information when compiling |
| `--jit[=BACKEND]` | run through LunaJIT (`cc` or `llvm`) |
| `--aot[=BACKEND]` | compile and run through LunaAOT (`llvm` default, `berry`, or `lbc`) |
| `--backend NAME` | backend for `--jit` / `--aot` |
| `--cache-dir DIR` | persistent LunaJIT shim cache |
| `--mem-limit N` | Lua allocator budget in bytes (0 = unlimited) |
| `--autoline` | REPL: evaluate as soon as the input is syntactically complete |
| `--autoclean` | REPL: rebuild the context after every evaluation |
| `--jit-level N` | JIT optimization level `-O0`..`-O3` (deprecated alias) |
| `-O N`, `--opt-level N` | optimization ladder `0`..`10` (Raw..The Stripper) |
| `--native` | add `-march=native -mtune=native` |
| `--internal-loop` | add `-funroll-loops` for tight loops |
| `--stats` | print LunaJIT statistics |

Examples:

```sh
lunavm examples/hello.lua
lunavm -e "print(1 + 2)"
lunavm -i examples/fib.lua
lunavm --autoline
lunavm -c -o build/hello.lbc examples/hello.lua
lunavm --jit=llvm --stats examples/fib.lua
lunavm --aot=llvm examples/fib.lua
lunavm --aot=berry examples/fib.lua
lunavm build/hello.lbc
```

`lunaberry` is the same binary under a second name.

### REPL

The prompt is `lunaberry > ` and changes to `lunaberry | ` while a
multi-line buffer is active. By default, multi-line input is explicit:

```
lunaberry > /begin
lunaberry | function add(a, b)
lunaberry |   return a + b
lunaberry | end
lunaberry | print(add(2, 3))
lunaberry | /run
5
lunaberry > 
```

`/run` is one-shot: the buffer is cleared after success or error, while the
scope persists. Use `.reset` for a fresh scope or `.autoclean` to rebuild the
context after every evaluation. `--autoline` skips `/begin` and evaluates as
soon as the input compiles; incomplete input continues on the next line.

| Command | Meaning |
| --- | --- |
| `/begin` | start a multi-line buffer |
| `/run` | execute the buffer, then clear it |
| `/clear` | clear the buffer, stay in multi-line mode |
| `/break`, `.break` | discard the buffer and return to `>` |
| `.help` | list REPL commands |
| `.version` | print the engine version |
| `.quit`, `.exit` | leave the REPL |
| `.reset` | replace the engine state with a fresh one |
| `.load FILE` | run a Lua source or `.lbc` file in the current scope |
| `.clear` | clear the terminal |
| `.autoline` | toggle automatic multi-line mode |
| `.autoclean` | toggle fresh-context-after-evaluation |

`=expr` evaluates an expression. `_` holds the last successful result and
`_error` holds the last error message. `Ctrl+C` discards an active buffer;
press it twice at the `>` prompt to exit. `Ctrl+D` runs a non-empty buffer at
the `|` prompt on an interactive terminal, otherwise it exits.

## `lunac`

Bytecode compiler.

```
lunac [options] file
```

| Option | Meaning |
| --- | --- |
| `-o FILE` | output file (default `luac.out`) |
| `-s` | strip debug information |
| `-v` | show version |
| `-h` | show help |

```sh
lunac -o app.lbc app.lua
lunavm app.lbc
```

## `lunajit`

LunaJIT driver.  It compiles a chunk and can produce a `.lbc`, a runtime JIT
launcher, or Berry artifacts.

```
lunajit [options] file
```

| Option | Meaning |
| --- | --- |
| `--backend NAME` | `cc`, `llvm`, or `berry` |
| `--target NAME` | `exe`, `lunavm`, or `both` (default `both`) |
| `-o BASE` | output base (default `dist/<stem>`, or `dist/berry/<stem>` for Berry) |
| `--cache-dir DIR` | persistent shim cache |
| `--stats` | print JIT statistics |
| `--run` | run the chunk after compiling |
| `--emit-ir`, `-o-llvm` | emit the shim's LLVM `.ll` (needs `--backend llvm`) |
| `-o-c`, `--emit-c` | emit the C shim source |
| `--jit-level N` | optimization level `-O0`..`-O3` (default `-O2`/`-O3`) |
| `-O N`, `--opt-level N` | optimization ladder `0`..`10` (Raw..The Stripper) |
| `--native` | add `-march=native -mtune=native` |
| `--internal-loop` | add `-funroll-loops` for tight loops |
| `--hot-threshold N` | reserved (whole-chunk shim compiles on first use) |
| `--verbose` | print toolchain commands |

Examples:

```sh
lunajit --backend cc   --target lunavm --run examples/fib.lua
lunajit --backend llvm --target exe -o dist/fib examples/fib.lua
lunajit --backend berry --target exe examples/fib.lua
lunajit --backend llvm -o-llvm examples/fib.lua   # -> fib.ll
lunajit -o-c examples/fib.lua                     # -> fib.c shim
```

A JIT launcher is portable if `liblunaberry.a` is placed next to it.  The
launcher embeds the bytecode and JIT-compiles on first execution.

## `lunaaot`

LunaAOT driver.

```
lunaaot [options] file
```

| Option | Meaning |
| --- | --- |
| `--backend NAME` | `llvm` (default) or `berry` |
| `--target NAME` | `exe`, `lunavm`, or `both` (default `exe`) |
| `-o PATH` | executable path (LLVM) or output base (Berry) |
| `--emit-ir`, `-o-llvm` | keep the LLVM `.ll` next to the executable |
| `-o-c`, `--emit-c` | emit the standalone C wrapper source |
| `--strip` | strip the produced executable |
| `--lto` | enable link-time optimization |
| `--o3` | compile at `-O3` (alias for `-O3`) |
| `-O N`, `--opt-level N` | optimization ladder `0`..`10` (Raw..The Stripper) |
| `--emit-s` | emit assembly next to the executable (dead assembly at `-O10`) |
| `--native` | add `-march=native -mtune=native` |
| `--static` | link `liblunaberry.a` statically (default) |
| `--verbose` | print toolchain commands |
| `--run` | run the produced executable/host |

Examples:

```sh
lunaaot --backend llvm --target both --emit-ir --o3 examples/app_lunaberry.lua
lunaaot --backend llvm -o dist/app-native.exe examples/app_lunaberry.lua
lunaaot --backend berry --target both -o dist/berry/app examples/app_lunaberry.lua
lunaaot --backend berry --target both --run examples/app_lunaberry.lua
lunaaot --target lunavm -o-llvm examples/app_lunaberry.lua   # -> app.lbc + app.ll
lunaaot -o-c examples/fib.lua                                 # -> fib.c wrapper
```

## `beasm`

The vendored Berry assembler is built as `build/beasm`.  `lunaaot` finds it
automatically next to the executable or under `tools/beasm`.

```sh
build/beasm module.basm -o module.bex --format bex --entry luna_app_entry
```

## The O0..O10 optimization ladder

`lunaaot`, `lunajit` and `lunavm --jit` share a single optimization ladder.
Each level is a real, measurable change in the generated code, driven by
clang/gcc flags.  `-O` accepts `0`..`10` (or `--opt-level N`); `--o3` and
`--jit-level N` remain as aliases.

| Level | Name | Effect |
| --- | --- | --- |
| `O0` | Raw | no optimization, no debug info |
| `O1` | Clean | `-O1` (mem2reg / SROA / instcombine) |
| `O2` | Solid | `-O2` (production default) |
| `O3` | Fast | `-O3` (vectorization, aggressive inlining) |
| `O4` | Mochi | + loop unrolling |
| `O5` | Mulch | + frame-pointer omission |
| `O6` | Akari | + errno-free math |
| `O7` | Trace | + hidden visibility |
| `O8` | Silicon | + native tuning |
| `O9` | Nirvana | + aggressive floating point |
| `O10` | The Stripper | + no debug info, no unwind tables, implicit strip, dead assembly |

At `O10` the produced executable is stripped automatically (a `--strip` flag is
redundant and warns), and `lunaaot --emit-s` writes "dead assembly": source
identity (`.file`, `.ident`, `.comment`, debug directives) is removed and every
non-exported label is anonymized to `.Lanon<N>`, so the assembly leaks nothing
about the original source path or function names.  Exported symbols keep their
names because the ABI needs them.

`make bench` times the AOT compile at every level and records the produced
binary size; every level must produce byte-identical output, and `O10` shows
the implicit strip as a smaller executable.

## The engine build level (VM)

The interpreter is native code, so its optimization level is a **build-time**
property, not a runtime flag:

```sh
make VM_LADDER=7     # engine built with the level-7 flag set
make VM_OPT="-O3"    # or any custom flags
```

`tools/ladder_flags.sh N` prints the flag set of level `N`; it mirrors the C
table in `luna-opt.c` (verified by `tests/test_opt.sh`), and
`lunavm --opt-flags N` prints the C side.  `make bench-vm` rebuilds the engine
at each level and measures the interpreter.  Measured on this tree
(best-of-3, fib+sieve workload): **O0 ≈ 2800 ms → O7 ≈ 850 ms, i.e. ~3.3×**.

## Full-stack ladder benchmark

`make bench-ladder` rebuilds the engine at each level and times every tier
against that engine — `vm`, runtime JIT (`--jit=cc -O N`), the copy-path-and-
patch launcher (`lunajit --target exe -O N`), `aot-llvm` and `aot-berry`:

| lvl | vm | jit-rt | jit-cpp | aot-llvm | aot-berry |
| --- | --- | --- | --- | --- | --- |
| O0  | 2805 | 3065 | 2835 | 2733 | 2848 |
| O2  | 938  | 1231 | 933  | 964  | 934  |
| O7  | 854  | 1094 | 854  | 846  | 844  |
| O10 | 859  | 1078 | 850  | 862  | 853  |

The engine build level dominates (O0→O7 ≈ 3.3×); the copy-path-and-patch JIT
launcher matches the interpreter exactly (no runtime compile), and the Berry
AOT host matches the LLVM AOT executable.

