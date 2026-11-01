#define WIN32_LEAN_AND_MEAN
#define MEMREPLACE_DLL_EXPORTS
#include "memreplace_api.h"
#include <string.h>

/* Обход виртуальной памяти процесса: VirtualQuery + VirtualProtect (OSiSP, с. 118). */

static BOOL RegionWritable(DWORD protect)
{
    if (protect & PAGE_GUARD)
        return FALSE;
    if (protect & PAGE_NOACCESS)
        return FALSE;

    switch (protect & 0xFF) {
    case PAGE_READWRITE:
    case PAGE_WRITECOPY:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
        return TRUE;
    default:
        return FALSE;
    }
}

static BOOL RegionOverlapsSkip(const MEMORY_BASIC_INFORMATION *mbi,
                               const void *skipStart, SIZE_T skipSize)
{
    const unsigned char *regionStart;
    const unsigned char *regionEnd;
    const unsigned char *skipLo;
    const unsigned char *skipHi;

    if (!skipStart || skipSize == 0)
        return FALSE;

    regionStart = (const unsigned char *)mbi->BaseAddress;
    regionEnd = regionStart + mbi->RegionSize;
    skipLo = (const unsigned char *)skipStart;
    skipHi = skipLo + skipSize;
    return regionStart < skipHi && skipLo < regionEnd;
}

static BOOL RegionSelected(const MEMORY_BASIC_INFORMATION *mbi, BOOL hostOnly)
{
    if (mbi->State != MEM_COMMIT || !RegionWritable(mbi->Protect))
        return FALSE;

    if (hostOnly)
        return (mbi->Type == MEM_PRIVATE);

    return (mbi->Type == MEM_PRIVATE || mbi->Type == MEM_IMAGE);
}

static int ReplaceInBuffer(unsigned char *base, SIZE_T size,
                           const char *search, const char *replace,
                           size_t len, MemReplaceStats *stats)
{
    int localMatches = 0;
    SIZE_T i;

    if (len == 0 || size < len)
        return 0;

    for (i = 0; i + len <= size; ++i) {
        if (memcmp(base + i, search, len) != 0)
            continue;

        DWORD oldProtect = 0;
        if (!VirtualProtect(base + i, len, PAGE_READWRITE, &oldProtect))
            continue;

        memcpy(base + i, replace, len);
        VirtualProtect(base + i, len, oldProtect, &oldProtect);

        localMatches++;
        if (stats)
            stats->bytesWritten += (DWORD)len;
    }
    return localMatches;
}

static int ScanProcessMemory(const char *search, const char *replace, MemReplaceStats *stats,
                             const void *skipStart, SIZE_T skipSize, BOOL hostOnly)
{
    SYSTEM_INFO si;
    unsigned char *cursor;
    unsigned char *maxAddr;
    size_t searchLen;
    size_t replaceLen;
    int total = 0;

    if (!search || !replace)
        return -1;

    searchLen = strlen(search);
    replaceLen = strlen(replace);
    if (searchLen == 0 || searchLen != replaceLen)
        return -2;

    if (stats) {
        stats->regionsScanned = 0;
        stats->matchesFound = 0;
        stats->bytesWritten = 0;
    }

    GetSystemInfo(&si);
    cursor = (unsigned char *)si.lpMinimumApplicationAddress;
    maxAddr = (unsigned char *)si.lpMaximumApplicationAddress;

    while (cursor < maxAddr) {
        MEMORY_BASIC_INFORMATION mbi;
        int found;

        if (VirtualQuery(cursor, &mbi, sizeof(mbi)) != sizeof(mbi))
            break;

        if (!RegionOverlapsSkip(&mbi, skipStart, skipSize) &&
            RegionSelected(&mbi, hostOnly)) {
            found = ReplaceInBuffer((unsigned char *)mbi.BaseAddress, mbi.RegionSize,
                                    search, replace, searchLen, stats);
            total += found;
            if (stats) {
                stats->regionsScanned++;
                stats->matchesFound += (DWORD)found;
            }
        }

        cursor = (unsigned char *)mbi.BaseAddress + mbi.RegionSize;
    }

    return total;
}

MEMREPLACE_API int __cdecl MemReplaceStringA(const char *search, const char *replace, MemReplaceStats *stats)
{
    return ScanProcessMemory(search, replace, stats, NULL, 0, FALSE);
}

MEMREPLACE_API DWORD WINAPI MemReplaceRemoteEntry(LPVOID lpParameter)
{
    MemReplaceStats stats = {0};
    int replaced;

    if (!lpParameter)
        return (DWORD)-1;

    /* lpParameter — блок MemReplaceParams из WriteProcessMemory (OSiSP, с. 156). */
    replaced = ScanProcessMemory(LAB5_DEMO_SEARCH, LAB5_DEMO_REPLACE, &stats,
                                 lpParameter, sizeof(MemReplaceParams), TRUE);
    if (replaced < 0)
        return (DWORD)-1;
    return (DWORD)replaced;
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
    (void)hinstDLL;
    (void)lpvReserved;
    (void)fdwReason;
    return TRUE;
}
