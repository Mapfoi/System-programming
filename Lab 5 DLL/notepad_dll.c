#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wchar.h>

#define LAB5_PAIR_MAX 260
#define MAX_DOC (256 * 1024)

// find / replace, переданные из Lab5InjectNotepad (wide)
typedef struct {
    wchar_t find[LAB5_PAIR_MAX];
    wchar_t replace[LAB5_PAIR_MAX];
} NotepadReplaceParams;

static int g_total;
static const wchar_t *g_find;
static const wchar_t *g_repl;

// Все вхождения g_find в буфере → g_repl (длины совпадают)
static void ReplaceAll(wchar_t *buf, size_t slen, int *count)
{
    wchar_t *p;

    for (p = buf; *p; ++p) {
        if (wcsncmp(p, g_find, slen) != 0)
            continue;
        wmemcpy(p, g_repl, slen);
        (*count)++;
        p += slen - 1;
    }
}

// Обход окон процесса notepad: WM_GETTEXT / WM_SETTEXT
static BOOL CALLBACK TryWindow(HWND hwnd, LPARAM unused)
{
    DWORD pid;
    int len;
    wchar_t *buf;
    int n = 0;
    size_t slen;

    (void)unused;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId())
        return TRUE;

    len = (int)SendMessageW(hwnd, WM_GETTEXTLENGTH, 0, 0);
    if (len > 0 && len <= MAX_DOC) {
        slen = wcslen(g_find);
        buf = (wchar_t *)HeapAlloc(GetProcessHeap(), 0, ((size_t)len + 2) * sizeof(wchar_t));
        // Если в тексте окна есть find — заменяем и пишем обратно
        if (buf && SendMessageW(hwnd, WM_GETTEXT, (WPARAM)(len + 1), (LPARAM)buf) > 0 && wcsstr(buf, g_find)) {
            ReplaceAll(buf, slen, &n);
            if (n > 0) {
                SendMessageW(hwnd, WM_SETTEXT, 0, (LPARAM)buf);
                g_total += n;
            }
        }
        if (buf)
            HeapFree(GetProcessHeap(), 0, buf);
    }

    // У Win11-notepad несколько внутренних буферов — обходим дочерние окна
    EnumChildWindows(hwnd, TryWindow, 0);
    return TRUE;
}

static int ReplaceNotepadText(const wchar_t *find, const wchar_t *repl)
{
    g_total = 0;
    g_find = find;
    g_repl = repl;
    EnumWindows(TryWindow, 0);
    return g_total;
}

// Точка входа для CreateRemoteThread (параметр — NotepadReplaceParams в памяти notepad)
__declspec(dllexport) DWORD WINAPI NotepadRemoteEntry(LPVOID lpParameter)
{
    NotepadReplaceParams *params = (NotepadReplaceParams *)lpParameter;
    int n;

    if (!params || !params->find[0])
        return (DWORD)-1;

    n = ReplaceNotepadText(params->find, params->replace);
    return (n < 1) ? (DWORD)-1 : (DWORD)n;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID reserved)
{
    (void)h;
    (void)reason;
    (void)reserved;
    return TRUE;
}
