#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tlhelp32.h>

#define DLL_NAME L"memreplace_notepad.dll"
#define ENTRY_NAME "NotepadRemoteEntry"
#define LAB5_PAIR_MAX 260

typedef struct {
    wchar_t find[LAB5_PAIR_MAX];
    wchar_t replace[LAB5_PAIR_MAX];
} NotepadReplaceParams;

// Путь к memreplace_notepad.dll рядом с Lab5InjectNotepad.exe
static void DllPath(wchar_t *path, DWORD cap)
{
    DWORD n = GetModuleFileNameW(NULL, path, cap);
    wchar_t *slash = wcsrchr(path, L'\\');

    if (n && slash) {
        slash[1] = L'\0';
        wcscat(path, DLL_NAME);
    }
}

// База memreplace_notepad.dll в списке модулей целевого процесса
static HMODULE RemoteBase(DWORD pid)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    MODULEENTRY32W me;
    HMODULE base = NULL;

    if (snap == INVALID_HANDLE_VALUE)
        return NULL;
    me.dwSize = sizeof(me);
    if (Module32FirstW(snap, &me)) {
        do {
            if (_wcsicmp(me.szModule, DLL_NAME) == 0) {
                base = (HMODULE)me.modBaseAddr;
                break;
            }
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    return base;
}

// Адрес NotepadRemoteEntry в мишени: remote_base + (local_fn − local_base)
static FARPROC RemoteEntry(HMODULE remoteBase, const wchar_t *path)
{
    HMODULE local = LoadLibraryW(path);
    FARPROC fn;
    ULONG_PTR delta;

    if (!local)
        return NULL;
    fn = GetProcAddress(local, ENTRY_NAME);
    delta = fn ? (ULONG_PTR)fn - (ULONG_PTR)local : 0;
    FreeLibrary(local);
    return fn ? (FARPROC)((ULONG_PTR)remoteBase + delta) : NULL;
}

// Удалённый поток в notepad вызывает LoadLibraryW с путём к DLL
static BOOL InjectDll(HANDLE proc, DWORD pid, const wchar_t *path, HMODULE *outBase)
{
    SIZE_T bytes = (wcslen(path) + 1) * sizeof(wchar_t);
    LPVOID remote = VirtualAllocEx(proc, NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    LPTHREAD_START_ROUTINE loadLib;
    HANDLE th;
    DWORD code = 0;
    BOOL ok = FALSE;

    if (!remote)
        return FALSE;

    if (WriteProcessMemory(proc, remote, path, bytes, NULL)) {
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
                        *outBase = RemoteBase(pid);
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
    DWORD pid, result = 0;
    wchar_t path[MAX_PATH];
    HANDLE proc, th;
    HMODULE base;
    FARPROC fn;
    NotepadReplaceParams localParams;
    LPVOID remoteParams = NULL;

    _setmode(_fileno(stdout), _O_U16TEXT);

    if (argc < 4) {
        wprintf(L"Lab5InjectNotepad <PID> <find> <replace>\n");
        return 1;
    }

    // find и replace должны быть одной длины (как в задании по памяти)
    if (strlen(argv[2]) != strlen(argv[3])) {
        wprintf(L"find и replace — одной длины\n");
        return 1;
    }

    memset(&localParams, 0, sizeof(localParams));
    MultiByteToWideChar(CP_ACP, 0, argv[2], -1, localParams.find, LAB5_PAIR_MAX);
    MultiByteToWideChar(CP_ACP, 0, argv[3], -1, localParams.replace, LAB5_PAIR_MAX);

    pid = (DWORD)strtoul(argv[1], NULL, 10);
    DllPath(path, MAX_PATH);
    if (!pid || !wcsstr(path, DLL_NAME)) {
        wprintf(L"неверный PID или путь к DLL\n");
        return 1;
    }

    // Открываем процесс Блокнота
    proc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                           PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                       FALSE, pid);
    if (!proc) {
        wprintf(L"OpenProcess: %lu\n", GetLastError());
        return 1;
    }

    // Шаг 1: загрузить memreplace_notepad.dll через LoadLibraryW
    if (!InjectDll(proc, pid, path, &base) || !(fn = RemoteEntry(base, path))) {
        wprintf(L"не удалось загрузить %ls\n", DLL_NAME);
        CloseHandle(proc);
        return 1;
    }

    // Шаг 2: записать find/replace в память notepad (аргумент NotepadRemoteEntry)
    remoteParams = VirtualAllocEx(proc, NULL, sizeof(localParams), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remoteParams || !WriteProcessMemory(proc, remoteParams, &localParams, sizeof(localParams), NULL)) {
        wprintf(L"VirtualAllocEx/WriteProcessMemory (params): %lu\n", GetLastError());
        if (remoteParams)
            VirtualFreeEx(proc, remoteParams, 0, MEM_RELEASE);
        CloseHandle(proc);
        return 1;
    }

    // Шаг 3: второй удалённый поток — NotepadRemoteEntry(remoteParams)
    th = CreateRemoteThread(proc, NULL, 0, (LPTHREAD_START_ROUTINE)(void *)fn, remoteParams, 0, NULL);
    if (th) {
        WaitForSingleObject(th, INFINITE);
        GetExitCodeThread(th, &result);
        CloseHandle(th);
    }
    VirtualFreeEx(proc, remoteParams, 0, MEM_RELEASE);
    CloseHandle(proc);

    if ((LONG)(DWORD)result == -1)
        wprintf(L"не найден find в окнах notepad\n");
    else
        wprintf(L"замен: %lu\n", result);
    return ((LONG)(DWORD)result == -1) ? 1 : 0;
}
