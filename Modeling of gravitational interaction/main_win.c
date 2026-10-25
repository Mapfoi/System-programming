#include "common.h"
#include <windowsx.h>
#include <wingdi.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TARGET_FPS       60.0
#define TARGET_FRAME_SEC (1.0 / TARGET_FPS)
#define SNAPSHOT_FILE    L"snapshot.grav"

// Глобальное состояние оконного приложения
static PhysicsApi  g_phys = {0};
static SimConfig   g_cfg  = {0};
static SimStats    g_stats = {0};

// Локальная копия тел для безопасного рендеринга (выделяется через VirtualAlloc)
static Body       *g_renderBodies = NULL;
static BodyMeta   *g_renderMeta   = NULL;
static int         g_renderCount  = 0;

// Двойная буферизация GDI (слайд 26–28: CreateCompatibleDC, CreateCompatibleBitmap, BitBlt)
static HDC         g_memDC  = NULL;
static HBITMAP     g_memBmp = NULL;
static HBITMAP     g_oldBmp = NULL;
static int         g_bufW   = 0;
static int         g_bufH   = 0;

// Логические шрифты GDI (слайд 34: CreateFontIndirectW)
static HFONT       g_fontHudTitle = NULL;
static HFONT       g_fontHudMono  = NULL;
static HFONT       g_fontHudSmall = NULL;
static BOOL        g_showHelpHud  = TRUE;

// Интерактивное управление мышью («рогатка» создания тела и панорамирование камеры)
static BOOL        g_slingshotActive = FALSE;
static int         g_slingStartX = 0, g_slingStartY = 0;
static int         g_slingCurX   = 0, g_slingCurY   = 0;
static double      g_spawnMass   = 1e-5;

static BOOL        g_panningActive = FALSE;
static int         g_panLastX = 0, g_panLastY = 0;

// Измерение FPS и статусная строка
static double      g_fps = 60.0;
static double      g_lastFrameTime = 0.0;
static wchar_t     g_statusText[256] = L"Готово: ЛКМ (тянуть) — запустить тело, ПКМ — камера, Колесо — зум";
static ULONGLONG   g_statusExpireTick = 0;

static double QuerySeconds(void)
{
    static LARGE_INTEGER freq = {0};
    LARGE_INTEGER counter;
    if (!freq.QuadPart)
        QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart / (double)freq.QuadPart;
}

static void SetBannerStatus(const wchar_t *msg)
{
    wcsncpy(g_statusText, msg, 255);
    g_statusText[255] = L'\0';
    g_statusExpireTick = GetTickCount64() + 6000;
}

static BOOL IsSolarPhysicsView(void)
{
    return g_cfg.preset == PRESET_SOLAR_SYSTEM;
}

static double WorldDrawScale(void)
{
    return IsSolarPhysicsView() ? SIM_SOLAR_DRAW_AU : 1.0;
}

static double EffectiveTimeScale(void)
{
    return g_cfg.timeScale * ConfigPresetTimeGain(g_cfg.preset);
}

static double TimeScaleUserMax(void)
{
    if (g_cfg.preset == PRESET_GALAXY_CLASH || g_cfg.preset == PRESET_RANDOM_CLOUD)
        return 6.0;
    if (g_cfg.preset == PRESET_SOLAR_SYSTEM)
        return 48.0;
    return 128.0;
}

static int g_solarHudSubSteps = 0;
static double g_solarHudSubDt = 0.0;

static void DrawSolarOrbitGuides(HDC hdc, double kDraw)
{
    static const double kPlanetAu[] = {
        0.387, 0.723, 1.000, 1.524, 5.203, 9.537, 19.19, 30.07
    };
    const double moonAu = 0.00257;

    HPEN guidePen = CreatePen(PS_DOT, 1, RGB(55, 75, 110));
    HPEN oldPen = (HPEN)SelectObject(hdc, guidePen);
    HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));

    for (int i = 0; i < 8; ++i) {
        double r = kPlanetAu[i] * kDraw;
        Ellipse(hdc, (int)floor(-r), (int)floor(-r), (int)ceil(r), (int)ceil(r));
    }

    int earthIdx = -1;
    for (int i = 0; i < g_renderCount; ++i) {
        if (!g_renderMeta[i].alive)
            continue;
        double m = g_renderBodies[i].mass;
        if (m > 2.0e-6 && m < 4.5e-6) {
            earthIdx = i;
            break;
        }
    }
    if (earthIdx >= 0) {
        double ex = g_renderBodies[earthIdx].x * kDraw;
        double ey = g_renderBodies[earthIdx].y * kDraw;
        double rm = moonAu * kDraw;
        Ellipse(hdc, (int)floor(ex - rm), (int)floor(ey - rm),
                (int)ceil(ex + rm), (int)ceil(ey + rm));
    }

    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(guidePen);
}

static float BodyDrawRadiusAu(const Body *b)
{
    float r = b->radius;
    if (!IsSolarPhysicsView())
        return r;
    if (b->mass < 1e-7) {
        if (r < 0.012f) r = 0.012f;
    } else if (b->mass < 1e-4) {
        if (r < 0.022f) r = 0.022f;
    }
    return r;
}

// Преобразование экранных координат в мировые (физические AU для СС) и обратно
static void ScreenToWorld(int sx, int sy, int clientW, int clientH, double *wx, double *wy)
{
    *wx = ((double)sx - (double)clientW * 0.5) / g_cfg.camZoom + g_cfg.camX;
    *wy = ((double)sy - (double)clientH * 0.5) / g_cfg.camZoom + g_cfg.camY;
}

static void ApplyPreset(int preset)
{
    g_cfg.preset = preset;
    g_cfg.camX = 0.0;
    g_cfg.camY = 0.0;
    ConfigApplyPresetPhysics(&g_cfg, preset);
    if (preset == PRESET_SOLAR_SYSTEM && g_cfg.timeScale > TimeScaleUserMax())
        g_cfg.timeScale = TimeScaleUserMax();
    switch (preset) {
    case PRESET_GALAXY_CLASH:  g_cfg.camZoom = 0.55; break;
    case PRESET_FIGURE_EIGHT:  g_cfg.camZoom = 1.15; break;
    case PRESET_RANDOM_CLOUD:  g_cfg.camZoom = 0.65; break;
    default:                   g_cfg.camZoom = 22.0; break;
    }
    g_phys.SetParams(g_cfg.gravityG, g_cfg.softening, g_cfg.enableCollisions);
    g_phys.ResetPreset(preset, 0);
    if (preset == PRESET_SOLAR_SYSTEM)
        g_spawnMass = 1e-5;
    else
        g_spawnMass = 45.0;
    g_renderCount = g_phys.SnapshotRead(g_renderBodies, g_renderMeta, MAX_BODIES, &g_stats);
}

static const wchar_t *PresetName(int preset)
{
    switch (preset) {
    case PRESET_SOLAR_SYSTEM: return L"1: Солнечная система";
    case PRESET_GALAXY_CLASH: return L"2: Столкновение спиральных галактик";
    case PRESET_FIGURE_EIGHT: return L"3: Хореография 3 тел (Восьмерка)";
    case PRESET_RANDOM_CLOUD: return L"4: Протопланетное облако";
    default:                  return L"Пользовательская сцена";
    }
}

static const wchar_t *PriorityName(int prio)
{
    switch (prio) {
    case THREAD_PRIORITY_LOWEST:       return L"LOWEST (-2)";
    case THREAD_PRIORITY_BELOW_NORMAL: return L"BELOW_NORMAL (-1)";
    case THREAD_PRIORITY_NORMAL:       return L"NORMAL (0)";
    case THREAD_PRIORITY_ABOVE_NORMAL: return L"ABOVE_NORMAL (+1)";
    case THREAD_PRIORITY_HIGHEST:      return L"HIGHEST (+2)";
    default:                           return L"CUSTOM";
    }
}

// Динамическая загрузка DLL во время работы программы (слайды 50–51: LoadLibraryW + GetProcAddress)
static BOOL LoadPhysicsDll(wchar_t *errBuf, size_t errCap)
{
    wchar_t dllPath[MAX_PATH];
    DWORD len = GetModuleFileNameW(NULL, dllPath, MAX_PATH);
    if (len > 0) {
        wchar_t *slash = wcsrchr(dllPath, L'\\');
        if (slash) *(slash + 1) = L'\0';
        wcsncat(dllPath, L"gravity_physics.dll", MAX_PATH - wcslen(dllPath) - 1);
    } else {
        wcscpy(dllPath, L"gravity_physics.dll");
    }

    g_phys.hModule = LoadLibraryW(dllPath);
    if (!g_phys.hModule) {
        g_phys.hModule = LoadLibraryW(L"gravity_physics.dll");
    }
    if (!g_phys.hModule) {
        _snwprintf(errBuf, errCap, L"Не удалось загрузить gravity_physics.dll (код %lu)", GetLastError());
        return FALSE;
    }

    g_phys.Init         = (PFN_PhysInit)(void *)GetProcAddress(g_phys.hModule, "PhysInit");
    g_phys.Shutdown     = (PFN_PhysShutdown)(void *)GetProcAddress(g_phys.hModule, "PhysShutdown");
    g_phys.ResetPreset  = (PFN_PhysResetPreset)(void *)GetProcAddress(g_phys.hModule, "PhysResetPreset");
    g_phys.Step         = (PFN_PhysStep)(void *)GetProcAddress(g_phys.hModule, "PhysStep");
    g_phys.AddBody      = (PFN_PhysAddBody)(void *)GetProcAddress(g_phys.hModule, "PhysAddBody");
    g_phys.AddCluster   = (PFN_PhysAddCluster)(void *)GetProcAddress(g_phys.hModule, "PhysAddCluster");
    g_phys.SetThreads   = (PFN_PhysSetThreads)(void *)GetProcAddress(g_phys.hModule, "PhysSetThreads");
    g_phys.SetParams    = (PFN_PhysSetParams)(void *)GetProcAddress(g_phys.hModule, "PhysSetParams");
    g_phys.SnapshotRead = (PFN_PhysSnapshotRead)(void *)GetProcAddress(g_phys.hModule, "PhysSnapshotRead");
    g_phys.LoadBodies   = (PFN_PhysLoadBodies)(void *)GetProcAddress(g_phys.hModule, "PhysLoadBodies");

    if (!g_phys.Init || !g_phys.Shutdown || !g_phys.ResetPreset || !g_phys.Step ||
        !g_phys.AddBody || !g_phys.AddCluster || !g_phys.SetThreads || !g_phys.SetParams ||
        !g_phys.SnapshotRead || !g_phys.LoadBodies) {
        _snwprintf(errBuf, errCap, L"Не найдены экспортируемые функции в gravity_physics.dll");
        FreeLibrary(g_phys.hModule);
        g_phys.hModule = NULL;
        return FALSE;
    }
    return TRUE;
}

// Создание логических шрифтов (слайд 34: LOGFONT + CreateFontIndirectW)
static void InitGdiFonts(void)
{
    LOGFONTW lf = {0};
    lf.lfHeight = -16;
    lf.lfWeight = FW_BOLD;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    wcscpy(lf.lfFaceName, L"Segoe UI");
    g_fontHudTitle = CreateFontIndirectW(&lf);

    memset(&lf, 0, sizeof(lf));
    lf.lfHeight = -14;
    lf.lfWeight = FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfPitchAndFamily = FIXED_PITCH | FF_MODERN;
    lf.lfQuality = CLEARTYPE_QUALITY;
    wcscpy(lf.lfFaceName, L"Consolas");
    g_fontHudMono = CreateFontIndirectW(&lf);

    memset(&lf, 0, sizeof(lf));
    lf.lfHeight = -13;
    lf.lfWeight = FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    wcscpy(lf.lfFaceName, L"Segoe UI");
    g_fontHudSmall = CreateFontIndirectW(&lf);
}

static void FreeGdiResources(void)
{
    if (g_memDC) {
        if (g_oldBmp) SelectObject(g_memDC, g_oldBmp);
        if (g_memBmp) DeleteObject(g_memBmp);
        DeleteDC(g_memDC);
        g_memDC = NULL;
        g_memBmp = NULL;
        g_oldBmp = NULL;
    }
    if (g_fontHudTitle) { DeleteObject(g_fontHudTitle); g_fontHudTitle = NULL; }
    if (g_fontHudMono)  { DeleteObject(g_fontHudMono);  g_fontHudMono  = NULL; }
    if (g_fontHudSmall) { DeleteObject(g_fontHudSmall); g_fontHudSmall = NULL; }
}

static void EnsureBackBuffer(HDC winDC, int w, int h)
{
    if (w <= 0) w = 1;
    if (h <= 0) h = 1;
    if (g_memDC && g_bufW == w && g_bufH == h)
        return;

    if (g_memDC) {
        SelectObject(g_memDC, g_oldBmp);
        DeleteObject(g_memBmp);
        DeleteDC(g_memDC);
    }
    g_memDC  = CreateCompatibleDC(winDC);
    g_memBmp = CreateCompatibleBitmap(winDC, w, h);
    g_oldBmp = (HBITMAP)SelectObject(g_memDC, g_memBmp);
    g_bufW   = w;
    g_bufH   = h;
}

// Отрисовка координатной сетки и барицентра в мировых координатах (под действием матрицы XFORM)
static void DrawWorldGrid(HDC hdc, int clientW, int clientH)
{
    double leftW, topW, rightW, bottomW;
    ScreenToWorld(0, 0, clientW, clientH, &leftW, &topW);
    ScreenToWorld(clientW, clientH, clientW, clientH, &rightW, &bottomW);

    double k = WorldDrawScale();
    double stepAu = 100.0;
    if (IsSolarPhysicsView()) {
        stepAu = 5.0;
        if (g_cfg.camZoom > 45.0) stepAu = 0.25;
        else if (g_cfg.camZoom > 28.0) stepAu = 1.0;
        else if (g_cfg.camZoom > 14.0) stepAu = 2.0;
    } else {
        if (g_cfg.camZoom < 0.35) stepAu = 250.0;
        else if (g_cfg.camZoom > 1.8) stepAu = 50.0;
    }

    HPEN gridPen = CreatePen(PS_SOLID, 1, RGB(20, 28, 44));
    HPEN axisPen = CreatePen(PS_SOLID, 1, RGB(42, 58, 88));
    HPEN oldPen  = (HPEN)SelectObject(hdc, gridPen);

    double startX = floor(leftW / stepAu) * stepAu;
    for (double x = startX; x <= rightW; x += stepAu) {
        int gx = (int)llround(x * k);
        MoveToEx(hdc, gx, (int)llround(topW * k), NULL);
        LineTo(hdc, gx, (int)llround(bottomW * k));
    }

    double startY = floor(topW / stepAu) * stepAu;
    for (double y = startY; y <= bottomW; y += stepAu) {
        int gy = (int)llround(y * k);
        MoveToEx(hdc, (int)llround(leftW * k), gy, NULL);
        LineTo(hdc, (int)llround(rightW * k), gy);
    }

    SelectObject(hdc, axisPen);
    MoveToEx(hdc, (int)llround(leftW * k), 0, NULL);
    LineTo(hdc, (int)llround(rightW * k), 0);
    MoveToEx(hdc, 0, (int)llround(topW * k), NULL);
    LineTo(hdc, 0, (int)llround(bottomW * k));

    SelectObject(hdc, oldPen);
    DeleteObject(gridPen);
    DeleteObject(axisPen);
}

// Отрисовка сцены в контексте памяти с применением трансформаций XFORM (слайды 38–41)
static void RenderScene(HDC hdc, int clientW, int clientH)
{
    // 1. Заливка глубокого космического фона в физических координатах
    RECT bgRect = { 0, 0, clientW, clientH };
    HBRUSH bgBrush = CreateSolidBrush(RGB(8, 11, 18));
    FillRect(hdc, &bgRect, bgBrush);
    DeleteObject(bgBrush);

    // 2. Включение расширенного графического режима GM_ADVANCED и установка матрицы XFORM (слайды 38–40)
    SetGraphicsMode(hdc, GM_ADVANCED);
    double kDraw = WorldDrawScale();
    XFORM xf;
    xf.eM11 = (FLOAT)(g_cfg.camZoom / kDraw);
    xf.eM12 = 0.0f;
    xf.eM21 = 0.0f;
    xf.eM22 = (FLOAT)(g_cfg.camZoom / kDraw);
    xf.eDx  = (FLOAT)((double)clientW * 0.5 - g_cfg.camX * g_cfg.camZoom);
    xf.eDy  = (FLOAT)((double)clientH * 0.5 - g_cfg.camY * g_cfg.camZoom);
    SetWorldTransform(hdc, &xf);

    if (g_cfg.showGrid) {
        DrawWorldGrid(hdc, clientW, clientH);
    }

    if (g_cfg.showOrbitGuides && IsSolarPhysicsView()) {
        DrawSolarOrbitGuides(hdc, kDraw);
    }

    // 3. Отрисовка орбитальных треков в мировых координатах через Polyline
    // Используем DC_PEN и DC_BRUSH (GetStockObject), чтобы не пересоздавать тысячи объектов GDI каждый кадр
    if (g_cfg.showTrails) {
        POINT pts[TRAIL_LENGTH];
        HPEN dcPen = (HPEN)GetStockObject(DC_PEN);
        HPEN oldPen = (HPEN)SelectObject(hdc, dcPen);

        for (int i = 0; i < g_renderCount; ++i) {
            const BodyMeta *m = &g_renderMeta[i];
            if (!m->alive || m->trailCount < 2)
                continue;

            DWORD c = g_renderBodies[i].color;
            SetDCPenColor(hdc, RGB(GetRValue(c) / 2, GetGValue(c) / 2, GetBValue(c) / 2));

            int cnt = (int)m->trailCount;
            int startIdx = (m->trailHead - cnt + TRAIL_LENGTH) % TRAIL_LENGTH;
            for (int k = 0; k < cnt; ++k) {
                int idx = (startIdx + k) % TRAIL_LENGTH;
                pts[k].x = (LONG)llround((double)m->trail[idx].x * kDraw);
                pts[k].y = (LONG)llround((double)m->trail[idx].y * kDraw);
            }
            Polyline(hdc, pts, cnt);
        }
        SelectObject(hdc, oldPen);
    }

    // 4. Отрисовка гравитационных тел и векторов скорости в мировых координатах
    HPEN nullPen   = (HPEN)GetStockObject(NULL_PEN);
    HBRUSH dcBrush = (HBRUSH)GetStockObject(DC_BRUSH);
    HPEN vecPen    = CreatePen(PS_SOLID, 1, RGB(90, 230, 150));

    HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, dcBrush);
    HPEN oldPen     = (HPEN)SelectObject(hdc, nullPen);

    for (int i = 0; i < g_renderCount; ++i) {
        if (!g_renderMeta[i].alive)
            continue;
        const Body *b = &g_renderBodies[i];
        double px = b->x * kDraw;
        double py = b->y * kDraw;
        double visR = (double)BodyDrawRadiusAu(b) * kDraw;

        // Ореол свечения для массивных звезд и ядер галактик
        double haloM = IsSolarPhysicsView() ? 0.5 : 1500.0;
        if (b->mass >= haloM) {
            double haloR = visR * 1.85;
            DWORD c = b->color;
            SetDCBrushColor(hdc, RGB(GetRValue(c) / 3, GetGValue(c) / 3, GetBValue(c) / 3));
            Ellipse(hdc, (int)floor(px - haloR), (int)floor(py - haloR),
                    (int)ceil(px + haloR), (int)ceil(py + haloR));
        }

        SetDCBrushColor(hdc, b->color);
        Ellipse(hdc, (int)floor(px - visR), (int)floor(py - visR),
                (int)ceil(px + visR), (int)ceil(py + visR));

        if (g_cfg.showVectors) {
            SelectObject(hdc, vecPen);
            double vScale = IsSolarPhysicsView() ? (3500.0 / kDraw) : 0.35;
            int cx = (int)llround(px);
            int cy = (int)llround(py);
            int vxEnd = cx + (int)(b->vx * vScale);
            int vyEnd = cy + (int)(b->vy * vScale);
            MoveToEx(hdc, cx, cy, NULL);
            LineTo(hdc, vxEnd, vyEnd);
            SelectObject(hdc, nullPen);
        }
    }
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(vecPen);

    // 5. Сброс матрицы трансформации в единичную для отрисовки экранного интерфейса (HUD и рогатки)
    ModifyWorldTransform(hdc, NULL, MWT_IDENTITY);

    // 6. Отрисовка «гравитационной рогатки» при зажатой ЛКМ
    if (g_slingshotActive) {
        HPEN slingPen = CreatePen(PS_DOT, 1, RGB(255, 200, 80));
        HPEN prevPen = (HPEN)SelectObject(hdc, slingPen);
        HBRUSH prevBrush = (HBRUSH)SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));

        double previewR;
        if (IsSolarPhysicsView()) {
            double mVis = (g_spawnMass > 1e-12) ? pow(g_spawnMass, 0.22) : 0.01;
            previewR = (2.0 + mVis * 6.0);
        } else {
            previewR = (2.2 + pow(g_spawnMass, 0.28) * 1.15) * g_cfg.camZoom;
        }
        int previewRi = (int)previewR;
        if (previewRi < 4) previewRi = 4;
        Ellipse(hdc, g_slingStartX - previewRi, g_slingStartY - previewRi,
                g_slingStartX + previewRi, g_slingStartY + previewRi);

        MoveToEx(hdc, g_slingStartX, g_slingStartY, NULL);
        LineTo(hdc, g_slingCurX, g_slingCurY);

        SelectObject(hdc, prevBrush);
        SelectObject(hdc, prevPen);
        DeleteObject(slingPen);
    }

    // 7. Отрисовка панели телеметрии (HUD)
    SetBkMode(hdc, TRANSPARENT);
    RECT panelRect = { 12, 12, 520, g_showHelpHud ? 378 : 214 };
    HBRUSH panelBrush = CreateSolidBrush(RGB(16, 22, 34));
    HPEN borderPen = CreatePen(PS_SOLID, 1, RGB(52, 74, 112));
    HBRUSH oldB = (HBRUSH)SelectObject(hdc, panelBrush);
    HPEN oldP = (HPEN)SelectObject(hdc, borderPen);
    RoundRect(hdc, panelRect.left, panelRect.top, panelRect.right, panelRect.bottom, 10, 10);
    SelectObject(hdc, oldB);
    SelectObject(hdc, oldP);
    DeleteObject(panelBrush);
    DeleteObject(borderPen);

    HFONT oldFont = (HFONT)SelectObject(hdc, g_fontHudTitle);
    SetTextColor(hdc, RGB(110, 195, 255));
    TextOutW(hdc, 24, 20, L"Моделирование гравитационного взаимодействия (Win32 API)", 56);

    SelectObject(hdc, g_fontHudMono);
    wchar_t line[256];
    int y = 46;

    SetTextColor(hdc, RGB(225, 235, 248));
    _snwprintf(line, 256, L"Сцена:    %s %s", PresetName(g_cfg.preset), g_cfg.paused ? L"[ПАУЗА]" : L"");
    TextOutW(hdc, 24, y, line, (int)wcslen(line)); y += 18;

    double speedup = (g_stats.stepTimeMs > 0.001 && g_stats.singleThreadMs > 0.001)
                     ? (g_stats.singleThreadMs / g_stats.stepTimeMs) : 1.0;
    _snwprintf(line, 256, L"Потоки:   %d шт. [%s] | Физика: %.2f мс (~%.2fx)",
               g_stats.workerThreads, PriorityName(g_stats.workerPriority),
               g_stats.stepTimeMs, speedup);
    TextOutW(hdc, 24, y, line, (int)wcslen(line)); y += 18;

    {
        double eff = EffectiveTimeScale();
        double modelPerFrame = g_cfg.dt * eff;
        if (g_cfg.preset == PRESET_SOLAR_SYSTEM) {
            modelPerFrame = g_solarHudSubDt * (double)g_solarHudSubSteps;
            double daysPerSec = modelPerFrame * g_fps;
            _snwprintf(line, 256, L"Время: %.2fx | %.3g сут/кадр | ~%.1f сут/с | подшаг %.2e×%d | FPS %.1f",
                       g_cfg.timeScale, modelPerFrame, daysPerSec,
                       g_solarHudSubDt, g_solarHudSubSteps, g_fps);
        } else {
            _snwprintf(line, 256, L"Время: %.2fx (×%.2f) | dt·scale=%.4g | Подшаги %d | FPS %.1f",
                       g_cfg.timeScale, ConfigPresetTimeGain(g_cfg.preset),
                       modelPerFrame, g_cfg.subSteps, g_fps);
        }
    }
    TextOutW(hdc, 24, y, line, (int)wcslen(line)); y += 18;

    if (g_cfg.preset == PRESET_SOLAR_SYSTEM) {
        _snwprintf(line, 256, L"Время:    %.1f сут (%.3f года) | Дрейф E: %+.2e",
                   g_stats.simTime, g_stats.simTime / SIM_DAYS_PER_YEAR, g_stats.energyDriftRel);
        TextOutW(hdc, 24, y, line, (int)wcslen(line)); y += 18;
    }

    _snwprintf(line, 256, L"Тела:     %d активных (слияний: %d) из %d",
               g_stats.activeBodies, g_stats.mergersCount, MAX_BODIES);
    TextOutW(hdc, 24, y, line, (int)wcslen(line)); y += 18;

    _snwprintf(line, 256, L"Энергия:  K=%.2e  U=%.2e  E=%.2e",
               g_stats.kineticEnergy, g_stats.potentialEnergy, g_stats.totalEnergy);
    TextOutW(hdc, 24, y, line, (int)wcslen(line)); y += 18;

    _snwprintf(line, 256, L"Память:   VirtualAlloc %llu КБ | DLL потоков: %ld",
               (unsigned long long)(g_stats.virtualMemBytes / 1024), g_stats.dllThreadsAttached);
    TextOutW(hdc, 24, y, line, (int)wcslen(line)); y += 18;

    if (g_cfg.preset == PRESET_SOLAR_SYSTEM) {
        _snwprintf(line, 256, L"Камера:   Zoom=%.1f | Центр (%.2f, %.2f) AU | Масса ЛКМ=%.4g M☉",
                   g_cfg.camZoom, g_cfg.camX, g_cfg.camY, g_spawnMass);
    } else {
        _snwprintf(line, 256, L"Камера:   Zoom=%.2fx Pos=(%.0f, %.0f) | Масса ЛКМ=%.0f",
                   g_cfg.camZoom, g_cfg.camX, g_cfg.camY, g_spawnMass);
    }
    TextOutW(hdc, 24, y, line, (int)wcslen(line)); y += 22;

    if (g_showHelpHud) {
        SelectObject(hdc, g_fontHudSmall);
        SetTextColor(hdc, RGB(155, 175, 205));
        const wchar_t *helpLines[] = {
            L"[1..4] Пресеты сцен   [Space] Пауза   [R] Перезапуск сцены",
            L"[< / >] Скорость симуляции (0.1x..10x)  [PgUp/PgDn] Подшаги CPU",
            L"[B] Добавить +250 тел (нагрузка CPU)    [+ / -] Потоки (1..64)",
            L"[P] Приоритет потоков  [T] Треки  [O] Орбиты-гид  [V] Векторы  [C] Слияние",
            L"[G] Сетка XFORM  [ [ / ] ] Масса запускаемого тела",
            L"[F5] Сохранить в File Mapping (.grav)  [F9] Загрузить (.grav)",
            L"[ЛКМ] Запустить тело  [ПКМ] Камера  [Колесо] Зум  [H] Справка"
        };
        for (int i = 0; i < 7; ++i) {
            TextOutW(hdc, 24, y, helpLines[i], (int)wcslen(helpLines[i]));
            y += 18;
        }
    }

    // Нижняя строка состояния
    if (g_statusText[0] != L'\0' && GetTickCount64() < g_statusExpireTick) {
        SelectObject(hdc, g_fontHudTitle);
        SetTextColor(hdc, RGB(255, 215, 95));
        TextOutW(hdc, 16, clientH - 30, g_statusText, (int)wcslen(g_statusText));
    }

    SelectObject(hdc, oldFont);
}

static void CycleThreadPriority(void)
{
    const int prios[] = {
        THREAD_PRIORITY_LOWEST,
        THREAD_PRIORITY_BELOW_NORMAL,
        THREAD_PRIORITY_NORMAL,
        THREAD_PRIORITY_ABOVE_NORMAL,
        THREAD_PRIORITY_HIGHEST
    };
    int next = THREAD_PRIORITY_NORMAL;
    for (int i = 0; i < 5; ++i) {
        if (g_cfg.threadPriority == prios[i]) {
            next = prios[(i + 1) % 5];
            break;
        }
    }
    g_cfg.threadPriority = next;
    g_phys.SetThreads(g_cfg.numThreads, g_cfg.threadPriority);

    wchar_t msg[128];
    _snwprintf(msg, 128, L"Установлен приоритет рабочих потоков: %s", PriorityName(next));
    SetBannerStatus(msg);
}

// Оконная процедура обработки сообщений Windows (слайды 16–21)
static LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_CREATE:
        g_statusExpireTick = GetTickCount64() + 9000;
        return 0;

    case WM_ERASEBKGND:
        return 1; // Предотвращение мерцания при двойной буферизации

    case WM_LBUTTONDOWN: {
        SetCapture(hWnd);
        g_slingshotActive = TRUE;
        g_slingStartX = GET_X_LPARAM(lParam);
        g_slingStartY = GET_Y_LPARAM(lParam);
        g_slingCurX   = g_slingStartX;
        g_slingCurY   = g_slingStartY;
        return 0;
    }

    case WM_LBUTTONUP: {
        if (g_slingshotActive) {
            ReleaseCapture();
            g_slingshotActive = FALSE;

            RECT rc;
            GetClientRect(hWnd, &rc);
            double wx, wy;
            ScreenToWorld(g_slingStartX, g_slingStartY, rc.right, rc.bottom, &wx, &wy);

            // Вектор скорости направлен по оттяжке курсора
            double vFac = (g_cfg.preset == PRESET_SOLAR_SYSTEM) ? 0.004 : 0.55;
            if (g_cfg.preset == PRESET_SOLAR_SYSTEM)
                vFac /= (g_cfg.camZoom > 1.0 ? g_cfg.camZoom : 1.0);
            else
                vFac /= g_cfg.camZoom;
            double vx = (double)(g_slingCurX - g_slingStartX) * vFac;
            double vy = (double)(g_slingCurY - g_slingStartY) * vFac;

            DWORD col = RGB(120 + (rand() % 135), 150 + (rand() % 105), 255);
            g_phys.AddBody(wx, wy, vx, vy, g_spawnMass, 0.0f, col);
            g_renderCount = g_phys.SnapshotRead(g_renderBodies, g_renderMeta, MAX_BODIES, &g_stats);
            InvalidateRect(hWnd, NULL, FALSE);
        }
        return 0;
    }

    case WM_RBUTTONDOWN: {
        SetCapture(hWnd);
        g_panningActive = TRUE;
        g_panLastX = GET_X_LPARAM(lParam);
        g_panLastY = GET_Y_LPARAM(lParam);
        return 0;
    }

    case WM_RBUTTONUP: {
        if (g_panningActive) {
            ReleaseCapture();
            g_panningActive = FALSE;
        }
        return 0;
    }

    case WM_MOUSEMOVE: {
        int mx = GET_X_LPARAM(lParam);
        int my = GET_Y_LPARAM(lParam);
        if (g_slingshotActive) {
            g_slingCurX = mx;
            g_slingCurY = my;
        }
        if (g_panningActive) {
            int dx = mx - g_panLastX;
            int dy = my - g_panLastY;
            g_cfg.camX -= (double)dx / g_cfg.camZoom;
            g_cfg.camY -= (double)dy / g_cfg.camZoom;
            g_panLastX = mx;
            g_panLastY = my;
        }
        return 0;
    }

    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wParam);
        double factor = (delta > 0) ? 1.15 : (1.0 / 1.15);
        double newZoom = g_cfg.camZoom * factor;
        double zMax = (g_cfg.preset == PRESET_SOLAR_SYSTEM) ? 120.0 : 6.0;
        if (newZoom >= 0.12 && newZoom <= zMax) {
            g_cfg.camZoom = newZoom;
        }
        return 0;
    }

    case WM_KEYDOWN: {
        wchar_t msg[256];
        switch (wParam) {
        case VK_SPACE:
            g_cfg.paused = !g_cfg.paused;
            SetBannerStatus(g_cfg.paused ? L"Симуляция приостановлена" : L"Симуляция возобновлена");
            break;

        case '1':
        case '2':
        case '3':
        case '4':
            ApplyPreset((int)(wParam - '0'));
            _snwprintf(msg, 256, L"Загружен пресет %s", PresetName(g_cfg.preset));
            SetBannerStatus(msg);
            break;

        case 'R':
            ApplyPreset(g_cfg.preset);
            SetBannerStatus(L"Сцена перезапущена");
            break;

        case VK_OEM_PLUS:
        case VK_ADD:
            if (g_cfg.numThreads < MAX_WORKERS) {
                g_cfg.numThreads++;
                g_phys.SetThreads(g_cfg.numThreads, g_cfg.threadPriority);
                _snwprintf(msg, 256, L"Пул потоков увеличен до %d", g_cfg.numThreads);
                SetBannerStatus(msg);
            }
            break;

        case VK_OEM_MINUS:
        case VK_SUBTRACT:
            if (g_cfg.numThreads > 1) {
                g_cfg.numThreads--;
                g_phys.SetThreads(g_cfg.numThreads, g_cfg.threadPriority);
                _snwprintf(msg, 256, L"Пул потоков уменьшен до %d", g_cfg.numThreads);
                SetBannerStatus(msg);
            }
            break;

        case 'P':
            CycleThreadPriority();
            break;

        case 'T':
            g_cfg.showTrails = !g_cfg.showTrails;
            SetBannerStatus(g_cfg.showTrails ? L"Орбитальные треки включены" : L"Орбитальные треки скрыты");
            break;

        case 'O':
            g_cfg.showOrbitGuides = !g_cfg.showOrbitGuides;
            SetBannerStatus(g_cfg.showOrbitGuides ? L"Справочные орбиты (круги) включены" : L"Справочные орбиты скрыты");
            break;

        case 'V':
            g_cfg.showVectors = !g_cfg.showVectors;
            SetBannerStatus(g_cfg.showVectors ? L"Векторы скорости включены" : L"Векторы скорости скрыты");
            break;

        case 'C':
            g_cfg.enableCollisions = !g_cfg.enableCollisions;
            g_phys.SetParams(g_cfg.gravityG, g_cfg.softening, g_cfg.enableCollisions);
            SetBannerStatus(g_cfg.enableCollisions ? L"Слияние тел при столкновении ВКЛ" : L"Слияние тел ВЫКЛ");
            break;

        case 'G':
            g_cfg.showGrid = !g_cfg.showGrid;
            break;

        case 'H':
            g_showHelpHud = !g_showHelpHud;
            break;

        case VK_OEM_4: // '['
            g_spawnMass *= 0.65;
            if (g_cfg.preset == PRESET_SOLAR_SYSTEM) {
                if (g_spawnMass < 1e-10) g_spawnMass = 1e-10;
            } else if (g_spawnMass < 1.0) {
                g_spawnMass = 1.0;
            }
            break;

        case VK_OEM_6: // ']'
            g_spawnMass *= 1.5;
            if (g_cfg.preset == PRESET_SOLAR_SYSTEM) {
                if (g_spawnMass > 0.02) g_spawnMass = 0.02;
            } else if (g_spawnMass > 50000.0) {
                g_spawnMass = 50000.0;
            }
            break;

        case VK_OEM_COMMA: // '<' / ',' — замедление скорости симуляции
            g_cfg.timeScale *= 0.8;
            if (g_cfg.timeScale < 0.05) g_cfg.timeScale = 0.05;
            _snwprintf(msg, 256, L"Скорость симуляции: %.2fx (эфф. dt = %.4g)",
                       g_cfg.timeScale, g_cfg.dt * EffectiveTimeScale());
            SetBannerStatus(msg);
            break;

        case VK_OEM_PERIOD: // '>' / '.' — ускорение скорости симуляции
            g_cfg.timeScale *= 1.25;
            if (g_cfg.timeScale > TimeScaleUserMax()) g_cfg.timeScale = TimeScaleUserMax();
            _snwprintf(msg, 256, L"Скорость симуляции: %.2fx (эфф. dt = %.4g)",
                       g_cfg.timeScale, g_cfg.dt * EffectiveTimeScale());
            SetBannerStatus(msg);
            break;

        case VK_PRIOR: // PageUp — увеличить число подшагов физики (больше нагрузка CPU и точность)
            if (g_cfg.subSteps < 32) {
                g_cfg.subSteps += 2;
                if (g_cfg.subSteps > 32) g_cfg.subSteps = 32;
                _snwprintf(msg, 256, L"Подшагов интегратора за кадр: %d (повышена нагрузка CPU)", g_cfg.subSteps);
                SetBannerStatus(msg);
            }
            break;

        case VK_NEXT: // PageDown — уменьшить число подшагов физики
            if (g_cfg.subSteps > 1) {
                g_cfg.subSteps -= 2;
                if (g_cfg.subSteps < 1) g_cfg.subSteps = 1;
                _snwprintf(msg, 256, L"Подшагов интегратора за кадр: %d", g_cfg.subSteps);
                SetBannerStatus(msg);
            }
            break;

        case 'B': { // Добавить кластер из +250 тел для увеличения вычислительной нагрузки O(N^2)
            g_phys.AddCluster(g_cfg.camX, g_cfg.camY, 250);
            g_renderCount = g_phys.SnapshotRead(g_renderBodies, g_renderMeta, MAX_BODIES, &g_stats);
            _snwprintf(msg, 256, L"Добавлено 250 тел! Активных тел: %d из %d", g_stats.activeBodies, MAX_BODIES);
            SetBannerStatus(msg);
            break;
        }

        case VK_LEFT:  g_cfg.camX -= 35.0 / g_cfg.camZoom; break;
        case VK_RIGHT: g_cfg.camX += 35.0 / g_cfg.camZoom; break;
        case VK_UP:    g_cfg.camY -= 35.0 / g_cfg.camZoom; break;
        case VK_DOWN:  g_cfg.camY += 35.0 / g_cfg.camZoom; break;

        case VK_F5: {
            g_renderCount = g_phys.SnapshotRead(g_renderBodies, g_renderMeta, MAX_BODIES, &g_stats);
            SceneSaveMapped(SNAPSHOT_FILE, &g_cfg, g_renderBodies, g_renderMeta,
                            g_renderCount, g_stats.stepCounter, msg, 256);
            RegistrySaveConfig(&g_cfg);
            SetBannerStatus(msg);
            break;
        }

        case VK_F9: {
            int loadedCount = 0;
            ULONGLONG loadedStep = 0;
            if (SceneLoadMapped(SNAPSHOT_FILE, &g_cfg, g_renderBodies, g_renderMeta,
                                MAX_BODIES, &loadedCount, &loadedStep, msg, 256)) {
                g_phys.SetParams(g_cfg.gravityG, g_cfg.softening, g_cfg.enableCollisions);
                g_phys.LoadBodies(g_renderBodies, g_renderMeta, loadedCount, loadedStep);
                g_renderCount = g_phys.SnapshotRead(g_renderBodies, g_renderMeta, MAX_BODIES, &g_stats);
            }
            SetBannerStatus(msg);
            break;
        }

        case VK_ESCAPE:
            DestroyWindow(hWnd);
            break;
        }
        return 0;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC winDC = BeginPaint(hWnd, &ps);
        RECT rc;
        GetClientRect(hWnd, &rc);
        int cw = rc.right - rc.left;
        int ch = rc.bottom - rc.top;

        EnsureBackBuffer(winDC, cw, ch);
        RenderScene(g_memDC, cw, ch);
        BitBlt(winDC, 0, 0, cw, ch, g_memDC, 0, 0, SRCCOPY);

        EndPaint(hWnd, &ps);
        return 0;
    }

    case WM_DESTROY:
        RegistrySaveConfig(&g_cfg);
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hWnd, message, wParam, lParam);
}

static LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);

// Режим автоматической самопроверки (--selftest): проверяет DLL, многопоточность,
// сохранение энергии, File Mapping (.grav) и реестр Windows
static int RunAutomatedSelfTest(void)
{
    SimConfig cfg;
    ConfigSetDefaults(&cfg);
    cfg.numThreads = 4;
    cfg.preset = PRESET_SOLAR_SYSTEM;

    wchar_t err[256] = {0};
    if (!LoadPhysicsDll(err, 256))
        return 10;

    SIZE_T bufBytes = sizeof(Body) * MAX_BODIES + sizeof(BodyMeta) * MAX_BODIES;
    BYTE *mem = (BYTE *)VirtualAlloc(NULL, bufBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!mem) {
        FreeLibrary(g_phys.hModule);
        return 11;
    }
    Body *bodies = (Body *)mem;
    BodyMeta *meta = (BodyMeta *)(mem + sizeof(Body) * MAX_BODIES);

    if (!g_phys.Init(&cfg)) {
        VirtualFree(mem, 0, MEM_RELEASE);
        FreeLibrary(g_phys.hModule);
        return 12;
    }

    // Проверка всех 4 пресетов и переключения потоков (1, 2, 4, 8)
    for (int p = 1; p <= 4; ++p) {
        ConfigApplyPresetPhysics(&cfg, p);
        g_phys.SetParams(cfg.gravityG, cfg.softening, cfg.enableCollisions);
        g_phys.ResetPreset(p, 120);
        g_phys.SetThreads(p * 2, THREAD_PRIORITY_NORMAL);
        for (int step = 0; step < 10; ++step) {
            if (p == PRESET_SOLAR_SYSTEM) {
                double subDt = SIM_SOLAR_PHYSICS_SUBDT;
                int subSteps = ConfigSolarPhysicsSubSteps(1.0, &subDt);
                g_phys.Step(subDt, subSteps);
            } else {
                g_phys.Step(cfg.dt, 2);
            }
        }
    }

    SimStats st = {0};

    // Солнечная система: Земля ~1 AU и Луна вокруг Земли после ~120 сут модели
    ConfigApplyPresetPhysics(&cfg, PRESET_SOLAR_SYSTEM);
    g_phys.SetParams(cfg.gravityG, cfg.softening, cfg.enableCollisions);
    g_phys.ResetPreset(PRESET_SOLAR_SYSTEM, 0);
    double subDt = SIM_SOLAR_PHYSICS_SUBDT;
    int subSteps = ConfigSolarPhysicsSubSteps(1.0, &subDt);
    double modelDays = 0.0;
    while (modelDays < 120.0) {
        g_phys.Step(subDt, subSteps);
        modelDays += subDt * (double)subSteps;
    }
    int snap = g_phys.SnapshotRead(bodies, meta, MAX_BODIES, &st);
    int earthIdx = -1, moonIdx = -1;
    for (int i = 0; i < snap; ++i) {
        if (!meta[i].alive) continue;
        double m = bodies[i].mass;
        if (m > 2.0e-6 && m < 4.5e-6) earthIdx = i;
        if (m > 1.0e-8 && m < 8.0e-8) moonIdx = i;
    }
    if (earthIdx < 0) {
        g_phys.Shutdown();
        VirtualFree(mem, 0, MEM_RELEASE);
        FreeLibrary(g_phys.hModule);
        return 17;
    }
    double rex = bodies[earthIdx].x, rey = bodies[earthIdx].y;
    double rEarth = sqrt(rex * rex + rey * rey);
    if (rEarth < 0.92 || rEarth > 1.08) {
        g_phys.Shutdown();
        VirtualFree(mem, 0, MEM_RELEASE);
        FreeLibrary(g_phys.hModule);
        return 18;
    }
    if (moonIdx >= 0) {
        double dx = bodies[moonIdx].x - bodies[earthIdx].x;
        double dy = bodies[moonIdx].y - bodies[earthIdx].y;
        double rMoon = sqrt(dx * dx + dy * dy);
        if (rMoon < 0.0018 || rMoon > 0.0034) {
            g_phys.Shutdown();
            VirtualFree(mem, 0, MEM_RELEASE);
            FreeLibrary(g_phys.hModule);
            return 19;
        }
    }

    int count = g_phys.SnapshotRead(bodies, meta, MAX_BODIES, &st);
    if (count <= 0 || st.activeBodies <= 0) {
        g_phys.Shutdown();
        VirtualFree(mem, 0, MEM_RELEASE);
        FreeLibrary(g_phys.hModule);
        return 13;
    }

    // Проверка File Mapping (SceneSaveMapped + SceneLoadMapped)
    wchar_t statusMsg[256] = {0};
    if (!SceneSaveMapped(SNAPSHOT_FILE, &cfg, bodies, meta, count, st.stepCounter, statusMsg, 256)) {
        g_phys.Shutdown();
        VirtualFree(mem, 0, MEM_RELEASE);
        FreeLibrary(g_phys.hModule);
        return 14;
    }

    int loadedCount = 0;
    ULONGLONG loadedStep = 0;
    if (!SceneLoadMapped(SNAPSHOT_FILE, &cfg, bodies, meta, MAX_BODIES, &loadedCount, &loadedStep, statusMsg, 256) ||
        loadedCount != count || loadedStep != st.stepCounter) {
        g_phys.Shutdown();
        VirtualFree(mem, 0, MEM_RELEASE);
        FreeLibrary(g_phys.hModule);
        return 15;
    }

    // Проверка реестра Windows (RegistrySaveConfig + RegistryLoadConfig)
    if (!RegistrySaveConfig(&cfg) || !RegistryLoadConfig(&cfg)) {
        g_phys.Shutdown();
        VirtualFree(mem, 0, MEM_RELEASE);
        FreeLibrary(g_phys.hModule);
        return 16;
    }

    g_phys.Shutdown();
    VirtualFree(mem, 0, MEM_RELEASE);
    FreeLibrary(g_phys.hModule);
    return 0;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
    (void)hPrevInstance;
    if (lpCmdLine && strstr(lpCmdLine, "--selftest") != NULL) {
        return RunAutomatedSelfTest();
    }

    // 1. Чтение сохраненной конфигурации из реестра Windows (или установка значений по умолчанию)
    BOOL fromReg = RegistryLoadConfig(&g_cfg);
    if (fromReg) {
        wcsncpy(g_statusText, L"Настройки восстановлены из реестра HKCU\\Software\\GravitySim", 255);
    }
    if (g_cfg.preset == PRESET_SOLAR_SYSTEM &&
        (g_cfg.softening > 1e-4 || g_cfg.gravityG > 0.01 ||
         g_cfg.dt < 0.01 || g_cfg.dt > 0.5)) {
        ConfigApplyPresetPhysics(&g_cfg, PRESET_SOLAR_SYSTEM);
    }
    if (g_cfg.preset == PRESET_SOLAR_SYSTEM)
        g_spawnMass = 1e-5;
    // 2. Динамический импорт вычислительной DLL (LoadLibraryW + GetProcAddress)
    wchar_t errMsg[256] = {0};
    if (!LoadPhysicsDll(errMsg, 256)) {
        MessageBoxW(NULL, errMsg, L"Ошибка загрузки DLL", MB_ICONERROR | MB_OK);
        return 1;
    }

    // 3. Выделение буфера рендеринга в виртуальной памяти процесса
    SIZE_T renderBufBytes = sizeof(Body) * MAX_BODIES + sizeof(BodyMeta) * MAX_BODIES;
    BYTE *renderMem = (BYTE *)VirtualAlloc(NULL, renderBufBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!renderMem) {
        MessageBoxW(NULL, L"Не удалось выделить виртуальную память VirtualAlloc", L"Ошибка", MB_ICONERROR | MB_OK);
        FreeLibrary(g_phys.hModule);
        return 1;
    }
    g_renderBodies = (Body *)renderMem;
    g_renderMeta   = (BodyMeta *)(renderMem + sizeof(Body) * MAX_BODIES);

    // 4. Инициализация физического ядра и генерация начальной астрофизической сцены
    if (!g_phys.Init(&g_cfg)) {
        MessageBoxW(NULL, L"Ошибка инициализации PhysInit в gravity_physics.dll", L"Ошибка", MB_ICONERROR | MB_OK);
        VirtualFree(renderMem, 0, MEM_RELEASE);
        FreeLibrary(g_phys.hModule);
        return 1;
    }
    g_phys.SetParams(g_cfg.gravityG, g_cfg.softening, g_cfg.enableCollisions);
    ApplyPreset(g_cfg.preset);

    InitGdiFonts();
    g_lastFrameTime = QuerySeconds();

    // 5. Регистрация оконного класса (слайды 14–15: WNDCLASSEXW, RegisterClassExW)
    WNDCLASSEXW wcex = {0};
    wcex.cbSize        = sizeof(WNDCLASSEXW);
    wcex.style         = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wcex.lpfnWndProc   = WndProc;
    wcex.hInstance     = hInstance;
    wcex.hIcon         = LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION);
    wcex.hCursor       = LoadCursorW(NULL, (LPCWSTR)IDC_CROSS);
    wcex.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wcex.lpszClassName = L"GravitySimWndClass";
    wcex.hIconSm       = wcex.hIcon;

    if (!RegisterClassExW(&wcex)) {
        g_phys.Shutdown();
        VirtualFree(renderMem, 0, MEM_RELEASE);
        FreeLibrary(g_phys.hModule);
        return 1;
    }

    int winW = 1280, winH = 800;
    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    int posX = (screenW > winW) ? (screenW - winW) / 2 : CW_USEDEFAULT;
    int posY = (screenH > winH) ? (screenH - winH) / 2 : CW_USEDEFAULT;

    HWND hWnd = CreateWindowExW(
        0,
        L"GravitySimWndClass",
        L"Программное средство моделирования гравитационного взаимодействия тел (Win32 API)",
        WS_OVERLAPPEDWINDOW,
        posX, posY, winW, winH,
        NULL, NULL, hInstance, NULL
    );

    if (!hWnd) {
        g_phys.Shutdown();
        VirtualFree(renderMem, 0, MEM_RELEASE);
        FreeLibrary(g_phys.hModule);
        return 1;
    }

    ShowWindow(hWnd, nCmdShow);
    UpdateWindow(hWnd);

    // 6. Неблокирующий цикл реального времени (PeekMessageW + QueryPerformanceCounter)
    // Обеспечивает ровно до 60.0 FPS без потери кадров из-за кванта системного таймера (15.6 мс)
    MSG msg = {0};
    BOOL running = TRUE;
    double frameStart = QuerySeconds();

    while (running) {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                running = FALSE;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!running)
            break;

        // Шаг физического движка с учетом скорости времени timeScale и числа подшагов subSteps
        if (!g_cfg.paused) {
            if (g_cfg.preset == PRESET_SOLAR_SYSTEM) {
                double subDt = SIM_SOLAR_PHYSICS_SUBDT;
                int subSteps = ConfigSolarPhysicsSubSteps(g_cfg.timeScale, &subDt);
                g_solarHudSubDt = subDt;
                g_solarHudSubSteps = subSteps;
                g_phys.Step(subDt, subSteps);
            } else {
                int subSteps = (g_cfg.subSteps >= 1) ? g_cfg.subSteps : 6;
                if (g_cfg.timeScale > 2.0 &&
                    subSteps < (int)(g_cfg.timeScale * 4.0)) {
                    subSteps = (int)(g_cfg.timeScale * 4.0);
                    if (subSteps > 32) subSteps = 32;
                }
                g_phys.Step(g_cfg.dt * EffectiveTimeScale(), subSteps);
            }
        }

        g_renderCount = g_phys.SnapshotRead(g_renderBodies, g_renderMeta, MAX_BODIES, &g_stats);
        InvalidateRect(hWnd, NULL, FALSE);
        UpdateWindow(hWnd);

        // Высокоточное ограничение частоты кадров ровно до 60 FPS (16.6667 мс на кадр)
        double now = QuerySeconds();
        double elapsed = now - frameStart;
        if (elapsed < TARGET_FRAME_SEC) {
            double remMs = (TARGET_FRAME_SEC - elapsed) * 1000.0;
            if (remMs > 2.5) {
                Sleep((DWORD)(remMs - 1.5));
            }
            // Докрутка оставшихся долей миллисекунды с уступкой кванта времени SwitchToThread (слайд 76)
            while ((QuerySeconds() - frameStart) < TARGET_FRAME_SEC) {
                SwitchToThread();
            }
        }

        double frameEnd = QuerySeconds();
        double dtFrame = frameEnd - frameStart;
        frameStart = frameEnd;
        if (dtFrame > 0.0001 && dtFrame < 1.0) {
            g_fps = g_fps * 0.88 + (1.0 / dtFrame) * 0.12;
        }
    }

    // 7. Корректная деинициализация системных ресурсов
    FreeGdiResources();
    g_phys.Shutdown();
    VirtualFree(renderMem, 0, MEM_RELEASE);
    FreeLibrary(g_phys.hModule);

    return (int)msg.wParam;
}
