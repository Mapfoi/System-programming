/*
 * MVP main_win.c — WinMain: LoadLibrary → Init → цикл PeekMessage + PhysStep + GDI BitBlt.
 * Буфер тел в exe (VirtualAlloc) обновляется через PhysSnapshotRead после каждого шага.
 */
#include "common.h"
#include <windowsx.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define TARGET_FPS 60.0
#define FRAME_SEC  (1.0 / TARGET_FPS)

static PhysicsApi g_phys;
static SimConfig  g_cfg;
static SimStats   g_stats;

static Body     *g_bodies = NULL;
static BodyMeta *g_meta   = NULL;
static int       g_count  = 0;

static HDC     g_memDC = NULL;
static HBITMAP g_memBmp = NULL, g_oldBmp = NULL;
static int     g_bufW = 0, g_bufH = 0;

static BOOL  g_pan = FALSE;
static int   g_panX = 0, g_panY = 0;
static double g_fps = 60.0, g_lastT = 0.0;

static double Seconds(void)
{
    static LARGE_INTEGER f = {0};
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}

static void ScreenToWorld(int sx, int sy, int cw, int ch, double *wx, double *wy)
{
    *wx = ((double)sx - cw * 0.5) / g_cfg.camZoom + g_cfg.camX;
    *wy = ((double)sy - ch * 0.5) / g_cfg.camZoom + g_cfg.camY;
}

/* Загрузка DLL из каталога exe */
static BOOL LoadDll(wchar_t *err, size_t cap)
{
    g_phys.hModule = LoadLibraryW(L"gravity_physics.dll");
    if (!g_phys.hModule) {
        _snwprintf(err, cap, L"Не найдена gravity_physics.dll (соберите рядом с exe)");
        return FALSE;
    }
    g_phys.Init = (PFN_PhysInit)GetProcAddress(g_phys.hModule, "PhysInit");
    g_phys.Shutdown = (PFN_PhysShutdown)GetProcAddress(g_phys.hModule, "PhysShutdown");
    g_phys.ResetScene = (PFN_PhysResetScene)GetProcAddress(g_phys.hModule, "PhysResetScene");
    g_phys.Step = (PFN_PhysStep)GetProcAddress(g_phys.hModule, "PhysStep");
    g_phys.SnapshotRead = (PFN_PhysSnapshotRead)GetProcAddress(g_phys.hModule, "PhysSnapshotRead");
    if (!g_phys.Init || !g_phys.Shutdown || !g_phys.ResetScene || !g_phys.Step || !g_phys.SnapshotRead) {
        _snwprintf(err, cap, L"Экспорт DLL неполный");
        FreeLibrary(g_phys.hModule);
        return FALSE;
    }
    return TRUE;
}

static void EnsureBuffer(HWND hwnd, int w, int h)
{
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (g_memDC && w == g_bufW && h == g_bufH) return;
    HDC hdc = GetDC(hwnd);
    if (g_memDC) {
        SelectObject(g_memDC, g_oldBmp);
        DeleteObject(g_memBmp);
        DeleteDC(g_memDC);
    }
    g_memDC = CreateCompatibleDC(hdc);
    g_memBmp = CreateCompatibleBitmap(hdc, w, h);
    g_oldBmp = (HBITMAP)SelectObject(g_memDC, g_memBmp);
    g_bufW = w; g_bufH = h;
    ReleaseDC(hwnd, hdc);
}

static void Paint(HWND hwnd)
{
    RECT rc;
    GetClientRect(hwnd, &rc);
    int cw = rc.right - rc.left, ch = rc.bottom - rc.top;
    EnsureBuffer(hwnd, cw, ch);

    HDC dc = g_memDC;
    HBRUSH bg = CreateSolidBrush(RGB(12, 14, 22));
    FillRect(dc, &rc, bg);
    DeleteObject(bg);

    SetGraphicsMode(dc, GM_ADVANCED);
    XFORM xf = { (FLOAT)g_cfg.camZoom, 0, 0, (FLOAT)g_cfg.camZoom,
                 (FLOAT)(cw * 0.5 - g_cfg.camX * g_cfg.camZoom),
                 (FLOAT)(ch * 0.5 - g_cfg.camY * g_cfg.camZoom) };
    SetWorldTransform(dc, &xf);

    for (int i = 0; i < g_count; ++i) {
        if (!g_meta[i].alive) continue;
        const Body *b = &g_bodies[i];
        HBRUSH br = CreateSolidBrush(b->color);
        HBRUSH old = (HBRUSH)SelectObject(dc, br);
        int px = (int)floor(b->x), py = (int)floor(b->y);
        int r = (int)(b->radius + 0.5f);
        if (r < 2) r = 2;
        Ellipse(dc, px - r, py - r, px + r, py + r);
        SelectObject(dc, old);
        DeleteObject(br);
    }

    ModifyWorldTransform(dc, NULL, MWT_IDENTITY);

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(210, 220, 235));
    wchar_t line[256];
    _snwprintf(line, 256, L"MVP 50%% | тела: %d | шаг: %llu | физика %.2f мс | FPS %.1f%s",
               g_stats.activeBodies, g_stats.stepCounter, g_stats.stepTimeMs, g_fps,
               g_cfg.paused ? L" | ПАУЗА" : L"");
    TextOutW(dc, 12, 12, line, (int)wcslen(line));
    TextOutW(dc, 12, 32, L"Пробел — пауза, R — сброс, ПКМ — камера, колёсико — зум", 52);

    HDC wnd = GetDC(hwnd);
    BitBlt(wnd, 0, 0, cw, ch, dc, 0, 0, SRCCOPY);
    ReleaseDC(hwnd, wnd);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        Paint(hwnd);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_KEYDOWN:
        if (wp == VK_SPACE)
            g_cfg.paused = !g_cfg.paused;
        else if (wp == 'R') {
            g_phys.ResetScene();
            g_count = g_phys.SnapshotRead(g_bodies, g_meta, MAX_BODIES, &g_stats);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;

    case WM_RBUTTONDOWN:
        g_pan = TRUE;
        g_panX = GET_X_LPARAM(lp);
        g_panY = GET_Y_LPARAM(lp);
        SetCapture(hwnd);
        return 0;

    case WM_RBUTTONUP:
        g_pan = FALSE;
        ReleaseCapture();
        return 0;

    case WM_MOUSEMOVE:
        if (g_pan) {
            int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
            RECT rc;
            GetClientRect(hwnd, &rc);
            int cw = rc.right - rc.left, ch = rc.bottom - rc.top;
            g_cfg.camX -= (double)(x - g_panX) / g_cfg.camZoom;
            g_cfg.camY -= (double)(y - g_panY) / g_cfg.camZoom;
            g_panX = x; g_panY = y;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;

    case WM_MOUSEWHEEL: {
        short delta = GET_WHEEL_DELTA_WPARAM(wp);
        double factor = (delta > 0) ? 1.12 : (1.0 / 1.12);
        g_cfg.camZoom *= factor;
        if (g_cfg.camZoom < 0.15) g_cfg.camZoom = 0.15;
        if (g_cfg.camZoom > 8.0) g_cfg.camZoom = 8.0;
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE hi, HINSTANCE hp, LPSTR cmd, int show)
{
    (void)hp; (void)cmd;

    g_cfg.gravityG = 120.0;
    g_cfg.softening = 4.5;
    g_cfg.dt = 0.016;
    g_cfg.camZoom = 0.85;
    g_cfg.paused = FALSE;

    wchar_t err[256];
    if (!LoadDll(err, 256)) {
        MessageBoxW(NULL, err, L"MVP GravitySim", MB_ICONERROR);
        return 1;
    }

    SIZE_T bytes = sizeof(Body) * MAX_BODIES + sizeof(BodyMeta) * MAX_BODIES;
    BYTE *mem = (BYTE *)VirtualAlloc(NULL, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!mem) {
        MessageBoxW(NULL, L"VirtualAlloc", L"MVP", MB_ICONERROR);
        return 1;
    }
    g_bodies = (Body *)mem;
    g_meta = (BodyMeta *)(mem + sizeof(Body) * MAX_BODIES);

    if (!g_phys.Init(&g_cfg)) {
        MessageBoxW(NULL, L"PhysInit", L"MVP", MB_ICONERROR);
        return 1;
    }
    g_phys.ResetScene();
    g_count = g_phys.SnapshotRead(g_bodies, g_meta, MAX_BODIES, &g_stats);

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"GravitySimMvp";
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(0, L"GravitySimMvp", L"Гравитация — MVP (50% срока)",
                                WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                960, 640, NULL, NULL, hi, NULL);
    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

    g_lastT = Seconds();
    BOOL run = TRUE;
    MSG msg;

    while (run) {
        /* Неблокирующая очередь сообщений: пока нет WM_QUIT, крутим симуляцию */
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { run = FALSE; break; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!run) break;

        if (!g_cfg.paused)
            g_phys.Step(g_cfg.dt);
        g_count = g_phys.SnapshotRead(g_bodies, g_meta, MAX_BODIES, &g_stats);

        double now = Seconds();
        double dt = now - g_lastT;
        g_lastT = now;
        if (dt > 0.0001)
            g_fps = 0.9 * g_fps + 0.1 * (1.0 / dt);

        InvalidateRect(hwnd, NULL, FALSE);
        UpdateWindow(hwnd);

        double elapsed = Seconds() - now;
        if (elapsed < FRAME_SEC) {
            DWORD ms = (DWORD)((FRAME_SEC - elapsed) * 1000.0);
            if (ms > 1) Sleep(ms - 1);
        }
    }

    g_phys.Shutdown();
    VirtualFree(mem, 0, MEM_RELEASE);
    if (g_memDC) {
        SelectObject(g_memDC, g_oldBmp);
        DeleteObject(g_memBmp);
        DeleteDC(g_memDC);
    }
    FreeLibrary(g_phys.hModule);
    return 0;
}
