/*
 * luna-opt.c — LunaBerry optimization ladder O0..O10.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "luna.h"
#include "luna-common.h"
#include "luna-opt.h"

typedef struct {
    const char *name;
    const char *flags;       /* CC/backend flags (gcc + clang compatible) */
    const char *llvm_flags;  /* clang-specific flags for the LLVM tier */
    const char *llvm_passes; /* new-PM `opt -passes=` pipeline (LLVM IR) */
    const char *summary;
    int strip;
    int dead_asm;
    int no_debug;
} OptLevel;

static const OptLevel levels[] = {
    /* 0  Raw         */
    { "Raw", "-O0 -g0", "-O0 -g", "default<O0>",
      "no optimization: compile is fast, run is slow", 0, 0, 1 },
    /* 1  Clean       */
    { "Clean", "-O1", "-O1", "default<O1>",
      "mem2reg + sroa + instcombine territory", 0, 0, 0 },
    /* 2  Solid       */
    { "Solid", "-O2", "-O2", "default<O2>", "standard -O2 (production "
      "default)", 0, 0, 0 },
    /* 3  Fast        */
    { "Fast", "-O3", "-O3", "default<O3>",
      "standard -O3 (vectorization, aggressive inlining)", 0, 0, 0 },
    /* 4  Mochi       */
    { "Mochi", "-O3 -funroll-loops", "-O3 -funroll-loops",
      "default<O3>,loop-unroll", "+ loop unrolling", 0, 0, 0 },
    /* 5  Mulch       */
    { "Mulch", "-O3 -funroll-loops -fomit-frame-pointer",
      "-O3 -funroll-loops -fomit-frame-pointer",
      "default<O3>,loop-unroll,loop-deletion", "+ frame-pointer omission",
      0, 0, 0 },
    /* 6  Akari       */
    { "Akari", "-O3 -funroll-loops -fomit-frame-pointer -fno-math-errno",
      "-O3 -funroll-loops -fomit-frame-pointer -fno-math-errno",
      "default<O3>,loop-unroll,loop-deletion,slp-vectorizer",
      "+ errno-free math", 0, 0, 0 },
    /* 7  Trace       */
    { "Trace",
      "-O3 -funroll-loops -fomit-frame-pointer -fno-math-errno "
      "-fvisibility=hidden",
      "-O3 -funroll-loops -fomit-frame-pointer -fno-math-errno "
      "-fvisibility=hidden -fvisibility-inlines-hidden",
      "default<O3>,loop-unroll,loop-deletion,slp-vectorizer,instcombine",
      "+ hidden visibility", 0, 0, 0 },
    /* 8  Silicon     */
    { "Silicon",
      "-O3 -funroll-loops -fomit-frame-pointer -fno-math-errno "
      "-fvisibility=hidden -mtune=native",
      "-O3 -funroll-loops -fomit-frame-pointer -fno-math-errno "
      "-fvisibility=hidden -fvisibility-inlines-hidden "
      "-march=native -mtune=native",
      "default<O3>,loop-unroll,loop-deletion,slp-vectorizer,instcombine,"
      "inline",
      "+ native tuning (host-specific ISA)", 0, 0, 0 },
    /* 9  Nirvana     -- exact Lua FP semantics kept: no -ffast-math */
    { "Nirvana",
      "-O3 -funroll-loops -fomit-frame-pointer -fno-math-errno "
      "-fvisibility=hidden -mtune=native -fno-trapping-math "
      "-fdevirtualize-speculatively -fno-semantic-interposition",
      "-O3 -funroll-loops -fomit-frame-pointer -fno-math-errno "
      "-fvisibility=hidden -fvisibility-inlines-hidden "
      "-march=native -mtune=native -fno-trapping-math "
      "-fno-semantic-interposition",
      "default<O3>,loop-unroll,loop-deletion,slp-vectorizer,instcombine,"
      "inline,gvn,licm,tailcallelim",
      "+ IPA: no FP traps, speculative devirt, interposition off "
      "(NaN/Inf/signed-zero semantics preserved)", 0, 0, 0 },
    /* 10 The Stripper */
    { "The Stripper",
      "-O3 -funroll-loops -fomit-frame-pointer -fno-math-errno "
      "-fvisibility=hidden -mtune=native -fno-trapping-math "
      "-fdevirtualize-speculatively -fno-semantic-interposition "
      "-g0 -fno-asynchronous-unwind-tables -fno-ident",
      "-O3 -funroll-loops -fomit-frame-pointer -fno-math-errno "
      "-fvisibility=hidden -fvisibility-inlines-hidden "
      "-march=native -mtune=native -fno-trapping-math "
      "-fno-semantic-interposition "
      "-g0 -fno-asynchronous-unwind-tables -fno-ident",
      "default<O3>,loop-unroll,loop-deletion,slp-vectorizer,instcombine,"
      "inline,gvn,licm,tailcallelim,strip-dead-prototypes,globaldce,globalopt",
      "+ no debug info, no unwind tables, implicit strip, dead assembly",
      1, 1, 1 }
};

#define LEVEL_COUNT ((int)(sizeof(levels) / sizeof(levels[0])))

int luna_opt_clamp(int level)
{
    if (level < LUNA_OPT_MIN)
        return LUNA_OPT_MIN;
    if (level > LUNA_OPT_MAX)
        return LUNA_OPT_MAX;
    return level;
}

const char *luna_opt_name(int level)
{
    return levels[luna_opt_clamp(level)].name;
}

const char *luna_opt_flags(int level)
{
    return levels[luna_opt_clamp(level)].flags;
}

const char *luna_opt_llvm_flags(int level)
{
    return levels[luna_opt_clamp(level)].llvm_flags;
}

const char *luna_opt_llvm_passes(int level)
{
    return levels[luna_opt_clamp(level)].llvm_passes;
}

const char *luna_opt_summary(int level)
{
    return levels[luna_opt_clamp(level)].summary;
}

int luna_opt_implies_strip(int level)
{
    return levels[luna_opt_clamp(level)].strip;
}

int luna_opt_implies_dead_asm(int level)
{
    return levels[luna_opt_clamp(level)].dead_asm;
}

int luna_opt_implies_no_debug(int level)
{
    return levels[luna_opt_clamp(level)].no_debug;
}

void luna_opt_warn_redundant(int level, int strip_given, int debug_given)
{
    level = luna_opt_clamp(level);
    if (strip_given && levels[level].strip)
        fprintf(stderr,
                "luna: warning: '--strip' is redundant at -O%d (stripping is "
                "implicit); flag ignored\n",
                level);
    if (debug_given && levels[level].no_debug)
        fprintf(stderr,
                "luna: warning: '-g' is redundant at -O%d (no debug info is "
                "emitted); flag ignored\n",
                level);
}

/* ------------------------------------------------------------------ */
/* Dead assembly                                                       */
/* ------------------------------------------------------------------ */

static int is_ident_start(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' ||
           c == '$' || c == '.';
}

static int is_ident_char(char c)
{
    return is_ident_start(c) || (c >= '0' && c <= '9');
}

typedef struct {
    char old[128];
    char neu[64];
} Rename;

static int is_exported(const char *label, char **exports, int nexports)
{
    int i;
    for (i = 0; i < nexports; i++) {
        if (strcmp(exports[i], label) == 0)
            return 1;
    }
    return 0;
}

/* Extract the symbol named by a directive such as ".globl main",
 * ".global  main" or ".def main;".  The directive may be followed by any
 * amount of whitespace (spaces or tabs), so a plain strncmp against
 * ".globl " would miss ".globl\tmain".  Returns 1 when a symbol was
 * found, 0 otherwise. */
static int directive_symbol(const char *q, const char *directive, char *out,
                            size_t outsz)
{
    size_t n = strlen(directive);
    const char *s;
    size_t k = 0;

    if (strncmp(q, directive, n) != 0)
        return 0;
    s = q + n;
    while (*s == ' ' || *s == '\t')
        s++;
    if (*s == '\0' || *s == '\r' || *s == '\n' || *s == ';')
        return 0;
    while (*s && (is_ident_char(*s) || *s == '@') && k + 1 < outsz)
        out[k++] = *s++;
    out[k] = '\0';
    return k > 0;
}

static void add_export(char ***exports, int *nexports, const char *name)
{
    char *copy = (char *)malloc(strlen(name) + 1);
    if (copy == NULL)
        return;
    strcpy(copy, name);
    *exports = (char **)realloc(*exports,
                                (size_t)(*nexports + 1) * sizeof(char *));
    if (*exports != NULL)
        (*exports)[(*nexports)++] = copy;
    else
        free(copy);
}

static int find_rename(const Rename *renames, int n, const char *old)
{
    int i;
    for (i = 0; i < n; i++) {
        if (strcmp(renames[i].old, old) == 0)
            return i;
    }
    return -1;
}

/* Replace every occurrence of a renamed label token in `line`. */
static void rewrite_references(char *line, size_t linelen, const Rename *renames,
                               int nrenames)
{
    char out[4096];
    size_t oi = 0;
    const char *p = line;

    out[0] = '\0';
    while (*p && oi + 1 < sizeof(out)) {
        if (is_ident_start(*p)) {
            char tok[128];
            size_t k = 0;
            const char *s = p;
            while (*s && is_ident_char(*s) && k + 1 < sizeof(tok))
                tok[k++] = *s++;
            tok[k] = '\0';
            {
                int ri = find_rename(renames, nrenames, tok);
                if (ri >= 0) {
                    size_t rl = strlen(renames[ri].neu);
                    if (oi + rl < sizeof(out)) {
                        memcpy(out + oi, renames[ri].neu, rl);
                        oi += rl;
                    }
                } else {
                    if (oi + k < sizeof(out)) {
                        memcpy(out + oi, tok, k);
                        oi += k;
                    }
                }
            }
            p = s;
        } else {
            out[oi++] = *p++;
            out[oi] = '\0';
        }
    }
    out[oi] = '\0';
    {
        size_t n = strlen(out);
        if (n > linelen - 1)
            n = linelen - 1;
        memcpy(line, out, n);
        line[n] = '\0';
    }
}

int luna_opt_dead_asm(const char *in_path, const char *out_path)
{
    size_t len = 0;
    char *src = luna_read_file(in_path, &len);
    char **exports = NULL;
    int nexports = 0;
    Rename *renames = NULL;
    int nrenames = 0;
    int counter = 0;
    FILE *out;
    const char *p;

    if (src == NULL)
        return -1;

    /* Pass 1a: collect exported symbol names. */
    p = src;
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t linelen = eol ? (size_t)(eol - p) : strlen(p);
        char line[1024];
        if (linelen >= sizeof(line))
            linelen = sizeof(line) - 1;
        memcpy(line, p, linelen);
        line[linelen] = '\0';
        {
            const char *q = line;
            char name[128];
            while (*q == ' ' || *q == '\t')
                q++;
            if (directive_symbol(q, ".globl", name, sizeof(name)) ||
                directive_symbol(q, ".global", name, sizeof(name)) ||
                directive_symbol(q, ".def", name, sizeof(name))) {
                if (name[0] != '\0')
                    add_export(&exports, &nexports, name);
            }
        }
        if (eol == NULL)
            break;
        p = eol + 1;
    }

    /* Pass 1b: collect non-exported labels to anonymize. */
    p = src;
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t linelen = eol ? (size_t)(eol - p) : strlen(p);
        char line[1024];
        if (linelen >= sizeof(line))
            linelen = sizeof(line) - 1;
        memcpy(line, p, linelen);
        line[linelen] = '\0';
        {
            const char *q = line;
            while (*q == ' ' || *q == '\t')
                q++;
            if (q[0] != '.' && is_ident_start(q[0])) {
                char name[128];
                size_t k = 0;
                const char *s = q;
                while (*s && is_ident_char(*s) && k + 1 < sizeof(name))
                    name[k++] = *s++;
                name[k] = '\0';
                if (k > 0 && *s == ':' && !is_exported(name, exports, nexports)) {
                    Rename *nr = (Rename *)realloc(
                        renames, (size_t)(nrenames + 1) * sizeof(*nr));
                    if (nr != NULL) {
                        renames = nr;
                        snprintf(renames[nrenames].old,
                                 sizeof(renames[nrenames].old), "%s", name);
                        snprintf(renames[nrenames].neu,
                                 sizeof(renames[nrenames].neu), ".Lanon%d",
                                 counter++);
                        nrenames++;
                    }
                }
            }
        }
        if (eol == NULL)
            break;
        p = eol + 1;
    }

    out = fopen(out_path, "wb");
    if (out == NULL) {
        free(src);
        free(exports);
        free(renames);
        return -1;
    }

    /* Pass 2: write the filtered assembly. */
    p = src;
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t linelen = eol ? (size_t)(eol - p) : strlen(p);
        char line[4096];
        const char *q;

        if (linelen >= sizeof(line))
            linelen = sizeof(line) - 1;
        memcpy(line, p, linelen);
        line[linelen] = '\0';

        q = line;
        while (*q == ' ' || *q == '\t')
            q++;

        if (q[0] == '#' || strncmp(q, ".file", 5) == 0 ||
            strncmp(q, ".ident", 6) == 0 || strncmp(q, ".comment", 8) == 0 ||
            strncmp(q, ".loc ", 5) == 0 || strncmp(q, ".cv_", 4) == 0 ||
            strncmp(q, ".debug_", 7) == 0 || strncmp(q, ".addrsig", 8) == 0 ||
            strncmp(q, ".addrsig_sym", 12) == 0) {
            if (eol == NULL)
                break;
            p = eol + 1;
            continue;
        }

        /* Rename label definitions. */
        if (q[0] != '.' && is_ident_start(q[0])) {
            char name[128];
            size_t k = 0;
            const char *s = q;
            while (*s && is_ident_char(*s) && k + 1 < sizeof(name))
                name[k++] = *s++;
            name[k] = '\0';
            if (k > 0 && *s == ':') {
                int ri = find_rename(renames, nrenames, name);
                if (ri >= 0) {
                    fprintf(out, "%s:%s\n", renames[ri].neu, s + 1);
                    if (eol == NULL)
                        break;
                    p = eol + 1;
                    continue;
                }
            }
        }

        rewrite_references(line, sizeof(line), renames, nrenames);
        fprintf(out, "%s\n", line);

        if (eol == NULL)
            break;
        p = eol + 1;
    }

    fclose(out);
    {
        int i;
        for (i = 0; i < nexports; i++)
            free(exports[i]);
        free(exports);
        free(renames);
    }
    free(src);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Binary stripping                                                    */
/* ------------------------------------------------------------------ */

int luna_opt_strip_binary(const char *path)
{
    char tool[512];
    char cmd[2048];
    int rc;

    if (luna_find_tool("strip", tool, sizeof(tool)) == 0) {
        snprintf(cmd, sizeof(cmd), "\"%s\" --strip-all \"%s\"", tool, path);
        rc = luna_run_cmd(cmd, NULL, 0);
        if (rc == 0)
            return 0;
    }
    if (luna_find_tool("llvm-strip", tool, sizeof(tool)) == 0) {
        snprintf(cmd, sizeof(cmd), "\"%s\" --strip-all \"%s\"", tool, path);
        rc = luna_run_cmd(cmd, NULL, 0);
        if (rc == 0)
            return 0;
    }
    if (luna_find_tool("objcopy", tool, sizeof(tool)) == 0) {
        snprintf(cmd, sizeof(cmd), "\"%s\" --strip-all \"%s\"", tool, path);
        rc = luna_run_cmd(cmd, NULL, 0);
        if (rc == 0)
            return 0;
    }
    return -1;
}
