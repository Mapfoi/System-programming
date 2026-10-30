#ifndef MEMREPLACE_API_H
#define MEMREPLACE_API_H

#include <windows.h>

#ifdef MEMREPLACE_DLL_EXPORTS
#define MEMREPLACE_API __declspec(dllexport)
#else
#define MEMREPLACE_API __declspec(dllimport)
#endif

typedef struct MemReplaceStats {
    DWORD regionsScanned;
    DWORD matchesFound;
    DWORD bytesWritten;
} MemReplaceStats;

typedef struct MemReplaceParams {
    char search[128];
    char replace[128];
} MemReplaceParams;

/* Поиск ASCII-строки во всех доступных для записи регионах текущего процесса. Длины должны совпадать. */
MEMREPLACE_API int __cdecl MemReplaceStringA(const char *search, const char *replace, MemReplaceStats *stats);

/* Точка входа для CreateRemoteThread: lpParameter -> MemReplaceParams в адресном пространстве процесса. */
MEMREPLACE_API DWORD WINAPI MemReplaceRemoteEntry(LPVOID lpParameter);

typedef int (__cdecl *PFN_MemReplaceStringA)(const char *, const char *, MemReplaceStats *);
typedef DWORD (WINAPI *PFN_MemReplaceRemoteEntry)(LPVOID);

#endif
