# Building LunaBerry

## Requirements

- A C99 compiler.  Windows: MinGW-w64 gcc.  Unix: gcc/clang.
- `make` (MinGW `mingw32-make` also works).
- Optional: LLVM/clang for the LLVM backends.
- Optional: `strip` for `lunaaot --strip`.

No external Lua installation is used; `vendor/lua` is the engine.

## Make targets

```sh
make            # engine, shared lib, CLIs, beasm
make lib        # build/liblunaberry.a
make shared     # build/lunaberry.dll or liblunaberry.so
make test       # smoke tests
make jit        # LLVM JIT demo
make native     # AOT LLVM build of examples/app_lunaberry.lua
make berry      # Berry build of examples/app_lunaberry.lua
make dist       # clean build + tests + self-contained dist/ tree
make install    # install to PREFIX (default /usr/local)
make clean
```

`make dist` produces `dist/bin`, `dist/lib`, and `dist/include`.
`liblunaberry.a` is copied into `dist/bin` so `lunajit`/`lunaaot` work from
that directory without extra flags.

Override the compiler or flags:

```sh
make CC=clang CFLAGS="-O2 -g -I. -Ivendor/lua"
```

## Windows notes

### Tool discovery

`luna_find_tool` searches:

- `%USERPROFILE%\scoop\apps\mingw\current\bin`
- `%USERPROFILE%\scoop\apps\gcc\current\bin`
- `%USERPROFILE%\scoop\apps\llvm\current\bin`
- `%USERPROFILE%\scoop\shims`
- `%ProgramFiles%\LLVM\bin`
- `C:\msys64\mingw64\bin`
- `C:\mingw64\bin`

This is why the tools work even when the process `PATH` inherited by
`CreateProcess` is not the same as the Git Bash `PATH`.

### clang target

clang on Windows defaults to the MSVC target and may not find MinGW headers.
LunaBerry adds:

```
--target=x86_64-w64-windows-gnu
-isystem <mingw>/x86_64-w64-mingw32/include
```

The include directory is found from `MINGW_PREFIX`, `MSYSTEM_PREFIX`, or
`gcc -print-file-name=include`.

### DLLs

JIT shims and AOT executables link `liblunaberry.a` statically.  You do not need
to ship `lunaberry.dll` for them.  The shared library is built for embedding
hosts that prefer dynamic loading.

## Unix notes

The Makefile uses `cc`, `ar`, and `-fPIC -shared` for the shared library.
JIT shims use the same static-library approach as on Windows, so no
`LD_LIBRARY_PATH` setup is required.

## Troubleshooting

### `gcc` or `clang` not found

Install MinGW-w64 and/or LLVM, or set `CC` / `CLANG` explicitly.  On Windows,
place the compiler in one of the directories listed above.

### `clang failed ... cannot find 'stdio.h'`

The MinGW sysroot was not found.  Set `MINGW_PREFIX` to the MinGW installation
prefix (the directory containing `x86_64-w64-mingw32/include`).

### Include directory / headers not found

`lunajit` and `lunaaot` locate `luna.h` automatically: the current directory,
`../include` next to the executable (installed/dist layout), or the source tree
root.  The Berry host compiler also gets `-I<root>/platform/berry` for the
runtime header.  If you use a custom layout, pass the root through the C API
(`include_dir`).

### JIT shim link error

Ensure `liblunaberry.a` is in the same directory as the executable, or pass
`--cache-dir`/`lib_dir` appropriately.  `lunajit` copies the static library next
to a generated launcher automatically.  Temporary shims are written to
`TMPDIR`/`TEMP`/`TMP`, not the executable directory.

### `beasm not found`

Run `make` to build `build/beasm`, or pass an explicit path with
`LunaBerryOptions.beasm`.  The `.lbc` and `.basm` are still valid without it.

### Bytecode version mismatch

`.lbc` files embed the Lua bytecode version.  Use the `lunavm` from the same
LunaBerry build that produced them.
