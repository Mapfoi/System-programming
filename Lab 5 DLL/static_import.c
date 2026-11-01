#define WIN32_LEAN_AND_MEAN
#include "memreplace_api.h"
#include <fcntl.h>
#include <io.h>
#include <stdio.h>

static volatile char g_demoBuffer[] = LAB5_DEMO_SEARCH;

int main(void)
{
    MemReplaceStats stats = {0};
    PFN_MemReplaceStringA fn;
    int count;

    _setmode(_fileno(stdout), _O_U16TEXT);

    wprintf(L"=== Lab5Static (статический импорт memreplace.dll) ===\n");
    fn = MemReplaceStringA;
    wprintf(L"Указатель на экспорт DLL: %p\n", (void *)fn);

    wprintf(L"До замены: \"%hs\"\n", (const char *)g_demoBuffer);
    count = fn(LAB5_DEMO_SEARCH, LAB5_DEMO_REPLACE, &stats);
    wprintf(L"После замены: \"%hs\"\n", (const char *)g_demoBuffer);
    wprintf(L"Заменено вхождений: %d (регионов: %lu, байт: %lu)\n",
            count, stats.regionsScanned, stats.bytesWritten);

    return count < 0 ? 1 : 0;
}
