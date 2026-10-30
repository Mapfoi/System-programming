#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <stdio.h>

/* Строка в RW-сегменте — её ищет DLL после внедрения в этот процесс. */
volatile char g_lab5Needle[] = "Lab5OldString!!";

static void PrintBanner(void)
{
    wprintf(L"=== Lab5Target (процесс-мишень) ===\n");
    wprintf(L"PID: %lu\n", GetCurrentProcessId());
    wprintf(L"Текущая строка в памяти: \"%hs\"\n", (const char *)g_lab5Needle);
    wprintf(L"Запустите Lab5Inject с этим PID.\n");
    wprintf(L"Ctrl+C для выхода.\n\n");
}

int main(void)
{
    _setmode(_fileno(stdout), _O_U16TEXT);

    PrintBanner();

    for (;;) {
        wprintf(L"[%lu] g_lab5Needle = \"%hs\"\r", GetTickCount(), (const char *)g_lab5Needle);
        fflush(stdout);
        Sleep(1000);
    }
    return 0;
}
