#define WIN32_LEAN_AND_MEAN
#include "memreplace_api.h"
#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <tlhelp32.h>

static BOOL EnableDebugPrivilege(void)
{
    HANDLE token = NULL;
    TOKEN_PRIVILEGES tp;
    LUID luid;

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
        return FALSE;

    if (!LookupPrivilegeValueW(NULL, L"SeDebugPrivilege", &luid)) {
        CloseHandle(token);
        return FALSE;
    }

    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), NULL, NULL);
    CloseHandle(token);
    return GetLastError() == ERROR_SUCCESS;
}

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

static HMODULE FindRemoteModule(DWORD pid, const wchar_t *dllFileName)
{
    HANDLE snap;
    MODULEENTRY32W me;
    HMODULE found = NULL;

    snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE)
        return NULL;

    me.dwSize = sizeof(me);
    if (Module32FirstW(snap, &me)) {
        do {
            if (_wcsicmp(me.szModule, dllFileName) == 0) {
                found = (HMODULE)me.modBaseAddr;
                break;
            }
        } while (Module32NextW(snap, &me));
    }

    CloseHandle(snap);
    return found;
}

static BOOL InjectLibrary(HANDLE process, DWORD pid, const wchar_t *dllPath, HMODULE *remoteModuleOut)
{
    SIZE_T pathBytes;
    LPVOID remotePath = NULL;
    HANDLE thread = NULL;
    HMODULE kernel32;
    LPTHREAD_START_ROUTINE loadLibraryW;
    DWORD exitCode = 0;
    BOOL ok = FALSE;

    pathBytes = (wcslen(dllPath) + 1) * sizeof(wchar_t);
    remotePath = VirtualAllocEx(process, NULL, pathBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remotePath)
        goto cleanup;

    if (!WriteProcessMemory(process, remotePath, dllPath, pathBytes, NULL))
        goto cleanup;

    kernel32 = GetModuleHandleW(L"kernel32.dll");
    loadLibraryW = (LPTHREAD_START_ROUTINE)(void *)GetProcAddress(kernel32, "LoadLibraryW");
    if (!loadLibraryW)
        goto cleanup;

    thread = CreateRemoteThread(process, NULL, 0, loadLibraryW, remotePath, 0, NULL);
    if (!thread)
        goto cleanup;

    WaitForSingleObject(thread, INFINITE);
    if (!GetExitCodeThread(thread, &exitCode) || exitCode == 0)
        goto cleanup;

    if (remoteModuleOut) {
        *remoteModuleOut = FindRemoteModule(pid, L"memreplace.dll");
        if (!*remoteModuleOut)
            *remoteModuleOut = (HMODULE)(ULONG_PTR)exitCode;
    }
    ok = TRUE;

cleanup:
    if (thread)
        CloseHandle(thread);
    if (remotePath)
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
    return ok;
}

static FARPROC ResolveRemoteExport(HANDLE process, HMODULE remoteModule,
                                   const wchar_t *dllPath, const char *exportName)
{
    HMODULE localModule;
    FARPROC localFn;
    FARPROC remoteFn;
    ULONG_PTR offset;

    localModule = LoadLibraryW(dllPath);
    if (!localModule)
        return NULL;

    localFn = GetProcAddress(localModule, exportName);
    if (!localFn) {
        FreeLibrary(localModule);
        return NULL;
    }

    offset = (ULONG_PTR)localFn - (ULONG_PTR)localModule;
    remoteFn = (FARPROC)((ULONG_PTR)remoteModule + offset);
    FreeLibrary(localModule);
    (void)process;
    return remoteFn;
}

static BOOL CallRemoteReplace(HANDLE process, FARPROC remoteEntry,
                              const char *search, const char *replace, DWORD *replacedOut)
{
    MemReplaceParams params;
    LPVOID remoteParams = NULL;
    HANDLE thread = NULL;
    DWORD exitCode = 0;
    BOOL ok = FALSE;

    memset(&params, 0, sizeof(params));
    strncpy(params.search, search, sizeof(params.search) - 1);
    strncpy(params.replace, replace, sizeof(params.replace) - 1);

    remoteParams = VirtualAllocEx(process, NULL, sizeof(params), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remoteParams)
        goto cleanup;

    if (!WriteProcessMemory(process, remoteParams, &params, sizeof(params), NULL))
        goto cleanup;

    thread = CreateRemoteThread(process, NULL, 0,
                                (LPTHREAD_START_ROUTINE)(void *)remoteEntry, remoteParams, 0, NULL);
    if (!thread)
        goto cleanup;

    WaitForSingleObject(thread, INFINITE);
    if (!GetExitCodeThread(thread, &exitCode) || exitCode == STILL_ACTIVE)
        goto cleanup;
    if (exitCode >= 0x80000000UL) {
        if (replacedOut)
            *replacedOut = 0;
        goto cleanup;
    }

    if (replacedOut)
        *replacedOut = exitCode;
    ok = TRUE;

cleanup:
    if (thread)
        CloseHandle(thread);
    if (remoteParams)
        VirtualFreeEx(process, remoteParams, 0, MEM_RELEASE);
    return ok;
}

int main(int argc, char **argv)
{
    DWORD pid;
    HANDLE process = NULL;
    wchar_t dllPath[MAX_PATH];
    HMODULE remoteModule = NULL;
    FARPROC remoteFn = NULL;
    DWORD replaced = 0;
    const char *search = "Lab5OldString!!";
    const char *replace = "Lab5NewString!!";

    _setmode(_fileno(stdout), _O_U16TEXT);

    wprintf(L"=== Lab5Inject (CreateRemoteThread + LoadLibraryW) ===\n");

    if (argc < 2) {
        wprintf(L"Использование: Lab5Inject <PID>\n");
        wprintf(L"Сначала запустите Lab5Target и передайте его PID.\n");
        return 1;
    }

    pid = (DWORD)strtoul(argv[1], NULL, 10);
    if (pid == 0) {
        wprintf(L"Некорректный PID\n");
        return 1;
    }

    if (!ResolveDllPath(dllPath, MAX_PATH)) {
        wprintf(L"Не удалось собрать путь к memreplace.dll\n");
        return 1;
    }

    EnableDebugPrivilege();

    process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                              PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                          FALSE, pid);
    if (!process) {
        wprintf(L"OpenProcess(PID=%lu) ошибка %lu\n", pid, GetLastError());
        return 1;
    }

    wprintf(L"Целевой процесс PID=%lu\n", pid);
    wprintf(L"DLL: %ls\n", dllPath);

    if (!InjectLibrary(process, pid, dllPath, &remoteModule)) {
        wprintf(L"Внедрение LoadLibraryW не удалось, код %lu\n", GetLastError());
        CloseHandle(process);
        return 1;
    }
    wprintf(L"База memreplace.dll в целевом процессе: %p\n", (void *)remoteModule);

    remoteFn = ResolveRemoteExport(process, remoteModule, dllPath, "MemReplaceRemoteEntry");
    if (!remoteFn) {
        wprintf(L"Не удалось вычислить адрес MemReplaceRemoteEntry в целевом процессе\n");
        CloseHandle(process);
        return 1;
    }
    wprintf(L"Адрес функции DLL в целевом процессе: %p\n", (void *)remoteFn);

    if (!CallRemoteReplace(process, remoteFn, search, replace, &replaced)) {
        wprintf(L"CreateRemoteThread(MemReplaceRemoteEntry) ошибка %lu\n", GetLastError());
        CloseHandle(process);
        return 1;
    }

    if (replaced == 0 || replaced >= 0x80000000UL) {
        wprintf(L"Удалённая замена не выполнена (код потока %lu)\n", replaced);
        CloseHandle(process);
        return 1;
    }

    wprintf(L"Удалённая замена выполнена, вхождений: %lu\n", replaced);
    wprintf(L"Проверьте окно Lab5Target — строка должна стать \"%hs\".\n", replace);

    CloseHandle(process);
    return 0;
}
