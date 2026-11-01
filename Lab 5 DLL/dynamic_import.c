#define WIN32_LEAN_AND_MEAN
#include "memreplace_api.h"
#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>

static volatile char g_demoBuffer[] = LAB5_DEMO_SEARCH;

static BOOL ResolveDllPath(wchar_t *path, size_t cap)
{
    DWORD len = GetModuleFileNameW(NULL, path, (DWORD)cap);
    wchar_t *slash;

    if (len == 0 || len >= cap)
        return FALSE;

    slash = wcsrchr(path, L'\\');
    if (!slash)
        return FALSE;
    *(slash + 1) = L'\0';

    if (wcslen(path) + 16 >= cap)
        return FALSE;
    wcscat(path, L"memreplace.dll");
    return TRUE;
}

int main(void)
{
    wchar_t dllPath[MAX_PATH];
    HMODULE module;
    PFN_MemReplaceStringA fn;
    MemReplaceStats stats = {0};
    int count;

    _setmode(_fileno(stdout), _O_U16TEXT);

    wprintf(L"=== Lab5Dynamic (динамический импорт LoadLibraryW + GetProcAddress) ===\n");

    if (!ResolveDllPath(dllPath, MAX_PATH)) {
        wprintf(L"Не удалось собрать путь к memreplace.dll\n");
        return 1;
    }

    module = LoadLibraryW(dllPath);
    if (!module) {
        wprintf(L"LoadLibraryW не удался, код %lu\n", GetLastError());
        return 1;
    }

    fn = (PFN_MemReplaceStringA)(void *)GetProcAddress(module, "MemReplaceStringA");
    if (!fn) {
        wprintf(L"GetProcAddress(MemReplaceStringA) не удался, код %lu\n", GetLastError());
        FreeLibrary(module);
        return 1;
    }

    wprintf(L"Загружена DLL: %ls\n", dllPath);
    wprintf(L"Указатель на функцию: %p\n", (void *)fn);

    wprintf(L"До замены: \"%hs\"\n", (const char *)g_demoBuffer);
    count = fn(LAB5_DEMO_SEARCH, LAB5_DEMO_REPLACE, &stats);
    wprintf(L"После замены: \"%hs\"\n", (const char *)g_demoBuffer);
    wprintf(L"Заменено вхождений: %d (регионов: %lu, байт: %lu)\n",
            count, stats.regionsScanned, stats.bytesWritten);

    FreeLibrary(module);
    return count < 0 ? 1 : 0;
}
