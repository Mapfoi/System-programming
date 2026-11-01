#ifndef MEMREPLACE_API_H
#define MEMREPLACE_API_H

#include <windows.h>

#ifdef MEMREPLACE_DLL_EXPORTS
#define MEMREPLACE_API __declspec(dllexport)
#else
#define MEMREPLACE_API __declspec(dllimport)
#endif

#define LAB5_DEMO_SEARCH "Lab5OldString!!"
#define LAB5_DEMO_REPLACE "Lab5NewString!!"

typedef struct MemReplaceStats {
    DWORD regionsScanned;
    DWORD matchesFound;
    DWORD bytesWritten;
} MemReplaceStats;

/* Аргументы, записанные в целевой процесс через WriteProcessMemory (OSiSP, с. 156). */
typedef struct MemReplaceParams {
    char search[128];
    char replace[128];
} MemReplaceParams;

MEMREPLACE_API int __cdecl MemReplaceStringA(const char *search, const char *replace, MemReplaceStats *stats);
MEMREPLACE_API DWORD WINAPI MemReplaceRemoteEntry(LPVOID lpParameter);

typedef int (__cdecl *PFN_MemReplaceStringA)(const char *, const char *, MemReplaceStats *);

#endif
