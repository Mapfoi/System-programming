#define WIN32_LEAN_AND_MEAN
#include "memreplace_api.h"
#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#include <string.h>

/* RW-блок через VirtualAlloc (OSiSP, с. 118) — регион MEM_PRIVATE. */
static char *g_lab5Needle = NULL;

static void PrintBanner(void)
{
    wprintf(L"=== Lab5Target (процесс-мишень) ===\n");
    wprintf(L"PID: %lu\n", GetCurrentProcessId());
    wprintf(L"Буфер строки (VirtualAlloc): %p\n", (void *)g_lab5Needle);
    wprintf(L"Текущая строка в памяти: \"%hs\"\n", g_lab5Needle);
    wprintf(L"Запустите Lab5Inject.exe с этим PID (не Lab5Dynamic).\n");
    wprintf(L"Ctrl+C для выхода.\n\n");
}

int main(void)
{
    size_t needleLen;

    _setmode(_fileno(stdout), _O_U16TEXT);

    needleLen = strlen(LAB5_DEMO_SEARCH);
    g_lab5Needle = (char *)VirtualAlloc(NULL, needleLen + 1,
                                        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!g_lab5Needle) {
        wprintf(L"VirtualAlloc не удался\n");
        return 1;
    }
    memcpy(g_lab5Needle, LAB5_DEMO_SEARCH, needleLen + 1);

    PrintBanner();

    for (;;) {
        wprintf(L"[%lu] g_lab5Needle = \"%hs\"\r", GetTickCount(), g_lab5Needle);
        fflush(stdout);
        Sleep(1000);
    }
    return 0;
}
