// ============================================================================
//  tools/cputime.c -- ukur CPU time (kernel+user) sebuah proses, resolusi 100ns.
//
//  MENGAPA ADA
//    Wall-clock di host ini tidak bisa dipercaya untuk membandingkan engine:
//    mesin dipakai bersama (agent lain build/benchmark paralel) dan beban
//    mereka menaikkan wall time hindari engine yang kebetulan jalan saat
//    kontensi.  CPU time milik proses sendiri tidak terpengaruh itu.
//
//  MENGAPA_Khusus
//    Alternatif `time` di shell mengukur subshell+pipe (bisa jadi 15 ms untuk
//    beban yang nyatanya ~800 ms), dan builtin `times` di bash tidak bisa
//    dibaca untuk satu anak tunggal.  MinGW-w64 juga tidak punya getrusage,
//    jadi ini memakai GetProcessTimes Win32 langsung.
//
//  PEMAKAIAN:  cputime <exe> [arg...]
//  KELUARAN:   "user_ms kernel_ms cpu_ms" (satu baris, dipisah spasi)
//             exit code = exit code anak (bukan alat)
// ============================================================================
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

/* 1 tick FILETIME = 100 nanosecond -> ms = tick/10000 */
static long long ft_ms(const FILETIME *ft)
{
    ULARGE_INTEGER u;
    u.LowPart = ft->dwLowDateTime;
    u.HighPart = ft->dwHighDateTime;
    return (long long)u.QuadPart / 10000LL;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "cputime <exe> [arg...]\n");
        return 2;
    }

    /* Bangun command line: kutip arg yang memuat spasi */
    char cmd[32768] = {0};
    size_t off = 0;
    for (int i = 1; i < argc; i++) {
        const char *sep = (i == 1) ? "" : " ";
        int n = snprintf(cmd + off, sizeof(cmd) - off, "%s%s", sep, argv[i]);
        if (n < 0 || (size_t)n >= sizeof(cmd) - off) {
            fprintf(stderr, "cputime: command line too long\n");
            return 2;
        }
        off += (size_t)n;
    }

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    if (!CreateProcessA(argv[1],            /* lpApplicationName */
                        cmd,                /* lpCommandLine    */
                        NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        fprintf(stderr, "cputime: CreateProcess failed (%lu) for %s\n",
                GetLastError(), argv[1]);
        return 127;
    }

    WaitForSingleObject(pi.hProcess, INFINITE);

    FILETIME c, e, k, u;
    if (!GetProcessTimes(pi.hProcess, &c, &e, &k, &u)) {
        fprintf(stderr, "cputime: GetProcessTimes failed (%lu)\n", GetLastError());
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
        return 3;
    }
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    long long ums = ft_ms(&u), kms = ft_ms(&k);
    printf("%lld %lld %lld\n", ums, kms, ums + kms);
    return (int)code;
}
