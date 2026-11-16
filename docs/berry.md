# Berry backend

The Berry backend mirrors the `build-berry.sh` flow used by libmochiberry:
produce a readable assembler module, assemble it to a native Berry binary, and
build a normal host executable that proves the same blob works.

## Artifacts

`lunaaot --backend berry` writes:

```
<stem>.lbc     Lua bytecode
<stem>.basm    BeASM module (blob + imports/exports)
<stem>.bex     Berry native binary (via beasm)
<stem>.host    host verification executable
```

The `.lbc` is also usable directly:

```sh
lunavm dist/berry/app.lbc
```

## BeASM module

The emitter writes a module with the runtime imports and one exported entry:

```asm
BERRY_IMPORT luna_berry_init
BERRY_IMPORT luna_berry_eval_binary
BERRY_IMPORT luna_berry_loop
BERRY_IMPORT luna_berry_destroy

luna_app_entry:
    ...
    BERRY_EXPORT luna_app_entry, 1
```

The blob is embedded as data.  At load time the Berry runtime resolves the
imports from `platform/berry`; the entry initializes an engine, evaluates the
blob, drains pending work, and tears the engine down.

## Assembling

`beasm` is vendored at `tools/beasm/beasm.c` and built as `build/beasm`:

```sh
make build/beasm
build/beasm module.basm -o module.bex --format bex --entry luna_app_entry
```

`luna_berry_emit` finds `beasm` in this order:

1. `LunaBerryOptions.beasm` (explicit path);
2. the directory of the running executable;
3. `<root>/tools/beasm`.

If `beasm` is missing, the `.lbc` and `.basm` are still produced and a warning
is printed.  A missing assembler is not a fatal error.

## Host verification

The host executable is compiled with the system C compiler and linked against
`liblunaberry.a`.  It includes `luna_berry.h` (found through `<root>` or
`<root>/platform/berry`), embeds the same blob, and runs:

```c
LunaBerryEngine e;
luna_berry_init(&e, 0, 0);
luna_berry_eval_binary(&e, luna_app_bytecode, luna_app_bytecode_len);
luna_berry_loop(&e);
luna_berry_destroy(&e);
```

Running it is the quickest way to confirm that the emitted blob is valid:

```sh
dist/berry/app.host.exe
```

## Runtime ABI

The Berry runtime only needs the functions declared in
`platform/berry/luna_berry.h`.  They are OS-independent apart from the core
engine, so the same header can be compiled for a device runtime and for the
host verifier.
