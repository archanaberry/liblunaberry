/*
 * luna_berry.c — Berry OS embed adapter implementation.
 */

#include "luna_berry.h"

#include <string.h>

#define LUNA_BERRY_DEFAULT_HEAP (256u * 1024u)

int luna_berry_init(LunaBerryEngine *e, size_t heap_limit, size_t stack_limit)
{
    LunaOptions opts;

    if (e == NULL)
        return -1;
    memset(e, 0, sizeof(*e));
    e->heap_limit = heap_limit ? heap_limit : LUNA_BERRY_DEFAULT_HEAP;
    e->stack_limit = stack_limit;

    memset(&opts, 0, sizeof(opts));
    opts.mem_limit = e->heap_limit;
    opts.open_libs = 1;
    opts.name = "@berry";

    e->state = luna_new(&opts);
    if (e->state == NULL)
        return -1;
    e->ready = 1;
    return 0;
}

int luna_berry_eval(LunaBerryEngine *e, const char *src, size_t len,
                    const char *name)
{
    if (e == NULL || !e->ready)
        return -1;
    return luna_dostring(e->state, src, len, name ? name : "@berry");
}

int luna_berry_eval_binary(LunaBerryEngine *e, const uint8_t *buf, size_t len)
{
    if (e == NULL || !e->ready || buf == NULL || len == 0)
        return -1;
    return luna_dobytecode(e->state, buf, len, "@berry");
}

int luna_berry_loop(LunaBerryEngine *e)
{
    return e != NULL && e->ready ? 0 : -1;
}

void luna_berry_destroy(LunaBerryEngine *e)
{
    if (e == NULL)
        return;
    if (e->state != NULL)
        luna_free(e->state);
    memset(e, 0, sizeof(*e));
}
