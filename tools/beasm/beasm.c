/* ═══════════════════════════════════════════════════════════════════════════
 * beasm.c — Berry Assembler (beasm)
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * Self-hosting assembler untuk UBerryNix.  Membaca source .basm dan
 * menghasilkan biner Berry native sesuai GRAMMAR AKTUAL kernel:
 *
 *   .befb — Berry Executive File Binary  (LBRT programs)   magic BEFB164
 *   .bex  — Berry Executable eXtended    (nano kernel)     magic BEXI64
 *   .bdl  — Berry Dynamic Library        (shared library)  magic BDLI64
 *   .lbrct/.hbrct — Runtime Components   (bundle)          magic RTCFI64
 *   .bsp  — Berry System Package         (scheduler)       magic BSP164
 *   .bpp  — Berry Panel Program          (GUI app DE/WM/DS) magic BPP164
 *   .bo   — Berry Object                 (intermediate)    magic BOBJ
 *
 * Grammar (sumber kebenaran: kernel/include/{bdl,befb,bex,rtcf,bsp}_format.h
 * dan tools/{bdl,befb}_packer.js):
 *   - magic 8 byte: "BDLI64\0\0" dst.
 *   - header bervariasi: BEX=16 B, BEFB/BDL=36 B (dengan BTP token 20 B),
 *     RTCF=12 B, BSP=24 B, BO=16 B
 *   - tabel seksi 16 B/entri: { type u32, offset u32, size u32, vaddr u32 }
 *   - entri .exports 12 B: { fn_hash u32, entry_off u32, flags u16, rsv u16 }
 *     terminator {0,0,0,0}; entry_off = RVA dari .text base (TIDAK di-patch)
 *   - entri CherryReva 8 B: { offset u32, type u8, width u8, rsv u16 }
 *     offset = posisi patch RELATIF .text base (vaddr seksi + off seksi)
 *
 * Assembler: dua-pass untuk x86_64 (REX, ModRM/SIB, RIP-relative),
 * label + direktif db/dw/dd/dq/equ/align/res*, BERRY_EXPORT dengan
 * BERRY_IMPORT (eksternal yang diresolusi loader OS, mis. runtime MBJS)
 * FNV-1a compile-time, dan relokasi absolut (CherryReva) untuk simbol.
 *
 * Build host:  cc -O2 -o beasm beasm.c
 * Usage:
 *   beasm <input.basm> -o <out> --format <befb|bex|bdl|lbrct|bsp|bpp|bo>
 *         [--token BEFB_00001] [--entry sym] [--version 1|2]
 *         [--win-w px] [--win-h px]   (bpp: ukuran jendela default)
 *
 * ═══════════════════════════════════════════════════════════════════════════ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * FNV-1a (identik kernel bdl_fnv1a / tools/*.js)
 * ═══════════════════════════════════════════════════════════════════════════ */
uint32_t fnv1a_32(const char *text) {
    uint32_t h = 0x811c9dc5u;
    while (*text) {
        h ^= (uint8_t)*text++;
        h *= 0x01000193u;
    }
    return h;
}

uint64_t fnv1a_64(const char *text) {
    uint64_t h = 0xcbf29ce484222325ULL;
    while (*text) {
        h ^= (uint8_t)*text++;
        h *= 0x100000001b3ULL;
    }
    return h;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Section types (identik kernel/include/*_format.h)
 * ═══════════════════════════════════════════════════════════════════════════ */
enum {
    SECT_TEXT       = 0,
    SECT_RODATA     = 1,
    SECT_DATA       = 2,
    SECT_BSS        = 3,
    SECT_CHERRYREVA = 4,
    SECT_TRUST      = 5,
    SECT_EXPORTS    = 6,
    SECT_HOOK       = 7,
    SECT_MANIFEST   = 8,
    SECT_EXPORTS_BEFB = 9,   /* BEFB/BEX: exports = 9 (6 = .icon0) */
};

#define BDL_HEADER_SIZE  36
#define BEFB_HEADER_SIZE 36
#define BEX_HEADER_SIZE  16
#define RTCF_HEADER_SIZE 12
#define BSP_HEADER_SIZE  24
#define BPP_HEADER_SIZE  24
#define BO_HEADER_SIZE   16
#define SECT_ENTRY_SIZE  16
#define EXPORT_ENTRY_SIZE 12
#define CR_ENTRY_SIZE    8

#define BDL_EXPORT_FN     0x0001
#define BDL_EXPORT_DATA   0x0002

/* ═══════════════════════════════════════════════════════════════════════════
 * Growable byte buffer
 * ═══════════════════════════════════════════════════════════════════════════ */
typedef struct {
    uint8_t *data;
    uint32_t len;
    uint32_t cap;
} buf_t;

static void buf_init(buf_t *b) { b->data = 0; b->len = 0; b->cap = 0; }
static void buf_free(buf_t *b) { free(b->data); b->data = 0; b->len = b->cap = 0; }
static void buf_reserve(buf_t *b, uint32_t extra) {
    if (b->len + extra <= b->cap) return;
    uint32_t nc = b->cap ? b->cap : 64;
    while (nc < b->len + extra) nc *= 2;
    b->data = (uint8_t*)realloc(b->data, nc);
    b->cap = nc;
}
static void buf_byte(buf_t *b, uint8_t v) { buf_reserve(b, 1); b->data[b->len++] = v; }
static void buf_u16(buf_t *b, uint16_t v) { buf_byte(b, v & 0xFF); buf_byte(b, (v >> 8) & 0xFF); }
static void buf_u32(buf_t *b, uint32_t v) {
    buf_byte(b, v & 0xFF); buf_byte(b, (v >> 8) & 0xFF);
    buf_byte(b, (v >> 16) & 0xFF); buf_byte(b, (v >> 24) & 0xFF);
}
static void buf_u64(buf_t *b, uint64_t v) { buf_u32(b, (uint32_t)v); buf_u32(b, (uint32_t)(v >> 32)); }
static void buf_bytes(buf_t *b, const uint8_t *p, uint32_t n) {
    buf_reserve(b, n);
    memcpy(b->data + b->len, p, n);
    b->len += n;
}
static void buf_pad(buf_t *b, uint32_t align) { while (b->len % align) buf_byte(b, 0); }

/* ═══════════════════════════════════════════════════════════════════════════
 * Symbols & sections
 * ═══════════════════════════════════════════════════════════════════════════ */
#define MAX_SYMS    4096
#define MAX_SECTS   16
#define MAX_EXPORTS 512
#define MAX_RELOCS  4096
#define MAX_LINE    4096

typedef struct {
    char     name[64];
    int      defined;
    int      is_const;
    uint64_t const_val;
    int      section;
    uint32_t off;
    int      is_extern;   /* BERRY_IMPORT: resolved by the Berry OS loader */
} sym_t;

typedef struct {
    char     name[32];
    int      type;
    buf_t    bytes;
    uint32_t size;
    uint32_t vaddr;
    int      align;
    int      seen;
} sect_t;

typedef struct {
    uint32_t hash;
    uint32_t entry_off;
    uint16_t flags;
} export_t;

typedef struct {
    uint32_t off;        /* posisi patch RELATIF .text base */
    uint8_t  type;       /* 0 = ABS */
    uint8_t  width;      /* 4 atau 8 */
    uint16_t rsv;
} reloc_t;

static sym_t    g_syms[MAX_SYMS];
static int      g_sym_count = 0;
static sect_t   g_sects[MAX_SECTS];
static int      g_sect_count = 0;
static export_t g_exports[MAX_EXPORTS];
static int      g_export_count = 0;
static reloc_t  g_relocs[MAX_RELOCS];
static int      g_reloc_count = 0;

static int g_cur_sect = -1;
static int g_pass = 0;
static int g_rep_prefix = 0;   /* 0xF2 / 0xF3 untuk string ops */
static int g_lock_prefix = 0;  /* 0xF0 sebelum instruksi berikutnya */

static const char *g_out_format = "befb";
static char g_token[64] = "BEFB_00001";
static char g_entry_sym[64] = "_start";
static int  g_version = 2;
static uint32_t g_bpp_win_w = 0;   /* .bpp: default window width  (0 = DE) */
static uint32_t g_bpp_win_h = 0;   /* .bpp: default window height (0 = DE) */
static char g_out_path[1024] = "out.befb";

/* errors */
static int g_error = 0;
static int g_line_no = 0;
static const char *g_file = "<input>";

/* Multi-architecture target selection */
enum {
    ARCH_X86_64  = 0,
    ARCH_X86_32  = 1,
    ARCH_X86_16  = 2,
    ARCH_ARM32   = 3,
    ARCH_ARM64   = 4,
    ARCH_RISCV32 = 5,
    ARCH_RISCV64 = 6,
    ARCH_RISCV128= 7,
};
static int g_arch = ARCH_X86_64;
static int g_bits = 64;



/* ═══════════════════════════════════════════════════════════════════════════
 * TRACK AI (.bcm — Berry Compiler Model) — hibrid dengan -O0..-O10 klasik.
 * Tanpa -EnAI, semua ini DIABAIKAN total (jalur deterministik 100% utuh).
 * ═══════════════════════════════════════════════════════════════════════════ */
#define BCM_MAGIC    0x4D4342u   /* 'BCM' */
#define BCM_VERSION  1
#define BCM_MAX_PATTERNS 8192

typedef struct {
    uint32_t ir_hash;
    uint16_t transform_id;
    uint8_t  confidence;
    uint8_t  min_oai;
    int8_t   weight;
} bcm_pattern_t;

typedef struct {
    uint32_t arch_id;
    uint32_t count;
    bcm_pattern_t pats[BCM_MAX_PATTERNS];
    int      loaded;
} bcm_model_t;

static bcm_model_t g_bcm;
static int  g_enai = 0;          /* -EnAI: master switch track AI */
static int  g_oai_level = 0;     /* -OAI0..-OAI10 */
static char g_bcm_path[1024] = "";
static uint32_t g_ai_hits = 0;
static uint32_t g_ai_misses = 0;
static uint32_t g_ai_seed = 0x9E3779B9;
static int  g_ai_stats = 0;

/* FNV-1a 32 (identik kernel) — dipakai hash pola IR */
static uint32_t bcm_fnv1a32(const char *s) {
    uint32_t h = 0x811c9dc5u;
    while (*s) { h ^= (uint8_t)*s++; h *= 0x01000193u; }
    return h;
}

/* muat model .bcm dari file (format: lihat tools/bcm_format.js) */
static int bcm_load_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    uint8_t hdr[36];
    if (fread(hdr, 1, 36, f) != 36) { fclose(f); return -2; }
    uint32_t magic = (uint32_t)hdr[0] | ((uint32_t)hdr[1] << 8) | ((uint32_t)hdr[2] << 16);
    uint32_t ver  = (uint32_t)hdr[4] | ((uint32_t)hdr[5] << 8) | ((uint32_t)hdr[6] << 16) | ((uint32_t)hdr[7] << 24);
    if (magic != BCM_MAGIC || ver != BCM_VERSION) { fclose(f); return -3; }
    uint32_t arch_id  = (uint32_t)hdr[8] | ((uint32_t)hdr[9] << 8) | ((uint32_t)hdr[10] << 16) | ((uint32_t)hdr[11] << 24);
    uint32_t n        = (uint32_t)hdr[12] | ((uint32_t)hdr[13] << 8) | ((uint32_t)hdr[14] << 16) | ((uint32_t)hdr[15] << 24);
    uint32_t pat_off  = (uint32_t)hdr[16] | ((uint32_t)hdr[17] << 8) | ((uint32_t)hdr[18] << 16) | ((uint32_t)hdr[19] << 24);
    uint32_t w_off    = (uint32_t)hdr[20] | ((uint32_t)hdr[21] << 8) | ((uint32_t)hdr[22] << 16) | ((uint32_t)hdr[23] << 24);
    uint32_t flags    = (uint32_t)hdr[32] | ((uint32_t)hdr[33] << 8) | ((uint32_t)hdr[34] << 16) | ((uint32_t)hdr[35] << 24);
    int wsize = (flags & 2) ? 2 : 1;
    if (n > BCM_MAX_PATTERNS) { fclose(f); return -4; }
    if (fseek(f, (long)pat_off, SEEK_SET) != 0) { fclose(f); return -5; }
    g_bcm.arch_id = arch_id;
    g_bcm.count = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t e[8];
        if (fread(e, 1, 8, f) != 8) break;
        g_bcm.pats[g_bcm.count].ir_hash    = (uint32_t)e[0] | ((uint32_t)e[1] << 8) | ((uint32_t)e[2] << 16) | ((uint32_t)e[3] << 24);
        g_bcm.pats[g_bcm.count].transform_id = (uint16_t)(e[4] | (e[5] << 8));
        g_bcm.pats[g_bcm.count].confidence  = e[6];
        g_bcm.pats[g_bcm.count].min_oai     = e[7];
        g_bcm.pats[g_bcm.count].weight      = 0;
        g_bcm.count++;
    }
    /* bobot int8 (int16 bila flags bit1) */
    if (fseek(f, (long)w_off, SEEK_SET) == 0) {
        for (uint32_t i = 0; i < g_bcm.count && i < n; i++) {
            if (wsize == 2) {
                uint8_t b[2];
                if (fread(b, 1, 2, f) != 2) break;
                g_bcm.pats[i].weight = (int8_t)((int16_t)(b[0] | (b[1] << 8)) >> 8);
            } else {
                int c = fgetc(f);
                if (c == EOF) break;
                g_bcm.pats[i].weight = (int8_t)c;
            }
        }
    }
    fclose(f);
    g_bcm.loaded = 1;
    return 0;
}

/* query pola — linear scan (pola sedikit, cache-hot) */
static const bcm_pattern_t *bcm_find(uint32_t hash) {
    if (!g_bcm.loaded) return 0;
    for (uint32_t i = 0; i < g_bcm.count; i++)
        if (g_bcm.pats[i].ir_hash == hash) return &g_bcm.pats[i];
    return 0;
}

static uint32_t bcm_rand(void) {
    g_ai_seed ^= g_ai_seed << 13; g_ai_seed ^= g_ai_seed >> 17; g_ai_seed ^= g_ai_seed << 5;
    return g_ai_seed;
}

/* threshold confidence untuk level -OAI */
static uint8_t bcm_threshold(int oai) {
    if (oai >= 10) return 60;
    if (oai >= 8)  return 90;
    if (oai >= 6)  return 120;
    if (oai >= 4)  return 150;
    if (oai >= 2)  return 190;
    return 220;   /* OAI0-1: hanya rekomendasi paling yakin */
}

static void err(const char *fmt, ...) {
    if (g_error) return;
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "beasm: %s:%d: ", g_file, g_line_no);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    g_error = 1;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Section helpers
 * ═══════════════════════════════════════════════════════════════════════════ */
static int sect_find_or_add(const char *name) {
    for (int i = 0; i < g_sect_count; i++)
        if (strcmp(g_sects[i].name, name) == 0) return i;
    if (g_sect_count >= MAX_SECTS) { err("too many sections"); return -1; }
    int i = g_sect_count++;
    sect_t *s = &g_sects[i];
    strncpy(s->name, name, sizeof(s->name) - 1);
    s->type = SECT_RODATA; s->size = 0; s->vaddr = 0; s->align = 16; s->seen = 0;
    buf_init(&s->bytes);
    if (strcmp(name, ".text") == 0) s->type = SECT_TEXT;
    else if (strcmp(name, ".rodata") == 0) s->type = SECT_RODATA;
    else if (strcmp(name, ".data") == 0) s->type = SECT_DATA;
    else if (strcmp(name, ".bss") == 0) s->type = SECT_BSS;
    else if (strcmp(name, ".cherryreva") == 0) s->type = SECT_CHERRYREVA;
    else if (strcmp(name, ".trust") == 0) s->type = SECT_TRUST;
    else if (strcmp(name, ".exports") == 0) s->type = SECT_EXPORTS;
    else if (strcmp(name, ".hook") == 0) s->type = SECT_HOOK;
    else if (strcmp(name, ".manifest") == 0) s->type = SECT_MANIFEST;
    return i;
}

static int sect_type_to_index(int type) {
    for (int i = 0; i < g_sect_count; i++)
        if (g_sects[i].type == type) return i;
    return -1;
}

/* alamat simbol (vaddr penuh); -2 = belum didefinisikan, -1 = forward ref,
 * -3 = BERRY_IMPORT (external, resolved by the OS loader) */
static int64_t sym_addr(const char *name) {
    for (int i = 0; i < g_sym_count; i++) {
        if (strcmp(g_syms[i].name, name) == 0) {
            if (g_syms[i].is_extern) return -3;
            if (g_syms[i].is_const) return (int64_t)g_syms[i].const_val;
            if (g_syms[i].defined && g_syms[i].section >= 0)
                return (int64_t)g_sects[g_syms[i].section].vaddr + g_syms[i].off;
            return -1;
        }
    }
    return -2;
}

/* BERRY_IMPORT name[, name ...] — declare symbols the OS loader must
 * resolve against another module (e.g. the MBJS runtime). References to
 * extern symbols emit .cherryreva relocations in every pass instead of
 * failing "unresolved" in pass 2. */
static void mark_extern(const char *name) {
    int i;
    for (i = 0; i < g_sym_count; i++) {
        if (strcmp(g_syms[i].name, name) == 0) {
            g_syms[i].is_extern = 1;
            return;
        }
    }
    if (g_sym_count >= MAX_SYMS) { err("too many symbols"); return; }
    sym_t *s = &g_syms[g_sym_count++];
    strncpy(s->name, name, sizeof(s->name) - 1);
    s->name[sizeof(s->name) - 1] = 0;
    s->is_extern = 1;
    s->defined = 0;
    s->is_const = 0;
    s->section = -1;
    s->off = 0;
}

static void define_label(const char *name) {
    if (g_cur_sect < 0) { err("label outside section"); return; }
    for (int i = 0; i < g_sym_count; i++) {
        if (strcmp(g_syms[i].name, name) == 0) {
            g_syms[i].defined = 1;
            g_syms[i].section = g_cur_sect;
            g_syms[i].off = g_sects[g_cur_sect].size;
            return;
        }
    }
    if (g_sym_count >= MAX_SYMS) { err("too many symbols"); return; }
    sym_t *s = &g_syms[g_sym_count++];
    strncpy(s->name, name, sizeof(s->name) - 1);
    s->defined = 1; s->is_const = 0; s->section = g_cur_sect;
    s->off = g_sects[g_cur_sect].size;
}

static void define_const(const char *name, uint64_t val) {
    for (int i = 0; i < g_sym_count; i++) {
        if (strcmp(g_syms[i].name, name) == 0) {
            g_syms[i].is_const = 1; g_syms[i].const_val = val;
            g_syms[i].defined = 1; return;
        }
    }
    if (g_sym_count >= MAX_SYMS) { err("too many symbols"); return; }
    sym_t *s = &g_syms[g_sym_count++];
    strncpy(s->name, name, sizeof(s->name) - 1);
    s->defined = 1; s->is_const = 1; s->const_val = val; s->section = -1;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Expression evaluator (+, -, *, /, %, parens, $, symbols)
 * ═══════════════════════════════════════════════════════════════════════════ */
typedef struct { const char *s; int i; int has_err; } expr_t;

static void expr_skip(expr_t *e) { while (e->s[e->i] == ' ' || e->s[e->i] == '\t') e->i++; }

static int64_t expr_atom(expr_t *e);
static int64_t expr_mul(expr_t *e);
static int64_t expr_add(expr_t *e);

static int64_t expr_atom(expr_t *e) {
    expr_skip(e);
    char c = e->s[e->i];
    if (c == '(') {
        e->i++;
        int64_t v = expr_add(e);
        expr_skip(e);
        if (e->s[e->i] == ')') e->i++;
        return v;
    }
    if (c == '$') {
        e->i++;
        if (e->s[e->i] == '$') e->i++;
        return (g_cur_sect >= 0) ? (int64_t)g_sects[g_cur_sect].size : 0;
    }
    if (c >= '0' && c <= '9') {
        int64_t v = 0;
        if (e->s[e->i] == '0' && (e->s[e->i+1] == 'x' || e->s[e->i+1] == 'X')) {
            e->i += 2;
            while (1) {
                char h = e->s[e->i];
                int d;
                if (h >= '0' && h <= '9') d = h - '0';
                else if (h >= 'a' && h <= 'f') d = h - 'a' + 10;
                else if (h >= 'A' && h <= 'F') d = h - 'A' + 10;
                else break;
                v = v * 16 + d; e->i++;
            }
        } else if (e->s[e->i] == '0' && (e->s[e->i+1] == 'b' || e->s[e->i+1] == 'B')) {
            e->i += 2;
            while (e->s[e->i] == '0' || e->s[e->i] == '1') { v = v * 2 + (e->s[e->i] - '0'); e->i++; }
        } else {
            while (e->s[e->i] >= '0' && e->s[e->i] <= '9') { v = v * 10 + (e->s[e->i] - '0'); e->i++; }
        }
        return v;
    }
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '.') {
        char name[64]; int n = 0;
        while (((e->s[e->i] >= 'a' && e->s[e->i] <= 'z') ||
                (e->s[e->i] >= 'A' && e->s[e->i] <= 'Z') ||
                (e->s[e->i] >= '0' && e->s[e->i] <= '9') ||
                e->s[e->i] == '_' || e->s[e->i] == '.') && n < 63) name[n++] = e->s[e->i++];
        name[n] = 0;
        int64_t a = sym_addr(name);
        if (a == -2) {
            /* pass 1: simbol belum didefinisikan (bisa label forward).
             * Assembler dua-pass: ukuran sama di pass 2, nilai dipakai
             * pass 2.  Kalau ternyata TYPO, pass 2 melaporkan. */
            if (g_pass == 1) return 0;
            err("undefined symbol '%s'", name); e->has_err = 1; return 0;
        }
        if (a == -1) {
            if (g_pass == 1) return 0;   /* forward reference: pass 2 benar */
            err("undefined symbol '%s'", name); e->has_err = 1; return 0;
        }
        return a;
    }
    err("bad expression");
    e->has_err = 1;
    return 0;
}

static int64_t expr_mul(expr_t *e) {
    int64_t v = expr_atom(e);
    for (;;) {
        expr_skip(e);
        char c = e->s[e->i];
        if (c == '*') { e->i++; v *= expr_atom(e); }
        else if (c == '/') { e->i++; int64_t d = expr_atom(e); if (d) v /= d; }
        else if (c == '%') { e->i++; int64_t d = expr_atom(e); if (d) v %= d; }
        else break;
    }
    return v;
}

static int64_t expr_add(expr_t *e) {
    expr_skip(e);
    int neg = 0;
    if (e->s[e->i] == '-') { neg = 1; e->i++; }
    else if (e->s[e->i] == '+') { e->i++; }
    int64_t v = expr_mul(e);
    if (neg) v = -v;
    for (;;) {
        expr_skip(e);
        char c = e->s[e->i];
        if (c == '+') { e->i++; v += expr_mul(e); }
        else if (c == '-') { e->i++; v -= expr_mul(e); }
        else break;
    }
    return v;
}

static int64_t eval_expr(const char *s, int *ok) {
    expr_t e; e.s = s; e.i = 0; e.has_err = 0;
    int64_t v = expr_add(&e);
    expr_skip(&e);
    if (e.has_err || e.s[e.i] != 0) { if (ok) *ok = 0; else err("trailing chars in expr"); return 0; }
    if (ok) *ok = 1;
    return v;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * x86-64 registers
 * ═══════════════════════════════════════════════════════════════════════════ */
typedef struct { const char *name; uint8_t code; uint8_t size; } reg_t;

static const reg_t g_regs[] = {
    { "al", 0, 1 }, { "cl", 1, 1 }, { "dl", 2, 1 }, { "bl", 3, 1 },
    { "ah", 4, 1 }, { "ch", 5, 1 }, { "dh", 6, 1 }, { "bh", 7, 1 },
    { "spl", 4, 1 }, { "bpl", 5, 1 }, { "sil", 6, 1 }, { "dil", 7, 1 },
    { "r8b", 8, 1 }, { "r9b", 9, 1 }, { "r10b", 10, 1 }, { "r11b", 11, 1 },
    { "r12b", 12, 1 }, { "r13b", 13, 1 }, { "r14b", 14, 1 }, { "r15b", 15, 1 },
    { "ax", 0, 2 }, { "cx", 1, 2 }, { "dx", 2, 2 }, { "bx", 3, 2 },
    { "sp", 4, 2 }, { "bp", 5, 2 }, { "si", 6, 2 }, { "di", 7, 2 },
    { "r8w", 8, 2 }, { "r9w", 9, 2 }, { "r10w", 10, 2 }, { "r11w", 11, 2 },
    { "r12w", 12, 2 }, { "r13w", 13, 2 }, { "r14w", 14, 2 }, { "r15w", 15, 2 },
    { "eax", 0, 4 }, { "ecx", 1, 4 }, { "edx", 2, 4 }, { "ebx", 3, 4 },
    { "esp", 4, 4 }, { "ebp", 5, 4 }, { "esi", 6, 4 }, { "edi", 7, 4 },
    { "r8d", 8, 4 }, { "r9d", 9, 4 }, { "r10d", 10, 4 }, { "r11d", 11, 4 },
    { "r12d", 12, 4 }, { "r13d", 13, 4 }, { "r14d", 14, 4 }, { "r15d", 15, 4 },
    { "rax", 0, 8 }, { "rcx", 1, 8 }, { "rdx", 2, 8 }, { "rbx", 3, 8 },
    { "rsp", 4, 8 }, { "rbp", 5, 8 }, { "rsi", 6, 8 }, { "rdi", 7, 8 },
    { "r8", 8, 8 }, { "r9", 9, 8 }, { "r10", 10, 8 }, { "r11", 11, 8 },
    { "r12", 12, 8 }, { "r13", 13, 8 }, { "r14", 14, 8 }, { "r15", 15, 8 },
    /* UBerryNix register aliases (System V AMD64) */
    { "r0", 0, 8 }, { "r1", 1, 8 }, { "r2", 2, 8 }, { "r3", 3, 8 },
    { "r4", 4, 8 }, { "r5", 5, 8 }, { "r6", 6, 8 }, { "r7", 7, 8 },
    { "r16", 3, 8 }, { "r17", 12, 8 }, { "r18", 13, 8 }, { "r19", 14, 8 },
    { "r20", 15, 8 }, { "r21", 10, 8 }, { "r22", 11, 8 },
};

static const reg_t *reg_lookup(const char *name) {
    for (size_t i = 0; i < sizeof(g_regs) / sizeof(g_regs[0]); i++)
        if (strcmp(g_regs[i].name, name) == 0) return &g_regs[i];
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Operand model
 * ═══════════════════════════════════════════════════════════════════════════ */
typedef enum { OP_NONE, OP_REG, OP_IMM, OP_MEM, OP_CR, OP_DR } op_kind_t;

typedef struct {
    op_kind_t kind;
    uint8_t  reg;       /* 0..15 */
    uint8_t  size;      /* 1,2,4,8 */
    int64_t  imm;
    int      has_base, has_index;
    uint8_t  base, index;
    uint8_t  scale;
    int64_t  disp;
    int      rip_rel;
    char     sym[64];
    int      has_sym;
} opnd_t;

/* potong teks menjadi satu "term" (sampai +, -, *, ] atau akhir) */
static int read_ident(const char *s, char *out, int max) {
    int n = 0;
    while (((s[n] >= 'a' && s[n] <= 'z') || (s[n] >= 'A' && s[n] <= 'Z') ||
            (s[n] >= '0' && s[n] <= '9') || s[n] == '_') && n < max - 1) n++;
    if (n == 0) return 0;
    memcpy(out, s, n); out[n] = 0;
    return n;
}

/* parser memory operand — kembalikan panjang atau 0 */
static int parse_mem(const char *s, opnd_t *o) {
    const char *p = s;
    if (*p != '[') return 0;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    o->kind = OP_MEM;

    if (strncmp(p, "rel ", 4) == 0) { o->rip_rel = 1; p += 4; }

    int sign = 1;
    for (;;) {
        while (*p == ' ' || *p == '\t') p++;
        char c = *p;
        if (c == 0 || c == ']') break;
        if (c == '+') { sign = 1; p++; continue; }
        if (c == '-') { sign = -1; p++; continue; }

        if (c >= '0' && c <= '9') {
            /* konstanta numerik / ekspresi */
            const char *t0 = p;
            while (*p && *p != '+' && *p != '-' && *p != ']' && *p != ' ') p++;
            char tmp[256]; int tl = (int)(p - t0);
            if (tl > 255) tl = 255;
            memcpy(tmp, t0, tl); tmp[tl] = 0;
            int ok = 1;
            int64_t v = eval_expr(tmp, &ok);
            if (!ok) return 0;
            o->disp += sign * v;
            sign = 1;
            continue;
        }
        if (c == '$') {
            const char *t0 = p;
            while (*p && *p != '+' && *p != '-' && *p != ']' && *p != ' ') p++;
            char tmp[256]; int tl = (int)(p - t0);
            if (tl > 255) tl = 255;
            memcpy(tmp, t0, tl); tmp[tl] = 0;
            int ok = 1;
            int64_t v = eval_expr(tmp, &ok);
            if (!ok) return 0;
            o->disp += sign * v;
            sign = 1;
            continue;
        }
        /* identifier: register | symbol | register*scale */
        {
            char name[64];
            int n = read_ident(p, name, sizeof(name));
            if (n == 0) { err("bad memory operand"); return 0; }
            p += n;
            while (*p == ' ' || *p == '\t') p++;
            int mult = 1;
            if (*p == '*') {
                p++;
                while (*p == ' ' || *p == '\t') p++;
                const char *t0 = p;
                while (*p && *p != '+' && *p != '-' && *p != ']' && *p != ' ') p++;
                char tmp[64]; int tl = (int)(p - t0);
                if (tl > 63) tl = 63;
                memcpy(tmp, t0, tl); tmp[tl] = 0;
                int ok = 1;
                int64_t m = eval_expr(tmp, &ok);
                if (!ok || m <= 0) { err("bad scale"); return 0; }
                mult = (int)m;
            }
            const reg_t *r = reg_lookup(name);
            if (r && mult == 1) {
                if (!o->has_base) { o->has_base = 1; o->base = r->code; }
                else if (!o->has_index) { o->has_index = 1; o->index = r->code; o->scale = 1; }
                else { err("too many registers in memory"); return 0; }
            } else if (r) {
                if (!o->has_index) { o->has_index = 1; o->index = r->code; o->scale = (uint8_t)mult; }
                else { err("too many index regs"); return 0; }
            } else {
            int64_t a = sym_addr(name);
            if (a == -2) {
                    if (g_pass == 1) { o->disp += sign * 0 * mult; sign = 1; continue; }
                    err("undefined symbol '%s'", name); return 0;
                }
                if (a == -1) {
                    if (g_pass == 1) { o->disp += sign * 0 * mult; sign = 1; continue; }
                    err("forward ref '%s'", name); return 0;
                }
                /* konstanta equ → nilai; label → relokasi (riwayat di pass 1) */
                int is_const = 0;
                for (int si = 0; si < g_sym_count; si++)
                    if (strcmp(g_syms[si].name, name) == 0 && g_syms[si].is_const) { is_const = 1; break; }
                if (is_const) {
                    o->disp += sign * a * mult;
                } else {
                    if (!o->has_base && !o->has_index && mult == 1) {
                        /* [sym] absolut → disp32 + relokasi */
                        o->disp += sign * a;
                        o->has_sym = 1;
                        strncpy(o->sym, name, sizeof(o->sym) - 1);
                    } else {
                        o->disp += sign * a * mult;
                    }
                }
                sign = 1;
            }
        }
    }
    if (*p == ']') p++;
    return (int)(p - s);
}

/* parse satu operand penuh */
/* Multi-arch register type definitions (forward declarations) */
typedef struct { const char *name; uint8_t code; } arm32_reg_t;
typedef struct { const char *name; uint8_t code; int is_w; } arm64_reg_t;
typedef struct { const char *name; uint8_t code; } riscv_reg_t;
static const arm32_reg_t *arm32_reg_lookup(const char *name);
static const arm64_reg_t *arm64_reg_lookup(const char *name);
static const riscv_reg_t *riscv_reg_lookup(const char *name);

static int parse_operand(const char *s, opnd_t *o) {
    memset(o, 0, sizeof(*o));
    o->kind = OP_NONE;
    const char *p = s;
    while (*p == ' ' || *p == '\t') p++;

    int mlen = parse_mem(p, o);
    if (mlen > 0) return mlen;

    /* register? */
    {
        char name[64];
        int n = read_ident(p, name, sizeof(name));
        if (n > 0) {
            const reg_t *r = reg_lookup(name);
            if (r) { o->kind = OP_REG; o->reg = r->code; o->size = r->size; return n; }
            /* Multi-architecture register lookup */
            if (g_arch == ARCH_ARM32) {
                const arm32_reg_t *ar = arm32_reg_lookup(name);
                if (ar) { o->kind = OP_REG; o->reg = ar->code; o->size = 4; return n; }
            }
            if (g_arch == ARCH_ARM64) {
                const arm64_reg_t *ar = arm64_reg_lookup(name);
                if (ar) { o->kind = OP_REG; o->reg = ar->code; o->size = ar->is_w ? 4 : 8; return n; }
            }
            if (g_arch == ARCH_RISCV32 || g_arch == ARCH_RISCV64 || g_arch == ARCH_RISCV128) {
                const riscv_reg_t *rr = riscv_reg_lookup(name);
                if (rr) { o->kind = OP_REG; o->reg = rr->code; o->size = 4; return n; }
            }
        }
        /* control / debug registers: cr0..cr15, dr0..dr15 */
        if (n >= 3 && (name[0] == 'c' || name[0] == 'd') && name[1] == 'r') {
            int num = 0, digit = 2;
            while (digit < n && name[digit] >= '0' && name[digit] <= '9') {
                num = num * 10 + (name[digit] - '0');
                digit++;
            }
            if (digit == n && num <= 15) {
                o->kind = (name[0] == 'c') ? OP_CR : OP_DR;
                o->reg = (uint8_t)num;
                o->size = 8;
                return n;
            }
        }
    }

    /* immediate: angka / ekspresi / simbol */
    {
        /* cek apakah ini simbol murni (tanpa operator) — tapi bukan angka */
        if (!(p[0] >= '0' && p[0] <= '9')) {
            char name[64];
            int n = read_ident(p, name, sizeof(name));
            if (n > 0) {
                const char *after = p + n;
                while (*after == ' ' || *after == '\t') after++;
                if (*after == 0 || *after == ',') {
                    int64_t a = sym_addr(name);
                    if (a == -2) {
                        if (g_pass == 1) {
                            /* forward ref di pass 1: ukuran tetap, nilai pass 2 */
                            o->kind = OP_IMM; o->imm = 0; o->has_sym = 1;
                            strncpy(o->sym, name, sizeof(o->sym) - 1);
                            return n;
                        }
                        err("undefined symbol '%s'", name);
                        return 0;
                    }
                    if (a == -1) { err("forward ref '%s'", name); return 0; }
                    /* simbol terdefinisi: konstanta (equ) → nilai langsung,
                     * label → relokasi CherryReva (has_sym) */
                    int is_const = 0;
                    for (int si = 0; si < g_sym_count; si++)
                        if (strcmp(g_syms[si].name, name) == 0 && g_syms[si].is_const) { is_const = 1; break; }
                    if (is_const) { o->kind = OP_IMM; o->imm = a; return n; }
                    o->kind = OP_IMM; o->imm = 0; o->has_sym = 1;
                    strncpy(o->sym, name, sizeof(o->sym) - 1);
                    return n;
                }
            }
        }
        /* ekspresi penuh */
        const char *t0 = p;
        int depth = 0;
        while (*p) {
            if (*p == '(') depth++;
            else if (*p == ')') depth--;
            else if ((*p == ',' || *p == ';') && depth <= 0) break;
            p++;
        }
        char tmp[256]; int tl = (int)(p - t0);
        if (tl > 255) tl = 255;
        memcpy(tmp, t0, tl); tmp[tl] = 0;
        int saved_err = g_error;
        g_error = 0;
        int ok = 1;
        int64_t v = eval_expr(tmp, &ok);
        if (!ok) { g_error = saved_err; return 0; }
        g_error = saved_err;
        o->kind = OP_IMM; o->imm = v;
        return (int)(p - s);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Encoding helpers — tulis ke seksi aktif
 * ═══════════════════════════════════════════════════════════════════════════ */

/* ---- ARM32 register table ---- */
/* arm32_reg_t already defined above */
static const arm32_reg_t g_arm32_regs[] = {
    {"r0",0},{"r1",1},{"r2",2},{"r3",3},{"r4",4},{"r5",5},{"r6",6},{"r7",7},
    {"r8",8},{"r9",9},{"r10",10},{"r11",11},{"r12",12},{"r13",13},{"r14",14},{"r15",15},
    {"fp",11},{"ip",12},{"sp",13},{"lr",14},{"pc",15},{"sl",10},
};
static const arm32_reg_t *arm32_reg_lookup(const char *name) {
    for (size_t i = 0; i < sizeof(g_arm32_regs)/sizeof(g_arm32_regs[0]); i++)
        if (strcmp(g_arm32_regs[i].name, name) == 0) return &g_arm32_regs[i];
    return 0;
}

/* ---- ARM64 register table ---- */
/* arm64_reg_t already defined above */
static const arm64_reg_t g_arm64_regs[] = {
    {"x0",0,0},{"x1",1,0},{"x2",2,0},{"x3",3,0},{"x4",4,0},{"x5",5,0},{"x6",6,0},{"x7",7,0},
    {"x8",8,0},{"x9",9,0},{"x10",10,0},{"x11",11,0},{"x12",12,0},{"x13",13,0},{"x14",14,0},{"x15",15,0},
    {"x16",16,0},{"x17",17,0},{"x18",18,0},{"x19",19,0},{"x20",20,0},{"x21",21,0},{"x22",22,0},{"x23",23,0},
    {"x24",24,0},{"x25",25,0},{"x26",26,0},{"x27",27,0},{"x28",28,0},{"x29",29,0},{"x30",30,0},{"x31",31,0},
    {"w0",0,1},{"w1",1,1},{"w2",2,1},{"w3",3,1},{"w4",4,1},{"w5",5,1},{"w6",6,1},{"w7",7,1},
    {"w8",8,1},{"w9",9,1},{"w10",10,1},{"w11",11,1},{"w12",12,1},{"w13",13,1},{"w14",14,1},{"w15",15,1},
    {"w16",16,1},{"w17",17,1},{"w18",18,1},{"w19",19,1},{"w20",20,1},{"w21",21,1},{"w22",22,1},{"w23",23,1},
    {"w24",24,1},{"w25",25,1},{"w26",26,1},{"w27",27,1},{"w28",28,1},{"w29",29,1},{"w30",30,1},{"wzr",31,1},
    {"xzr",31,0},{"sp",31,0},{"lr",30,0},{"fp",29,0},
};
static const arm64_reg_t *arm64_reg_lookup(const char *name) {
    for (size_t i = 0; i < sizeof(g_arm64_regs)/sizeof(g_arm64_regs[0]); i++)
        if (strcmp(g_arm64_regs[i].name, name) == 0) return &g_arm64_regs[i];
    return 0;
}

/* ---- RISC-V register table ---- */
/* riscv_reg_t already defined above */
static const riscv_reg_t g_riscv_regs[] = {
    {"x0",0},{"x1",1},{"x2",2},{"x3",3},{"x4",4},{"x5",5},{"x6",6},{"x7",7},
    {"x8",8},{"x9",9},{"x10",10},{"x11",11},{"x12",12},{"x13",13},{"x14",14},{"x15",15},
    {"x16",16},{"x17",17},{"x18",18},{"x19",19},{"x20",20},{"x21",21},{"x22",22},{"x23",23},
    {"x24",24},{"x25",25},{"x26",26},{"x27",27},{"x28",28},{"x29",29},{"x30",30},{"x31",31},
    {"zero",0},{"ra",1},{"sp",2},{"gp",3},{"tp",4},
    {"t0",5},{"t1",6},{"t2",7},{"s0",8},{"fp",8},{"s1",9},
    {"a0",10},{"a1",11},{"a2",12},{"a3",13},{"a4",14},{"a5",15},{"a6",16},{"a7",17},
    {"s2",18},{"s3",19},{"s4",20},{"s5",21},{"s6",22},{"s7",23},{"s8",24},{"s9",25},
    {"s10",26},{"s11",27},{"t3",28},{"t4",29},{"t5",30},{"t6",31},
};
static const riscv_reg_t *riscv_reg_lookup(const char *name) {
    for (size_t i = 0; i < sizeof(g_riscv_regs)/sizeof(g_riscv_regs[0]); i++)
        if (strcmp(g_riscv_regs[i].name, name) == 0) return &g_riscv_regs[i];
    return 0;
}

static void emit_byte(uint8_t v) {
    if (g_cur_sect < 0) { err("no section"); return; }
    sect_t *s = &g_sects[g_cur_sect];
    if (s->type == SECT_BSS) { s->size++; return; }
    if (g_pass == 2) buf_byte(&s->bytes, v);
    s->size++;
}

static uint32_t here_off(void) {
    if (g_cur_sect < 0) return 0;
    return g_sects[g_cur_sect].size;
}

/* posisi patch RELATIF .text base = vaddr(seksi) + off(seksi) */
static uint32_t here_abs_off(void) {
    if (g_cur_sect < 0) return 0;
    return g_sects[g_cur_sect].vaddr + g_sects[g_cur_sect].size;
}


/* ---- ARM32 A32 encoding ---- */
static void arm32_emit_u32(uint32_t w) {
    emit_byte(w & 0xFF); emit_byte((w>>8)&0xFF);
    emit_byte((w>>16)&0xFF); emit_byte((w>>24)&0xFF);
}
static void arm32_dp_imm(uint8_t opc, uint8_t rd, uint8_t rn, uint8_t imm8, uint8_t rot) {
    arm32_emit_u32(0xE2000000 | (opc<<21) | (rn<<16) | (rd<<12) | ((rot&0xF)<<8) | (imm8&0xFF));
}
static void arm32_dp_reg(uint8_t opc, uint8_t rd, uint8_t rn, uint8_t rm, int s) {
    arm32_emit_u32(0xE0000000 | (opc<<21) | (s?(1u<<20):0) | (rn<<16) | (rd<<12) | rm);
}
static void arm32_ldr_pre(int U, int L, uint8_t rn, uint8_t rd, int16_t off) {
    arm32_emit_u32(0xE5000000 | (U<<23) | (L<<20) | (rn<<16) | (rd<<12) | ((uint16_t)off&0xFFF));
}
static void arm32_branch_imm(int link, int32_t off) {
    arm32_emit_u32(0xEA000000 | (link?(1u<<24):0) | ((((uint32_t)off-8)/4)&0x00FFFFFF));
}
static void arm32_bx_reg(uint8_t rm) { arm32_emit_u32(0xE12FFF10 | rm); }
static void arm32_swi(uint32_t imm24) { arm32_emit_u32(0xEF000000 | (imm24&0x00FFFFFF)); }
static void arm32_mul_reg(uint8_t rd, uint8_t rn, uint8_t rm) {
    arm32_emit_u32(0xE0000090 | (rd<<16) | (rn<<12) | (9<<4) | rm);
}
static void arm32_nop(void) { arm32_emit_u32(0xE1A00000); }

/* ---- ARM64 A64 encoding ---- */
static void arm64_emit_u32(uint32_t w) {
    emit_byte(w & 0xFF); emit_byte((w>>8)&0xFF);
    emit_byte((w>>16)&0xFF); emit_byte((w>>24)&0xFF);
}
static void arm64_addsub_imm(int sf, int op, int shift, int imm12, uint8_t rn, uint8_t rd) {
    arm64_emit_u32((sf<<31)|(op<<30)|(0x22<<23)|(shift<<22)|((imm12&0xFFF)<<10)|(rn<<5)|rd);
}
static void arm64_addsub_reg(int sf, int op, uint8_t rd, uint8_t rn, uint8_t rm, int sh, int imm6) {
    arm64_emit_u32((sf<<31)|(op<<30)|(0x50<<24)|(sh<<22)|(rm<<16)|((imm6&0x3F)<<10)|(rn<<5)|rd);
}
static void arm64_log_shift_reg(int sf, int opc, uint8_t rd, uint8_t rn, uint8_t rm, int sh, int imm6) {
    arm64_emit_u32((sf<<31)|(opc<<29)|(0x0A<<24)|(sh<<22)|(rm<<16)|((imm6&0x3F)<<10)|(rn<<5)|rd);
}
static void arm64_movw(int sf, int opc, int hw, int imm16, uint8_t rd) {
    arm64_emit_u32((sf<<31)|(opc<<29)|(0x25<<23)|(hw<<21)|((imm16&0xFFFF)<<5)|rd);
}
static void arm64_ldrstr_uimm(int size, int V, int opc, int imm12, uint8_t rn, uint8_t rt) {
    arm64_emit_u32((size<<30)|(1<<28)|(opc<<22)|((imm12&0xFFF)<<10)|(rn<<5)|rt);
}
static void arm64_b_imm(int imm26) {
    arm64_emit_u32((0x05u<<26)|((uint32_t)(imm26)&0x03FFFFFF));
}
static void arm64_bl_imm(int imm26) {
    arm64_emit_u32((1u<<31)|(0x05u<<26)|((uint32_t)(imm26)&0x03FFFFFF));
}
static void arm64_br_reg(int opc, uint8_t rn) {
    arm64_emit_u32(0xD61F0000 | (opc<<21) | (rn<<5));
}
static void arm64_bcond(int cond, int imm19) {
    arm64_emit_u32(0x54000000 | (((uint32_t)imm19&0x7FFFF)<<5) | cond);
}
static void arm64_svc(int imm16) {
    arm64_emit_u32(0xD4000001 | ((imm16&0xFFFF)<<5));
}
static void arm64_nop(void) { arm64_emit_u32(0xD503201F); }
static void arm64_adrp(int immlo, int immhi, uint8_t rd) {
    arm64_emit_u32((1u<<31)|(1<<28)|((immlo&3)<<29)|((immhi&0x7FFFF)<<5)|rd);
}

/* ---- RISC-V encoding ---- */
static void rv_emit_u32(uint32_t w) {
    emit_byte(w & 0xFF); emit_byte((w>>8)&0xFF);
    emit_byte((w>>16)&0xFF); emit_byte((w>>24)&0xFF);
}
static void rv_r(uint32_t f7, uint8_t rs2, uint8_t rs1, uint32_t f3, uint8_t rd, uint32_t opc) {
    rv_emit_u32((f7<<25)|((uint32_t)rs2<<20)|((uint32_t)rs1<<15)|(f3<<12)|((uint32_t)rd<<7)|opc);
}
static void rv_i(int32_t imm, uint8_t rs1, uint32_t f3, uint8_t rd, uint32_t opc) {
    rv_emit_u32(((uint32_t)(imm&0xFFF)<<20)|((uint32_t)rs1<<15)|(f3<<12)|((uint32_t)rd<<7)|opc);
}
static void rv_s(int32_t imm, uint8_t rs2, uint8_t rs1, uint32_t f3, uint32_t opc) {
    uint32_t u=(uint32_t)imm;
    rv_emit_u32(((u>>5)<<25)|((uint32_t)rs2<<20)|((uint32_t)rs1<<15)|(f3<<12)|((u&0x1F)<<7)|opc);
}
static void rv_b(int32_t imm, uint8_t rs2, uint8_t rs1, uint32_t f3, uint32_t opc) {
    uint32_t u=(uint32_t)imm;
    rv_emit_u32((((u>>12)&1)<<31)|(((u>>5)&0x3F)<<25)|((uint32_t)rs2<<20)|((uint32_t)rs1<<15)|
            (f3<<12)|(((u>>1)&0xF)<<8)|(((u>>11)&1)<<7)|opc);
}
static void rv_u(int32_t imm, uint8_t rd, uint32_t opc) {
    rv_emit_u32(((uint32_t)imm&0xFFFFF000)|((uint32_t)rd<<7)|opc);
}
static void rv_j(int32_t imm, uint8_t rd, uint32_t opc) {
    uint32_t u=(uint32_t)imm;
    rv_emit_u32(((u>>20&1)<<31)|(((u>>1)&0x3FF)<<21)|(((u>>11)&1)<<20)|
            (((u>>12)&0xFF)<<12)|((uint32_t)rd<<7)|opc);
}

static void emit_reloc_abs(uint8_t width) {
    if (g_reloc_count >= MAX_RELOCS) { err("too many relocs"); return; }
    reloc_t *r = &g_relocs[g_reloc_count++];
    r->off = here_abs_off();
    r->type = 0;
    r->width = width;
    r->rsv = 0;
}

static void emit_modrm(uint8_t mod, uint8_t reg, uint8_t rm) {
    emit_byte((uint8_t)((mod << 6) | ((reg & 7) << 3) | (rm & 7)));
}
static void emit_rex(uint8_t w, uint8_t r, uint8_t x, uint8_t b) {
    emit_byte((uint8_t)(0x40 | (w << 3) | (r << 2) | (x << 1) | b));
}

/* patch 4 byte terakhir yang ditulis (untuk disp32/rel32 di pass 2) */
static void patch_last_u32(int32_t v) {
    if (g_pass != 2 || g_cur_sect < 0) return;
    uint32_t cur = g_sects[g_cur_sect].size;
    if (cur < 4) return;
    uint8_t *b = g_sects[g_cur_sect].bytes.data;
    b[cur - 4] = (uint8_t)v;
    b[cur - 3] = (uint8_t)(v >> 8);
    b[cur - 2] = (uint8_t)(v >> 16);
    b[cur - 1] = (uint8_t)(v >> 24);
}

/* encode operand memori (sudah punya .rip_rel / base/index/disp) */
static void emit_mem(opnd_t *o, uint8_t reg_field) {
    if (o->rip_rel) {
        emit_modrm(0, reg_field, 5);
        emit_byte(0); emit_byte(0); emit_byte(0); emit_byte(0);
        if (g_pass == 2) {
            int64_t next = (int64_t)g_sects[g_cur_sect].vaddr + here_off();
            int32_t d = (int32_t)(o->disp - next);
            patch_last_u32(d);
        }
        return;
    }
    if (!o->has_base && !o->has_index) {
        /* [disp32] absolute — SIB no-base (scale=0 index=100b base=101b = 0x25) */
        emit_modrm(0, reg_field, 4);
        emit_byte(0x25);
        /* disp32 absolut: jika referensi simbol → relokasi ABS 4 B */
        if (o->has_sym) emit_reloc_abs(4);
        emit_byte(0); emit_byte(0); emit_byte(0); emit_byte(0);
        if (g_pass == 2) patch_last_u32((int32_t)o->disp);
        return;
    }
    int need_sib = o->has_index || (o->has_base && (o->base == 4 || o->base == 12));
    if (need_sib) {
        uint8_t scale = 0;
        if (o->has_index) {
            if (o->scale == 2) scale = 1;
            else if (o->scale == 4) scale = 2;
            else if (o->scale == 8) scale = 3;
        }
        uint8_t index = o->has_index ? (o->index & 7) : 4;
        uint8_t base = o->has_base ? (o->base & 7) : 5;
        uint8_t mod;
        if (o->disp == 0 && o->has_base && o->base != 5 && o->base != 13) mod = 0;
        else if (o->disp >= -128 && o->disp <= 127) mod = 1;
        else mod = 2;
        emit_modrm(mod, reg_field, 4);
        emit_byte((uint8_t)((scale << 6) | (index << 3) | base));
        if (mod == 1) emit_byte((uint8_t)(int8_t)o->disp);
        else if (mod == 2) {
            uint32_t d = (uint32_t)o->disp;
            emit_byte(d & 0xFF); emit_byte((d >> 8) & 0xFF);
            emit_byte((d >> 16) & 0xFF); emit_byte((d >> 24) & 0xFF);
        }
        return;
    }
    /* tanpa SIB */
    uint8_t rm = o->base & 7;
    uint8_t mod;
    if (o->disp == 0 && rm != 5) mod = 0;
    else if (o->disp >= -128 && o->disp <= 127) mod = 1;
    else mod = 2;
    emit_modrm(mod, reg_field, rm);
    if (mod == 1) emit_byte((uint8_t)(int8_t)o->disp);
    else if (mod == 2) {
        uint32_t d = (uint32_t)o->disp;
        emit_byte(d & 0xFF); emit_byte((d >> 8) & 0xFF);
        emit_byte((d >> 16) & 0xFF); emit_byte((d >> 24) & 0xFF);
    }
}

/* apakah perlu REX untuk operand r/m + reg_field? */
static int rm_needs_rex(opnd_t *o, uint8_t reg_field, int w) {
    int r = reg_field >= 8 ? 1 : 0;
    if (o->kind == OP_REG) {
        int b = o->reg >= 8 ? 1 : 0;
        return w || r || b;
    }
    if (o->kind == OP_MEM) {
        int x = o->has_index && o->index >= 8 ? 1 : 0;
        int b = o->has_base && o->base >= 8 ? 1 : 0;
        return w || r || x || b;
    }
    return w || r;
}

/* tulis REX sebelum opcode bila perlu */
static void emit_rex_rm(opnd_t *o, uint8_t reg_field, int w) {
    if (!rm_needs_rex(o, reg_field, w)) return;
    int r = reg_field >= 8 ? 1 : 0;
    int x = 0, b = 0;
    if (o->kind == OP_REG) b = o->reg >= 8 ? 1 : 0;
    else if (o->kind == OP_MEM) {
        x = o->has_index && o->index >= 8 ? 1 : 0;
        b = o->has_base && o->base >= 8 ? 1 : 0;
    }
    emit_rex(w ? 1 : 0, r, x, b);
}

/* encode operand r/m (reg atau memori) — TANPA REX (dipanggil setelah
 * emit_rex_rm).  reg_field = kode reg (0..15). */
static void emit_rm(opnd_t *o, uint8_t reg_field, int rex_w) {
    (void)rex_w;
    if (o->kind == OP_REG) {
        emit_modrm(3, reg_field, o->reg);
        return;
    }
    if (o->kind == OP_MEM) {
        emit_mem(o, reg_field);
        return;
    }
    err("expected register or memory operand");
}

/* imm32; simbol → relokasi 4 B */
static void emit_imm32(opnd_t *b) {
    if (b->has_sym) {
        if (g_pass == 1) { emit_byte(0); emit_byte(0); emit_byte(0); emit_byte(0); return; }
        int64_t v = sym_addr(b->sym);
        if (v < 0) { err("symbol '%s' unresolved", b->sym); return; }
        emit_reloc_abs(4);
        emit_byte((uint8_t)v); emit_byte((uint8_t)(v >> 8));
        emit_byte((uint8_t)(v >> 16)); emit_byte((uint8_t)(v >> 24));
        return;
    }
    uint32_t v = (uint32_t)b->imm;
    emit_byte(v & 0xFF); emit_byte((v >> 8) & 0xFF);
    emit_byte((v >> 16) & 0xFF); emit_byte((v >> 24) & 0xFF);
}

static void emit_u64_plain(uint64_t v) {
    emit_byte((uint8_t)v); emit_byte((uint8_t)(v >> 8));
    emit_byte((uint8_t)(v >> 16)); emit_byte((uint8_t)(v >> 24));
    emit_byte((uint8_t)(v >> 32)); emit_byte((uint8_t)(v >> 40));
    emit_byte((uint8_t)(v >> 48)); emit_byte((uint8_t)(v >> 56));
}

/* imm64; simbol → relokasi 8 B */
static void emit_imm64(opnd_t *b) {
    if (b->has_sym) {
        if (g_pass == 1) { for (int i = 0; i < 8; i++) emit_byte(0); return; }
        int64_t v = sym_addr(b->sym);
        if (v < 0) { err("symbol '%s' unresolved", b->sym); return; }
        emit_reloc_abs(8);
        emit_u64_plain((uint64_t)v);
        return;
    }
    emit_u64_plain((uint64_t)b->imm);
}

/* rel32 untuk call/jmp/jcc ke simbol (patch di pass 2) */
static void emit_rel32_sym(const char *sym) {
    emit_byte(0); emit_byte(0); emit_byte(0); emit_byte(0);
    if (g_pass == 2) {
        int64_t target = sym_addr(sym);
        if (target < 0) { err("jump target '%s' unresolved", sym); return; }
        int64_t next = (int64_t)g_sects[g_cur_sect].vaddr + here_off();
        int32_t d = (int32_t)(target - next);
        patch_last_u32(d);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Instructions
 * ═══════════════════════════════════════════════════════════════════════════ */
typedef struct { const char *name; uint8_t op_rm8, op_rm, op_al8, op_ax, op_imm_digit; } alu_t;

static const alu_t g_alu[] = {
    { "add", 0x00, 0x01, 0x04, 0x05, 0 },
    { "or",  0x08, 0x09, 0x0C, 0x0D, 1 },
    { "adc", 0x10, 0x11, 0x14, 0x15, 2 },
    { "sbb", 0x18, 0x19, 0x1C, 0x1D, 3 },
    { "and", 0x20, 0x21, 0x24, 0x25, 4 },
    { "sub", 0x28, 0x29, 0x2C, 0x2D, 5 },
    { "xor", 0x30, 0x31, 0x34, 0x35, 6 },
    { "cmp", 0x38, 0x39, 0x3C, 0x3D, 7 },
};

static void do_mov_sysreg(opnd_t a, opnd_t b);

/* ── mov ── */
static void do_mov(opnd_t a, opnd_t b) {
    if (a.kind == OP_CR || a.kind == OP_DR || b.kind == OP_CR || b.kind == OP_DR) {
        do_mov_sysreg(a, b);
        return;
    }
    if (a.kind == OP_REG && b.kind == OP_REG) {
        uint8_t op = (a.size == 1) ? 0x8A : 0x8B;
        emit_rex_rm(&b, a.reg, a.size == 8);
        emit_byte(op);
        emit_rm(&b, a.reg, 0);
        return;
    }
    if (a.kind == OP_REG && b.kind == OP_IMM) {
        if (a.size == 1) {
            if (a.reg >= 8) emit_rex(0, 0, 0, 1);
            emit_byte(0xB0 + (a.reg & 7));
            emit_byte((uint8_t)b.imm);
            return;
        }
        if (a.size == 8 && (b.has_sym ||
            b.imm > 0x7FFFFFFFLL || b.imm < -0x80000000LL)) {
            /* movabs r64, imm64 */
            emit_rex(1, a.reg >= 8 ? 1 : 0, 0, a.reg >= 8 ? 1 : 0);
            emit_byte(0xB8 + (a.reg & 7));
            emit_imm64(&b);
            return;
        }
        if (a.size == 8) {
            emit_rex(1, a.reg >= 8 ? 1 : 0, 0, 0);
            emit_byte(0xC7);
            emit_modrm(3, 0, a.reg);
            emit_imm32(&b);
            return;
        }
        /* 32-bit: B8+r imm32 */
        if (a.reg >= 8) emit_rex(0, 0, 0, 1);
        emit_byte(0xB8 + (a.reg & 7));
        emit_imm32(&b);
        return;
    }
    if (a.kind == OP_REG && b.kind == OP_MEM) {
        uint8_t op = (a.size == 1) ? 0x8A : 0x8B;
        emit_rex_rm(&b, a.reg, a.size == 8);
        emit_byte(op);
        emit_rm(&b, a.reg, 0);
        return;
    }
    if (a.kind == OP_MEM && b.kind == OP_REG) {
        uint8_t op = (b.size == 1) ? 0x88 : 0x89;
        emit_rex_rm(&a, b.reg, b.size == 8);
        emit_byte(op);
        emit_rm(&a, b.reg, 0);
        return;
    }
    if (a.kind == OP_MEM && b.kind == OP_IMM) {
        if (b.has_sym) { err("mov [mem], symbol not supported"); return; }
        emit_rex_rm(&a, 0, a.size == 8);
        emit_byte((a.size == 1) ? 0xC6 : 0xC7);
        emit_rm(&a, 0, 0);
        emit_imm32(&b);
        return;
    }
    err("bad mov operands");
}

/* ── lea ── */
static void do_lea(opnd_t a, opnd_t b) {
    if (a.kind == OP_REG && b.kind == OP_MEM) {
        emit_rex_rm(&b, a.reg, a.size == 8);
        emit_byte(0x8D);
        emit_rm(&b, a.reg, 0);
        return;
    }
    err("bad lea operands");
}

/* ── push/pop ── */
static void do_push(opnd_t a, opnd_t b) {
    (void)b;
    if (a.kind == OP_REG && a.size == 8) {
        if (a.reg >= 8) emit_rex(0, 0, 0, 1);
        emit_byte(0x50 + (a.reg & 7));
        return;
    }
    if (a.kind == OP_IMM) {
        if (!a.has_sym && a.imm >= -128 && a.imm <= 127) {
            emit_byte(0x6A); emit_byte((uint8_t)(int8_t)a.imm);
            return;
        }
        emit_byte(0x68);
        emit_imm32(&a);
        return;
    }
    if (a.kind == OP_MEM) {
        emit_rex_rm(&a, 6, 0);
        emit_byte(0xFF); emit_rm(&a, 6, 0);
        return;
    }
    err("bad push");
}
static void do_pop(opnd_t a, opnd_t b) {
    (void)b;
    if (a.kind == OP_REG && a.size == 8) {
        if (a.reg >= 8) emit_rex(0, 0, 0, 1);
        emit_byte(0x58 + (a.reg & 7));
        return;
    }
    if (a.kind == OP_MEM) {
        emit_rex_rm(&a, 0, 0);
        emit_byte(0x8F); emit_rm(&a, 0, 0);
        return;
    }
    err("bad pop");
}

/* emit rel32 ke target absolut (sudah ter-resolve di pass 2) */
static void emit_rel32_to(int64_t target) {
    emit_byte(0); emit_byte(0); emit_byte(0); emit_byte(0);
    if (g_pass == 2) {
        int64_t next = (int64_t)g_sects[g_cur_sect].vaddr + here_off();
        int32_t d = (int32_t)(target - next);
        patch_last_u32(d);
    }
}

/* patch 1 byte terakhir (untuk rel8 loop/jrcxz di pass 2) */
static void patch_last_u8(int8_t v) {
    if (g_pass != 2 || g_cur_sect < 0) return;
    uint32_t cur = g_sects[g_cur_sect].size;
    if (cur < 1) return;
    uint8_t *b = g_sects[g_cur_sect].bytes.data;
    b[cur - 1] = (uint8_t)v;
}

static void emit_rel8_sym(const char *sym) {
    emit_byte(0);
    if (g_pass == 2) {
        int64_t target = sym_addr(sym);
        if (target < 0) { err("jump target '%s' unresolved", sym); return; }
        int64_t next = (int64_t)g_sects[g_cur_sect].vaddr + here_off();
        patch_last_u8((int8_t)(target - next));
    }
}

static void emit_rel8_to(int64_t target) {
    emit_byte(0);
    if (g_pass == 2) {
        int64_t next = (int64_t)g_sects[g_cur_sect].vaddr + here_off();
        patch_last_u8((int8_t)(target - next));
    }
}

/* ── call/jmp ── */
static void do_call(opnd_t a, opnd_t b) {
    (void)b;
    if (a.kind == OP_IMM && a.has_sym) { emit_byte(0xE8); emit_rel32_sym(a.sym); return; }
    if (a.kind == OP_IMM) { emit_byte(0xE8); emit_rel32_to(a.imm); return; }
    if (a.kind == OP_REG && a.size == 8) {
        emit_byte(0xFF);
        if (a.reg >= 8) emit_rex(0, 0, 0, 1);
        emit_modrm(3, 2, a.reg);
        return;
    }
    if (a.kind == OP_MEM) { emit_rex_rm(&a, 2, 0); emit_byte(0xFF); emit_rm(&a, 2, 0); return; }
    err("bad call");
}
static void do_jmp(opnd_t a, opnd_t b) {
    (void)b;
    if (a.kind == OP_IMM && a.has_sym) { emit_byte(0xE9); emit_rel32_sym(a.sym); return; }
    if (a.kind == OP_IMM) { emit_byte(0xE9); emit_rel32_to(a.imm); return; }
    if (a.kind == OP_REG && a.size == 8) {
        emit_byte(0xFF);
        if (a.reg >= 8) emit_rex(0, 0, 0, 1);
        emit_modrm(3, 4, a.reg);
        return;
    }
    if (a.kind == OP_MEM) { emit_rex_rm(&a, 4, 0); emit_byte(0xFF); emit_rm(&a, 4, 0); return; }
    err("bad jmp");
}

/* ── jcc ── */
static int jcc_code(const char *name) {
    static const struct { const char *n; uint8_t cc; } cc[] = {
        { "jo", 0 }, { "jno", 1 }, { "jb", 2 }, { "jnae", 2 }, { "jc", 2 },
        { "jae", 3 }, { "jnb", 3 }, { "jnc", 3 }, { "je", 4 }, { "jz", 4 },
        { "jne", 5 }, { "jnz", 5 }, { "jbe", 6 }, { "jna", 6 },
        { "ja", 7 }, { "jnbe", 7 }, { "js", 8 }, { "jns", 9 },
        { "jp", 10 }, { "jpe", 10 }, { "jnp", 11 }, { "jpo", 11 },
        { "jl", 12 }, { "jnge", 12 }, { "jge", 13 }, { "jnl", 13 },
        { "jle", 14 }, { "jng", 14 }, { "jg", 15 }, { "jnle", 15 },
    };
    for (size_t i = 0; i < sizeof(cc)/sizeof(cc[0]); i++)
        if (strcmp(cc[i].n, name) == 0) return cc[i].cc;
    return -1;
}
static void do_jcc(const char *name, opnd_t a, opnd_t b) {
    (void)b;
    int c = jcc_code(name);
    if (c < 0 || a.kind != OP_IMM) { err("bad jcc"); return; }
    emit_byte(0x0F);
    emit_byte(0x80 + (uint8_t)c);
    if (a.has_sym) emit_rel32_sym(a.sym);
    else emit_rel32_to(a.imm);
}

/* ── inc/dec/neg/not ── */
static void do_unary(const char *name, opnd_t a, opnd_t b) {
    (void)b;
    uint8_t digit;
    if (strcmp(name, "inc") == 0) digit = 0;
    else if (strcmp(name, "dec") == 0) digit = 1;
    else if (strcmp(name, "neg") == 0) digit = 3;
    else if (strcmp(name, "not") == 0) digit = 2;
    else return;
    if (a.kind != OP_REG && a.kind != OP_MEM) { err("bad %s", name); return; }
    emit_rex_rm(&a, digit, a.size == 8);
    emit_byte(0xFF);
    emit_rm(&a, digit, 0);
}

/* ── imul ── */
static void do_imul(opnd_t a, opnd_t b) {
    if (a.kind == OP_REG && b.kind == OP_REG) {
        emit_rex_rm(&b, a.reg, 1);
        emit_byte(0x0F); emit_byte(0xAF);
        emit_rm(&b, a.reg, 0);
        return;
    }
    if (a.kind == OP_REG && b.kind == OP_MEM) {
        emit_rex_rm(&b, a.reg, 1);
        emit_byte(0x0F); emit_byte(0xAF);
        emit_rm(&b, a.reg, 0);
        return;
    }
    if (a.kind == OP_REG && b.kind == OP_IMM && !b.has_sym) {
        if (b.imm >= -128 && b.imm <= 127) {
            emit_rex(1, a.reg >= 8 ? 1 : 0, 0, a.reg >= 8 ? 1 : 0);
            emit_byte(0x6B);
            emit_modrm(3, a.reg, a.reg);
            emit_byte((uint8_t)(int8_t)b.imm);
        } else {
            emit_rex(1, a.reg >= 8 ? 1 : 0, 0, a.reg >= 8 ? 1 : 0);
            emit_byte(0x69);
            emit_modrm(3, a.reg, a.reg);
            emit_imm32(&b);
        }
        return;
    }
    err("bad imul");
}
/* ── shl/shr/sar/rol/ror ── */
static void do_shift(const char *name, opnd_t a, opnd_t b) {
    uint8_t digit;
    if (strcmp(name, "shl") == 0 || strcmp(name, "sal") == 0) digit = 4;
    else if (strcmp(name, "shr") == 0) digit = 5;
    else if (strcmp(name, "sar") == 0) digit = 7;
    else if (strcmp(name, "rol") == 0) digit = 0;
    else if (strcmp(name, "ror") == 0) digit = 1;
    else return;
    int w = (a.size == 8);
    if (b.kind == OP_IMM && !b.has_sym && b.imm == 1) {
        emit_rex_rm(&a, digit, w);
        emit_byte(0xD1);
        emit_rm(&a, digit, 0);
        return;
    }
    if (b.kind == OP_IMM && !b.has_sym) {
        emit_rex_rm(&a, digit, w);
        emit_byte(0xC1);
        emit_rm(&a, digit, 0);
        emit_byte((uint8_t)b.imm);
        return;
    }
    if (b.kind == OP_REG && b.reg == 1 && b.size == 1) { /* cl */
        emit_rex_rm(&a, digit, w);
        emit_byte(0xD3);
        emit_rm(&a, digit, 0);
        return;
    }
    err("bad shift");
}

/* ── test ── */
static void do_test(opnd_t a, opnd_t b) {
    if (a.kind == OP_REG && b.kind == OP_REG) {
        if (a.size == 1) { emit_byte(0x84); emit_rm(&b, a.reg, 0); }
        else { emit_rex_rm(&b, a.reg, a.size == 8); emit_byte(0x85); emit_rm(&b, a.reg, 0); }
        return;
    }
    if (a.kind == OP_REG && b.kind == OP_IMM && !b.has_sym) {
        if (a.size == 1) {
            if (a.reg == 0) { emit_byte(0xA8); emit_byte((uint8_t)b.imm); return; }
            if (a.reg >= 8) emit_rex(0, 0, 0, 1);
            emit_byte(0xF6); emit_modrm(3, 0, a.reg); emit_byte((uint8_t)b.imm);
            return;
        }
        if (a.reg == 0 && a.size == 4) { emit_byte(0xA9); emit_imm32(&b); return; }
        emit_rex_rm(&a, 0, a.size == 8);
        emit_byte(0xF7);
        emit_rm(&a, 0, 0);
        emit_imm32(&b);
        return;
    }
    if (a.kind == OP_MEM && b.kind == OP_IMM && !b.has_sym) {
        emit_rex_rm(&a, 0, 0);
        emit_byte(0xF6);
        emit_rm(&a, 0, 0);
        emit_imm32(&b);
        return;
    }
    err("bad test");
}

/* ── movzx/movsx ── */
static void do_movx(const char *name, opnd_t a, opnd_t b) {
    int sign = (name[3] == 's');
    if (a.kind != OP_REG) { err("bad %s", name); return; }
    if (b.kind == OP_REG || b.kind == OP_MEM) {
        if (b.size == 1) { emit_rex_rm(&b, a.reg, 1); emit_byte(0x0F); emit_byte(sign ? 0xBE : 0xB6); emit_rm(&b, a.reg, 0); }
        else if (b.size == 2) { emit_rex_rm(&b, a.reg, 1); emit_byte(0x0F); emit_byte(sign ? 0xBF : 0xB7); emit_rm(&b, a.reg, 0); }
        else { err("bad %s width", name); }
        return;
    }
    err("bad %s", name);
}

/* ── xchg ── */
static void do_xchg(opnd_t a, opnd_t b) {
    if (a.kind == OP_REG && b.kind == OP_REG) {
        if (a.size == 1) { emit_byte(0x86); emit_rm(&b, a.reg, 0); }
        else { emit_rex_rm(&b, a.reg, a.size == 8); emit_byte(0x87); emit_rm(&b, a.reg, 0); }
        return;
    }
    if (a.kind == OP_REG && b.kind == OP_MEM) {
        emit_rex_rm(&b, a.reg, a.size == 8);
        emit_byte(0x87);
        emit_rm(&b, a.reg, 0);
        return;
    }
    err("bad xchg");
}

/* ── setcc ── */
static int setcc_code(const char *name) {
    static const struct { const char *n; uint8_t cc; } cc[] = {
        { "seto", 0 }, { "setno", 1 }, { "setb", 2 }, { "setc", 2 },
        { "setae", 3 }, { "setnb", 3 }, { "setnc", 3 }, { "sete", 4 },
        { "setz", 4 }, { "setne", 5 }, { "setnz", 5 }, { "setbe", 6 },
        { "seta", 7 }, { "sets", 8 }, { "setns", 9 }, { "setp", 10 },
        { "setnp", 11 }, { "setl", 12 }, { "setge", 13 }, { "setle", 14 },
        { "setg", 15 },
    };
    for (size_t i = 0; i < sizeof(cc)/sizeof(cc[0]); i++)
        if (strcmp(cc[i].n, name) == 0) return cc[i].cc;
    return -1;
}
static void do_setcc(const char *name, opnd_t a, opnd_t b) {
    (void)b;
    int c = setcc_code(name);
    if (c < 0 || (a.kind != OP_REG && a.kind != OP_MEM)) { err("bad setcc"); return; }
    emit_byte(0x0F);
    emit_byte(0x90 + (uint8_t)c);
    if (a.kind == OP_REG) {
        if (a.reg >= 8) emit_rex(0, 0, 0, 1);
        emit_modrm(3, 0, a.reg);
    } else {
        emit_rex_rm(&a, 0, 0);
        emit_rm(&a, 0, 0);
    }
}

/* ── alu (add/or/and/sub/xor/cmp) ── */
static void do_alu(const alu_t *t, opnd_t a, opnd_t b) {
    if (a.kind == OP_REG && b.kind == OP_REG) {
        if (a.size == 1) { emit_byte(t->op_rm8); emit_rm(&b, a.reg, 0); }
        else { emit_rex_rm(&b, a.reg, a.size == 8); emit_byte(t->op_rm); emit_rm(&b, a.reg, 0); }
        return;
    }
    if (a.kind == OP_MEM && b.kind == OP_REG) {
        if (b.size == 1) { emit_byte(t->op_rm8); emit_rm(&a, b.reg, 0); }
        else { emit_rex_rm(&a, b.reg, b.size == 8); emit_byte(t->op_rm); emit_rm(&a, b.reg, 0); }
        return;
    }
    if (a.kind == OP_REG && b.kind == OP_MEM) {
        if (a.size == 1) { emit_byte(t->op_rm8 + 2); emit_rm(&b, a.reg, 0); }
        else { emit_rex_rm(&b, a.reg, a.size == 8); emit_byte(t->op_rm + 2); emit_rm(&b, a.reg, 0); }
        return;
    }
    if (a.kind == OP_REG && b.kind == OP_IMM && !b.has_sym) {
        if (a.size == 1) {
            if (a.reg == 0) { emit_byte(t->op_al8); emit_byte((uint8_t)b.imm); return; }
            if (a.reg >= 8) emit_rex(0, 0, 0, 1);
            emit_byte(0x80); emit_modrm(3, t->op_imm_digit, a.reg); emit_byte((uint8_t)b.imm);
            return;
        }
        if (b.imm >= -128 && b.imm <= 127) {
            emit_rex_rm(&a, t->op_imm_digit, a.size == 8);
            emit_byte(0x83);
            emit_rm(&a, t->op_imm_digit, 0);
            emit_byte((uint8_t)(int8_t)b.imm);
            return;
        }
        emit_rex_rm(&a, t->op_imm_digit, a.size == 8);
        emit_byte(0x81);
        emit_rm(&a, t->op_imm_digit, 0);
        emit_imm32(&b);
        return;
    }
    if (a.kind == OP_MEM && b.kind == OP_IMM && !b.has_sym) {
        if (b.imm >= -128 && b.imm <= 127) {
            emit_rex_rm(&a, t->op_imm_digit, 0); emit_byte(0x83); emit_rm(&a, t->op_imm_digit, 0); emit_byte((uint8_t)(int8_t)b.imm);
        } else {
            emit_rex_rm(&a, t->op_imm_digit, 0); emit_byte(0x81); emit_rm(&a, t->op_imm_digit, 0); emit_imm32(&b);
        }
        return;
    }
    err("bad %s operands", t->name);
}

/* ── in/out (I/O ports) ── */
static void do_io(const char *name, opnd_t a, opnd_t b) {
    if (strcmp(name, "in") == 0) {
        /* in al/ax/eax, dx */
        if (a.kind == OP_REG && b.kind == OP_REG && b.reg == 2 && b.size == 2) {
            if (a.size == 1) emit_byte(0xEC);
            else if (a.size == 2) { emit_byte(0x66); emit_byte(0xED); }
            else if (a.size == 4) emit_byte(0xED);
            else { err("bad in"); return; }
            return;
        }
        /* in al/ax/eax, imm8 */
        if (a.kind == OP_REG && b.kind == OP_IMM && !b.has_sym) {
            if (a.size == 1) emit_byte(0xE4);
            else if (a.size == 2) { emit_byte(0x66); emit_byte(0xE5); }
            else if (a.size == 4) emit_byte(0xE5);
            else { err("bad in"); return; }
            emit_byte((uint8_t)b.imm);
            return;
        }
        err("bad in operands");
        return;
    }
    /* out dx/imm8, al/ax/eax */
    if (a.kind == OP_REG && a.reg == 2 && a.size == 2) {
        if (b.size == 1) emit_byte(0xEE);
        else if (b.size == 2) { emit_byte(0x66); emit_byte(0xEF); }
        else if (b.size == 4) emit_byte(0xEF);
        else { err("bad out"); return; }
        return;
    }
    if (a.kind == OP_IMM && !a.has_sym) {
        if (b.size == 1) emit_byte(0xE6);
        else if (b.size == 2) { emit_byte(0x66); emit_byte(0xE7); }
        else if (b.size == 4) emit_byte(0xE7);
        else { err("bad out"); return; }
        emit_byte((uint8_t)a.imm);
        return;
    }
    err("bad out operands");
}

/* ── mov cr/dr (control & debug registers) ── */
static void do_mov_sysreg(opnd_t a, opnd_t b) {
    /* mov r64, crN → 0F 20 /r ; mov r64, drN → 0F 21 /r */
    if (a.kind == OP_REG && (b.kind == OP_CR || b.kind == OP_DR)) {
        emit_rex(0, b.reg >= 8 ? 1 : 0, 0, a.reg >= 8 ? 1 : 0);
        emit_byte(0x0F);
        emit_byte(b.kind == OP_CR ? 0x20 : 0x21);
        emit_modrm(3, b.reg, a.reg);
        return;
    }
    /* mov crN, r64 → 0F 22 /r ; mov drN, r64 → 0F 23 /r */
    if ((a.kind == OP_CR || a.kind == OP_DR) && b.kind == OP_REG) {
        emit_rex(0, a.reg >= 8 ? 1 : 0, 0, b.reg >= 8 ? 1 : 0);
        emit_byte(0x0F);
        emit_byte(a.kind == OP_CR ? 0x22 : 0x23);
        emit_modrm(3, a.reg, b.reg);
        return;
    }
    err("bad mov cr/dr operands");
}

/* ── descriptor table & system ops (lgdt/lidt/sgdt/sidt/lldt/ltr/str/invlpg) ── */
static void do_desc(const char *name, opnd_t a) {
    int digit;
    int op_00 = 0; /* 0F 00 */
    if (strcmp(name, "lgdt") == 0) { digit = 2; }
    else if (strcmp(name, "lidt") == 0) { digit = 3; }
    else if (strcmp(name, "sgdt") == 0) { digit = 0; }
    else if (strcmp(name, "sidt") == 0) { digit = 1; }
    else if (strcmp(name, "lldt") == 0) { digit = 2; op_00 = 1; }
    else if (strcmp(name, "ltr") == 0) { digit = 3; op_00 = 1; }
    else if (strcmp(name, "sldt") == 0) { digit = 0; op_00 = 1; }
    else if (strcmp(name, "str") == 0) { digit = 1; op_00 = 1; }
    else if (strcmp(name, "invlpg") == 0) { digit = 7; }
    else return;
    /* lldt/ltr/sldt/str: r/m16 (register atau memori) */
    if (op_00 && a.kind == OP_REG) {
        emit_rex_rm(&a, (uint8_t)digit, 0);
        emit_byte(0x0F); emit_byte(0x00);
        emit_rm(&a, (uint8_t)digit, 0);
        return;
    }
    if (a.kind != OP_MEM) { err("bad %s operand (need [mem])", name); return; }
    emit_rex_rm(&a, (uint8_t)digit, 0);
    emit_byte(0x0F);
    emit_byte(op_00 ? 0x00 : 0x01);
    emit_rm(&a, (uint8_t)digit, 0);
}

/* ── string ops (lods/stos/movs/cmps/scas/ins/outs) + rep prefix ── */
static void do_strops(const char *name) {
    /* [F2/F3] opcode ; 66 untuk 16-bit ; 48 (REX.W) untuk qword */
    int base = 0;
    int w_byte = 0;   /* opcode dgn 66/REX.W */
    const char *s = name;
    if (strncmp(s, "lods", 4) == 0) { base = 0xAC; s += 4; }
    else if (strncmp(s, "stos", 4) == 0) { base = 0xAA; s += 4; }
    else if (strncmp(s, "movs", 4) == 0) { base = 0xA4; s += 4; }
    else if (strncmp(s, "cmps", 4) == 0) { base = 0xA6; s += 4; }
    else if (strncmp(s, "scas", 4) == 0) { base = 0xAE; s += 4; }
    else if (strncmp(s, "ins", 3) == 0) { base = 0x6C; s += 3; }
    else if (strncmp(s, "outs", 4) == 0) { base = 0x6E; s += 4; }
    else return;
    /* opcode = base untuk byte, base+1 untuk word/dword/qword;
     * prefix 66 untuk word, 48 (REX.W) untuk qword */
    int suffix;
    if (*s == 'b') { suffix = 0; w_byte = 0; }
    else if (*s == 'w') { suffix = 1; w_byte = 0x66; }
    else if (*s == 'd') { suffix = 1; w_byte = 0; }
    else if (*s == 'q') { suffix = 1; w_byte = 0x48; }
    else { err("bad string op '%s'", name); return; }
    if (g_rep_prefix) { emit_byte((uint8_t)g_rep_prefix); g_rep_prefix = 0; }
    if (w_byte) emit_byte((uint8_t)w_byte);
    emit_byte((uint8_t)(base + suffix));
}

/* ── bit ops: bt/bts/btr/btc ── */
static void do_bit(const char *name, opnd_t a, opnd_t b) {
    int op, digit;
    if (strcmp(name, "bt") == 0) { op = 0xA3; digit = 4; }
    else if (strcmp(name, "bts") == 0) { op = 0xAB; digit = 5; }
    else if (strcmp(name, "btr") == 0) { op = 0xB3; digit = 6; }
    else if (strcmp(name, "btc") == 0) { op = 0xBB; digit = 7; }
    else return;
    if (b.kind == OP_REG) {
        emit_rex_rm(&a, b.reg, a.size == 8);
        emit_byte(0x0F); emit_byte((uint8_t)op);
        emit_rm(&a, b.reg, 0);
        return;
    }
    if (b.kind == OP_IMM && !b.has_sym) {
        emit_rex_rm(&a, (uint8_t)digit, a.size == 8);
        emit_byte(0x0F); emit_byte(0xBA);
        emit_rm(&a, (uint8_t)digit, 0);
        emit_byte((uint8_t)b.imm);
        return;
    }
    err("bad %s operands", name);
}

/* ── rdrand/rdseed: r32/r64 (0F C7 /6, /7) ── */
static void do_rdrand(const char *name, opnd_t a) {
    if (a.kind != OP_REG || (a.size != 4 && a.size != 8)) { err("bad %s", name); return; }
    int digit = strcmp(name, "rdrand") == 0 ? 6 : 7;
    emit_rex_rm(&a, (uint8_t)digit, a.size == 8);
    emit_byte(0x0F); emit_byte(0xC7);
    emit_rm(&a, (uint8_t)digit, 0);
}

/* ── clflush (0F AE /7), clflushopt (66 0F AE /6), prefetch* (0F 18 /0..3) ── */
static void do_cache(const char *name, opnd_t a) {
    if (a.kind != OP_MEM) { err("bad %s operand", name); return; }
    int digit, op, pref66 = 0;
    if (strcmp(name, "clflush") == 0) { digit = 7; op = 0xAE; }
    else if (strcmp(name, "clflushopt") == 0) { digit = 6; op = 0xAE; pref66 = 1; }
    else if (strcmp(name, "prefetcht0") == 0) { digit = 1; op = 0x18; }
    else if (strcmp(name, "prefetcht1") == 0) { digit = 2; op = 0x18; }
    else if (strcmp(name, "prefetcht2") == 0) { digit = 3; op = 0x18; }
    else if (strcmp(name, "prefetchnta") == 0) { digit = 0; op = 0x18; }
    else return;
    if (pref66) emit_byte(0x66);
    emit_rex_rm(&a, (uint8_t)digit, 0);
    emit_byte(0x0F);
    emit_byte((uint8_t)op);
    emit_rm(&a, (uint8_t)digit, 0);
}

/* ── bsf/bsr ── */
static void do_bsf_bsr(const char *name, opnd_t a, opnd_t b) {
    if (a.kind != OP_REG || (b.kind != OP_REG && b.kind != OP_MEM)) {
        err("bad %s operands", name); return;
    }
    emit_rex_rm(&b, a.reg, a.size == 8);
    emit_byte(0x0F);
    emit_byte(strcmp(name, "bsf") == 0 ? 0xBC : 0xBD);
    emit_rm(&b, a.reg, 0);
}

/* ── bswap r32/r64 ── */
static void do_bswap(opnd_t a) {
    if (a.kind != OP_REG || (a.size != 4 && a.size != 8)) { err("bad bswap"); return; }
    if (a.size == 8) emit_rex(1, a.reg >= 8 ? 1 : 0, 0, a.reg >= 8 ? 1 : 0);
    else if (a.reg >= 8) emit_rex(0, 0, 0, 1);
    emit_byte(0x0F);
    emit_byte(0xC8 + (a.reg & 7));
}

/* ── cmpxchg / xadd ── */
static void do_cmpxchg(const char *name, opnd_t a, opnd_t b) {
    if (b.kind != OP_REG || (a.kind != OP_REG && a.kind != OP_MEM)) {
        err("bad %s operands", name); return;
    }
    int w = b.size == 8;
    emit_rex_rm(&a, b.reg, w);
    emit_byte(0x0F);
    if (strcmp(name, "cmpxchg") == 0) emit_byte(b.size == 1 ? 0xB0 : 0xB1);
    else emit_byte(b.size == 1 ? 0xC0 : 0xC1);
    emit_rm(&a, b.reg, 0);
}

/* ── cmpxchg8b/cmpxchg16b m64/m128 ── */
static void do_cmpxchg8b(const char *name, opnd_t a) {
    if (a.kind != OP_MEM) { err("bad %s operand", name); return; }
    int is16 = strcmp(name, "cmpxchg16b") == 0;
    emit_rex_rm(&a, 1, is16);
    emit_byte(0x0F); emit_byte(0xC7);
    emit_rm(&a, 1, 0);
}

/* ── loop / loope / loopne / jrcxz / jecxz (rel8) ── */
static void do_loop(const char *name, opnd_t a) {
    uint8_t op;
    int use_67 = 0;
    if (strcmp(name, "loop") == 0) op = 0xE2;
    else if (strcmp(name, "loope") == 0 || strcmp(name, "loopz") == 0) op = 0xE1;
    else if (strcmp(name, "loopne") == 0 || strcmp(name, "loopnz") == 0) op = 0xE0;
    else if (strcmp(name, "jrcxz") == 0) op = 0xE3;
    else if (strcmp(name, "jecxz") == 0) { op = 0xE3; use_67 = 1; }
    else return;
    if (a.kind != OP_IMM) { err("bad %s", name); return; }
    if (use_67) emit_byte(0x67);
    emit_byte(op);
    if (a.has_sym) emit_rel8_sym(a.sym);
    else emit_rel8_to(a.imm);
}

/* ── shld / shrd: r/m, reg, imm8|cl ── */
static void do_shld(const char *name, opnd_t a, opnd_t b, opnd_t c) {
    if ((a.kind != OP_REG && a.kind != OP_MEM) || b.kind != OP_REG) {
        err("bad %s operands", name); return;
    }
    int w = a.size == 8;
    if (c.kind == OP_REG && c.reg == 1 && c.size == 1) { /* cl */
        emit_rex_rm(&a, b.reg, w);
        emit_byte(0x0F);
        emit_byte(strcmp(name, "shld") == 0 ? 0xA5 : 0xAD);
        emit_rm(&a, b.reg, 0);
        return;
    }
    if (c.kind == OP_IMM && !c.has_sym) {
        emit_rex_rm(&a, b.reg, w);
        emit_byte(0x0F);
        emit_byte(strcmp(name, "shld") == 0 ? 0xA4 : 0xAC);
        emit_rm(&a, b.reg, 0);
        emit_byte((uint8_t)c.imm);
        return;
    }
    err("bad %s shift count", name);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Data directives
 * ═══════════════════════════════════════════════════════════════════════════ */
static int is_ident_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

static int emit_data(int width, const char *args) {
    const char *p = args;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        if (*p == 0) break;
        if (*p == '\'' || *p == '"') {
            char q = *p++;
            while (*p && *p != q) {
                if (width == 1) emit_byte((uint8_t)*p);
                else {
                    uint64_t v = (uint8_t)*p;
                    for (int i = 0; i < width; i++) emit_byte((uint8_t)(v >> (8 * i)));
                }
                p++;
            }
            if (*p == q) p++;
            continue;
        }
        /* ekspresi sampai koma */
        const char *t0 = p;
        int depth = 0;
        while (*p) {
            if (*p == '(') depth++;
            else if (*p == ')') depth--;
            else if (*p == ',' && depth <= 0) break;
            p++;
        }
        char tmp[256]; int tl = (int)(p - t0);
        if (tl > 255) tl = 255;
        memcpy(tmp, t0, tl); tmp[tl] = 0;

        /* simbol murni? (bukan angka, tidak diawali digit) */
        int only_ident = 1;
        if (tmp[0] >= '0' && tmp[0] <= '9') only_ident = 0;
        for (char *q = tmp; *q && only_ident; q++)
            if (!is_ident_char(*q) && *q != '.') { only_ident = 0; break; }
        if (only_ident && tmp[0]) {
            char name[64]; strncpy(name, tmp, sizeof(name) - 1); name[sizeof(name)-1] = 0;
            int64_t a = sym_addr(name);
            if (a == -3) {
                /* BERRY_IMPORT: external; emit a reloc in every pass. */
                emit_reloc_abs(width == 8 ? 8 : 4);
                for (int i = 0; i < width; i++) emit_byte(0);
                continue;
            }
            if (a == -2) {
                if (g_pass == 1) {
                    emit_reloc_abs(width == 8 ? 8 : 4);
                    for (int i = 0; i < width; i++) emit_byte(0);
                } else {
                    err("symbol '%s' unresolved", name);
                }
                continue;
            }
            if (a == -1) { err("forward ref '%s'", name); return -1; }
            if (width == 8) emit_reloc_abs(8);
            else emit_reloc_abs(4);
            for (int i = 0; i < width; i++) emit_byte((uint8_t)(a >> (8 * i)));
            continue;
        }
        int ok = 1;
        int64_t v = eval_expr(tmp, &ok);
        if (!ok) return -1;
        for (int i = 0; i < width; i++) emit_byte((uint8_t)(v >> (8 * i)));
    }
    return 0;
}

static void do_align(const char *args) {
    int ok = 1;
    int64_t a = eval_expr(args, &ok);
    if (!ok) { err("bad align"); return; }
    if (a <= 0) a = 16;
    sect_t *s = (g_cur_sect >= 0) ? &g_sects[g_cur_sect] : 0;
    if (!s) { err("align outside section"); return; }
    while (s->size % a) emit_byte(0);
}

static void do_res(const char *args, int width) {
    int ok = 1;
    int64_t n = eval_expr(args, &ok);
    if (!ok) { err("bad res"); return; }
    for (int64_t i = 0; i < n * width; i++) emit_byte(0);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Line parser
 * ═══════════════════════════════════════════════════════════════════════════ */
static void strip_comment(char *line) {
    int in_str = 0;
    char q = 0;
    for (char *p = line; *p; p++) {
        if (in_str) { if (*p == q) in_str = 0; continue; }
        if (*p == '\'' || *p == '"') { in_str = 1; q = *p; continue; }
        if (*p == ';') { *p = 0; break; }
    }
    /* buang trailing whitespace / newline */
    size_t len = strlen(line);
    while (len > 0 && (line[len-1] == ' ' || line[len-1] == '\t' ||
                       line[len-1] == '\r' || line[len-1] == '\n')) line[--len] = 0;
}

/* ── HOOK AI: peephole model-driven.  Dipanggil setelah operand ter-parse,
 * SEBELUM dispatch mnemonic.  Mengembalikan 1 bila transformasi diterapkan
 * (instruksi sudah di-emit), 0 = lanjut jalur deterministik normal.          */
static int bcm_opt_hook(const char *mnem, opnd_t a, opnd_t b, opnd_t c) {
    if (!g_enai || !g_bcm.loaded) return 0;
    if (g_oai_level <= 0) return 0;
    /* bangun pola: mnem|dst_kind|src_kind|imm_class — immediate boleh di
     * operand ke-2 (mov r,5) atau ke-3 (imul r,r,8); pilih yang IMM. */
    char pat[96];
    const char *dk = a.kind == OP_REG ? "REG" : (a.kind == OP_MEM ? "MEM" : (a.kind == OP_IMM ? "IMM" : "0"));
    const opnd_t *imm_op = 0;
    int64_t imm_val = 0;
    const char *sk;
    if (b.kind == OP_IMM) { sk = "IMM"; imm_op = &b; imm_val = b.imm; }
    else if (c.kind == OP_IMM) { sk = "REG"; imm_op = &c; imm_val = c.imm; }   /* imul r,r,imm */
    else sk = b.kind == OP_REG ? "REG" : (b.kind == OP_MEM ? "MEM" : "0");
    const char *ic = "0";
    if (imm_op && !imm_op->has_sym) {
        int64_t v = imm_op->imm;
        if (v >= -1 && v <= 1) ic = "01";
        else if (v >= -8 && v <= 7) ic = "8";
        else if (v >= -128 && v <= 127) ic = "8";
        else if (v >= -32768 && v <= 32767) ic = "16";
        else ic = "32";
    }
    snprintf(pat, sizeof pat, "%s|%s|%s|%s", mnem, dk, sk, ic);
    uint32_t h = bcm_fnv1a32(pat);
    const bcm_pattern_t *p = bcm_find(h);
    if (!p) { g_ai_misses++; return 0; }
    if (p->confidence < bcm_threshold(g_oai_level)) { g_ai_misses++; return 0; }
    if ((int)p->min_oai > g_oai_level) { g_ai_misses++; return 0; }
    /* bobot negatif = transformasi dihindari (dipelajari dari corpus) */
    if (p->weight < 0 && (bcm_rand() & 0xFF) < (uint32_t)(-p->weight)) { g_ai_misses++; return 0; }
    g_ai_hits++;
    /* terapkan transformasi lewat emitter yang sudah ada (guard ketat) */
    switch (p->transform_id) {
    case 1:  /* add r,r → shl r,1 */
        if (strcmp(mnem, "add") == 0 && a.kind == OP_REG && b.kind == OP_REG &&
            a.reg == b.reg && a.size == b.size) {
            opnd_t one; memset(&one, 0, sizeof one); one.kind = OP_IMM; one.imm = 1;
            do_shift("shl", a, one);
            return 1;
        }
        break;
    case 2:  /* imul r,r,2^k → shl r,k (imm di b atau c) */
        if (strcmp(mnem, "imul") == 0 && a.kind == OP_REG &&
            imm_op && !imm_op->has_sym && imm_val > 0 &&
            (imm_val & (imm_val - 1)) == 0) {
            int64_t k = 0, v = imm_val;
            while (v > 1) { v >>= 1; k++; }
            opnd_t sh; memset(&sh, 0, sizeof sh); sh.kind = OP_IMM; sh.imm = k;
            do_shift("shl", a, sh);
            return 1;
        }
        break;
    case 4:  /* mov r,0 → xor r,r */
        if (strcmp(mnem, "mov") == 0 && a.kind == OP_REG &&
            b.kind == OP_IMM && !b.has_sym && b.imm == 0) {
            do_alu(&g_alu[6], a, a);   /* g_alu[6] = xor */
            return 1;
        }
        break;
    case 8:  /* lea r,[r+1] → inc r */
        if (strcmp(mnem, "lea") == 0 && a.kind == OP_REG && b.kind == OP_MEM &&
            b.has_base && !b.has_index && b.base == a.reg && b.disp == 1 &&
            !b.rip_rel && !b.has_sym) {
            do_unary("inc", a, b);
            return 1;
        }
        break;
    default:
        break;   /* transformasi lain: emisi deterministik (aman) */
    }
    /* transformasi dikenal tapi guard gagal → hitungan jujur */
    g_ai_hits--;
    g_ai_misses++;
    return 0;
}

static void parse_line(char *line) {
    strip_comment(line);
    char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == 0) return;

    /* "name equ expr" atau "name = expr" — cek SEBELUM label (tanpa ':') */
    {
        char *save = p;
        char name[64];
        int n = 0;
        while (is_ident_char(*p) && n < 63) name[n++] = *p++;
        name[n] = 0;
        while (*p == ' ' || *p == '\t') p++;
        if (n > 0 && (strncmp(p, "equ", 3) == 0 || *p == '=')) {
            if (strncmp(p, "equ", 3) == 0) p += 3;
            else p += 1;
            while (*p == ' ' || *p == '\t') p++;
            int ok = 1;
            int64_t v = eval_expr(p, &ok);
            if (!ok) { err("bad equ"); return; }
            define_const(name, (uint64_t)v);
            return;
        }
        p = save;
    }

    /* label implisit: "name db ..." / "name dq ..." (tanpa ':') */
    {
        char *save = p;
        char nm[64]; int nn = 0;
        while (is_ident_char(*p) && nn < 63) nm[nn++] = *p++;
        nm[nn] = 0;
        while (*p == ' ' || *p == '\t') p++;
        /* ekstrak token direktif berikutnya */
        char dname[32]; int dn = 0;
        while (is_ident_char(*p) && dn < 31) dname[dn++] = *p++;
        dname[dn] = 0;
        int is_data_dir = (strcmp(dname, "db") == 0 || strcmp(dname, "dw") == 0 ||
                           strcmp(dname, "dd") == 0 || strcmp(dname, "dq") == 0 ||
                           strcmp(dname, "resb") == 0 || strcmp(dname, "resw") == 0 ||
                           strcmp(dname, "resd") == 0 || strcmp(dname, "resq") == 0 ||
                           strcmp(dname, "align") == 0);
        if (nn > 0 && is_data_dir) {
            /* label implisit: konsumsi nama, arahkan p ke direktif */
            define_label(nm);
            p = save + nn;   /* p = posisi direktif (spasi dilewati di parse_mnemonic) */
            goto parse_mnemonic;
        }
        p = save;
    }

    /* label: name: */
    {
        char *colon = strchr(p, ':');
        if (colon) {
            int is_label = 1;
            for (char *q = p; q < colon; q++)
                if (!is_ident_char(*q) && *q != '.') { is_label = 0; break; }
            if (is_label) {
                char name[64]; int n = (int)(colon - p);
                if (n > 63) n = 63;
                memcpy(name, p, n); name[n] = 0;
                define_label(name);
                p = colon + 1;
                while (*p == ' ' || *p == '\t') p++;
                if (*p == 0) return;
            }
        }
    }

parse_mnemonic:
    while (*p == ' ' || *p == '\t') p++;
    /* mnemonic */
    char mnem[64]; int n = 0;
    while (is_ident_char(*p) && n < 63) mnem[n++] = *p++;
    mnem[n] = 0;
    while (*p == ' ' || *p == '\t') p++;
    const char *args = p;

    /* rep prefix: rep/repe/repz/repne/repnz → lanjut instruksi berikutnya */
    if (strcmp(mnem, "rep") == 0 || strcmp(mnem, "repe") == 0 || strcmp(mnem, "repz") == 0) {
        g_rep_prefix = 0xF3;
        p = (char*)args;
        goto parse_mnemonic;
    }
    if (strcmp(mnem, "repne") == 0 || strcmp(mnem, "repnz") == 0) {
        g_rep_prefix = 0xF2;
        p = (char*)args;
        goto parse_mnemonic;
    }
    /* lock prefix → F0 sebelum instruksi berikutnya */
    if (strcmp(mnem, "lock") == 0) {
        g_lock_prefix = 1;
        p = (char*)args;
        goto parse_mnemonic;
    }

    /* directives */
    if (strcmp(mnem, "section") == 0 || strcmp(mnem, "SECTION") == 0) {
        char name[32]; int m = 0;
        const char *q = args;
        if (*q == '\'') { q++; while (*q && *q != '\'' && m < 31) name[m++] = *q++; }
        else { while (*q && *q != ' ' && *q != '\t' && m < 31) name[m++] = *q++; }
        name[m] = 0;
        g_cur_sect = sect_find_or_add(name);
        g_sects[g_cur_sect].seen = 1;
        return;
    }

    if (strcmp(mnem, ".arch") == 0 || strcmp(mnem, "arch") == 0) {
        char a[32]; int m = 0;
        while ((args[m] >= 'a' && args[m] <= 'z') || (args[m] >= 'A' && args[m] <= 'Z') || (args[m] >= '0' && args[m] <= '9') || args[m] == '_') {
            if (m < 31) a[m] = args[m]; m++;
        }
        a[m] = 0;
        if (strcmp(a, "x86_64") == 0 || strcmp(a, "amd64") == 0) { g_arch = ARCH_X86_64; g_bits = 64; }
        else if (strcmp(a, "x86_32") == 0 || strcmp(a, "i386") == 0) { g_arch = ARCH_X86_32; g_bits = 32; }
        else if (strcmp(a, "x86_16") == 0 || strcmp(a, "i8086") == 0) { g_arch = ARCH_X86_16; g_bits = 16; }
        else if (strcmp(a, "arm32") == 0 || strcmp(a, "arm") == 0) { g_arch = ARCH_ARM32; g_bits = 32; }
        else if (strcmp(a, "arm64") == 0 || strcmp(a, "aarch64") == 0) { g_arch = ARCH_ARM64; g_bits = 64; }
        else if (strcmp(a, "riscv32") == 0 || strcmp(a, "rv32") == 0) { g_arch = ARCH_RISCV32; g_bits = 32; }
        else if (strcmp(a, "riscv64") == 0 || strcmp(a, "rv64") == 0) { g_arch = ARCH_RISCV64; g_bits = 64; }
        else if (strcmp(a, "riscv128") == 0 || strcmp(a, "rv128") == 0) { g_arch = ARCH_RISCV128; g_bits = 128; }
        else { err("unknown architecture '%s'", a); }
        return;
    }

    if (strcmp(mnem, "format") == 0) {
        char f[32]; int m = 0;
        while (is_ident_char(*args) && m < 31) f[m++] = *args++;
        f[m] = 0;
        if (f[0]) g_out_format = f[0] == 'B' || f[0] == 'b' ? (strncmp(f, "BEFB", 4) == 0 || strncmp(f, "befb", 4) == 0 ? "befb" : "bex") : f;
        return;
    }
    if (strcmp(mnem, "db") == 0) { emit_data(1, args); return; }
    if (strcmp(mnem, "dw") == 0) { emit_data(2, args); return; }
    if (strcmp(mnem, "dd") == 0) { emit_data(4, args); return; }
    if (strcmp(mnem, "dq") == 0) { emit_data(8, args); return; }
    if (strcmp(mnem, "align") == 0) { do_align(args); return; }
    if (strcmp(mnem, "resb") == 0) { do_res(args, 1); return; }
    if (strcmp(mnem, "resw") == 0) { do_res(args, 2); return; }
    if (strcmp(mnem, "resd") == 0) { do_res(args, 4); return; }
    if (strcmp(mnem, "resq") == 0) { do_res(args, 8); return; }
    if (strcmp(mnem, "BERRY_EXPORT") == 0) {
        char sym[64]; int m = 0;
        while (is_ident_char(*args) && m < 63) sym[m++] = *args++;
        sym[m] = 0;
        int flags = 1;
        while (*args == ' ' || *args == '\t' || *args == ',') args++;
        if (*args) {
            int ok = 1;
            int64_t f = eval_expr(args, &ok);
            if (ok) flags = (int)f;
        }
        if (m == 0) { err("BERRY_EXPORT needs a symbol"); return; }
        int64_t a = sym_addr(sym);
        if (a < 0) { err("export '%s' unresolved", sym); return; }
        if (g_export_count >= MAX_EXPORTS) { err("too many exports"); return; }
        g_exports[g_export_count].hash = fnv1a_32(sym);
        g_exports[g_export_count].entry_off = (uint32_t)a;
        g_exports[g_export_count].flags = (uint16_t)flags;
        g_export_count++;
        return;
    }

    if (strcmp(mnem, "BERRY_IMPORT") == 0) {
        /* BERRY_IMPORT mbjs_berry_init, mbjs_berry_eval_binary, ...
         * Declares external symbols the OS loader resolves against another
         * module (the MBJS runtime). References emit .cherryreva relocs. */
        char sym[64]; int m = 0;
        const char *p = args;
        while (1) {
            while (*p == ' ' || *p == '\t' || *p == ',') p++;
            m = 0;
            while (is_ident_char(*p) && m < 63) sym[m++] = *p++;
            sym[m] = 0;
            if (m == 0) break;
            mark_extern(sym);
        }
        return;
    }
    /* instructions: parse operands (hingga 3: shld/shrd r/m, reg, imm8|cl) */
    opnd_t a, b, c;
    memset(&a, 0, sizeof(a)); memset(&b, 0, sizeof(b)); memset(&c, 0, sizeof(c));
    int used = 0;
    if (*args) {
        used = parse_operand(args, &a);
        if (used > 0) {
            const char *rest = args + used;
            while (*rest == ' ' || *rest == '\t') rest++;
            if (*rest == ',') {
                rest++;
                used = parse_operand(rest, &b);
                const char *rest2 = rest;
                while (*rest2 == ' ' || *rest2 == '\t') rest2++;
                while (*rest2 && *rest2 != ',') rest2++;   /* lewati isi b */
                while (*rest2 == ' ' || *rest2 == '\t') rest2++;
                if (*rest2 == ',') { rest2++; parse_operand(rest2, &c); }
            }
        }
    }
    if (g_error) return;

    /* lock prefix: F0 di-emit sebelum instruksi (valid utk atomics/alu mem) */
    if (g_lock_prefix) { emit_byte(0xF0); g_lock_prefix = 0; }

    /* ── TRACK AI: peephole model-driven (hanya jika -EnAI && model) ── */
    if (bcm_opt_hook(mnem, a, b, c)) return;

    /* no-operand */
    if (strcmp(mnem, "ret") == 0) { emit_byte(0xC3); return; }
    if (strcmp(mnem, "nop") == 0) { emit_byte(0x90); return; }
    if (strcmp(mnem, "hlt") == 0) { emit_byte(0xF4); return; }
    if (strcmp(mnem, "cli") == 0) { emit_byte(0xFA); return; }
    if (strcmp(mnem, "sti") == 0) { emit_byte(0xFB); return; }
    if (strcmp(mnem, "cld") == 0) { emit_byte(0xFC); return; }
    if (strcmp(mnem, "std") == 0) { emit_byte(0xFD); return; }
    if (strcmp(mnem, "syscall") == 0) { emit_byte(0x0F); emit_byte(0x05); return; }
    if (strcmp(mnem, "sysret") == 0) { emit_byte(0x0F); emit_byte(0x07); return; }
    if (strcmp(mnem, "sysenter") == 0) { emit_byte(0x0F); emit_byte(0x34); return; }
    if (strcmp(mnem, "sysexit") == 0) { emit_byte(0x0F); emit_byte(0x35); return; }
    if (strcmp(mnem, "int3") == 0) { emit_byte(0xCC); return; }
    if (strcmp(mnem, "rdmsr") == 0) { emit_byte(0x0F); emit_byte(0x32); return; }
    if (strcmp(mnem, "wrmsr") == 0) { emit_byte(0x0F); emit_byte(0x30); return; }
    if (strcmp(mnem, "rdpmc") == 0) { emit_byte(0x0F); emit_byte(0x33); return; }
    if (strcmp(mnem, "rdtsc") == 0) { emit_byte(0x0F); emit_byte(0x31); return; }
    if (strcmp(mnem, "rdtscp") == 0) { emit_byte(0x0F); emit_byte(0x01); emit_byte(0xF9); return; }
    if (strcmp(mnem, "cpuid") == 0) { emit_byte(0x0F); emit_byte(0xA2); return; }
    if (strcmp(mnem, "swapgs") == 0) { emit_byte(0x0F); emit_byte(0x01); emit_byte(0xF8); return; }
    if (strcmp(mnem, "iretq") == 0) { emit_byte(0x48); emit_byte(0xCF); return; }
    if (strcmp(mnem, "wbinvd") == 0) { emit_byte(0x0F); emit_byte(0x09); return; }
    if (strcmp(mnem, "invd") == 0) { emit_byte(0x0F); emit_byte(0x08); return; }
    if (strcmp(mnem, "pause") == 0) { emit_byte(0xF3); emit_byte(0x90); return; }
    if (strcmp(mnem, "mfence") == 0) { emit_byte(0x0F); emit_byte(0xAE); emit_byte(0xF0); return; }
    if (strcmp(mnem, "lfence") == 0) { emit_byte(0x0F); emit_byte(0xAE); emit_byte(0xE8); return; }
    if (strcmp(mnem, "sfence") == 0) { emit_byte(0x0F); emit_byte(0xAE); emit_byte(0xF8); return; }
    if (strcmp(mnem, "xlatb") == 0) { emit_byte(0xD7); return; }
    if (strcmp(mnem, "lahf") == 0) { emit_byte(0x9F); return; }
    if (strcmp(mnem, "sahf") == 0) { emit_byte(0x9E); return; }
    if (strcmp(mnem, "clc") == 0) { emit_byte(0xF8); return; }
    if (strcmp(mnem, "stc") == 0) { emit_byte(0xF9); return; }
    if (strcmp(mnem, "cmc") == 0) { emit_byte(0xF5); return; }
    if (strcmp(mnem, "endbr64") == 0) { emit_byte(0xF3); emit_byte(0x0F); emit_byte(0x1E); emit_byte(0xFA); return; }
    if (strcmp(mnem, "xgetbv") == 0) { emit_byte(0x0F); emit_byte(0x01); emit_byte(0xD0); return; }
    if (strcmp(mnem, "xsetbv") == 0) { emit_byte(0x0F); emit_byte(0x01); emit_byte(0xD1); return; }
    if (strcmp(mnem, "clac") == 0) { emit_byte(0x0F); emit_byte(0x01); emit_byte(0xCA); return; }
    if (strcmp(mnem, "stac") == 0) { emit_byte(0x0F); emit_byte(0x01); emit_byte(0xCB); return; }
    if (strcmp(mnem, "ud2") == 0) { emit_byte(0x0F); emit_byte(0x0B); return; }
    if (strcmp(mnem, "cdq") == 0) { emit_byte(0x99); return; }
    if (strcmp(mnem, "cqo") == 0) { emit_rex(1, 0, 0, 0); emit_byte(0x99); return; }
    if (strcmp(mnem, "cwde") == 0) { emit_byte(0x98); return; }
    if (strcmp(mnem, "cdqe") == 0) { emit_rex(1, 0, 0, 0); emit_byte(0x98); return; }
    if (strcmp(mnem, "leave") == 0) { emit_byte(0xC9); return; }
    if (strcmp(mnem, "pushfq") == 0) { emit_byte(0x9C); return; }
    if (strcmp(mnem, "popfq") == 0) { emit_byte(0x9D); return; }
    if (strcmp(mnem, "int") == 0) {
        if (a.kind == OP_IMM && !a.has_sym) { emit_byte(0xCD); emit_byte((uint8_t)a.imm); return; }
        err("bad int"); return;
    }

    if (strcmp(mnem, "mov") == 0) { do_mov(a, b); return; }
    if (strcmp(mnem, "lea") == 0) { do_lea(a, b); return; }
    if (strcmp(mnem, "push") == 0) { do_push(a, b); return; }
    if (strcmp(mnem, "pop") == 0) { do_pop(a, b); return; }
    if (strcmp(mnem, "call") == 0) { do_call(a, b); return; }
    if (strcmp(mnem, "jmp") == 0) { do_jmp(a, b); return; }
    if (strcmp(mnem, "imul") == 0) { do_imul(a, b); return; }
    if (strcmp(mnem, "test") == 0) { do_test(a, b); return; }
    if (strcmp(mnem, "xchg") == 0) { do_xchg(a, b); return; }
    if (strcmp(mnem, "movzx") == 0) { do_movx("movzx", a, b); return; }
    if (strcmp(mnem, "movsx") == 0) { do_movx("movsx", a, b); return; }

    for (size_t i = 0; i < sizeof(g_alu)/sizeof(g_alu[0]); i++) {
        if (strcmp(mnem, g_alu[i].name) == 0) { do_alu(&g_alu[i], a, b); return; }
    }

    if (jcc_code(mnem) >= 0) { do_jcc(mnem, a, b); return; }
    if (strcmp(mnem,"inc")==0||strcmp(mnem,"dec")==0||strcmp(mnem,"neg")==0||strcmp(mnem,"not")==0) { do_unary(mnem, a, b); return; }
    if (strcmp(mnem,"shl")==0||strcmp(mnem,"sal")==0||strcmp(mnem,"shr")==0||
        strcmp(mnem,"sar")==0||strcmp(mnem,"rol")==0||strcmp(mnem,"ror")==0) { do_shift(mnem, a, b); return; }
    if (setcc_code(mnem) >= 0) { do_setcc(mnem, a, b); return; }

    /* in/out (I/O ports) */
    if (strcmp(mnem, "in") == 0 || strcmp(mnem, "out") == 0) { do_io(mnem, a, b); return; }
    /* descriptor tables & invlpg */
    if (strcmp(mnem, "lgdt") == 0 || strcmp(mnem, "lidt") == 0 || strcmp(mnem, "sgdt") == 0 ||
        strcmp(mnem, "sidt") == 0 || strcmp(mnem, "lldt") == 0 || strcmp(mnem, "ltr") == 0 ||
        strcmp(mnem, "sldt") == 0 || strcmp(mnem, "str") == 0 || strcmp(mnem, "invlpg") == 0) {
        do_desc(mnem, a); return;
    }
    /* string ops */
    if (strncmp(mnem, "lods", 4) == 0 || strncmp(mnem, "stos", 4) == 0 ||
        strncmp(mnem, "movs", 4) == 0 || strncmp(mnem, "cmps", 4) == 0 ||
        strncmp(mnem, "scas", 4) == 0 || strncmp(mnem, "ins", 3) == 0 ||
        strncmp(mnem, "outs", 4) == 0) {
        do_strops(mnem); return;
    }
    /* bit ops */
    if (strcmp(mnem, "bt") == 0 || strcmp(mnem, "bts") == 0 ||
        strcmp(mnem, "btr") == 0 || strcmp(mnem, "btc") == 0) { do_bit(mnem, a, b); return; }
    if (strcmp(mnem, "bsf") == 0 || strcmp(mnem, "bsr") == 0) { do_bsf_bsr(mnem, a, b); return; }
    if (strcmp(mnem, "bswap") == 0) { do_bswap(a); return; }
    if (strcmp(mnem, "rdrand") == 0 || strcmp(mnem, "rdseed") == 0) { do_rdrand(mnem, a); return; }
    if (strcmp(mnem, "clflush") == 0 || strcmp(mnem, "clflushopt") == 0 ||
        strcmp(mnem, "prefetcht0") == 0 || strcmp(mnem, "prefetcht1") == 0 ||
        strcmp(mnem, "prefetcht2") == 0 || strcmp(mnem, "prefetchnta") == 0) { do_cache(mnem, a); return; }
    /* atomic */
    if (strcmp(mnem, "cmpxchg") == 0 || strcmp(mnem, "xadd") == 0) { do_cmpxchg(mnem, a, b); return; }
    if (strcmp(mnem, "cmpxchg8b") == 0 || strcmp(mnem, "cmpxchg16b") == 0) { do_cmpxchg8b(mnem, a); return; }
    /* loop / jrcxz */
    if (strcmp(mnem, "loop") == 0 || strcmp(mnem, "loope") == 0 || strcmp(mnem, "loopne") == 0 ||
        strcmp(mnem, "loopz") == 0 || strcmp(mnem, "loopnz") == 0 ||
        strcmp(mnem, "jrcxz") == 0 || strcmp(mnem, "jecxz") == 0) { do_loop(mnem, a); return; }
    /* shld/shrd (3 operand) */
    if (strcmp(mnem, "shld") == 0 || strcmp(mnem, "shrd") == 0) { do_shld(mnem, a, b, c); return; }


    /* Multi-architecture instruction dispatch */
    if (g_arch == ARCH_ARM32) {
        if (strcmp(mnem, "nop") == 0) { arm32_nop(); return; }
        if (strcmp(mnem, "ret") == 0) { arm32_bx_reg(14); return; }
        if (strcmp(mnem, "b") == 0 && a.kind == OP_IMM) { arm32_branch_imm(0, a.imm); return; }
        if (strcmp(mnem, "bl") == 0 && a.kind == OP_IMM) { arm32_branch_imm(1, a.imm); return; }
        if (strcmp(mnem, "bx") == 0 && a.kind == OP_REG) { arm32_bx_reg(a.reg); return; }
        if (strcmp(mnem, "mov") == 0 && a.kind == OP_REG && b.kind == OP_IMM) { arm32_dp_imm(0xD, a.reg, 0, b.imm & 0xFF, ((uint32_t)b.imm >> 8) & 0xF); return; }
        if (strcmp(mnem, "mov") == 0 && a.kind == OP_REG && b.kind == OP_REG) { arm32_dp_reg(0xD, a.reg, b.reg, 0, 0); return; }
        if (strcmp(mnem, "add") == 0 && a.kind == OP_REG && b.kind == OP_REG && c.kind == OP_IMM) { arm32_dp_imm(4, a.reg, b.reg, c.imm & 0xFF, 0); return; }
        if (strcmp(mnem, "add") == 0 && a.kind == OP_REG && b.kind == OP_REG) { arm32_dp_reg(4, a.reg, a.reg, b.reg, 0); return; }
        if (strcmp(mnem, "sub") == 0 && a.kind == OP_REG && b.kind == OP_REG && c.kind == OP_IMM) { arm32_dp_imm(2, a.reg, b.reg, c.imm & 0xFF, 0); return; }
        if (strcmp(mnem, "sub") == 0 && a.kind == OP_REG && b.kind == OP_REG) { arm32_dp_reg(2, a.reg, a.reg, b.reg, 0); return; }
        if (strcmp(mnem, "cmp") == 0 && a.kind == OP_REG && b.kind == OP_IMM) { arm32_dp_imm(10, 0, a.reg, b.imm & 0xFF, 0); return; }
        if (strcmp(mnem, "cmp") == 0 && a.kind == OP_REG && b.kind == OP_REG) { arm32_dp_reg(10, 0, a.reg, b.reg, 1); return; }
        if (strcmp(mnem, "cmn") == 0) { arm32_dp_reg(11, 0, a.reg, b.reg, 1); return; }
        if (strcmp(mnem, "tst") == 0) { arm32_dp_reg(8, 0, a.reg, b.reg, 1); return; }
        if (strcmp(mnem, "and") == 0) { arm32_dp_reg(0, a.reg, b.reg, c.kind==OP_REG?c.reg:0, 0); return; }
        if (strcmp(mnem, "orr") == 0) { arm32_dp_reg(12, a.reg, b.reg, c.kind==OP_REG?c.reg:0, 0); return; }
        if (strcmp(mnem, "eor") == 0) { arm32_dp_reg(1, a.reg, b.reg, c.kind==OP_REG?c.reg:0, 0); return; }
        if (strcmp(mnem, "mvn") == 0 && a.kind == OP_REG && b.kind == OP_IMM) { arm32_dp_imm(14, a.reg, 0, b.imm & 0xFF, ((uint32_t)b.imm >> 8) & 0xF); return; }
        if (strcmp(mnem, "mul") == 0 && a.kind == OP_REG && b.kind == OP_REG) { arm32_mul_reg(a.reg, a.reg, b.reg); return; }
        if (strcmp(mnem, "ldr") == 0 && a.kind == OP_REG && b.kind == OP_IMM) { arm32_ldr_pre(1, 1, 15, a.reg, b.imm); return; }
        if (strcmp(mnem, "ldr") == 0 && a.kind == OP_REG && b.kind == OP_MEM) { arm32_ldr_pre(1, 1, b.base, a.reg, b.disp); return; }
        if (strcmp(mnem, "str") == 0 && a.kind == OP_REG && b.kind == OP_MEM) { arm32_ldr_pre(1, 0, b.base, a.reg, b.disp); return; }
        if (strcmp(mnem, "swi") == 0 || strcmp(mnem, "svc") == 0) { arm32_swi(a.imm); return; }
        if (strcmp(mnem, "push") == 0 && a.kind == OP_REG) { arm32_ldr_pre(0, 0, 13, a.reg, -4); return; }
        if (strcmp(mnem, "pop") == 0 && a.kind == OP_REG) { arm32_ldr_pre(1, 1, 13, a.reg, 4); return; }
        if (strcmp(mnem, "wfi") == 0) { arm32_emit_u32(0xE320F003); return; }
        if (strcmp(mnem, "wfe") == 0) { arm32_emit_u32(0xE320F002); return; }
        if (strcmp(mnem, "sev") == 0) { arm32_emit_u32(0xE320F004); return; }
        if (strcmp(mnem, "isb") == 0) { arm32_emit_u32(0xF57FF06F); return; }
        if (strcmp(mnem, "dsb") == 0) { arm32_emit_u32(0xF57FF04F); return; }
        if (strcmp(mnem, "dmb") == 0) { arm32_emit_u32(0xF57FF05F); return; }
        if (strcmp(mnem, "clz") == 0 && a.kind == OP_REG) { arm32_emit_u32(0xE16F0F10 | ((uint32_t)a.reg<<12)); return; }
    }

    if (g_arch == ARCH_ARM64) {
        if (strcmp(mnem, "nop") == 0) { arm64_nop(); return; }
        if (strcmp(mnem, "ret") == 0) { arm64_br_reg(2, 30); return; }
        if (strcmp(mnem, "b") == 0 && a.kind == OP_IMM) { arm64_b_imm(a.imm / 4); return; }
        if (strcmp(mnem, "bl") == 0 && a.kind == OP_IMM) { arm64_bl_imm(a.imm / 4); return; }
        if (strcmp(mnem, "br") == 0 && a.kind == OP_REG) { arm64_br_reg(0, a.reg); return; }
        if (strcmp(mnem, "blr") == 0 && a.kind == OP_REG) { arm64_br_reg(1, a.reg); return; }
        if (strcmp(mnem, "svc") == 0 || strcmp(mnem, "swi") == 0) { arm64_svc(a.imm); return; }
        if (strcmp(mnem, "mov") == 0 && a.kind == OP_REG && b.kind == OP_IMM) {
            arm64_movw(1, 2, 0, b.imm & 0xFFFF, a.reg);
            if ((b.imm & ~0xFFFFL) != 0) arm64_movw(1, 3, 1, (b.imm>>16)&0xFFFF, a.reg);
            return;
        }
        if (strcmp(mnem, "mov") == 0 && a.kind == OP_REG && b.kind == OP_REG) {
            arm64_log_shift_reg(1, 1, a.reg, 31, b.reg, 0, 0); return;
        }
        if (strcmp(mnem, "add") == 0 && a.kind == OP_REG && b.kind == OP_REG && c.kind == OP_IMM) { arm64_addsub_imm(1, 0, 0, c.imm, b.reg, a.reg); return; }
        if (strcmp(mnem, "add") == 0 && a.kind == OP_REG && b.kind == OP_REG) { arm64_addsub_imm(1, 0, 0, 0, b.reg, a.reg); return; }
        if (strcmp(mnem, "sub") == 0 && a.kind == OP_REG && b.kind == OP_REG && c.kind == OP_IMM) { arm64_addsub_imm(1, 1, 0, c.imm, b.reg, a.reg); return; }
        if (strcmp(mnem, "sub") == 0 && a.kind == OP_REG && b.kind == OP_REG) { arm64_addsub_reg(1, 1, a.reg, b.reg, 0, 0, 0); return; }
        if (strcmp(mnem, "ldr") == 0 && a.kind == OP_REG && b.kind == OP_MEM) { arm64_ldrstr_uimm(3, 0, 1, b.disp/8, b.base, a.reg); return; }
        if (strcmp(mnem, "ldr") == 0 && a.kind == OP_REG && b.kind == OP_IMM) { arm64_ldrstr_uimm(3, 0, 1, b.imm/8, 0, a.reg); return; }
        if (strcmp(mnem, "str") == 0 && a.kind == OP_REG && b.kind == OP_MEM) { arm64_ldrstr_uimm(3, 0, 0, b.disp/8, b.base, a.reg); return; }
        if (strcmp(mnem, "cmp") == 0 && a.kind == OP_REG && b.kind == OP_REG) { arm64_addsub_reg(1, 1, 31, a.reg, b.reg, 0, 0); return; }
        if (strcmp(mnem, "cmp") == 0 && a.kind == OP_REG && b.kind == OP_IMM) { arm64_addsub_imm(1, 1, 0, b.imm, a.reg, 31); return; }
        if (strcmp(mnem, "and") == 0 && a.kind == OP_REG && b.kind == OP_REG) { arm64_log_shift_reg(1, 0, a.reg, b.reg, c.kind==OP_REG?c.reg:0, 0, 0); return; }
        if (strcmp(mnem, "orr") == 0 && a.kind == OP_REG && b.kind == OP_REG) { arm64_log_shift_reg(1, 1, a.reg, b.reg, c.kind==OP_REG?c.reg:0, 0, 0); return; }
        if (strcmp(mnem, "eor") == 0 && a.kind == OP_REG && b.kind == OP_REG) { arm64_log_shift_reg(1, 2, a.reg, b.reg, c.kind==OP_REG?c.reg:0, 0, 0); return; }
        if (strcmp(mnem, "push") == 0 && a.kind == OP_REG) { arm64_addsub_imm(1, 1, 0, 16, 31, 31); arm64_ldrstr_uimm(3, 0, 0, 0, 31, a.reg); return; }
        if (strcmp(mnem, "pop") == 0 && a.kind == OP_REG) { arm64_ldrstr_uimm(3, 0, 1, 0, 31, a.reg); arm64_addsub_imm(1, 0, 0, 16, 31, 31); return; }
        if (strcmp(mnem, "adrp") == 0 && a.kind == OP_REG && b.kind == OP_IMM) { arm64_adrp((b.imm >> 12) & 3, (b.imm >> 14) & 0x7FFFF, a.reg); return; }
        if (strcmp(mnem, "wfi") == 0) { arm64_emit_u32(0xD503207F); return; }
        if (strcmp(mnem, "wfe") == 0) { arm64_emit_u32(0xD503205F); return; }
        if (strcmp(mnem, "sev") == 0) { arm64_emit_u32(0xD503209F); return; }
        if (strcmp(mnem, "isb") == 0) { arm64_emit_u32(0xD5033FDF); return; }
        if (strcmp(mnem, "dsb") == 0) { arm64_emit_u32(0xD5033F9F); return; }
        if (strcmp(mnem, "dmb") == 0) { arm64_emit_u32(0xD5033FBF); return; }
    }

    if (g_arch == ARCH_RISCV32 || g_arch == ARCH_RISCV64 || g_arch == ARCH_RISCV128) {
        uint32_t opc_op   = 0x33;
        uint32_t opc_opw  = 0x3B;
        uint32_t opc_imm  = 0x13;
        uint32_t opc_immw = 0x1B;
        uint32_t opc_load = 0x03;
        uint32_t opc_store= 0x23;
        uint32_t opc_branch=0x63;
        uint32_t opc_jalr = 0x67;
        uint32_t opc_jal  = 0x6F;
        uint32_t opc_lui  = 0x37;
        uint32_t opc_auipc= 0x17;
        uint32_t opc_system=0x73;

        if (strcmp(mnem, "nop") == 0) { rv_i(0, 0, 0, 0, opc_imm); return; }
        if (strcmp(mnem, "ret") == 0) { rv_i(0, 1, 0, 0, opc_jalr); return; }
        if (strcmp(mnem, "mv") == 0 && a.kind == OP_REG && b.kind == OP_REG) { rv_i(0, b.reg, 0, a.reg, opc_imm); return; }
        if (strcmp(mnem, "not") == 0 && a.kind == OP_REG && b.kind == OP_REG) { rv_i(-1, b.reg, 7, a.reg, opc_imm); return; }
        if (strcmp(mnem, "neg") == 0 && a.kind == OP_REG && b.kind == OP_REG) { rv_r(0x20, b.reg, 0, 0, a.reg, opc_op); return; }
        if (strcmp(mnem, "li") == 0 && a.kind == OP_REG && b.kind == OP_IMM) { rv_i(b.imm & 0xFFF, 0, 0, a.reg, opc_imm); return; }
        if (strcmp(mnem, "j") == 0 && a.kind == OP_IMM) { rv_j(a.imm, 0, opc_jal); return; }
        if (strcmp(mnem, "jr") == 0 && a.kind == OP_REG) { rv_i(0, a.reg, 0, 0, opc_jalr); return; }
        if (strcmp(mnem, "call") == 0 && a.kind == OP_IMM) { rv_j(a.imm, 1, opc_jal); return; }
        if (strcmp(mnem, "tail") == 0 && a.kind == OP_IMM) { rv_j(a.imm, 0, opc_jal); return; }

        /* ALU reg-reg */
        if (strcmp(mnem, "add") == 0 && a.kind == OP_REG && b.kind == OP_REG && c.kind == OP_REG) { rv_r(0, c.reg, b.reg, 0, a.reg, opc_op); return; }
        if (strcmp(mnem, "add") == 0 && a.kind == OP_REG && b.kind == OP_REG) { rv_r(0, b.reg, a.reg, 0, a.reg, opc_op); return; }
        if (strcmp(mnem, "sub") == 0 && a.kind == OP_REG && b.kind == OP_REG) { rv_r(0x20, b.reg, a.reg, 0, a.reg, opc_op); return; }
        if (strcmp(mnem, "and") == 0 && a.kind == OP_REG && b.kind == OP_REG) { rv_r(0, b.reg, a.reg, 7, a.reg, opc_op); return; }
        if (strcmp(mnem, "or") == 0 && a.kind == OP_REG && b.kind == OP_REG) { rv_r(0, b.reg, a.reg, 6, a.reg, opc_op); return; }
        if (strcmp(mnem, "xor") == 0 && a.kind == OP_REG && b.kind == OP_REG) { rv_r(0, b.reg, a.reg, 4, a.reg, opc_op); return; }
        if (strcmp(mnem, "sll") == 0 && a.kind == OP_REG && b.kind == OP_REG) { rv_r(0, b.reg, a.reg, 1, a.reg, opc_op); return; }
        if (strcmp(mnem, "srl") == 0 && a.kind == OP_REG && b.kind == OP_REG) { rv_r(0, b.reg, a.reg, 5, a.reg, opc_op); return; }
        if (strcmp(mnem, "sra") == 0 && a.kind == OP_REG && b.kind == OP_REG) { rv_r(0x20, b.reg, a.reg, 5, a.reg, opc_op); return; }
        if (strcmp(mnem, "slt") == 0 && a.kind == OP_REG && b.kind == OP_REG) { rv_r(0, b.reg, a.reg, 2, a.reg, opc_op); return; }
        if (strcmp(mnem, "sltu") == 0 && a.kind == OP_REG && b.kind == OP_REG) { rv_r(0, b.reg, a.reg, 3, a.reg, opc_op); return; }

        /* ALU reg-imm */
        if (strcmp(mnem, "addi") == 0 && a.kind == OP_REG && b.kind == OP_REG) { rv_i(c.imm, b.reg, 0, a.reg, opc_imm); return; }
        if (strcmp(mnem, "addi") == 0 && a.kind == OP_REG && b.kind == OP_IMM) { rv_i(b.imm, a.reg, 0, a.reg, opc_imm); return; }
        if (strcmp(mnem, "slti") == 0 && a.kind == OP_REG && b.kind == OP_REG) { rv_i(c.imm, b.reg, 2, a.reg, opc_imm); return; }
        if (strcmp(mnem, "sltiu") == 0 && a.kind == OP_REG && b.kind == OP_REG) { rv_i(c.imm, b.reg, 3, a.reg, opc_imm); return; }
        if (strcmp(mnem, "xori") == 0 && a.kind == OP_REG && b.kind == OP_REG) { rv_i(c.imm, b.reg, 4, a.reg, opc_imm); return; }
        if (strcmp(mnem, "ori") == 0 && a.kind == OP_REG && b.kind == OP_REG) { rv_i(c.imm, b.reg, 6, a.reg, opc_imm); return; }
        if (strcmp(mnem, "andi") == 0 && a.kind == OP_REG && b.kind == OP_REG) { rv_i(c.imm, b.reg, 7, a.reg, opc_imm); return; }
        if (strcmp(mnem, "slli") == 0 && a.kind == OP_REG && b.kind == OP_REG) { rv_i(c.imm & 0x1F, b.reg, 1, a.reg, opc_imm); return; }
        if (strcmp(mnem, "srli") == 0 && a.kind == OP_REG && b.kind == OP_REG) { rv_i(c.imm & 0x1F, b.reg, 5, a.reg, opc_imm); return; }
        if (strcmp(mnem, "srai") == 0 && a.kind == OP_REG && b.kind == OP_REG) { rv_i(0x400 | (c.imm & 0x1F), b.reg, 5, a.reg, opc_imm); return; }

        /* M extension */
        if (strcmp(mnem, "mul") == 0 && a.kind == OP_REG && b.kind == OP_REG) { rv_r(1, b.reg, a.reg, 0, a.reg, opc_op); return; }
        if (strcmp(mnem, "mulh") == 0) { rv_r(1, b.reg, a.reg, 1, a.reg, opc_op); return; }
        if (strcmp(mnem, "mulhu") == 0) { rv_r(1, b.reg, a.reg, 3, a.reg, opc_op); return; }
        if (strcmp(mnem, "div") == 0 && a.kind == OP_REG && b.kind == OP_REG) { rv_r(1, b.reg, a.reg, 4, a.reg, opc_op); return; }
        if (strcmp(mnem, "divu") == 0) { rv_r(1, b.reg, a.reg, 5, a.reg, opc_op); return; }
        if (strcmp(mnem, "rem") == 0 && a.kind == OP_REG && b.kind == OP_REG) { rv_r(1, b.reg, a.reg, 6, a.reg, opc_op); return; }
        if (strcmp(mnem, "remu") == 0) { rv_r(1, b.reg, a.reg, 7, a.reg, opc_op); return; }

        /* RV64I W-instructions */
        if (g_arch >= ARCH_RISCV64) {
            if (strcmp(mnem, "addw") == 0 && a.kind == OP_REG && b.kind == OP_REG) { rv_r(0, b.reg, a.reg, 0, a.reg, opc_opw); return; }
            if (strcmp(mnem, "subw") == 0 && a.kind == OP_REG && b.kind == OP_REG) { rv_r(0x20, b.reg, a.reg, 0, a.reg, opc_opw); return; }
            if (strcmp(mnem, "addiw") == 0 && a.kind == OP_REG && b.kind == OP_REG) { rv_i(c.imm, b.reg, 0, a.reg, opc_immw); return; }
            if (strcmp(mnem, "sllw") == 0) { rv_r(0, b.reg, a.reg, 1, a.reg, opc_opw); return; }
            if (strcmp(mnem, "srlw") == 0) { rv_r(0, b.reg, a.reg, 5, a.reg, opc_opw); return; }
            if (strcmp(mnem, "sraw") == 0) { rv_r(0x20, b.reg, a.reg, 5, a.reg, opc_opw); return; }
        }

        /* Loads */
        if (strcmp(mnem, "lb") == 0 && a.kind == OP_REG && b.kind == OP_MEM) { rv_i(b.disp, b.base, 0, a.reg, opc_load); return; }
        if (strcmp(mnem, "lh") == 0 && a.kind == OP_REG && b.kind == OP_MEM) { rv_i(b.disp, b.base, 1, a.reg, opc_load); return; }
        if (strcmp(mnem, "lw") == 0 && a.kind == OP_REG && b.kind == OP_MEM) { rv_i(b.disp, b.base, 2, a.reg, opc_load); return; }
        if (strcmp(mnem, "lbu") == 0 && a.kind == OP_REG && b.kind == OP_MEM) { rv_i(b.disp, b.base, 4, a.reg, opc_load); return; }
        if (strcmp(mnem, "lhu") == 0 && a.kind == OP_REG && b.kind == OP_MEM) { rv_i(b.disp, b.base, 5, a.reg, opc_load); return; }
        if (g_arch >= ARCH_RISCV64 && strcmp(mnem, "ld") == 0 && a.kind == OP_REG && b.kind == OP_MEM) { rv_i(b.disp, b.base, 3, a.reg, opc_load); return; }
        if (g_arch >= ARCH_RISCV64 && strcmp(mnem, "lwu") == 0 && a.kind == OP_REG && b.kind == OP_MEM) { rv_i(b.disp, b.base, 6, a.reg, opc_load); return; }

        /* Stores */
        if (strcmp(mnem, "sb") == 0 && a.kind == OP_REG && b.kind == OP_MEM) { rv_s(b.disp, a.reg, b.base, 0, opc_store); return; }
        if (strcmp(mnem, "sh") == 0 && a.kind == OP_REG && b.kind == OP_MEM) { rv_s(b.disp, a.reg, b.base, 1, opc_store); return; }
        if (strcmp(mnem, "sw") == 0 && a.kind == OP_REG && b.kind == OP_MEM) { rv_s(b.disp, a.reg, b.base, 2, opc_store); return; }
        if (g_arch >= ARCH_RISCV64 && strcmp(mnem, "sd") == 0 && a.kind == OP_REG && b.kind == OP_MEM) { rv_s(b.disp, a.reg, b.base, 3, opc_store); return; }

        /* Branches */
        if (strcmp(mnem, "beq") == 0 && c.kind == OP_IMM) { rv_b(c.imm, a.reg, b.reg, 0, opc_branch); return; }
        if (strcmp(mnem, "bne") == 0 && c.kind == OP_IMM) { rv_b(c.imm, a.reg, b.reg, 1, opc_branch); return; }
        if (strcmp(mnem, "blt") == 0 && c.kind == OP_IMM) { rv_b(c.imm, a.reg, b.reg, 4, opc_branch); return; }
        if (strcmp(mnem, "bge") == 0 && c.kind == OP_IMM) { rv_b(c.imm, a.reg, b.reg, 5, opc_branch); return; }
        if (strcmp(mnem, "bltu") == 0 && c.kind == OP_IMM) { rv_b(c.imm, a.reg, b.reg, 6, opc_branch); return; }
        if (strcmp(mnem, "bgeu") == 0 && c.kind == OP_IMM) { rv_b(c.imm, a.reg, b.reg, 7, opc_branch); return; }

        /* JAL/JALR */
        if (strcmp(mnem, "jal") == 0 && a.kind == OP_REG) { rv_j(b.imm, a.reg, opc_jal); return; }
        if (strcmp(mnem, "jalr") == 0 && a.kind == OP_REG && b.kind == OP_MEM) { rv_i(b.disp, b.base, 0, a.reg, opc_jalr); return; }
        if (strcmp(mnem, "jalr") == 0 && a.kind == OP_REG && b.kind == OP_REG) { rv_i(0, b.reg, 0, a.reg, opc_jalr); return; }

        /* Upper immediate */
        if (strcmp(mnem, "lui") == 0 && a.kind == OP_REG) { rv_u(b.imm, a.reg, opc_lui); return; }
        if (strcmp(mnem, "auipc") == 0 && a.kind == OP_REG) { rv_u(b.imm, a.reg, opc_auipc); return; }

        /* System */
        if (strcmp(mnem, "ecall") == 0) { rv_emit_u32(opc_system); return; }
        if (strcmp(mnem, "ebreak") == 0) { rv_emit_u32(opc_system | 1); return; }
        if (strcmp(mnem, "fence") == 0) { rv_emit_u32(0x0FF0000F); return; }
        if (strcmp(mnem, "rdcycle") == 0) { rv_i(0xC00, 0, 0, a.reg, opc_system); return; }
        if (strcmp(mnem, "rdtime") == 0) { rv_i(0xC01, 0, 0, a.reg, opc_system); return; }
        if (strcmp(mnem, "rdinstret") == 0) { rv_i(0xC02, 0, 0, a.reg, opc_system); return; }

        /* CSR */
        if (strcmp(mnem, "csrrw") == 0) { rv_i(a.imm, b.reg, 1, a.reg, opc_system); return; }
        if (strcmp(mnem, "csrrs") == 0) { rv_i(a.imm, b.reg, 2, a.reg, opc_system); return; }
        if (strcmp(mnem, "csrrc") == 0) { rv_i(a.imm, b.reg, 3, a.reg, opc_system); return; }
    }

    if (g_arch == ARCH_X86_16) {
        if (g_bits == 16) {
            if (strcmp(mnem, "int") == 0 && a.kind == OP_IMM) { emit_byte(0xCD); emit_byte((uint8_t)a.imm); return; }
            if (strcmp(mnem, "nop") == 0) { emit_byte(0x90); return; }
            if (strcmp(mnem, "hlt") == 0) { emit_byte(0xF4); return; }
            if (strcmp(mnem, "cli") == 0) { emit_byte(0xFA); return; }
            if (strcmp(mnem, "sti") == 0) { emit_byte(0xFB); return; }
            if (strcmp(mnem, "cld") == 0) { emit_byte(0xFC); return; }
            if (strcmp(mnem, "std") == 0) { emit_byte(0xFD); return; }
            if (strcmp(mnem, "ret") == 0) { emit_byte(0xC3); return; }
            if (strcmp(mnem, "retf") == 0) { emit_byte(0xCB); return; }
            if (strcmp(mnem, "iret") == 0) { emit_byte(0xCF); return; }
            if (strcmp(mnem, "int3") == 0) { emit_byte(0xCC); return; }
            if (strcmp(mnem, "pushf") == 0) { emit_byte(0x9C); return; }
            if (strcmp(mnem, "popf") == 0) { emit_byte(0x9D); return; }
            if (strcmp(mnem, "push") == 0 && a.kind == OP_REG) { emit_byte(0x50 + (a.reg & 7)); return; }
            if (strcmp(mnem, "pop") == 0 && a.kind == OP_REG) { emit_byte(0x58 + (a.reg & 7)); return; }
            if (strcmp(mnem, "mov") == 0 && a.kind == OP_REG && b.kind == OP_IMM) {
                emit_byte(0xB8 + (a.reg & 7));
                emit_byte((uint8_t)(b.imm)); emit_byte((uint8_t)(b.imm >> 8));
                return;
            }
        }
    }

    err("unknown instruction or directive '%s'", mnem);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Two-pass assembly
 * ═══════════════════════════════════════════════════════════════════════════ */
static void reset_pass_state(int keep_syms) {
    if (!keep_syms) g_sym_count = 0;
    for (int i = 0; i < g_sect_count; i++) {
        g_sects[i].size = 0;
        g_sects[i].bytes.len = 0;
    }
    g_reloc_count = 0;
    g_export_count = 0;
    g_cur_sect = -1;
    g_error = 0;
}

static int assemble_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "beasm: cannot open %s\n", path); return -1; }
    char **lines = 0;
    int nlines = 0, cap = 0;
    char line[MAX_LINE];
    while (fgets(line, sizeof(line), f)) {
        if (nlines >= cap) { cap = cap ? cap * 2 : 256; lines = (char**)realloc(lines, cap * sizeof(char*)); }
        lines[nlines] = strdup(line);
        nlines++;
    }
    fclose(f);

    /* Pass 1: ukuran + definisi label */
    g_pass = 1;
    reset_pass_state(0);
    for (int i = 0; i < nlines; i++) {
        g_line_no = i + 1;
        parse_line(lines[i]);
        if (g_error) { fprintf(stderr, "beasm: pass 1 failed\n"); for (int j = 0; j < nlines; j++) free(lines[j]); free(lines); return -1; }
    }

    /* assign vaddr: .text=0, dst berurutan (align 16) */
    {
        uint32_t cur = 0;
        int order[] = { SECT_TEXT, SECT_RODATA, SECT_DATA, SECT_BSS,
                        SECT_CHERRYREVA, SECT_TRUST, SECT_EXPORTS, SECT_HOOK, SECT_MANIFEST };
        for (size_t k = 0; k < sizeof(order)/sizeof(order[0]); k++) {
            int idx = sect_type_to_index(order[k]);
            if (idx < 0) continue;
            sect_t *s = &g_sects[idx];
            cur = (cur + s->align - 1) & ~(uint32_t)(s->align - 1);
            s->vaddr = cur;
            cur += s->size;
        }
    }

    /* Pass 2: emit + relokasi — seksi/ukuran di-reset, simbol dipertahankan */
    g_pass = 2;
    reset_pass_state(1);
    for (int i = 0; i < nlines; i++) {
        g_line_no = i + 1;
        parse_line(lines[i]);
        if (g_error) { fprintf(stderr, "beasm: pass 2 failed\n"); for (int j = 0; j < nlines; j++) free(lines[j]); free(lines); return -1; }
    }

    for (int j = 0; j < nlines; j++) free(lines[j]);
    free(lines);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Output writer (grammar kernel)
 * ═══════════════════════════════════════════════════════════════════════════ */
static void write_magic(buf_t *out, const char *m8) {
    for (int i = 0; i < 8; i++) buf_byte(out, (uint8_t)m8[i]);
}

static void write_token(buf_t *out, const char *token) {
    const char *us = strchr(token, '_');
    char prefix[9] = {0};
    const char *seed = "00000";
    if (us) {
        int pl = (int)(us - token);
        if (pl > 8) pl = 8;
        memcpy(prefix, token, pl);
        seed = us + 1;
    } else {
        strncpy(prefix, token, 8);
    }
    for (int i = 0; i < 8; i++) buf_byte(out, (uint8_t)prefix[i]);
    int dlen = (int)strlen(seed);
    if (dlen > 10) dlen = 10;
    buf_u32(out, (uint32_t)dlen);
    uint64_t s = 0;
    for (int i = 0; i < dlen; i++) {
        char c = seed[i];
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'A' && c <= 'Z') d = c - 'A' + 10;
        else if (c >= 'a' && c <= 'z') d = c - 'a' + 36;
        else d = 0;
        s = s * 62 + (uint64_t)d;
    }
    buf_u32(out, (uint32_t)s);
    buf_u32(out, (uint32_t)(s >> 32));
}

static void build_trust(buf_t *out, const char *token) {
    buf_u16(out, 0x0001); buf_u16(out, (uint16_t)strlen(token));
    buf_bytes(out, (const uint8_t*)token, (uint32_t)strlen(token));
    buf_u16(out, 0x0002); buf_u16(out, 4);
    const char *us = strchr(token, '_');
    const char *seed = us ? us + 1 : "00000";
    buf_u32(out, (uint32_t)strlen(seed));
    buf_u16(out, 0xFFFF); buf_u16(out, 0);
}

static int write_out_file(const char *path) {
    buf_t out; buf_init(&out);

    int exports_type = (strcmp(g_out_format, "befb") == 0 ||
                        strcmp(g_out_format, "bex") == 0) ? SECT_EXPORTS_BEFB : SECT_EXPORTS;

    /* seksi .cherryreva dibuat OTOMATIS bila ada relokasi */
    if (g_reloc_count > 0 && sect_type_to_index(SECT_CHERRYREVA) < 0) {
        int idx = sect_find_or_add(".cherryreva");
        g_sects[idx].seen = 1;
    }

    /* jumlah seksi nyata */
    int n_sects = 0;
    for (int i = 0; i < g_sect_count; i++) if (g_sects[i].seen) n_sects++;

    /* header */
    if (strcmp(g_out_format, "bex") == 0) {
        write_magic(&out, "BEXI64\0\0");
        buf_u16(&out, (uint16_t)n_sects);
        buf_byte(&out, 1);
        buf_byte(&out, 1);
        int64_t entry = sym_addr(g_entry_sym);
        buf_u32(&out, entry > 0 ? (uint32_t)entry : 0);
    } else if (strcmp(g_out_format, "bdl") == 0) {
        write_magic(&out, "BDLI64\0\0");
        buf_u16(&out, (uint16_t)n_sects);
        buf_byte(&out, (uint8_t)g_version);
        buf_byte(&out, 1);
        buf_u32(&out, 0);
        write_token(&out, g_token);
    } else if (strcmp(g_out_format, "befb") == 0) {
        write_magic(&out, "BEFB164\0\0");
        buf_u16(&out, (uint16_t)n_sects);
        buf_byte(&out, 1);
        buf_byte(&out, 0);
        int64_t entry = sym_addr(g_entry_sym);
        buf_u32(&out, entry > 0 ? (uint32_t)entry : 0);
        write_token(&out, g_token);
    } else if (strcmp(g_out_format, "lbrct") == 0 || strcmp(g_out_format, "hbrct") == 0) {
        write_magic(&out, "RTCFI64\0\0");
        buf_u16(&out, (uint16_t)n_sects);
        buf_byte(&out, 1);
        buf_byte(&out, 1);
    } else if (strcmp(g_out_format, "bsp") == 0) {
        write_magic(&out, "BSP164\0\0");
        buf_u16(&out, (uint16_t)n_sects);
        buf_byte(&out, 1);
        buf_byte(&out, 1);
        buf_u32(&out, 0);
        buf_u32(&out, 0);
        buf_u32(&out, 0);
    } else if (strcmp(g_out_format, "bpp") == 0) {
        /* .bpp — Berry Panel Program (GUI app DE/WM/DS).  Header 24 B
         * identik bb_bpp_header (kernel/include/bpp_format.h):
         *   magic(8) ns(2) ver(1) flags(1) entry(4) win_w(4) win_h(4)
         * entry_point = 0 → DE memakai .hook API table, bukan entry. */
        write_magic(&out, "BPP164\0\0");
        buf_u16(&out, (uint16_t)n_sects);
        buf_byte(&out, 1);
        buf_byte(&out, 0);
        buf_u32(&out, 0);   /* entry_point: 0 = .hook API table */
        buf_u32(&out, (uint32_t)g_bpp_win_w);
        buf_u32(&out, (uint32_t)g_bpp_win_h);
    } else if (strcmp(g_out_format, "bo") == 0) {
        write_magic(&out, "BOBJ\0\0\0\0");
        buf_u16(&out, (uint16_t)n_sects);
        buf_byte(&out, 1);
        buf_byte(&out, 0);
        buf_u32(&out, 0);
    } else {
        fprintf(stderr, "beasm: unknown format '%s'\n", g_out_format);
        buf_free(&out);
        return -1;
    }

    uint32_t header_size;
    if (strcmp(g_out_format, "bex") == 0) header_size = BEX_HEADER_SIZE;
    else if (strcmp(g_out_format, "bdl") == 0 || strcmp(g_out_format, "befb") == 0) header_size = 36;
    else if (strcmp(g_out_format, "lbrct") == 0 || strcmp(g_out_format, "hbrct") == 0) header_size = RTCF_HEADER_SIZE;
    else if (strcmp(g_out_format, "bsp") == 0) header_size = BSP_HEADER_SIZE;
    else if (strcmp(g_out_format, "bpp") == 0) header_size = BPP_HEADER_SIZE;
    else header_size = BO_HEADER_SIZE;

    uint32_t table_off = header_size;
    for (int i = 0; i < n_sects; i++) { buf_u32(&out, 0); buf_u32(&out, 0); buf_u32(&out, 0); buf_u32(&out, 0); }

    /* section data */
    uint32_t s_off[MAX_SECTS];
    for (int i = 0; i < MAX_SECTS; i++) s_off[i] = 0;
    for (int i = 0; i < g_sect_count; i++) {
        sect_t *s = &g_sects[i];
        if (!s->seen) continue;
        if (s->type == SECT_BSS) { s_off[i] = 0; continue; }
        buf_pad(&out, 16);
        s_off[i] = out.len;
        if (s->type == SECT_CHERRYREVA) {
            for (int r = 0; r < g_reloc_count; r++) {
                buf_u32(&out, g_relocs[r].off);
                buf_byte(&out, g_relocs[r].type);
                buf_byte(&out, g_relocs[r].width);
                buf_u16(&out, g_relocs[r].rsv);
            }
        } else if (s->type == SECT_TRUST) {
            build_trust(&out, g_token);
        } else if (s->type == SECT_EXPORTS) {
            for (int e = 0; e < g_export_count; e++) {
                buf_u32(&out, g_exports[e].hash);
                buf_u32(&out, g_exports[e].entry_off);
                buf_u16(&out, g_exports[e].flags);
                buf_u16(&out, 0);
            }
            buf_u32(&out, 0); buf_u32(&out, 0); buf_u32(&out, 0);
        } else {
            buf_bytes(&out, s->bytes.data, s->bytes.len);
        }
    }

    /* patch section table */
    uint8_t *d = out.data;
    uint32_t idx = 0;
    for (int i = 0; i < g_sect_count; i++) {
        sect_t *s = &g_sects[i];
        if (!s->seen) continue;
        uint32_t so = table_off + idx * SECT_ENTRY_SIZE;
        uint32_t stype = (uint32_t)s->type;
        if (stype == SECT_EXPORTS) stype = (uint32_t)exports_type;
        uint32_t off = s_off[i];
        uint32_t sz = s->size;
        uint32_t va = s->vaddr;
        /* perbaiki ukuran seksi sintetis */
        if (s->type == SECT_CHERRYREVA) sz = (uint32_t)g_reloc_count * CR_ENTRY_SIZE;
        else if (s->type == SECT_TRUST) sz = out.len - off;
        else if (s->type == SECT_EXPORTS) sz = (uint32_t)(g_export_count + 1) * EXPORT_ENTRY_SIZE;
        d[so+0] = stype & 0xFF; d[so+1] = (stype >> 8) & 0xFF;
        d[so+2] = (stype >> 16) & 0xFF; d[so+3] = (stype >> 24) & 0xFF;
        d[so+4] = off & 0xFF; d[so+5] = (off >> 8) & 0xFF;
        d[so+6] = (off >> 16) & 0xFF; d[so+7] = (off >> 24) & 0xFF;
        d[so+8] = sz & 0xFF; d[so+9] = (sz >> 8) & 0xFF;
        d[so+10] = (sz >> 16) & 0xFF; d[so+11] = (sz >> 24) & 0xFF;
        d[so+12] = va & 0xFF; d[so+13] = (va >> 8) & 0xFF;
        d[so+14] = (va >> 16) & 0xFF; d[so+15] = (va >> 24) & 0xFF;
        idx++;
    }

    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "beasm: cannot write %s\n", path); buf_free(&out); return -1; }
    fwrite(out.data, 1, out.len, f);
    fclose(f);
    printf("beasm: %s written (%u bytes, %d sections, %d exports, %d relocs, format=%s)\n",
           path, out.len, n_sects, g_export_count, g_reloc_count, g_out_format);
    buf_free(&out);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * main
 * ═══════════════════════════════════════════════════════════════════════════ */
static void usage(void) {
    fprintf(stderr,
        "beasm — Berry Assembler for UBerryNix\n\n"
        "Usage:\n"
        "  beasm <input.basm> -o <output> [options]\n\n"
        "Options:\n"
        "  -o <file>          output path (default out.befb)\n"
        "  --format <fmt>     befb | bex | bdl | lbrct | hbrct | bsp | bpp | bo\n"
        "                     (default befb)\n"
        "  --token <t>        BTP token (default BEFB_00001)\n"
        "  --entry <sym>      entry symbol (default _start)\n"
        "  --win-w <px>       .bpp: default window width  (0 = DE decides)\n"
        "  --win-h <px>       .bpp: default window height (0 = DE decides)\n"
        "  --version <1|2>    BDL component dir version (default 2)\n"
        "  --hash <name>      print FNV-1a 32-bit hash of <name> and exit\n"
        "  -EnAI              enable AI optimization track (.bcm model)\n"
        "  -OAI<0..10>        AI optimization level (implies -EnAI)\n"
        "  --bcm <file>       .bcm model path (default /system/share/beasm/models/berry.bcm)\n"
        "  --seed <n>         deterministic seed for AI transform selection\n"
        "  --ai-stats         print AI hit/miss stats to stderr\n"
        "  -h | --help        this help\n\n"
        "Track AI (.bcm): pola IR -> transformasi, confidence-gated, hibrid\n"
        "dengan -O0..-O10 klasik.  Tanpa -EnAI semua AI diabaikan.\n"
        "Grammar: magic 8-byte (BDLI64/BEFB164/BEXI64/RTCFI64/BSP164), header\n"
        "sesuai kernel/include/*_format.h, section table 16 B, .exports 12 B\n"
        "per entri {fn_hash u32, entry_off u32, flags u16, rsv u16}.\n");
}

int main(int argc, char **argv) {
    const char *input = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) { strncpy(g_out_path, argv[++i], sizeof(g_out_path)-1); }
        else if (strcmp(argv[i], "--format") == 0 && i + 1 < argc) { g_out_format = argv[++i]; }
        else if (strcmp(argv[i], "--token") == 0 && i + 1 < argc) { strncpy(g_token, argv[++i], sizeof(g_token)-1); }
        else if (strcmp(argv[i], "--entry") == 0 && i + 1 < argc) { strncpy(g_entry_sym, argv[++i], sizeof(g_entry_sym)-1); }
        else if (strcmp(argv[i], "--version") == 0 && i + 1 < argc) { g_version = atoi(argv[++i]); }
        else if (strcmp(argv[i], "--win-w") == 0 && i + 1 < argc) { g_bpp_win_w = (uint32_t)atoi(argv[++i]); }
        else if (strcmp(argv[i], "--win-h") == 0 && i + 1 < argc) { g_bpp_win_h = (uint32_t)atoi(argv[++i]); }
        else if (strcmp(argv[i], "--hash") == 0 && i + 1 < argc) { printf("0x%08x\n", fnv1a_32(argv[++i])); return 0; }
        else if (strcmp(argv[i], "--bcm") == 0 && i + 1 < argc) { strncpy(g_bcm_path, argv[++i], sizeof(g_bcm_path)-1); }
        else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) { g_ai_seed = (uint32_t)strtoul(argv[++i], 0, 0); }
        else if (strcmp(argv[i], "--ai-stats") == 0) { g_ai_stats = 1; }
        else if (strcmp(argv[i], "-EnAI") == 0) { g_enai = 1; }
        else if (strncmp(argv[i], "-OAI", 4) == 0) {
            g_enai = 1;
            g_oai_level = atoi(argv[i] + 4);
            if (g_oai_level < 0) g_oai_level = 0;
            if (g_oai_level > 10) g_oai_level = 10;
        }
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) { usage(); return 0; }
        else if (argv[i][0] != '-') { input = argv[i]; }
    }
    if (!input) { usage(); return 1; }
    /* -EnAI / -OAI memicu load model (hanya bila --bcm diberikan; tanpa model
     * jalur deterministik penuh — default path self-host tidak dicoba di host) */
    if (g_enai && g_bcm_path[0]) {
        if (bcm_load_file(g_bcm_path) != 0) {
            fprintf(stderr, "[beasm] peringatan: model .bcm tidak dapat dimuat (%s) — jalur deterministik\n", g_bcm_path);
        } else {
            fprintf(stderr, "[beasm] -EnAI aktif: model %s (%u pola, arch=%u), -OAI%d\n",
                    g_bcm_path, g_bcm.count, g_bcm.arch_id, g_oai_level);
        }
    }
    g_file = input;
    if (assemble_file(input) != 0) return 1;
    if (write_out_file(g_out_path) != 0) return 1;
    if (g_ai_stats && g_enai) {
        fprintf(stderr, "[beasm] AI stats: hit=%u miss=%u\n", g_ai_hits, g_ai_misses);
    }
    return 0;
}
