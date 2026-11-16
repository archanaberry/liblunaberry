/*
 * luna-common.c — host plumbing for the LunaBerry tools.
 */

#include "luna-common.h"

#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  include <direct.h>
#  include <windows.h>
#  define LUNA_MKDIR(p) _mkdir(p)
#  define LUNA_POPEN _popen
#  define LUNA_PCLOSE _pclose
#else
#  include <sys/stat.h>
#  include <sys/types.h>
#  include <unistd.h>
#  define LUNA_MKDIR(p) mkdir((p), 0755)
#  define LUNA_POPEN popen
#  define LUNA_PCLOSE pclose
#endif

char *luna_read_file(const char *path, size_t *plen)
{
    FILE *f;
    long sz;
    char *buf;

    if (plen != NULL)
        *plen = 0;
    f = fopen(path, "rb");
    if (f == NULL)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0 || (sz = ftell(f)) < 0 ||
        fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    buf = (char *)malloc((size_t)sz + 1);
    if (buf == NULL) {
        fclose(f);
        return NULL;
    }
    if (sz > 0 && fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf);
        fclose(f);
        return NULL;
    }
    buf[sz] = '\0';
    fclose(f);
    if (plen != NULL)
        *plen = (size_t)sz;
    return buf;
}

int luna_write_file(const char *path, const void *data, size_t len)
{
    FILE *f;
    size_t wrote;

    luna_ensure_parent(path);
    f = fopen(path, "wb");
    if (f == NULL)
        return -1;
    wrote = len ? fwrite(data, 1, len, f) : 0;
    if (fclose(f) != 0)
        return -1;
    return wrote == len ? 0 : -1;
}

int luna_file_exists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return 0;
    fclose(f);
    return 1;
}

int luna_mkdir_p(const char *path)
{
    char tmp[1024];
    size_t len;
    size_t i;

    if (path == NULL || path[0] == '\0')
        return 0;
    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);
    for (i = 1; i < len; i++) {
        if (tmp[i] == '/' || tmp[i] == '\\') {
            char saved = tmp[i];
            tmp[i] = '\0';
            if (tmp[0] != '\0' &&
                !(strlen(tmp) == 2 && tmp[1] == ':')) {
                (void)LUNA_MKDIR(tmp);
            }
            tmp[i] = saved;
        }
    }
    (void)LUNA_MKDIR(tmp);
    return 0;
}

int luna_ensure_parent(const char *path)
{
    char tmp[1024];
    char *p;

    if (path == NULL)
        return -1;
    snprintf(tmp, sizeof(tmp), "%s", path);
    p = strrchr(tmp, '/');
#if defined(_WIN32)
    {
        char *q = strrchr(tmp, '\\');
        if (q != NULL && (p == NULL || q > p))
            p = q;
    }
#endif
    if (p == NULL)
        return 0;
    *p = '\0';
    if (tmp[0] == '\0')
        return 0;
    return luna_mkdir_p(tmp);
}

const char *luna_basename(const char *path)
{
    const char *p;
    const char *q;

    if (path == NULL)
        return "";
    p = strrchr(path, '/');
    q = strrchr(path, '\\');
    if (q != NULL && (p == NULL || q > p))
        p = q;
    return p != NULL ? p + 1 : path;
}

void luna_stem(const char *path, char *out, size_t out_size)
{
    const char *base;
    const char *dot;
    size_t n;

    if (out_size == 0)
        return;
    base = luna_basename(path);
    dot = strrchr(base, '.');
    n = dot != NULL && dot != base ? (size_t)(dot - base) : strlen(base);
    if (n >= out_size)
        n = out_size - 1;
    memcpy(out, base, n);
    out[n] = '\0';
}

void luna_dirname(const char *path, char *out, size_t out_size)
{
    const char *p;
    const char *q;
    size_t n;

    if (out_size == 0)
        return;
    p = strrchr(path, '/');
    q = strrchr(path, '\\');
    if (q != NULL && (p == NULL || q > p))
        p = q;
    if (p == NULL) {
        snprintf(out, out_size, ".");
        return;
    }
    n = (size_t)(p - path);
    if (n == 0)
        n = 1;
    if (n >= out_size)
        n = out_size - 1;
    memcpy(out, path, n);
    out[n] = '\0';
}

void luna_path_join(char *out, size_t out_size, const char *a, const char *b)
{
    size_t n;

    if (out_size == 0)
        return;
    if (a == NULL)
        a = "";
    if (b == NULL)
        b = "";
    n = strlen(a);
    if (n > 0 && (a[n - 1] == '/' || a[n - 1] == '\\'))
        snprintf(out, out_size, "%s%s", a, b);
    else
        snprintf(out, out_size, "%s/%s", a, b);
}

void luna_exe_dir(char *out, size_t out_size)
{
    if (out_size == 0)
        return;
#if defined(_WIN32)
    {
        char buf[MAX_PATH];
        DWORD n = GetModuleFileNameA(NULL, buf, (DWORD)sizeof(buf));
        if (n == 0) {
            snprintf(out, out_size, ".");
            return;
        }
        luna_dirname(buf, out, out_size);
    }
#else
    {
        char buf[1024];
        ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if (n <= 0) {
            snprintf(out, out_size, ".");
            return;
        }
        buf[n] = '\0';
        luna_dirname(buf, out, out_size);
    }
#endif
}

void luna_tmp_dir(char *out, size_t out_size)
{
    const char *t;

    if (out == NULL || out_size == 0)
        return;
#if defined(_WIN32)
    t = getenv("TEMP");
    if (t == NULL || t[0] == '\0')
        t = getenv("TMP");
    if (t == NULL || t[0] == '\0')
        t = getenv("TMPDIR");
#else
    t = getenv("TMPDIR");
    if (t == NULL || t[0] == '\0')
        t = getenv("TMP");
#endif
    if (t != NULL && t[0] != '\0')
        snprintf(out, out_size, "%s", t);
    else
        luna_exe_dir(out, out_size);
}

void luna_find_include_dir(char *out, size_t out_size)
{
    char exe[1024];
    char cand[1200];

    if (out == NULL || out_size == 0)
        return;
    if (luna_file_exists("luna.h")) {
        snprintf(out, out_size, ".");
        return;
    }
    luna_exe_dir(exe, sizeof(exe));
    luna_path_join(cand, sizeof(cand), exe, "../include/luna.h");
    if (luna_file_exists(cand)) {
        snprintf(out, out_size, "%s/../include", exe);
        return;
    }
    luna_path_join(cand, sizeof(cand), exe, "../luna.h");
    if (luna_file_exists(cand)) {
        snprintf(out, out_size, "%s/..", exe);
        return;
    }
    luna_path_join(cand, sizeof(cand), exe, "luna.h");
    if (luna_file_exists(cand)) {
        snprintf(out, out_size, "%s", exe);
        return;
    }
    snprintf(out, out_size, ".");
}

int luna_find_tool(const char *name, char *out, size_t out_size)
{
    if (name == NULL || out == NULL || out_size == 0)
        return -1;
    if (strchr(name, '/') != NULL || strchr(name, '\\') != NULL) {
        if (luna_file_exists(name)) {
            snprintf(out, out_size, "%s", name);
            return 0;
        }
        return -1;
    }
#if defined(_WIN32)
    {
        const char *home = getenv("USERPROFILE");
        const char *pf = getenv("ProgramFiles");
        const char *pf86 = getenv("ProgramFiles(x86)");
        const char *sys = getenv("SystemDrive");
        char cand[1200];

        if (home != NULL && home[0] != '\0') {
            const char *subs[] = {
                "\\scoop\\apps\\mingw\\current\\bin\\",
                "\\scoop\\apps\\gcc\\current\\bin\\",
                "\\scoop\\apps\\llvm\\current\\bin\\",
                "\\scoop\\shims\\",
                "\\scoop\\apps\\msys2\\current\\mingw64\\bin\\"
            };
            size_t k;
            for (k = 0; k < sizeof(subs) / sizeof(subs[0]); k++) {
                snprintf(cand, sizeof(cand), "%s%s%s.exe", home, subs[k],
                         name);
                if (luna_file_exists(cand)) {
                    snprintf(out, out_size, "%s", cand);
                    return 0;
                }
            }
        }
        if (pf != NULL && pf[0] != '\0') {
            snprintf(cand, sizeof(cand), "%s\\LLVM\\bin\\%s.exe", pf, name);
            if (luna_file_exists(cand)) {
                snprintf(out, out_size, "%s", cand);
                return 0;
            }
        }
        if (pf86 != NULL && pf86[0] != '\0') {
            snprintf(cand, sizeof(cand), "%s\\LLVM\\bin\\%s.exe", pf86, name);
            if (luna_file_exists(cand)) {
                snprintf(out, out_size, "%s", cand);
                return 0;
            }
        }
        if (sys != NULL && sys[0] != '\0') {
            snprintf(cand, sizeof(cand), "%s\\msys64\\mingw64\\bin\\%s.exe",
                     sys, name);
            if (luna_file_exists(cand)) {
                snprintf(out, out_size, "%s", cand);
                return 0;
            }
            snprintf(cand, sizeof(cand), "%s\\mingw64\\bin\\%s.exe", sys,
                     name);
            if (luna_file_exists(cand)) {
                snprintf(out, out_size, "%s", cand);
                return 0;
            }
        }
        if (luna_file_exists(name)) {
            snprintf(out, out_size, "%s", name);
            return 0;
        }
        snprintf(cand, sizeof(cand), "%s.exe", name);
        if (luna_file_exists(cand)) {
            snprintf(out, out_size, "%s", cand);
            return 0;
        }
    }
    return -1;
#else
    snprintf(out, out_size, "%s", name);
    return 0;
#endif
}

int luna_mingw_include(char *out, size_t out_size)
{
#if defined(_WIN32)
    char buf[1024];
    char root[1024];
    char probe[1500];
    char gcc[1200];
    char *p;
    const char *env;

    if (out_size == 0)
        return -1;

    env = getenv("MINGW_PREFIX");
    if (env == NULL || env[0] == '\0')
        env = getenv("MSYSTEM_PREFIX");
    if (env != NULL && env[0] != '\0') {
        snprintf(probe, sizeof(probe), "%s/x86_64-w64-mingw32/include/stdio.h",
                 env);
        if (luna_file_exists(probe)) {
            snprintf(out, out_size, "%s/x86_64-w64-mingw32/include", env);
            return 0;
        }
    }

    if (luna_find_tool("gcc", gcc, sizeof(gcc)) != 0)
        snprintf(gcc, sizeof(gcc), "gcc");
    snprintf(probe, sizeof(probe), "\"%s\" -print-file-name=include", gcc);
    if (luna_run_cmd(probe, buf, sizeof(buf)) == 0) {
        /* Trim trailing whitespace. */
        size_t n = strlen(buf);
        while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' ||
                         buf[n - 1] == ' '))
            buf[--n] = '\0';
        p = strstr(buf, "/lib/gcc/");
        if (p == NULL)
            p = strstr(buf, "\\lib\\gcc\\");
        if (p != NULL) {
            size_t k = (size_t)(p - buf);
            if (k >= sizeof(root))
                k = sizeof(root) - 1;
            memcpy(root, buf, k);
            root[k] = '\0';
            snprintf(probe, sizeof(probe),
                     "%s/x86_64-w64-mingw32/include/stdio.h", root);
            if (luna_file_exists(probe)) {
                snprintf(out, out_size, "%s/x86_64-w64-mingw32/include", root);
                return 0;
            }
        }
    }
    (void)out;
    return -1;
#else
    (void)out;
    (void)out_size;
    return -1;
#endif
}

#if defined(_WIN32)
int luna_run_cmd(const char *cmd, char *out, size_t out_size)
{
    SECURITY_ATTRIBUTES sa;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    HANDLE rd = NULL;
    HANDLE wr = NULL;
    char *cmdline;
    DWORD code = (DWORD)-1;
    BOOL ok;

    if (out != NULL && out_size > 0)
        out[0] = '\0';
    if (cmd == NULL)
        return -1;

    cmdline = (char *)malloc(strlen(cmd) + 1);
    if (cmdline == NULL)
        return -1;
    strcpy(cmdline, cmd);

    ZeroMemory(&sa, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    if (!CreatePipe(&rd, &wr, &sa, 0)) {
        free(cmdline);
        return -1;
    }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = wr;
    si.hStdError = wr;

    ZeroMemory(&pi, sizeof(pi));
    ok = CreateProcessA(NULL, cmdline, NULL, NULL, TRUE, CREATE_NO_WINDOW,
                        NULL, NULL, &si, &pi);
    CloseHandle(wr);
    if (!ok) {
        CloseHandle(rd);
        free(cmdline);
        return -1;
    }

    if (out != NULL && out_size > 1) {
        DWORD n = 0;
        size_t total = 0;
        while (total < out_size - 1 &&
               ReadFile(rd, out + total,
                        (DWORD)(out_size - 1 - total), &n, NULL) &&
               n > 0)
            total += n;
        out[total] = '\0';
    } else {
        char tmp[512];
        DWORD n;
        while (ReadFile(rd, tmp, sizeof(tmp), &n, NULL) && n > 0) {
            /* discard */
        }
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(rd);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    free(cmdline);
    return (int)code;
}
#else
int luna_run_cmd(const char *cmd, char *out, size_t out_size)
{
    FILE *p;
    int rc;

    if (out != NULL && out_size > 0)
        out[0] = '\0';
    p = LUNA_POPEN(cmd, "r");
    if (p == NULL)
        return -1;
    if (out != NULL && out_size > 1) {
        size_t n = fread(out, 1, out_size - 1, p);
        out[n] = '\0';
    }
    rc = LUNA_PCLOSE(p);
    return rc;
}
#endif

#if defined(_WIN32)
int luna_run_cmd_inherit(const char *cmd)
{
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    char *cmdline;
    BOOL ok;
    DWORD code = (DWORD)-1;

    if (cmd == NULL)
        return -1;
    cmdline = (char *)malloc(strlen(cmd) + 1);
    if (cmdline == NULL)
        return -1;
    strcpy(cmdline, cmd);

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));
    ok = CreateProcessA(NULL, cmdline, NULL, NULL, TRUE, 0, NULL, NULL, &si,
                        &pi);
    free(cmdline);
    if (!ok)
        return -1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)code;
}
#else
int luna_run_cmd_inherit(const char *cmd)
{
    return system(cmd);
}
#endif

void luna_escape_c_string(const char *src, char *out, size_t out_size)
{
    size_t i = 0;
    const unsigned char *p = (const unsigned char *)src;

    if (out_size == 0)
        return;
    while (p != NULL && *p != '\0' && i + 5 < out_size) {
        unsigned char c = *p++;
        switch (c) {
        case '"':
            out[i++] = '\\';
            out[i++] = '"';
            break;
        case '\\':
            out[i++] = '\\';
            out[i++] = '\\';
            break;
        case '\n':
            out[i++] = '\\';
            out[i++] = 'n';
            break;
        case '\r':
            out[i++] = '\\';
            out[i++] = 'r';
            break;
        case '\t':
            out[i++] = '\\';
            out[i++] = 't';
            break;
        default:
            if (c < 0x20 || c >= 0x7f) {
                out[i++] = '\\';
                out[i++] = (char)('0' + ((c >> 6) & 7));
                out[i++] = (char)('0' + ((c >> 3) & 7));
                out[i++] = (char)('0' + (c & 7));
            } else {
                out[i++] = (char)c;
            }
            break;
        }
    }
    out[i] = '\0';
}

void luna_emit_c_array(FILE *f, const char *name, const unsigned char *data,
                       size_t len)
{
    size_t i;

    fprintf(f, "static const unsigned char %s[] = {\n", name);
    for (i = 0; i < len; i++) {
        if (i % 12 == 0)
            fprintf(f, "    ");
        fprintf(f, "0x%02x,", data[i]);
        if (i % 12 == 11 || i + 1 == len)
            fprintf(f, "\n");
        else
            fprintf(f, " ");
    }
    if (len == 0)
        fprintf(f, "    0x00\n");
    fprintf(f, "};\n");
    fprintf(f, "static const size_t %s_len = sizeof(%s);\n", name, name);
}

void luna_emit_c_string(FILE *f, const char *name, const char *value)
{
    char *esc = (char *)malloc(strlen(value) * 4 + 8);
    if (esc == NULL) {
        fprintf(f, "static const char %s[] = \"\";\n", name);
        return;
    }
    luna_escape_c_string(value, esc, strlen(value) * 4 + 8);
    fprintf(f, "static const char %s[] = \"%s\";\n", name, esc);
    free(esc);
}

unsigned long long luna_hash64(const void *data, size_t len)
{
    const unsigned char *p = (const unsigned char *)data;
    unsigned long long h = 0xcbf29ce484222325ULL;
    size_t i;

    for (i = 0; i < len; i++) {
        h ^= (unsigned long long)p[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

unsigned long long luna_hash64_str(const char *s)
{
    return luna_hash64(s, s != NULL ? strlen(s) : 0);
}
