/*
 * luna-inline.c — LunaBerry inline language (luna.compile / luna.asm).
 *
 * Compiles a C source string into a tiny shared object with the host
 * toolchain at runtime, then exposes the native functions to Lua with
 * automatic marshalling.  This is the LunaBerry counterpart of
 * libmochiberry's mbjs.compile().
 *
 * Design:
 *   - A small heuristic parser discovers `typedef struct {...} Name;`
 *     and `Ret name(args) { ... }` definitions.
 *   - A C shim is generated that embeds the user source and, for each
 *     function, exports `luna_inline_entry_N(double *args, double *out)`.
 *   - The shim is compiled with `gcc -shared` (MinGW on Windows) or
 *     `clang/cc -shared -fPIC` on Unix.
 *   - The Lua side builds a packed double[] from arguments (numbers,
 *     strings as pointers, tables as struct fields), calls the entry,
 *     and decodes the double[] result back into Lua values.
 */

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <dlfcn.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>

#include "luna.h"
#include "lua.h"
#include "lauxlib.h"
#include "luna-common.h"
#include "luna-inline.h"

/* Set LUNA_INLINE_KEEP=1 to retain the generated shim + module. */
static int luna_inline_keep_files(void)
{
    const char *e = getenv("LUNA_INLINE_KEEP");
    return e != NULL && e[0] != '\0' && e[0] != '0';
}

/* ------------------------------------------------------------------ */
/* Growable string buffer                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    char *p;
    size_t len;
    size_t cap;
} SB;

static void sb_init(SB *b) { b->p = NULL; b->len = 0; b->cap = 0; }

static void sb_free(SB *b)
{
    free(b->p);
    b->p = NULL;
    b->len = b->cap = 0;
}

static int sb_reserve(SB *b, size_t need)
{
    size_t cap;
    char *p;
    if (b->len + need + 1 <= b->cap)
        return 0;
    cap = b->cap ? b->cap * 2 : 256;
    while (b->len + need + 1 > cap)
        cap *= 2;
    p = (char *)realloc(b->p, cap);
    if (p == NULL)
        return -1;
    b->p = p;
    b->cap = cap;
    return 0;
}

static int sb_appendn(SB *b, const char *s, size_t n)
{
    if (sb_reserve(b, n) != 0)
        return -1;
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = '\0';
    return 0;
}

static int sb_append(SB *b, const char *s) { return sb_appendn(b, s, strlen(s)); }

static int sb_printf(SB *b, const char *fmt, ...)
{
    char tmp[512];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0)
        return -1;
    if ((size_t)n < sizeof(tmp))
        return sb_appendn(b, tmp, (size_t)n);
    {
        char *big = (char *)malloc((size_t)n + 1);
        int rc;
        if (big == NULL)
            return -1;
        va_start(ap, fmt);
        vsnprintf(big, (size_t)n + 1, fmt, ap);
        va_end(ap);
        rc = sb_appendn(b, big, (size_t)n);
        free(big);
        return rc;
    }
}

/* ------------------------------------------------------------------ */
/* Type model                                                          */
/* ------------------------------------------------------------------ */

typedef enum {
    LT_VOID, LT_DOUBLE, LT_FLOAT, LT_INT, LT_UINT, LT_LONG, LT_ULONG,
    LT_LLONG, LT_ULLONG, LT_CHAR, LT_STR, LT_PTR, LT_STRUCT
} LunaTypeKind;

typedef struct {
    char name[64];
    char type[64];
} LunaField;

typedef struct {
    char name[64];
    LunaField *fields;
    int field_count;
} LunaStruct;

typedef struct {
    LunaTypeKind kind;
    char tname[64];
    int struct_idx; /* index into Parser.structs for LT_STRUCT */
} LunaType;

typedef struct {
    char name[64];
    LunaType ret;
    LunaType *args;
    int argc;
} LunaFunc;

/* ------------------------------------------------------------------ */
/* Parser context                                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    LunaStruct *structs;
    int nstructs;
    int capstructs;
    LunaFunc *funcs;
    int nfuncs;
    int capfuncs;
    char hist[32][64];
    int hist_n;
} Parser;

static int is_ident_start(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static int is_ident_char(char c)
{
    return is_ident_start(c) || (c >= '0' && c <= '9');
}

/* Returns 0 EOF, 1 ident, 2 punct, 3 string-literal (skipped). */
static int next_tok(const char **pp, char *out, size_t n)
{
    const char *p = *pp;

    for (;;) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
            p++;
        if (*p == '/' && p[1] == '/') {
            p += 2;
            while (*p && *p != '\n')
                p++;
            continue;
        }
        if (*p == '/' && p[1] == '*') {
            p += 2;
            while (*p && !(*p == '*' && p[1] == '/'))
                p++;
            if (*p)
                p += 2;
            continue;
        }
        if (*p == '#') {
            while (*p && *p != '\n')
                p++;
            continue;
        }
        break;
    }

    if (*p == '\0') {
        *pp = p;
        return 0;
    }

    if (*p == '"' || *p == '\'') {
        char q = *p;
        p++;
        while (*p && *p != q) {
            if (*p == '\\' && p[1])
                p++;
            p++;
        }
        if (*p)
            p++;
        *pp = p;
        return 3;
    }

    if (is_ident_start(*p)) {
        size_t k = 0;
        while (*p && is_ident_char(*p) && k + 1 < n)
            out[k++] = *p++;
        out[k] = '\0';
        *pp = p;
        return 1;
    }

    if (n > 1) {
        out[0] = *p;
        out[1] = '\0';
    }
    *pp = p + 1;
    return 2;
}

static void trim_str(char *s)
{
    size_t len = strlen(s);
    size_t i = 0;
    while (i < len && (s[i] == ' ' || s[i] == '\t'))
        i++;
    if (i > 0) {
        memmove(s, s + i, len - i + 1);
        len -= i;
    }
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t'))
        s[--len] = '\0';
}

static void append_tok(char *buf, size_t bufsz, const char *tok)
{
    size_t bl = strlen(buf);
    if (bl > 0 && buf[bl - 1] != '*' && tok[0] != '*' && tok[0] != ',' &&
        bl + 1 < bufsz) {
        buf[bl++] = ' ';
        buf[bl] = '\0';
    }
    if (bl + strlen(tok) < bufsz)
        strcat(buf, tok);
}

/* Split a declaration like "unsigned long x" or "int a, b" into type+name.
 * The last identifier is treated as the name when there is more than one. */
static void split_type_name(const char *decl, char *type, size_t tz,
                            char *name, size_t nz)
{
    char tmp[256];
    char idents[16][64];
    int nid = 0;
    int last_start = -1;
    size_t i, len = strlen(decl);
    int k;

    for (i = 0; i < len && i < sizeof(tmp) - 1; i++)
        tmp[i] = decl[i];
    tmp[i] = '\0';

    i = 0;
    while (tmp[i]) {
        while (tmp[i] == ' ' || tmp[i] == '\t' || tmp[i] == '*')
            i++;
        if (!tmp[i])
            break;
        if (is_ident_start(tmp[i])) {
            int s = (int)i;
            int l = 0;
            while (tmp[i] && is_ident_char(tmp[i])) {
                l++;
                i++;
            }
            if (nid < 16) {
                memcpy(idents[nid], tmp + s, (size_t)l);
                idents[nid][l] = '\0';
                last_start = s;
                nid++;
            }
        } else {
            i++;
        }
    }

    if (nid == 0) {
        strncpy(type, tmp, tz - 1);
        type[tz - 1] = '\0';
        trim_str(type);
        snprintf(name, nz, "v");
        return;
    }

    if (nid >= 2) {
        char tmp2[256];
        size_t bl = 0;
        for (k = 0; k < last_start; k++) {
            if (bl + 1 < sizeof(tmp2))
                tmp2[bl++] = tmp[k];
        }
        tmp2[bl] = '\0';
        trim_str(tmp2);
        strncpy(type, tmp2, tz - 1);
        type[tz - 1] = '\0';
        trim_str(type);
        snprintf(name, nz, "%s", idents[nid - 1]);
    } else {
        snprintf(type, tz, "%s", idents[0]);
        snprintf(name, nz, "v");
    }
}

static int add_struct(Parser *pa, const char *name, const LunaField *fields,
                      int nfields)
{
    LunaStruct *st;
    if (pa->nstructs == pa->capstructs) {
        int cap = pa->capstructs ? pa->capstructs * 2 : 8;
        LunaStruct *ns =
            (LunaStruct *)realloc(pa->structs, (size_t)cap * sizeof(*ns));
        if (ns == NULL)
            return -1;
        pa->structs = ns;
        pa->capstructs = cap;
    }
    st = &pa->structs[pa->nstructs];
    memset(st, 0, sizeof(*st));
    snprintf(st->name, sizeof(st->name), "%s", name);
    if (nfields > 0) {
        st->fields = (LunaField *)calloc((size_t)nfields, sizeof(*st->fields));
        if (st->fields == NULL)
            return -1;
        memcpy(st->fields, fields, (size_t)nfields * sizeof(*st->fields));
        st->field_count = nfields;
    }
    pa->nstructs++;
    return 0;
}

static int find_struct(const Parser *pa, const char *name)
{
    int i;
    for (i = 0; i < pa->nstructs; i++) {
        if (strcmp(pa->structs[i].name, name) == 0)
            return i;
    }
    return -1;
}

static void push_hist(Parser *pa, const char *tok)
{
    if (strcmp(tok, ";") == 0 || strcmp(tok, "{") == 0 ||
        strcmp(tok, "}") == 0 || strcmp(tok, ",") == 0 ||
        strcmp(tok, "=") == 0) {
        pa->hist_n = 0;
        return;
    }
    if (pa->hist_n < 32) {
        snprintf(pa->hist[pa->hist_n], 64, "%s", tok);
        pa->hist_n++;
    }
}

static int is_scalar_keyword(const char *s)
{
    static const char *kw[] = {
        "void", "double", "float", "int", "char", "long",
        "unsigned", "signed", "short", "const", "struct", "size_t",
        "int8_t", "int16_t", "int32_t", "int64_t", "uint8_t", "uint16_t",
        "uint32_t", "uint64_t", "bool"
    };
    size_t i;
    for (i = 0; i < sizeof(kw) / sizeof(kw[0]); i++) {
        if (strcmp(s, kw[i]) == 0)
            return 1;
    }
    return 0;
}

static int is_control_keyword(const char *s)
{
    static const char *kw[] = {
        "if", "for", "while", "switch", "return", "sizeof", "do", "else",
        "case", "goto", "break", "continue", "default"
    };
    size_t i;
    for (i = 0; i < sizeof(kw) / sizeof(kw[0]); i++) {
        if (strcmp(s, kw[i]) == 0)
            return 1;
    }
    return 0;
}

static int resolve_type(const char *ts, const Parser *pa, LunaType *out)
{
    char t[128];
    char base[128];
    int isptr = 0;
    size_t len;

    strncpy(t, ts, sizeof(t) - 1);
    t[sizeof(t) - 1] = '\0';
    trim_str(t);
    len = strlen(t);
    while (len > 0 && t[len - 1] == '*') {
        isptr = 1;
        t[--len] = '\0';
    }
    trim_str(t);
    if (strncmp(t, "const ", 6) == 0)
        memmove(t, t + 6, strlen(t + 6) + 1);
    if (strncmp(t, "struct ", 7) == 0)
        memmove(t, t + 7, strlen(t + 7) + 1);
    trim_str(t);
    strncpy(base, t, sizeof(base) - 1);
    base[sizeof(base) - 1] = '\0';

    out->struct_idx = -1;
    if (isptr) {
        if (strcmp(base, "char") == 0) {
            out->kind = LT_STR;
            snprintf(out->tname, sizeof(out->tname), "const char *");
        } else {
            out->kind = LT_PTR;
            snprintf(out->tname, sizeof(out->tname), "void *");
        }
        return 0;
    }

    if (strcmp(base, "void") == 0)
        out->kind = LT_VOID;
    else if (strcmp(base, "double") == 0)
        out->kind = LT_DOUBLE;
    else if (strcmp(base, "float") == 0)
        out->kind = LT_FLOAT;
    else if (strcmp(base, "int") == 0 || strcmp(base, "signed") == 0 ||
             strcmp(base, "signed int") == 0)
        out->kind = LT_INT;
    else if (strcmp(base, "unsigned") == 0 || strcmp(base, "unsigned int") == 0)
        out->kind = LT_UINT;
    else if (strcmp(base, "long") == 0)
        out->kind = LT_LONG;
    else if (strcmp(base, "unsigned long") == 0)
        out->kind = LT_ULONG;
    else if (strcmp(base, "long long") == 0)
        out->kind = LT_LLONG;
    else if (strcmp(base, "unsigned long long") == 0)
        out->kind = LT_ULLONG;
    else if (strcmp(base, "char") == 0 || strcmp(base, "int8_t") == 0 ||
             strcmp(base, "uint8_t") == 0)
        out->kind = LT_CHAR;
    else {
        int si = find_struct(pa, base);
        if (si < 0)
            return -1;
        out->kind = LT_STRUCT;
        out->struct_idx = si;
    }
    snprintf(out->tname, sizeof(out->tname), "%.63s", base);
    return 0;
}

static int add_func(Parser *pa, const LunaFunc *f)
{
    if (pa->nfuncs == pa->capfuncs) {
        int cap = pa->capfuncs ? pa->capfuncs * 2 : 8;
        LunaFunc *nf = (LunaFunc *)realloc(pa->funcs, (size_t)cap * sizeof(*nf));
        if (nf == NULL)
            return -1;
        pa->funcs = nf;
        pa->capfuncs = cap;
    }
    pa->funcs[pa->nfuncs++] = *f;
    return 0;
}

static void free_funcs(LunaFunc *funcs, int n)
{
    int i;
    for (i = 0; i < n; i++)
        free(funcs[i].args);
    free(funcs);
}

static void free_parser(Parser *pa)
{
    int i;
    for (i = 0; i < pa->nstructs; i++)
        free(pa->structs[i].fields);
    free_funcs(pa->funcs, pa->nfuncs);
    free(pa->structs);
    memset(pa, 0, sizeof(*pa));
}

/* ------------------------------------------------------------------ */
/* Heuristic parsing                                                   */
/* ------------------------------------------------------------------ */

static void parse_typedef(const char **pp, Parser *pa)
{
    const char *p = *pp;
    char tok[64];
    int t;

    t = next_tok(&p, tok, sizeof(tok));
    if (t != 1 || strcmp(tok, "struct") != 0) {
        while ((t = next_tok(&p, tok, sizeof(tok))) != 0) {
            if (t == 2 && tok[0] == ';')
                break;
        }
        *pp = p;
        return;
    }

    t = next_tok(&p, tok, sizeof(tok));
    if (!(t == 2 && tok[0] == '{')) {
        *pp = p;
        return;
    }

    {
        char decl[512] = "";
        LunaField fields[32];
        int nf = 0;
        char name[64] = "";
        int stop = 0;

        while (!stop) {
            t = next_tok(&p, tok, sizeof(tok));
            if (t == 0)
                break;
            if (t == 2 && tok[0] == '}') {
                stop = 1;
            } else if (t == 2 && tok[0] == ';') {
                if (decl[0] && nf < 32) {
                    char *part = decl;
                    while (*part) {
                        char piece[256];
                        char *comma = strchr(part, ',');
                        if (comma)
                            *comma = '\0';
                        strncpy(piece, part, sizeof(piece) - 1);
                        piece[sizeof(piece) - 1] = '\0';
                        trim_str(piece);
                        if (piece[0]) {
                            char ftype[128], fname[64];
                            split_type_name(piece, ftype, sizeof(ftype),
                                            fname, sizeof(fname));
                            snprintf(fields[nf].name,
                                     sizeof(fields[nf].name), "%.63s", fname);
                            snprintf(fields[nf].type,
                                     sizeof(fields[nf].type), "%.63s", ftype);
                            nf++;
                        }
                        if (!comma)
                            break;
                        part = comma + 1;
                    }
                }
                decl[0] = '\0';
            } else {
                append_tok(decl, sizeof(decl), tok);
            }
        }

        t = next_tok(&p, tok, sizeof(tok));
        if (t == 1)
            snprintf(name, sizeof(name), "%s", tok);
        next_tok(&p, tok, sizeof(tok));
        if (name[0] && nf > 0)
            add_struct(pa, name, fields, nf);
    }

    *pp = p;
}

static int find_paren_end(const char *q, const char **after_paren)
{
    int depth = 1;
    char tok[64];
    for (;;) {
        int t = next_tok(&q, tok, sizeof(tok));
        if (t == 0)
            return -1;
        if (t == 2 && tok[0] == '(') {
            depth++;
        } else if (t == 2 && tok[0] == ')') {
            depth--;
            if (depth == 0) {
                *after_paren = q;
                return 0;
            }
        } else if (t == 2 && tok[0] == ';') {
            return -1;
        }
    }
}

static void parse_args(const char *start, const char *end, Parser *pa,
                       LunaFunc *f)
{
    char decl[512] = "";
    const char *p = start;
    char tok[64];

    while (p < end) {
        int t = next_tok(&p, tok, sizeof(tok));
        if (t == 0)
            break;
        if (t == 2 && tok[0] == ')')
            break;
        if (t == 2 && tok[0] == ',') {
            char ftype[128], fname[64];
            if (decl[0]) {
                split_type_name(decl, ftype, sizeof(ftype), fname,
                                sizeof(fname));
                if (ftype[0]) {
                    LunaType *na = (LunaType *)realloc(
                        f->args, (size_t)(f->argc + 1) * sizeof(*na));
                    if (na) {
                        f->args = na;
                        if (resolve_type(ftype, pa, &f->args[f->argc]) == 0)
                            f->argc++;
                    }
                }
            }
            decl[0] = '\0';
        } else {
            append_tok(decl, sizeof(decl), tok);
        }
    }
    if (decl[0]) {
        char ftype[128], fname[64];
        split_type_name(decl, ftype, sizeof(ftype), fname, sizeof(fname));
        if (ftype[0] && strcmp(ftype, "void") != 0) {
            LunaType *na = (LunaType *)realloc(
                f->args, (size_t)(f->argc + 1) * sizeof(*na));
            if (na) {
                f->args = na;
                if (resolve_type(ftype, pa, &f->args[f->argc]) == 0)
                    f->argc++;
            }
        }
    }
}

static void parse_source(const char *src, Parser *pa)
{
    const char *p = src;
    char cur[64], prev[64] = "", prev2[64] = "";

    for (;;) {
        int t = next_tok(&p, cur, sizeof(cur));
        if (t == 0)
            break;
        if (t == 3)
            continue;

        if (t == 1 && strcmp(cur, "typedef") == 0) {
            parse_typedef(&p, pa);
            pa->hist_n = 0;
            prev[0] = prev2[0] = '\0';
            continue;
        }

        push_hist(pa, cur);

        if (t == 2 && cur[0] == '(' && pa->hist_n >= 2) {
            const char *name = pa->hist[pa->hist_n - 2];
            const char *after_paren = NULL;
            const char *peek;

            if (!is_control_keyword(name) &&
                (is_scalar_keyword(prev2) ||
                 find_struct(pa, prev2) >= 0 || strcmp(prev2, "*") == 0)) {
                if (find_paren_end(p, &after_paren) == 0) {
                    char nxt[64];
                    peek = after_paren;
                    if (next_tok(&peek, nxt, sizeof(nxt)) == 2 &&
                        nxt[0] == '{') {
                        char rtype[256] = "";
                        int i, start = pa->hist_n - 3;
                        while (start - 1 >= 0 &&
                               strcmp(pa->hist[start - 1], ";") != 0 &&
                               strcmp(pa->hist[start - 1], "{") != 0 &&
                               strcmp(pa->hist[start - 1], "}") != 0 &&
                               strcmp(pa->hist[start - 1], ",") != 0 &&
                               strcmp(pa->hist[start - 1], "=") != 0)
                            start--;
                        for (i = start; i <= pa->hist_n - 3; i++)
                            append_tok(rtype, sizeof(rtype), pa->hist[i]);
                        trim_str(rtype);
                        if (rtype[0]) {
                            LunaFunc f;
                            memset(&f, 0, sizeof(f));
                            snprintf(f.name, sizeof(f.name), "%s", name);
                            if (resolve_type(rtype, pa, &f.ret) == 0) {
                                parse_args(p, after_paren, pa, &f);
                                add_func(pa, &f);
                            }
                        }
                        p = after_paren;
                        pa->hist_n = 0;
                        prev[0] = prev2[0] = '\0';
                        continue;
                    }
                }
            }
        }

        prev2[0] = '\0';
        strcpy(prev2, prev);
        strcpy(prev, cur);
    }
}

/* ------------------------------------------------------------------ */
/* C shim generation                                                   */
/* ------------------------------------------------------------------ */

static const char *ctype_of(const LunaType *t)
{
    switch (t->kind) {
    case LT_DOUBLE: return "double";
    case LT_FLOAT:  return "float";
    case LT_INT:    return "int";
    case LT_UINT:   return "unsigned int";
    case LT_LONG:   return "long";
    case LT_ULONG:  return "unsigned long";
    case LT_LLONG:  return "long long";
    case LT_ULLONG: return "unsigned long long";
    case LT_CHAR:   return "char";
    case LT_STR:    return "const char *";
    case LT_PTR:    return "void *";
    case LT_STRUCT: return t->tname;
    case LT_VOID:
    default:        return "void";
    }
}

static void gen_shim(SB *out, const char *user_src, const Parser *pa)
{
    int i;

    sb_append(out, "/* generated by luna.compile — do not edit */\n");
    sb_append(out, "#include <stdint.h>\n#include <string.h>\n"
                   "#include <stdlib.h>\n#include <stdio.h>\n\n");
#if defined(_WIN32)
    sb_append(out, "#define LUNA_EXPORT __declspec(dllexport)\n\n");
#else
    sb_append(out, "#define LUNA_EXPORT __attribute__((visibility(\"default\")))\n\n");
#endif

    sb_append(out, "/* ---- user source ---- */\n");
    sb_append(out, user_src);
    sb_append(out, "\n/* ---- marshalling shims ---- */\n");

    for (i = 0; i < pa->nfuncs; i++) {
        const LunaFunc *f = &pa->funcs[i];
        int j;

        sb_printf(out,
                  "\nLUNA_EXPORT void luna_inline_entry_%d(double *args, "
                  "double *out)\n{\n",
                  i);
        sb_append(out, "    double *p = args;\n");

        for (j = 0; j < f->argc; j++) {
            const LunaType *a = &f->args[j];
            char vname[32];
            snprintf(vname, sizeof(vname), "a%d", j);
            if (a->kind == LT_STRUCT) {
                int k;
                sb_printf(out, "    %s %s;\n", a->tname, vname);
                for (k = 0; k < pa->structs[a->struct_idx].field_count; k++) {
                    const LunaField *fld =
                        &pa->structs[a->struct_idx].fields[k];
                    if (strstr(fld->type, "*") != NULL)
                        sb_printf(out, "    %s.%s = (%s)(uintptr_t)(*p++);\n",
                                  vname, fld->name, fld->type);
                    else
                        sb_printf(out, "    %s.%s = (%s)(*p++);\n", vname,
                                  fld->name, fld->type);
                }
            } else {
                sb_printf(out, "    %s %s;\n", ctype_of(a), vname);
                if (a->kind == LT_STR)
                    sb_printf(out,
                              "    %s = (const char *)(uintptr_t)(*p++);\n",
                              vname);
                else if (a->kind == LT_PTR)
                    sb_printf(out, "    %s = (void *)(uintptr_t)(*p++);\n",
                              vname);
                else
                    sb_printf(out, "    %s = (%s)(*p++);\n", vname,
                              ctype_of(a));
            }
        }

        if (f->ret.kind == LT_VOID) {
            sb_printf(out, "    %s(", f->name);
            for (j = 0; j < f->argc; j++)
                sb_printf(out, "%sa%d", j ? ", " : "", j);
            sb_append(out, ");\n");
        } else {
            sb_printf(out, "    %s _r = %s(", ctype_of(&f->ret), f->name);
            for (j = 0; j < f->argc; j++)
                sb_printf(out, "%sa%d", j ? ", " : "", j);
            sb_append(out, ");\n");
        }

        if (f->ret.kind == LT_STRUCT) {
            int k;
            for (k = 0; k < pa->structs[f->ret.struct_idx].field_count; k++) {
                const LunaField *fld =
                    &pa->structs[f->ret.struct_idx].fields[k];
                sb_printf(out, "    *out++ = (double)_r.%s;\n", fld->name);
            }
        } else if (f->ret.kind == LT_STR || f->ret.kind == LT_PTR) {
            sb_append(out, "    *out++ = (double)(uintptr_t)_r;\n");
        } else if (f->ret.kind != LT_VOID) {
            sb_append(out, "    *out++ = (double)_r;\n");
        }

        sb_append(out, "}\n");
    }
}

/* ------------------------------------------------------------------ */
/* Compile + load                                                      */
/* ------------------------------------------------------------------ */

typedef void (*LunaInlineFn)(double *args, double *out);

typedef struct {
    void *handle;
    char path[640];
    char cpath[640];
    LunaInlineFn *fns;
    int n;
    LunaFunc *funcs;
    int nfuncs;
    LunaStruct *structs;
    int nstructs;
} LunaInlineModule;

static int slots_of_type(const LunaInlineModule *m, const LunaType *t)
{
    if (t->kind == LT_VOID)
        return 0;
    if (t->kind == LT_STRUCT) {
        if (t->struct_idx >= 0 && t->struct_idx < m->nstructs)
            return m->structs[t->struct_idx].field_count;
        return 0;
    }
    return 1;
}

static int compile_and_load(const char *src, const Parser *pa, char *modpath,
                            size_t modsz, char *cpath, size_t cpathsz,
                            char *err, size_t errlen)
{
    char tmp[512];
    char cc[256];
    char cmd[4096];
    char capout[4096];
    unsigned long long h = luna_hash64_str(src);
    int rc;

    luna_tmp_dir(tmp, sizeof(tmp));
    snprintf(cpath, cpathsz, "%s/lunainline_%llx.c", tmp, h);

    {
        SB out;
        sb_init(&out);
        gen_shim(&out, src, pa);
        rc = luna_write_file(cpath, out.p ? out.p : "", out.len);
        sb_free(&out);
        if (rc != 0) {
            snprintf(err, errlen, "cannot write shim: %s", cpath);
            return -1;
        }
    }

#if defined(_WIN32)
    snprintf(modpath, modsz, "%s/lunainline_%llx.dll", tmp, h);
    if (luna_find_tool("gcc", cc, sizeof(cc)) != 0) {
        if (luna_find_tool("cc", cc, sizeof(cc)) != 0)
            strcpy(cc, "gcc");
    }
    snprintf(cmd, sizeof(cmd), "\"%s\" -O2 -shared \"%s\" -o \"%s\"", cc,
             cpath, modpath);
#else
    snprintf(modpath, modsz, "%s/lunainline_%llx.so", tmp, h);
    if (luna_find_tool("clang", cc, sizeof(cc)) != 0) {
        if (luna_find_tool("cc", cc, sizeof(cc)) != 0)
            strcpy(cc, "cc");
    }
    snprintf(cmd, sizeof(cmd), "\"%s\" -O2 -shared -fPIC \"%s\" -o \"%s\"",
             cc, cpath, modpath);
#endif

    rc = luna_run_cmd(cmd, capout, sizeof(capout));
    if (rc != 0) {
        snprintf(err, errlen, "compile failed (%d):\n%.1023s", rc,
                 capout[0] ? capout : "(no output)");
        if (!luna_inline_keep_files())
            remove(cpath);
        return -1;
    }
    return 0;
}

static void module_destroy(LunaInlineModule *m)
{
    int i;
    if (m == NULL)
        return;
#if defined(_WIN32)
    if (m->handle != NULL)
        FreeLibrary((HMODULE)m->handle);
#else
    if (m->handle != NULL)
        dlclose(m->handle);
#endif
    if (m->path[0] && !luna_inline_keep_files())
        remove(m->path);
    if (m->cpath[0] && !luna_inline_keep_files())
        remove(m->cpath);
    free(m->fns);
    for (i = 0; i < m->nfuncs; i++)
        free(m->funcs[i].args);
    free(m->funcs);
    for (i = 0; i < m->nstructs; i++)
        free(m->structs[i].fields);
    free(m->structs);
}

static int luna_inline_module_gc(lua_State *L)
{
    LunaInlineModule *m = (LunaInlineModule *)lua_touserdata(L, 1);
    module_destroy(m);
    return 0;
}

static void set_module_metatable(lua_State *L)
{
    if (luaL_newmetatable(L, "LunaBerry.Inline")) {
        lua_pushcfunction(L, luna_inline_module_gc);
        lua_setfield(L, -2, "__gc");
    }
    lua_setmetatable(L, -2);
}

/* ------------------------------------------------------------------ */
/* Lua marshalling                                                     */
/* ------------------------------------------------------------------ */

static int build_args_from_lua(lua_State *L, const LunaInlineModule *m,
                               const LunaFunc *f, int argstart,
                               double **pargs, char *err, size_t errlen)
{
    int total = 0;
    int j, slot = 0;
    double *a;

    for (j = 0; j < f->argc; j++)
        total += slots_of_type(m, &f->args[j]);
    a = (double *)malloc(sizeof(double) * (total ? (size_t)total : 1));
    if (a == NULL) {
        snprintf(err, errlen, "out of memory");
        return -1;
    }

    for (j = 0; j < f->argc; j++) {
        const LunaType *t = &f->args[j];
        if (t->kind == LT_STRUCT) {
            int k;
            const LunaStruct *st = &m->structs[t->struct_idx];
            for (k = 0; k < st->field_count; k++) {
                lua_getfield(L, argstart + j, st->fields[k].name);
                a[slot++] = lua_isnumber(L, -1) ? lua_tonumber(L, -1) : 0.0;
                lua_pop(L, 1);
            }
        } else if (t->kind == LT_STR) {
            const char *s = lua_tolstring(L, argstart + j, NULL);
            if (s == NULL) {
                snprintf(err, errlen, "argument %d of '%s' must be a string",
                         j + 1, f->name);
                free(a);
                return -1;
            }
            a[slot++] = (double)(uintptr_t)s;
        } else if (t->kind == LT_PTR) {
            a[slot++] = (double)(uintptr_t)lua_touserdata(L, argstart + j);
        } else {
            a[slot++] = lua_tonumber(L, argstart + j);
        }
    }

    *pargs = a;
    return 0;
}

static void push_result_to_lua(lua_State *L, const LunaInlineModule *m,
                               const LunaType *t, const double *out,
                               size_t *slot)
{
    if (t->kind == LT_STRUCT) {
        const LunaStruct *st = &m->structs[t->struct_idx];
        int k;
        lua_newtable(L);
        for (k = 0; k < st->field_count; k++) {
            lua_pushnumber(L, out[(*slot)++]);
            lua_setfield(L, -2, st->fields[k].name);
        }
    } else if (t->kind == LT_STR) {
        lua_pushstring(L, (const char *)(uintptr_t)out[(*slot)++]);
    } else if (t->kind == LT_PTR) {
        lua_pushlightuserdata(L, (void *)(uintptr_t)out[(*slot)++]);
    } else if (t->kind != LT_VOID) {
        lua_pushnumber(L, out[(*slot)++]);
    }
}

static int luna_inline_call(lua_State *L)
{
    LunaInlineModule *m =
        (LunaInlineModule *)lua_touserdata(L, lua_upvalueindex(1));
    int idx = (int)lua_tointeger(L, lua_upvalueindex(2));
    const LunaFunc *f;
    double *args = NULL;
    double *out = NULL;
    size_t nret = 0, slot = 0;
    char err[256] = "";
    int nresults;

    if (m == NULL || idx < 0 || idx >= m->nfuncs)
        return luaL_error(L, "luna: invalid native function");
    f = &m->funcs[idx];

    if (build_args_from_lua(L, m, f, 1, &args, err, sizeof(err)) != 0)
        return luaL_error(L, "luna: %s", err);

    nret = (size_t)slots_of_type(m, &f->ret);
    out = (double *)malloc(sizeof(double) * (nret ? nret : 1));
    if (out == NULL) {
        free(args);
        return luaL_error(L, "luna: out of memory");
    }
    m->fns[idx](args, out);

    if (f->ret.kind != LT_VOID) {
        push_result_to_lua(L, m, &f->ret, out, &slot);
        nresults = 1;
    } else {
        nresults = 0;
    }

    free(args);
    free(out);
    return nresults;
}

/* ------------------------------------------------------------------ */
/* Signature table (luna.compile(src, sigs))                           */
/* ------------------------------------------------------------------ */

static int build_funcs_from_sigs(lua_State *L, int idx, Parser *pa,
                                 char *err, size_t errlen)
{
    lua_pushnil(L);
    while (lua_next(L, idx) != 0) {
        const char *name = lua_tostring(L, -2);
        if (name != NULL && lua_istable(L, -1)) {
            int n = (int)lua_rawlen(L, -1);
            if (n >= 1) {
                LunaFunc f;
                memset(&f, 0, sizeof(f));
                snprintf(f.name, sizeof(f.name), "%s", name);
                lua_rawgeti(L, -1, 1);
                {
                    const char *rt = lua_tostring(L, -1);
                    if (rt == NULL || resolve_type(rt, pa, &f.ret) != 0) {
                        lua_pop(L, 2);
                        snprintf(err, errlen, "bad return type for '%s'",
                                 name);
                        return -1;
                    }
                }
                lua_pop(L, 1);
                {
                    int k;
                    for (k = 2; k <= n; k++) {
                        lua_rawgeti(L, -1, k);
                        {
                            const char *at = lua_tostring(L, -1);
                            LunaType *na = (LunaType *)realloc(
                                f.args,
                                (size_t)(f.argc + 1) * sizeof(*na));
                            if (na == NULL) {
                                lua_pop(L, 2);
                                free(f.args);
                                snprintf(err, errlen, "out of memory");
                                return -1;
                            }
                            f.args = na;
                            if (at == NULL ||
                                resolve_type(at, pa, &f.args[f.argc]) != 0) {
                                lua_pop(L, 2);
                                free(f.args);
                                snprintf(err, errlen,
                                         "bad arg type for '%s'", name);
                                return -1;
                            }
                            f.argc++;
                        }
                        lua_pop(L, 1);
                    }
                }
                add_func(pa, &f);
            }
        }
        lua_pop(L, 1);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* luna.compile / luna.asm                                             */
/* ------------------------------------------------------------------ */

static int luna_inline_build(lua_State *L, int is_asm)
{
    size_t srclen = 0;
    const char *src = luaL_checklstring(L, 1, &srclen);
    (void)srclen;
    Parser pa;
    LunaInlineModule *m;
    char modpath[640], cpath[640];
    char err[2048] = "";
    int i;

    (void)is_asm;
    memset(&pa, 0, sizeof(pa));

    parse_source(src, &pa);

    if (lua_istable(L, 2)) {
        /* explicit signatures override the heuristic function list */
        free_funcs(pa.funcs, pa.nfuncs);
        pa.funcs = NULL;
        pa.nfuncs = pa.capfuncs = 0;
        if (build_funcs_from_sigs(L, 2, &pa, err, sizeof(err)) != 0) {
            free_parser(&pa);
            return luaL_error(L, "luna: %s", err);
        }
    }

    if (pa.nfuncs == 0) {
        free_parser(&pa);
        return luaL_error(L,
                          "luna: no functions detected; pass an explicit "
                          "signature table as the 2nd argument");
    }

    if (compile_and_load(src, &pa, modpath, sizeof(modpath), cpath,
                         sizeof(cpath), err, sizeof(err)) != 0) {
        free_parser(&pa);
        return luaL_error(L, "luna: %s", err);
    }

    m = (LunaInlineModule *)lua_newuserdatauv(L, sizeof(*m), 0);
    memset(m, 0, sizeof(*m));
    snprintf(m->path, sizeof(m->path), "%s", modpath);
    snprintf(m->cpath, sizeof(m->cpath), "%s", cpath);

    m->nfuncs = pa.nfuncs;
    m->nstructs = pa.nstructs;
    m->funcs = pa.funcs;
    m->structs = pa.structs;
    pa.funcs = NULL;
    pa.structs = NULL;
    pa.nfuncs = pa.nstructs = 0;

    m->fns = (LunaInlineFn *)calloc((size_t)m->nfuncs, sizeof(*m->fns));
    if (m->fns == NULL) {
        module_destroy(m);
        return luaL_error(L, "luna: out of memory");
    }

#if defined(_WIN32)
    m->handle = (void *)LoadLibraryA(m->path);
#else
    m->handle = dlopen(m->path, RTLD_NOW | RTLD_LOCAL);
#endif
    if (m->handle == NULL) {
        module_destroy(m);
        return luaL_error(L, "luna: cannot load %s", m->path);
    }

    for (i = 0; i < m->nfuncs; i++) {
        char sym[80];
        LunaInlineFn fn = NULL;
        snprintf(sym, sizeof(sym), "luna_inline_entry_%d", i);
#if defined(_WIN32)
        {
            FARPROC p = GetProcAddress((HMODULE)m->handle, sym);
            if (p == NULL) {
                module_destroy(m);
                return luaL_error(L, "luna: symbol not found: %s", sym);
            }
            memcpy(&fn, &p, sizeof(fn));
        }
#else
        {
            void *p = dlsym(m->handle, sym);
            if (p == NULL) {
                module_destroy(m);
                return luaL_error(L, "luna: symbol not found: %s", sym);
            }
            memcpy(&fn, &p, sizeof(fn));
        }
#endif
        m->fns[i] = fn;
    }

    set_module_metatable(L);

    {
        lua_newtable(L);
        for (i = 0; i < m->nfuncs; i++) {
            lua_pushvalue(L, -2); /* module userdata */
            lua_pushinteger(L, i);
            lua_pushcclosure(L, luna_inline_call, 2);
            lua_setfield(L, -2, m->funcs[i].name);
        }
        lua_remove(L, -2);
    }

    return 1;
}

static int lua_luna_compile(lua_State *L) { return luna_inline_build(L, 0); }
static int lua_luna_asm(lua_State *L) { return luna_inline_build(L, 1); }

int luna_inline_open(lua_State *L)
{
    lua_newtable(L);
    lua_pushcfunction(L, lua_luna_compile);
    lua_setfield(L, -2, "compile");
    lua_pushcfunction(L, lua_luna_asm);
    lua_setfield(L, -2, "asm");
    lua_setglobal(L, "luna");
    return 0;
}
