#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tlhelp32.h>

#define LAB5_PAIR_MAX 260

typedef struct {
    char find[LAB5_PAIR_MAX];
    char replace[LAB5_PAIR_MAX];
} MemReplaceRemoteParams;

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

static HMODULE RemoteModuleBase(DWORD pid)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    MODULEENTRY32W me;
    HMODULE base = NULL;

    if (snap == INVALID_HANDLE_VALUE)
        return NULL;
    me.dwSize = sizeof(me);
    if (Module32FirstW(snap, &me)) {
        do {
            if (_wcsicmp(me.szModule, L"memreplace.dll") == 0) {
                base = (HMODULE)me.modBaseAddr;
                break;
            }
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    return base;
}

static FARPROC RemoteProcAddress(HMODULE remoteBase, const wchar_t *dllPath, const char *name)
{
    HMODULE local = LoadLibraryW(dllPath);
    FARPROC fn;
    ULONG_PTR delta;

    if (!local)
        return NULL;
    fn = GetProcAddress(local, name);
    if (!fn) {
        FreeLibrary(local);
        return NULL;
    }
    delta = (ULONG_PTR)fn - (ULONG_PTR)local;
    FreeLibrary(local);
    return (FARPROC)((ULONG_PTR)remoteBase + delta);
}

static BOOL RemoteLoadDll(HANDLE proc, DWORD pid, const wchar_t *dllPath, HMODULE *outBase)
{
    SIZE_T bytes = (wcslen(dllPath) + 1) * sizeof(wchar_t);
    LPVOID remote = NULL;
    LPTHREAD_START_ROUTINE loadLib;
    HANDLE th;
    DWORD code = 0;
    BOOL ok = FALSE;

    remote = VirtualAllocEx(proc, NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote)
        return FALSE;

    if (WriteProcessMemory(proc, remote, dllPath, bytes, NULL)) {
        loadLib = (LPTHREAD_START_ROUTINE)(void *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"),
                                                               "LoadLibraryW");
        if (loadLib) {
            th = CreateRemoteThread(proc, NULL, 0, loadLib, remote, 0, NULL);
            if (th) {
                WaitForSingleObject(th, INFINITE);
                GetExitCodeThread(th, &code);
                CloseHandle(th);
                if (code != 0) {
                    if (outBase)
                        *outBase = RemoteModuleBase(pid);
                    ok = TRUE;
                }
            }
        }
    }

    VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
    return ok;
}

int main(int argc, char **argv)
{
    DWORD pid;
    HANDLE proc;
    wchar_t dllPath[MAX_PATH];
    HMODULE remoteBase;
    FARPROC remoteFn;
    HANDLE th;
    DWORD result = 0;
    MemReplaceRemoteParams localParams;
    LPVOID remoteParams = NULL;

    _setmode(_fileno(stdout), _O_U16TEXT);

    if (argc < 4) {
        wprintf(L"Lab5Inject <PID> <find> <replace>\n");
        return 1;
    }

    if (strlen(argv[2]) != strlen(argv[3])) {
        wprintf(L"find и replace — одной длины\n");
        return 1;
    }

    pid = (DWORD)strtoul(argv[1], NULL, 10);
    if (!pid || !DllPathNextToExe(dllPath, MAX_PATH)) {
        wprintf(L"неверный PID или путь к DLL\n");
        return 1;
    }

    memset(&localParams, 0, sizeof(localParams));
    strncpy(localParams.find, argv[2], LAB5_PAIR_MAX - 1);
    strncpy(localParams.replace, argv[3], LAB5_PAIR_MAX - 1);

    wprintf(L"memreplace.dll: %ls\n", dllPath);

    proc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                          PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                      FALSE, pid);
    if (!proc) {
        wprintf(L"OpenProcess: %lu\n", GetLastError());
        return 1;
    }

    if (!RemoteLoadDll(proc, pid, dllPath, &remoteBase)) {
        wprintf(L"не удалось загрузить DLL в процесс\n");
        CloseHandle(proc);
        return 1;
    }

    remoteFn = RemoteProcAddress(remoteBase, dllPath, "MemReplaceRemoteEntry");
    if (!remoteFn) {
        wprintf(L"не найден MemReplaceRemoteEntry\n");
        CloseHandle(proc);
        return 1;
    }

    remoteParams = VirtualAllocEx(proc, NULL, sizeof(localParams), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remoteParams || !WriteProcessMemory(proc, remoteParams, &localParams, sizeof(localParams), NULL)) {
        wprintf(L"VirtualAllocEx/WriteProcessMemory (params): %lu\n", GetLastError());
        if (remoteParams)
            VirtualFreeEx(proc, remoteParams, 0, MEM_RELEASE);
        CloseHandle(proc);
        return 1;
    }

    th = CreateRemoteThread(proc, NULL, 0, (LPTHREAD_START_ROUTINE)(void *)remoteFn, remoteParams, 0, NULL);
    if (!th) {
        wprintf(L"CreateRemoteThread: %lu\n", GetLastError());
        VirtualFreeEx(proc, remoteParams, 0, MEM_RELEASE);
        CloseHandle(proc);
        return 1;
    }
    WaitForSingleObject(th, INFINITE);
    GetExitCodeThread(th, &result);
    CloseHandle(th);
    VirtualFreeEx(proc, remoteParams, 0, MEM_RELEASE);
    CloseHandle(proc);

    if ((LONG)(DWORD)result == -1)
        wprintf(L"ошибка или замен не найдено\n");
    else
        wprintf(L"замен в памяти мишени: %lu\n", result);
    return ((LONG)(DWORD)result == -1) ? 1 : 0;
}
