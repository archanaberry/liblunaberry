-- examples/inline_c.lua — inline C and inline assembly via luna.compile / luna.asm.
--
-- This is the LunaBerry counterpart of mochiberry's mbjs.compile().  The C
-- source string is compiled into a tiny shared object at runtime and exposed
-- as a Lua table of native functions with automatic marshalling.
--
--   luna.compile(src)        auto-detect typedef structs + function definitions
--   luna.compile(src, sigs)  explicit signatures: { name = {ret, arg1, ...} }
--   luna.asm(src)            compile with GCC inline __asm__ support

local native = luna.compile([[
    typedef struct { int x; int y; } Vec2;

    Vec2 vec2_add(Vec2 a, Vec2 b) {
        return (Vec2){ a.x + b.x, a.y + b.y };
    }

    double square(double x) { return x * x; }

    const char *greet(const char *name) { return name; }
]])

local v = native.vec2_add({ x = 1, y = 2 }, { x = 3, y = 4 })
print("vec2_add  ->", v.x, v.y)          -- 4  6
print("square(9) ->", native.square(9))  -- 81
print("greet     ->", native.greet("luna"))  -- luna

local asm = luna.asm([[
    /* Operand numbering is positional: "=r"(r) is %0, "0"(a) is %1,
     * "r"(b) is %2.  Use "+r"(r) with a single "r"(b) input instead. */
    int asm_add(int a, int b) {
        int r = a;
        __asm__ __volatile__("addl %1, %0" : "+r"(r) : "r"(b));
        return r;
    }
]])
print("asm_add   ->", asm.asm_add(20, 22))  -- 42

local explicit = luna.compile([[
    double mul(double a, double b) { return a * b; }
]], { mul = { "double", "double", "double" } })
print("mul       ->", explicit.mul(6, 7))  -- 42
