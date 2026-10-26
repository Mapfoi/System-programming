/*
 * main_win.c — клиент gravity_sim.exe (Win32 + GDI).
 *
 * PhysStep вызывается только из главного цикла while(run), не из WndProc.
 * WndProc: Space/R/мышь; Paint рисует g_bodies[] с камерой через SetWorldTransform (XFORM).
 *
 * Цепочка кадра: PhysStep → SnapshotRead → InvalidateRect → WM_PAINT → Ellipse для каждого тела.
 */
#include "common.h" // PhysicsApi, SimConfig, Body
#include <windowsx.h> // GET_X_LPARAM, GET_WHEEL_DELTA_WPARAM
#include <math.h>     // floor
#include <stdio.h>    // _snwprintf
#include <string.h>

#define TARGET_FPS 60.0 // целевой FPS главного цикла
#define FRAME_SEC  (1.0 / TARGET_FPS) // ~16.67 ms на кадр

static PhysicsApi g_phys; // LoadLibrary + указатели на экспорт DLL
static SimConfig  g_cfg;  // dt, zoom, камера, пауза
static SimStats   g_stats; // заполняется SnapshotRead для HUD

static Body     *g_bodies = NULL; // локальная копия тел для отрисовки
static BodyMeta *g_meta   = NULL; // alive-флаги копии
static int       g_count  = 0;    // сколько тел в снимке

static HDC     g_memDC = NULL; // off-screen DC для двойной буферизации
static HBITMAP g_memBmp = NULL, g_oldBmp = NULL; // bitmap в memDC
static int     g_bufW = 0, g_bufH = 0; // размер буфера = клиентское окно

static BOOL  g_pan = FALSE; // ПКМ — перетаскивание камеры
static int   g_panX = 0, g_panY = 0; // последняя позиция мыши при панораме
static double g_fps = 60.0, g_lastT = 0.0; // сглаженный FPS и время прошлого кадра

static double Seconds(void)
{
    static LARGE_INTEGER f = {0}; // частота QPC
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart; // секунды
}

static void ScreenToWorld(int sx, int sy, int cw, int ch, double *wx, double *wy)
{
    *wx = ((double)sx - cw * 0.5) / g_cfg.camZoom + g_cfg.camX; // центр экрана → мир + camX
    *wy = ((double)sy - ch * 0.5) / g_cfg.camZoom + g_cfg.camY; // то же по Y
}

static BOOL LoadDll(wchar_t *err, size_t cap)
{
    g_phys.hModule = LoadLibraryW(L"gravity_physics.dll"); // рядом с exe или в PATH
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
        _snwprintf(err, cap, L"Экспорт DLL неполный"); // несовместимая сборка DLL
        FreeLibrary(g_phys.hModule);
        return FALSE;
    }
    return TRUE;
}

static void EnsureBuffer(HWND hwnd, int w, int h)
{
    if (w < 1) w = 1; // защита от нулевой ширины
    if (h < 1) h = 1;
    if (g_memDC && w == g_bufW && h == g_bufH) return; // размер не менялся
    HDC hdc = GetDC(hwnd); // DC окна для CreateCompatible*
    if (g_memDC) { // пересоздать буфер при ресайзе
        SelectObject(g_memDC, g_oldBmp);
        DeleteObject(g_memBmp);
        DeleteDC(g_memDC);
    }
    g_memDC = CreateCompatibleDC(hdc); // память DC
    g_memBmp = CreateCompatibleBitmap(hdc, w, h); // bitmap того же формата
    g_oldBmp = (HBITMAP)SelectObject(g_memDC, g_memBmp); // привязать к memDC
    g_bufW = w; g_bufH = h;
    ReleaseDC(hwnd, hdc);
}

static void Paint(HWND hwnd)
{
    RECT rc;
    GetClientRect(hwnd, &rc); // клиентская область
    int cw = rc.right - rc.left, ch = rc.bottom - rc.top;
    EnsureBuffer(hwnd, cw, ch); // подготовить off-screen

    HDC dc = g_memDC; // рисуем в буфер
    HBRUSH bg = CreateSolidBrush(RGB(12, 14, 22)); // тёмный фон
    FillRect(dc, &rc, bg);
    DeleteObject(bg);

    SetGraphicsMode(dc, GM_ADVANCED); // нужен для SetWorldTransform
    XFORM xf = { (FLOAT)g_cfg.camZoom, 0, 0, (FLOAT)g_cfg.camZoom, // масштаб мира
                 (FLOAT)(cw * 0.5 - g_cfg.camX * g_cfg.camZoom), // сдвиг: центр экрана = cam
                 (FLOAT)(ch * 0.5 - g_cfg.camY * g_cfg.camZoom) };
    SetWorldTransform(dc, &xf); // мировые координаты → пиксели

    for (int i = 0; i < g_count; ++i) { // все тела из последнего SnapshotRead
        if (!g_meta[i].alive) continue;
        const Body *b = &g_bodies[i];
        HBRUSH br = CreateSolidBrush(b->color); // цвет тела
        HBRUSH old = (HBRUSH)SelectObject(dc, br);
        int px = (int)floor(b->x), py = (int)floor(b->y); // центр круга в мире
        int r = (int)(b->radius + 0.5f); // радиус в мировых единицах (масштабирует XFORM)
        if (r < 2) r = 2; // минимум 2 пикселя после зума
        Ellipse(dc, px - r, py - r, px + r, py + r); // GDI-диск
        SelectObject(dc, old);
        DeleteObject(br);
    }

    ModifyWorldTransform(dc, NULL, MWT_IDENTITY); // сброс для текста HUD в экранных координатах

    SetBkMode(dc, TRANSPARENT); // без заливки фона текста
    SetTextColor(dc, RGB(210, 220, 235));
    wchar_t line[256];
    _snwprintf(line, 256, L"тела: %d | шаг: %llu | физика %.2f мс | FPS %.1f%s",
               g_stats.activeBodies, g_stats.stepCounter, g_stats.stepTimeMs, g_fps,
               g_cfg.paused ? L" | ПАУЗА" : L"");
    TextOutW(dc, 12, 12, line, (int)wcslen(line)); // строка статистики
    TextOutW(dc, 12, 32, L"Пробел — пауза, R — сброс, ПКМ — камера, колёсико — зум", 52);

    HDC wnd = GetDC(hwnd);
    BitBlt(wnd, 0, 0, cw, ch, dc, 0, 0, SRCCOPY); // буфер → экран
    ReleaseDC(hwnd, wnd);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_DESTROY:
        PostQuitMessage(0); // завершить цикл WinMain
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps); // валидировать область
        Paint(hwnd); // полная отрисовка кадра
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_KEYDOWN:
        if (wp == VK_SPACE)
            g_cfg.paused = !g_cfg.paused; // пауза без вызова DLL
        else if (wp == 'R') {
            g_phys.ResetScene(); // пересобрать сцену в DLL
            g_count = g_phys.SnapshotRead(g_bodies, g_meta, MAX_BODIES, &g_stats); // обновить копию
            InvalidateRect(hwnd, NULL, FALSE); // запросить WM_PAINT
        }
        return 0;

    case WM_RBUTTONDOWN:
        g_pan = TRUE;
        g_panX = GET_X_LPARAM(lp); // якорь панорамы
        g_panY = GET_Y_LPARAM(lp);
        SetCapture(hwnd); // мышь вне окна тоже считается
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
            int cw = rc.right - rc.left, ch = rc.bottom - rc.top; // (cw,ch не используются в формуле, только delta)
            g_cfg.camX -= (double)(x - g_panX) / g_cfg.camZoom; // сдвиг камеры в мире
            g_cfg.camY -= (double)(y - g_panY) / g_cfg.camZoom;
            g_panX = x; g_panY = y; // новый якорь
            InvalidateRect(hwnd, NULL, FALSE); // перерисовать при движении
        }
        return 0;

    case WM_MOUSEWHEEL: {
        short delta = GET_WHEEL_DELTA_WPARAM(wp); // ±120 за щелчок
        double factor = (delta > 0) ? 1.12 : (1.0 / 1.12); // зум in/out
        g_cfg.camZoom *= factor;
        if (g_cfg.camZoom < 0.15) g_cfg.camZoom = 0.15; // нижний предел
        if (g_cfg.camZoom > 8.0) g_cfg.camZoom = 8.0; // верхний предел зума
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp); // стандартная обработка
}

int WINAPI WinMain(HINSTANCE hi, HINSTANCE hp, LPSTR cmd, int show)
{
    (void)hp; (void)cmd;

    g_cfg.gravityG = 120.0; // согласовано с дефолтом PhysInit
    g_cfg.softening = 4.5;
    g_cfg.dt = 0.016; // ~60 шагов/с модельного времени за кадр при 60 FPS
    g_cfg.camZoom = 0.85; // чуть отдалить облако
    g_cfg.paused = FALSE;

    wchar_t err[256];
    if (!LoadDll(err, 256)) {
        MessageBoxW(NULL, err, L"GravitySim", MB_ICONERROR);
        return 1;
    }

    SIZE_T bytes = sizeof(Body) * MAX_BODIES + sizeof(BodyMeta) * MAX_BODIES; // буфер снимка
    BYTE *mem = (BYTE *)VirtualAlloc(NULL, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!mem) {
        MessageBoxW(NULL, L"VirtualAlloc", L"GravitySim", MB_ICONERROR);
        return 1;
    }
    g_bodies = (Body *)mem; // первый сегмент
    g_meta = (BodyMeta *)(mem + sizeof(Body) * MAX_BODIES); // второй сегмент

    if (!g_phys.Init(&g_cfg)) {
        MessageBoxW(NULL, L"PhysInit", L"GravitySim", MB_ICONERROR);
        return 1;
    }
    g_phys.ResetScene(); // начальная сцена в DLL
    g_count = g_phys.SnapshotRead(g_bodies, g_meta, MAX_BODIES, &g_stats); // первый кадр

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = WndProc; // обработчик ввода и paint
    wc.hInstance = hi;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"GravitySimWndClass";
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(0, L"GravitySimWndClass",
                                L"Моделирование гравитационного взаимодействия тел",
                                WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                960, 640, NULL, NULL, hi, NULL);
    ShowWindow(hwnd, show);
    UpdateWindow(hwnd); // первый WM_PAINT

    g_lastT = Seconds(); // якорь для FPS
    BOOL run = TRUE;
    MSG msg;

    while (run) { // главный цикл приложения
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) { // не блокируемся на GetMessage
            if (msg.message == WM_QUIT) { run = FALSE; break; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg); // WndProc: клавиши, мышь, paint по запросу
        }
        if (!run) break;

        if (!g_cfg.paused)
            g_phys.Step(g_cfg.dt); // один Verlet-подшаг в DLL
        g_count = g_phys.SnapshotRead(g_bodies, g_meta, MAX_BODIES, &g_stats); // копия для Paint

        double now = Seconds();
        double dt = now - g_lastT; // длительность кадра
        g_lastT = now;
        if (dt > 0.0001)
            g_fps = 0.9 * g_fps + 0.1 * (1.0 / dt); // EMA FPS

        InvalidateRect(hwnd, NULL, FALSE); // пометить окно «грязным»
        UpdateWindow(hwnd); // синхронный WM_PAINT → Paint

        double elapsed = Seconds() - now; // время после paint
        if (elapsed < FRAME_SEC) { // ограничение ~60 FPS
            DWORD ms = (DWORD)((FRAME_SEC - elapsed) * 1000.0);
            if (ms > 1) Sleep(ms - 1); // грубая задержка
        }
    }

    g_phys.Shutdown(); // VirtualFree в DLL, delete CS
    VirtualFree(mem, 0, MEM_RELEASE); // буфер снимка exe
    if (g_memDC) { // GDI cleanup
        SelectObject(g_memDC, g_oldBmp);
        DeleteObject(g_memBmp);
        DeleteDC(g_memDC);
    }
    FreeLibrary(g_phys.hModule);
    return 0;
}
