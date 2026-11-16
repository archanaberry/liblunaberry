/*
 * luna-inline.h — LunaBerry inline language (luna.compile / luna.asm).
 *
 * This is the LunaBerry counterpart of mochiberry's mbjs.compile().  It
 * compiles a C (or GCC inline-asm) source string into a tiny shared object
 * at runtime with the host C toolchain, then hands back a Lua table of
 * callable native functions with automatic marshalling:
 *
 *   local native = luna.compile([[
 *     typedef struct { int x; int y; } Vec2;
 *     Vec2 vec2_add(Vec2 a, Vec2 b) {
 *         return (Vec2){ a.x + b.x, a.y + b.y };
 *     }
 *   ]])
 *   print(native.vec2_add({x=1,y=2}, {x=3,y=4}).x)  -- 4
 *
 * Supported signatures (v1):
 *   - scalars: double, float, int, unsigned, long, long long, char
 *   - const char* / char*  (strings)
 *   - other pointers (opaque, passed as lightuserdata)
 *   - struct-by-value: typedef struct { ... } Name; (scalar fields)
 *   - void return
 *
 * Function signatures are auto-detected from the source; pass an explicit
 * `sigs` table as the second argument of luna.compile/luna.asm to override:
 *
 *   local native = luna.compile(src, { vec2_add = {"Vec2", "Vec2", "Vec2"} })
 */

#ifndef LUNA_INLINE_H
#define LUNA_INLINE_H

#include <stddef.h>
#include "lua.h"
#include "luna.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Register the `luna` global table (luna.compile, luna.asm) on `L`.
 * Returns 0 on success, -1 if the table cannot be created. */
LUNA_API int luna_inline_open(lua_State *L);

#ifdef __cplusplus
}
#endif

#endif /* LUNA_INLINE_H */
