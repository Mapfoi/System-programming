#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wingdi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Кадр 1920×1080, окно фиксированного размера 960×540 (16:9)
#define WIDTH 1920
#define HEIGHT 1080
#define WND_W 1920
#define WND_H 1080
#define TILE 64
#define MAX_ITER 800
#define MAX_THREADS 64
#define TILES_X ((WIDTH + TILE - 1) / TILE)
#define TILES_Y ((HEIGHT + TILE - 1) / TILE)
#define MAX_TILES (TILES_X * TILES_Y)

// Задание очереди: тайл кадра; SLIST_ENTRY выровнен для Interlocked SList
typedef struct DECLSPEC_ALIGN(MEMORY_ALLOCATION_ALIGNMENT) {
    SLIST_ENTRY link;
    int x, y, w, h;
} Tile;

// Атомарная очередь заданий: lock-free SLIST, вставка несколькими потоками
DECLSPEC_ALIGN(MEMORY_ALLOCATION_ALIGNMENT) static SLIST_HEADER g_queue;
static Tile g_tiles[MAX_TILES];
static BYTE g_pixels[WIDTH * HEIGHT * 3];
static HANDLE g_sem, g_allDone;
static HWND g_hwnd;
static LONG g_done;
static int g_nThreads;
static double g_t0;

static double Now(void)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER t;
    if (!freq.QuadPart)
        QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)freq.QuadPart;
}

// Итерации Мандельброта для пикселя, RGB в BGR
static void PutPixel(BYTE *img, int x, int y)
{
    double cr = (x - WIDTH * 0.5) * (3.0 / WIDTH) - 0.55;
    double ci = (y - HEIGHT * 0.5) * (3.0 / WIDTH);
    double zr = 0, zi = 0, t;
    int n = 0;
    BYTE *p = img + ((size_t)y * WIDTH + (size_t)x) * 3;
    while (n < MAX_ITER && zr * zr + zi * zi <= 4.0) {
        t = zr * zr - zi * zi + cr;
        zi = 2.0 * zr * zi + ci;
        zr = t;
        ++n;
    }
    if (n == MAX_ITER)
        p[0] = p[1] = p[2] = 0;
    else {
        p[0] = (BYTE)(n * 9);
        p[1] = (BYTE)(n * 5);
        p[2] = (BYTE)(255 - n);
    }
}

static void FillTile(BYTE *img, int x0, int y0, int tw, int th)
{
    int x, y;
    for (y = y0; y < y0 + th; ++y)
        for (x = x0; x < x0 + tw; ++x)
            PutPixel(img, x, y);
}

// Вставка в очередь атомарна: InterlockedPushEntrySList
static void Enqueue(Tile *tile)
{
    InterlockedPushEntrySList(&g_queue, &tile->link);
    ReleaseSemaphore(g_sem, 1, NULL);
}

// Извлечение задания атомарно: InterlockedPopEntrySList
static Tile *Dequeue(void)
{
    PSLIST_ENTRY e = InterlockedPopEntrySList(&g_queue);
    return e ? CONTAINING_RECORD(e, Tile, link) : NULL;
}

// Готовый тайл сразу показывается в окне (координаты 1920×1080 → 960×540)
static void InvalidateTile(const Tile *t)
{
    HWND w = g_hwnd;
    RECT r;
    if (!w)
        return;
    r.left = t->x * WND_W / WIDTH;
    r.top = t->y * WND_H / HEIGHT;
    r.right = (t->x + t->w) * WND_W / WIDTH;
    r.bottom = (t->y + t->h) * WND_H / HEIGHT;
    if (r.right <= r.left)
        r.right = r.left + 1;
    if (r.bottom <= r.top)
        r.bottom = r.top + 1;
    InvalidateRect(w, &r, FALSE);
}

// Входной поток: делит кадр на тайлы 64×64 и кладёт задания в очередь
static DWORD WINAPI InputProc(void *unused)
{
    int x, y, n = 0;
    (void)unused;
    for (y = 0; y < HEIGHT; y += TILE)
        for (x = 0; x < WIDTH; x += TILE) {
            Tile *t = &g_tiles[n++];
            t->x = x;
            t->y = y;
            t->w = WIDTH - x < TILE ? WIDTH - x : TILE;
            t->h = HEIGHT - y < TILE ? HEIGHT - y : TILE;
            Enqueue(t);
        }
    return 0;
}

// Рабочий поток: считает тайл, пишет RGB и просит окно перерисовать его
static DWORD WINAPI WorkerProc(void *unused)
{
    Tile *t;
    (void)unused;
    for (;;) {
        WaitForSingleObject(g_sem, INFINITE);
        t = Dequeue();
        if (!t)
            break;
        FillTile(g_pixels, t->x, t->y, t->w, t->h);
        InvalidateTile(t);
        if (InterlockedIncrement(&g_done) == MAX_TILES)
            SetEvent(g_allDone);
    }
    return 0;
}

// Выходной поток: после всех тайлов — время в заголовке и финальная отрисовка
static DWORD WINAPI OutputProc(void *unused)
{
    wchar_t title[160];
    HWND w;
    (void)unused;
    WaitForSingleObject(g_allDone, INFINITE);
    _snwprintf(title, 160, L"Mandelbrot — %d threads, %.3f s",
               g_nThreads, Now() - g_t0);
    w = g_hwnd;
    if (w) {
        SetWindowTextW(w, title);
        InvalidateRect(w, NULL, FALSE);
    }
    return 0;
}

// Кадр из буфера в клиент фиксированного размера
static void DrawFrame(HDC hdc)
{
    BITMAPINFOHEADER ih = {0};
    ih.biSize = sizeof(ih);
    ih.biWidth = WIDTH;
    ih.biHeight = -HEIGHT;
    ih.biPlanes = 1;
    ih.biBitCount = 24;
    StretchDIBits(hdc, 0, 0, WND_W, WND_H, 0, 0, WIDTH, HEIGHT,
                  g_pixels, (BITMAPINFO *)&ih, DIB_RGB_COLORS, SRCCOPY);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        DrawFrame(hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DESTROY:
        g_hwnd = NULL;
        SetEvent(g_allDone);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// Обработчик очереди: N рабочих + цикл сообщений, чтобы окно жило во время рендера
static void RunLive(HINSTANCE instance, int show, int nThreads)
{
    WNDCLASSW wc = {0};
    RECT wr = {0, 0, WND_W, WND_H};
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    HANDLE workers[MAX_THREADS], hIn, hOut;
    MSG msg;
    int i, x, y, ww, wh;
    if (nThreads < 1)
        nThreads = 1;
    if (nThreads > MAX_THREADS)
        nThreads = MAX_THREADS;
    g_nThreads = nThreads;

    memset(g_pixels, 0, sizeof(g_pixels));
    InitializeSListHead(&g_queue);
    g_done = 0;
    g_sem = CreateSemaphoreW(NULL, 0, MAX_TILES + MAX_THREADS, NULL);
    g_allDone = CreateEventW(NULL, TRUE, FALSE, NULL);

    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = L"Lab3Mandelbrot";
    RegisterClassW(&wc);

    AdjustWindowRect(&wr, style, FALSE);
    ww = wr.right - wr.left;
    wh = wr.bottom - wr.top;
    x = (GetSystemMetrics(SM_CXSCREEN) - ww) / 2;
    y = (GetSystemMetrics(SM_CYSCREEN) - wh) / 2;
    g_hwnd = CreateWindowW(L"Lab3Mandelbrot", L"Mandelbrot — rendering...",
                           style, x, y, ww, wh, NULL, NULL, instance, NULL);
    if (!g_hwnd)
        return;
    ShowWindow(g_hwnd, show);
    UpdateWindow(g_hwnd);

    for (i = 0; i < nThreads; ++i)
        workers[i] = CreateThread(NULL, 0, WorkerProc, NULL, 0, NULL);
    hOut = CreateThread(NULL, 0, OutputProc, NULL, 0, NULL);
    g_t0 = Now();
    hIn = CreateThread(NULL, 0, InputProc, NULL, 0, NULL);
    WaitForSingleObject(hIn, INFINITE);
    CloseHandle(hIn);
    for (i = 0; i < nThreads; ++i)
        ReleaseSemaphore(g_sem, 1, NULL);

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    WaitForSingleObject(hOut, INFINITE);
    CloseHandle(hOut);
    WaitForMultipleObjects((DWORD)nThreads, workers, TRUE, INFINITE);
    for (i = 0; i < nThreads; ++i)
        CloseHandle(workers[i]);
    CloseHandle(g_sem);
    CloseHandle(g_allDone);
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE prev, LPSTR cmd, int show)
{
    SYSTEM_INFO si;
    int nThreads;
    (void)prev;
    GetSystemInfo(&si);
    nThreads = atoi(cmd);
    if (nThreads < 1)
        nThreads = (int)si.dwNumberOfProcessors;
    if (nThreads < 1)
        nThreads = 1;
    RunLive(instance, show, nThreads);
    return 0;
}
