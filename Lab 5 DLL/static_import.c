#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>

#define LAB5_SEARCH "Lab5OldString!!"
#define LAB5_REPLACE "Lab5NewString!!"

// Статический импорт: линковщик подключает memreplace.dll при запуске EXE
__declspec(dllimport) int __cdecl MemReplaceStringA(const char *search, const char *replace);

// Тестовая строка в памяти этого процесса
static volatile char g_buffer[] = LAB5_SEARCH;

int main(void)
{
    int count;

    _setmode(_fileno(stdout), _O_U16TEXT);
    wprintf(L"Lab5Static — статический импорт memreplace.dll\n");

    wprintf(L"до:  \"%hs\"\n", (const char *)g_buffer);
    // Вызов экспорта DLL как обычной функции
    count = MemReplaceStringA(LAB5_SEARCH, LAB5_REPLACE);
    wprintf(L"после: \"%hs\"\n", (const char *)g_buffer);
    wprintf(L"замен: %d\n", count);

    fflush(stdout);
    system("pause");
    return (count < 0) ? 1 : 0;
}
