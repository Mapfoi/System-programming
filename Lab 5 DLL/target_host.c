#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <stdio.h>

#define LAB5_SEARCH "Lab5OldString!!"

// Строка, которую Lab5Inject должен заменить через внедрённую DLL
volatile char g_text[] = LAB5_SEARCH;

int main(void)
{
    _setmode(_fileno(stdout), _O_U16TEXT);
    wprintf(L"Lab5Target — PID %lu\n", GetCurrentProcessId());
    wprintf(L"строка: \"%hs\"\n", (const char *)g_text);
    wprintf(L"запустите Lab5Inject.exe <PID>\n\n");

    // Периодически показываем, изменилась ли строка после инъекции
    for (;;) {
        wprintf(L"[%lu] \"%hs\"   \r", GetTickCount(), (const char *)g_text);
        fflush(stdout);
        Sleep(1000);
    }
    return 0;
}
