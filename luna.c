/*
 * luna.c — LunaBerry core: a thin, deterministic wrapper around Lua 5.5.1.
 *
 * Everything here is deliberately boring: one allocator, one error buffer,
 * one place that knows how to load source or bytecode.  The JIT, AOT and
 * Berry layers are separate translation units so the core stays small and
 * embeddable.
 */

#include "luna.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "luna-inline.h"

/* ------------------------------------------------------------------ */
/* Tracking allocator                                                  */
/* ------------------------------------------------------------------ */

typedef struct LunaAlloc {
    size_t used;
    size_t peak;
    size_t limit;
    int oom;
} LunaAlloc;

static void *luna_alloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
    LunaAlloc *a = (LunaAlloc *)ud;

    if (nsize == 0) {
        if (ptr != NULL) {
            a->used -= osize;
            free(ptr);
        }
        return NULL;
    }

    if (a->limit != 0 && nsize > osize) {
        size_t grow = nsize - osize;
        if (a->used + grow > a->limit) {
            a->oom = 1;
            return NULL;
        }
    }

    {
        void *np = realloc(ptr, nsize);
        if (np == NULL)
            return NULL;
        if (nsize >= osize)
            a->used += nsize - osize;
        else
            a->used -= osize - nsize;
        if (a->used > a->peak)
            a->peak = a->used;
        return np;
    }
}

struct LunaState {
    lua_State *L;
    LunaAlloc alloc;
    char err[512];
};

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static void luna_clear_error(LunaState *s)
{
    if (s != NULL)
        s->err[0] = '\0';
}

static void luna_set_error(LunaState *s, const char *msg)
{
    if (s == NULL)
        return;
    if (msg == NULL)
        msg = "unknown error";
    snprintf(s->err, sizeof(s->err), "%s", msg);
}

static void luna_capture_error(LunaState *s)
{
    const char *msg;

    if (s == NULL || s->L == NULL) {
        luna_set_error(s, "no Lua state");
        return;
    }
    msg = lua_tostring(s->L, -1);
    if (msg == NULL) {
        /* The error object is not a string: let Lua stringify it. */
        luaL_tolstring(s->L, -1, NULL);
        msg = lua_tostring(s->L, -1);
    }
    luna_set_error(s, msg);
    lua_settop(s->L, 0);
}

static char *luna_read_all(const char *path, size_t *plen)
{
    FILE *f;
    long sz;
    char *buf;
    size_t got;

    f = fopen(path, "rb");
    if (f == NULL) {
        *plen = 0;
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (sz = ftell(f)) < 0 ||
        fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        *plen = 0;
        return NULL;
    }
    buf = (char *)malloc((size_t)sz + 1);
    if (buf == NULL) {
        fclose(f);
        *plen = 0;
        return NULL;
    }
    got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) {
        free(buf);
        *plen = 0;
        return NULL;
    }
    buf[sz] = '\0';
    *plen = (size_t)sz;
    return buf;
}

typedef struct LunaBuf {
    unsigned char *data;
    size_t len;
    size_t cap;
} LunaBuf;

static int luna_writer(lua_State *L, const void *p, size_t sz, void *ud)
{
    LunaBuf *b = (LunaBuf *)ud;
    (void)L;

    if (b->len + sz > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 4096;
        unsigned char *np;
        while (nc < b->len + sz)
            nc *= 2;
        np = (unsigned char *)realloc(b->data, nc);
        if (np == NULL)
            return 1;
        b->data = np;
        b->cap = nc;
    }
    memcpy(b->data + b->len, p, sz);
    b->len += sz;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Public core API                                                     */
/* ------------------------------------------------------------------ */

LunaState *luna_new(const LunaOptions *opts)
{
    LunaState *s = (LunaState *)calloc(1, sizeof(*s));

    if (s == NULL)
        return NULL;
    if (opts != NULL) {
        s->alloc.limit = opts->mem_limit;
    }

    s->L = lua_newstate(luna_alloc, &s->alloc, 0x4C554E41u);
    if (s->L == NULL) {
        free(s);
        return NULL;
    }
    lua_setwarnf(s->L, NULL, NULL);

    if (opts == NULL || opts->open_libs) {
#if defined(LUNA_TIER_SLIM)
        /* slim: only what a small script needs.  io/os/package/debug stay
         * out of the binary entirely because nothing references them. */
        static const luaL_Reg slimlibs[] = {
            { LUA_GNAME, luaopen_base },
            { LUA_STRLIBNAME, luaopen_string },
            { LUA_TABLIBNAME, luaopen_table },
            { LUA_MATHLIBNAME, luaopen_math },
            { LUA_COLIBNAME, luaopen_coroutine },
            { NULL, NULL }
        };
        int i;
        for (i = 0; slimlibs[i].name != NULL; i++) {
            luaL_requiref(s->L, slimlibs[i].name, slimlibs[i].func, 1);
            lua_pop(s->L, 1);
        }
#elif defined(LUNA_TIER_CORE)
        /* core: everything except the heavy package/debug loaders */
        static const luaL_Reg corelibs[] = {
            { LUA_GNAME, luaopen_base },
            { LUA_STRLIBNAME, luaopen_string },
            { LUA_TABLIBNAME, luaopen_table },
            { LUA_MATHLIBNAME, luaopen_math },
            { LUA_COLIBNAME, luaopen_coroutine },
            { LUA_IOLIBNAME, luaopen_io },
            { LUA_OSLIBNAME, luaopen_os },
            { LUA_UTF8LIBNAME, luaopen_utf8 },
            { NULL, NULL }
        };
        int i;
        for (i = 0; corelibs[i].name != NULL; i++) {
            luaL_requiref(s->L, corelibs[i].name, corelibs[i].func, 1);
            lua_pop(s->L, 1);
        }
#else
        luaL_openlibs(s->L);
#endif
    }

    luna_inline_open(s->L);

    if (opts != NULL && opts->name != NULL) {
        lua_settop(s->L, 0);
        lua_pushstring(s->L, opts->name);
        lua_setglobal(s->L, "_LUNA_CHUNK");
    }

    return s;
}

void luna_free(LunaState *s)
{
    if (s == NULL)
        return;
    if (s->L != NULL) {
        lua_settop(s->L, 0);
        lua_close(s->L);
    }
    free(s);
}

int luna_dostring(LunaState *s, const char *src, size_t len, const char *name)
{
    int rc;

    if (s == NULL || s->L == NULL) {
        luna_set_error(s, "no Lua state");
        return -1;
    }
    luna_clear_error(s);
    lua_settop(s->L, 0);

    if (luaL_loadbufferx(s->L, src, len, name ? name : "=(luna)", "t") !=
        LUA_OK) {
        luna_capture_error(s);
        return -1;
    }
    rc = lua_pcall(s->L, 0, LUA_MULTRET, 0);
    if (rc != LUA_OK) {
        luna_capture_error(s);
        return -1;
    }
    return 0;
}

int luna_dofile(LunaState *s, const char *path)
{
    char *buf;
    size_t len;
    char chunk[512];
    int rc;

    buf = luna_read_all(path, &len);
    if (buf == NULL) {
        luna_set_error(s, "cannot read file");
        return -1;
    }
    snprintf(chunk, sizeof(chunk), "@%s", path);
    rc = luna_dostring(s, buf, len, chunk);
    free(buf);
    return rc;
}

int luna_dobytecode(LunaState *s, const unsigned char *bc, size_t len,
                    const char *name)
{
    int rc;

    if (s == NULL || s->L == NULL) {
        luna_set_error(s, "no Lua state");
        return -1;
    }
    if (bc == NULL || len == 0) {
        luna_set_error(s, "empty bytecode");
        return -1;
    }
    luna_clear_error(s);
    lua_settop(s->L, 0);

    if (luaL_loadbufferx(s->L, (const char *)bc, len,
                         name ? name : "=(luna-bc)", "b") != LUA_OK) {
        luna_capture_error(s);
        return -1;
    }
    rc = lua_pcall(s->L, 0, LUA_MULTRET, 0);
    if (rc != LUA_OK) {
        luna_capture_error(s);
        return -1;
    }
    return 0;
}

int luna_compile(LunaState *s, const char *src, size_t len, const char *name,
                 unsigned char **out, size_t *out_len, int strip)
{
    LunaBuf b;

    if (out != NULL)
        *out = NULL;
    if (out_len != NULL)
        *out_len = 0;
    if (s == NULL || s->L == NULL || src == NULL || out == NULL ||
        out_len == NULL) {
        luna_set_error(s, "invalid compile arguments");
        return -1;
    }

    luna_clear_error(s);
    lua_settop(s->L, 0);
    if (luaL_loadbufferx(s->L, src, len, name ? name : "=(luna)", "t") !=
        LUA_OK) {
        luna_capture_error(s);
        return -1;
    }

    memset(&b, 0, sizeof(b));
    if (lua_dump(s->L, luna_writer, &b, strip ? 1 : 0) != 0) {
        free(b.data);
        lua_settop(s->L, 0);
        luna_set_error(s, "bytecode dump failed");
        return -1;
    }
    lua_settop(s->L, 0);

    *out = b.data;
    *out_len = b.len;
    return 0;
}

int luna_compile_file(LunaState *s, const char *path, unsigned char **out,
                      size_t *out_len, int strip)
{
    char *buf;
    size_t len;
    char chunk[512];
    int rc;

    buf = luna_read_all(path, &len);
    if (buf == NULL) {
        luna_set_error(s, "cannot read file");
        return -1;
    }
    snprintf(chunk, sizeof(chunk), "@%s", path);
    rc = luna_compile(s, buf, len, chunk, out, out_len, strip);
    free(buf);
    return rc;
}

const char *luna_version(void)
{
    return LUNA_VERSION_STRING;
}

const char *luna_engine_name(void)
{
    return LUNA_ENGINE_NAME;
}

const char *luna_package_name(void)
{
    return LUNA_PACKAGE_NAME;
}

const char *luna_last_error(const LunaState *s)
{
    return s != NULL ? s->err : "no state";
}

lua_State *luna_raw(LunaState *s)
{
    return s != NULL ? s->L : NULL;
}

void luna_set_mem_limit(LunaState *s, size_t limit)
{
    if (s != NULL)
        s->alloc.limit = limit;
}

size_t luna_mem_used(const LunaState *s)
{
    return s != NULL ? s->alloc.used : 0;
}

size_t luna_mem_peak(const LunaState *s)
{
    return s != NULL ? s->alloc.peak : 0;
}

int luna_result_count(LunaState *s)
{
    return s != NULL && s->L != NULL ? lua_gettop(s->L) : 0;
}

int luna_result_type(LunaState *s, int idx)
{
    int t;

    if (s == NULL || s->L == NULL)
        return 0;
    t = lua_type(s->L, idx);
    switch (t) {
    case LUA_TNUMBER:
        return 1;
    case LUA_TSTRING:
        return 2;
    case LUA_TBOOLEAN:
        return 3;
    case LUA_TNIL:
        return 0;
    default:
        return 4;
    }
}

double luna_result_number(LunaState *s, int idx)
{
    if (s == NULL || s->L == NULL)
        return 0.0;
    return (double)lua_tonumber(s->L, idx);
}

const char *luna_result_string(LunaState *s, int idx)
{
    if (s == NULL || s->L == NULL)
        return NULL;
    return lua_tostring(s->L, idx);
}

int luna_result_boolean(LunaState *s, int idx)
{
    if (s == NULL || s->L == NULL)
        return 0;
    return lua_toboolean(s->L, idx);
}

/* ------------------------------------------------------------------ */
/* JIT host bridge                                                     */
/* ------------------------------------------------------------------ */

static LunaState *g_jit_state = NULL;

double luna_jit_host_run(const char *bc, size_t len, char *err, size_t errlen)
{
    LunaOptions opts;
    int n;
    double result = 0.0;

    if (err != NULL && errlen > 0)
        err[0] = '\0';

    if (g_jit_state == NULL) {
        memset(&opts, 0, sizeof(opts));
        opts.open_libs = 1;
        opts.mem_limit = 0;
        g_jit_state = luna_new(&opts);
        if (g_jit_state == NULL) {
            if (err != NULL && errlen > 0)
                snprintf(err, errlen, "cannot create JIT host state");
            return 0.0;
        }
    }

    if (luna_dobytecode(g_jit_state, (const unsigned char *)bc, len,
                        "=(luna-jit)") != 0) {
        if (err != NULL && errlen > 0)
            snprintf(err, errlen, "%s", luna_last_error(g_jit_state));
        return 0.0;
    }

    n = luna_result_count(g_jit_state);
    if (n >= 1 && luna_result_type(g_jit_state, n) == 1)
        result = luna_result_number(g_jit_state, n);

    lua_settop(g_jit_state->L, 0);
    return result;
}
