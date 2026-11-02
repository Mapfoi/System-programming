#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <stdio.h>

#define LAB5_SEARCH "Lab5OldString!!"
#define LAB5_REPLACE "Lab5NewString!!"

typedef int (__cdecl *PFN_MemReplaceStringA)(const char *, const char *);

static volatile char g_buffer[] = LAB5_SEARCH;

// Пауза перед закрытием консоли (после wprintf нужен текстовый режим stdout)
static void WaitConsole(void)
{
    fflush(stdout);
    _setmode(_fileno(stdout), _O_TEXT);
    _setmode(_fileno(stdin), _O_TEXT);
    system("pause");
}

// Путь к memreplace.dll в каталоге рядом с EXE
static BOOL DllPathNextToExe(wchar_t *path, size_t cap)
{
    DWORD len = GetModuleFileNameW(NULL, path, (DWORD)cap);
    wchar_t *slash;

    if (len == 0 || len >= cap)
        return FALSE;
    slash = wcsrchr(path, L'\\');
    if (!slash)
        return FALSE;
    slash[1] = L'\0';
    if (wcslen(path) + 16 >= cap)
        return FALSE;
    wcscat(path, L"memreplace.dll");
    return TRUE;
}

int main(void)
{
    wchar_t dllPath[MAX_PATH];
    HMODULE mod;
    PFN_MemReplaceStringA fn;
    int count;

    _setmode(_fileno(stdout), _O_U16TEXT);
    wprintf(L"Lab5Dynamic — LoadLibraryW + GetProcAddress\n");

    if (!DllPathNextToExe(dllPath, MAX_PATH)) {
        wprintf(L"не найден путь к memreplace.dll\n");
        WaitConsole();
        return 1;
    }

    // Динамическая загрузка DLL во время работы программы
    mod = LoadLibraryW(dllPath);
    if (!mod) {
        wprintf(L"LoadLibraryW: ошибка %lu\n", GetLastError());
        WaitConsole();
        return 1;
    }

    // Адрес экспортируемой функции в загруженной DLL
    fn = (PFN_MemReplaceStringA)(void *)GetProcAddress(mod, "MemReplaceStringA");
    if (!fn) {
        wprintf(L"GetProcAddress: ошибка %lu\n", GetLastError());
        FreeLibrary(mod);
        WaitConsole();
        return 1;
    }

    wprintf(L"до:  \"%hs\"\n", (const char *)g_buffer);
    count = fn(LAB5_SEARCH, LAB5_REPLACE);
    wprintf(L"после: \"%hs\"\n", (const char *)g_buffer);
    wprintf(L"замен: %d\n", count);

    WaitConsole();
    FreeLibrary(mod);
    return (count < 0) ? 1 : 0;
}
