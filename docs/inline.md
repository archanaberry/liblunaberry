# Inline language (`luna.compile` / `luna.asm`)

LunaBerry's inline-language tier is the counterpart of mochiberry's
`mbjs.compile()`.  It compiles a C source string into a tiny shared object at
runtime with the host toolchain (`gcc` on Windows/MinGW, `clang`/`cc` on Unix)
and hands back a Lua table of callable native functions with automatic
marshalling.

```lua
local native = luna.compile([[
    typedef struct { int x; int y; } Vec2;

    Vec2 vec2_add(Vec2 a, Vec2 b) {
        return (Vec2){ a.x + b.x, a.y + b.y };
    }

    double square(double x) { return x * x; }
    const char *greet(const char *name) { return name; }
]])

print(native.vec2_add({x=1,y=2}, {x=3,y=4}).x)   -- 4
print(native.square(9))                          -- 81
print(native.greet("luna"))                      -- luna
```

The `luna` global table is registered automatically by `luna_new()`.

## Supported signatures

| Lua type | C type |
| --- | --- |
| `number` | `double`, `float`, `int`, `unsigned`, `long`, `long long`, `char` |
| `string` | `const char *`, `char *` |
| `lightuserdata` | any other pointer |
| `table` (by value) | `typedef struct { ... } Name;` with scalar fields |
| — (no result) | `void` return |

Function signatures are auto-detected from the source: the parser looks for
`typedef struct { ... } Name;` and `Ret name(args) { ... }` definitions.
Structs are marshalled field-by-field as a packed `double[]`.

## Explicit signatures

When auto-detection is not suitable, pass an explicit signature table as the
second argument:

```lua
local native = luna.compile(src, {
    vec2_add = { "Vec2", "Vec2", "Vec2" },   -- { ret, arg1, arg2, ... }
    square   = { "double", "double" },
})
```

The struct definitions are still parsed from the source; only the function
list is taken from the table.

## Inline assembly (`luna.asm`)

`luna.asm(src)` compiles the string the same way but is intended for GCC
inline `__asm__` blocks.  Functions can be detected automatically when they
are ordinary C definitions wrapping inline assembly:

```lua
local asm = luna.asm([[
    int asm_add(int a, int b) {
        int r;
        __asm__ __volatile__("addl %1, %0" : "=r"(r) : "0"(a), "r"(b));
        return r;
    }
]])
print(asm.asm_add(20, 22))   -- 42
```

## Notes

- The generated shim is cached on disk in the temp directory (keyed by a
  hash of the source) so repeated `luna.compile` calls with the same source
  reuse the same shared object; it is deleted when the Lua module is
  garbage-collected.
- The compiler is discovered with `luna_find_tool` (same search as the JIT
  and AOT tiers).  `-O2 -shared` is used (plus `-fPIC` on Unix).
- `luna.asm` does not have a separate raw-assembly path in v1: it compiles
  the string as C where you embed assembly with `__asm__` / `__asm`.
