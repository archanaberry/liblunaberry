/*
 * luna_berry.h — LunaBerry embed adapter for Berry OS class devices.
 *
 * This is the small ABI bridge used by BeASM-generated modules
 * (dist/berry/<name>.basm).  It has no OS dependencies beyond the LunaBerry
 * core, so the same header can be compiled on the host for verification
 * and on a Berry target by the device runtime.
 */

#ifndef LUNA_PLATFORM_BERRY_H
#define LUNA_PLATFORM_BERRY_H

#include <stddef.h>
#include <stdint.h>

#include "luna.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct LunaBerryEngine {
    LunaState *state;
    size_t heap_limit;
    size_t stack_limit;
    int ready;
} LunaBerryEngine;

/* Boot the engine. heap_limit 0 selects the built-in default budget.
 * Returns 0 on success, -1 on failure. */
int luna_berry_init(LunaBerryEngine *e, size_t heap_limit, size_t stack_limit);

/* Evaluate source text in the global scope. */
int luna_berry_eval(LunaBerryEngine *e, const char *src, size_t len,
                    const char *name);

/* Evaluate a compiled Luna bytecode blob (lunac output / .lbc). This is
 * the entry a BeASM-built app module calls with its embedded blob. */
int luna_berry_eval_binary(LunaBerryEngine *e, const uint8_t *buf, size_t len);

/* Drain pending work. Lua has no promise queue, so this is a no-op that
 * keeps the Berry runtime ABI stable. */
int luna_berry_loop(LunaBerryEngine *e);

/* Tear down the engine and release all memory. */
void luna_berry_destroy(LunaBerryEngine *e);

#ifdef __cplusplus
}
#endif

#endif /* LUNA_PLATFORM_BERRY_H */
