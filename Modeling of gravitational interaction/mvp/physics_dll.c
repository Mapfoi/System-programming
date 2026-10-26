/*
 * physics_dll.c — вычислительное ядро gravity_physics.dll.
 *
 * PhysInit: VirtualAlloc одним блоком [Body][Meta][newAx][newAy].
 * PhysStep: EnterCriticalSection → VerletSubstep(dt) → LeaveCriticalSection.
 * VerletSubstep: сдвиг r, пересчёт сил O(N²), обновление v (Velocity Verlet).
 * Сцена по умолчанию: ядро + гравитационное облако (PhysResetScene).
 */
#include "common.h" // Body, SimConfig, MAX_BODIES, RGB
#include <math.h>   // sqrt, cos, sin
#include <stdlib.h>
#include <string.h> // memset, memcpy

#define DLL_EXPORT __declspec(dllexport) // экспорт имён для GetProcAddress в exe

typedef struct {
    Body    *bodies;      // массив тел в одном регионе VirtualAlloc
    BodyMeta *meta;       // флаги alive и служебные поля
    double  *newAx;       // буфер новых ускорений по X после пересчёта сил
    double  *newAy;       // буфер новых ускорений по Y
    SIZE_T   virtualBytes; // размер выделенного блока для статистики
    int      bodyCount;   // сколько слотов занято (0..MAX_BODIES)
    int      activeBodies; // счётчик alive для HUD
    ULONGLONG stepCounter; // число вызовов PhysStep
    double   gravityG;    // G сцены
    double   softening;   // ε сглаживания расстояния
    double   curDt;       // последний dt шага (для отладки)
    double   lastStepMs;  // длительность последнего PhysStep в мс
    CRITICAL_SECTION lock; // защита от одновременного Step и SnapshotRead
    LARGE_INTEGER qpcFreq; // частота QPC для NowMs
} Engine;

static Engine g_eng; // единственное состояние DLL (без потоков-воркеров)

static double RandUnit(unsigned int *seed)
{
    *seed = (*seed * 1664525u + 1013904223u); // LCG, обновляет seed
    return (double)(*seed & 0x00FFFFFFu) / (double)0x01000000u; // [0,1)
}

static double NowMs(void)
{
    LARGE_INTEGER t; // текущий счётчик QPC
    if (!g_eng.qpcFreq.QuadPart)
        QueryPerformanceFrequency(&g_eng.qpcFreq); // тиков в секунду, один раз
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)g_eng.qpcFreq.QuadPart; // мс
}

static SIZE_T Align64K(SIZE_T n)
{
    const SIZE_T g = 65536; // гранула VirtualAlloc 64 КБ
    return (n + g - 1) & ~(g - 1); // округление размера вверх
}

static int AddBody(double x, double y, double vx, double vy, double mass, float rad, DWORD col)
{
    if (g_eng.bodyCount >= MAX_BODIES) // массив полон
        return -1;
    int i = g_eng.bodyCount++; // новый индекс
    Body *b = &g_eng.bodies[i];
    b->x = x; b->y = y; // позиция
    b->vx = vx; b->vy = vy; // скорость
    b->ax = 0; b->ay = 0; // ускорение заполнит ComputeAccelerations / Verlet
    b->mass = mass;
    b->radius = rad; // пиксельный радиус для Ellipse в exe
    b->color = col; // COLORREF
    g_eng.meta[i].alive = 1; // участвует в O(N²)
    g_eng.activeBodies++;
    return i;
}

static void ComputeAccelerations(void)
{
    int n = g_eng.bodyCount; // число слотов
    double G = g_eng.gravityG;
    double eps2 = g_eng.softening * g_eng.softening; // ε² в знаменателе

    for (int i = 0; i < n; ++i) { // для каждого тела i
        if (!g_eng.meta[i].alive) {
            g_eng.newAx[i] = g_eng.newAy[i] = 0.0; // мёртвые не ускоряются
            continue;
        }
        double xi = g_eng.bodies[i].x, yi = g_eng.bodies[i].y; // позиция i
        double axi = 0, ayi = 0; // сумма вкладов от j
        for (int j = 0; j < n; ++j) {
            if (i == j || !g_eng.meta[j].alive) // не себя и не мёртвых
                continue;
            double dx = g_eng.bodies[j].x - xi; // вектор i→j
            double dy = g_eng.bodies[j].y - yi;
            double d2 = dx * dx + dy * dy + eps2; // |r|² + ε²
            double inv = 1.0 / sqrt(d2); // 1/|r|
            double s = G * g_eng.bodies[j].mass * inv * inv * inv; // G·m_j/|r|³
            axi += dx * s; // a_x
            ayi += dy * s; // a_y
        }
        g_eng.newAx[i] = axi; // записать для фазы скоростей Verlet
        g_eng.newAy[i] = ayi;
    }
}

static void VerletSubstep(double dt)
{
    int n = g_eng.bodyCount;
    double h2 = 0.5 * dt * dt; // 0.5·dt² для сдвига r
    Body *b = g_eng.bodies;
    BodyMeta *m = g_eng.meta;

    for (int i = 0; i < n; ++i) { // фаза 1: r += v·dt + 0.5·a·dt²
        if (!m[i].alive) continue;
        b[i].x += b[i].vx * dt + b[i].ax * h2;
        b[i].y += b[i].vy * dt + b[i].ay * h2;
    }

    ComputeAccelerations(); // a' из новых позиций (один поток, полный O(N²))

    double half = 0.5 * dt; // половина шага для v
    for (int i = 0; i < n; ++i) { // фаза 2: v += 0.5(a+a')dt, a←a'
        if (!m[i].alive) continue;
        b[i].vx += (b[i].ax + g_eng.newAx[i]) * half;
        b[i].vy += (b[i].ay + g_eng.newAy[i]) * half;
        b[i].ax = g_eng.newAx[i]; // сохранить для следующего шага
        b[i].ay = g_eng.newAy[i];
    }
}

static void BuildDefaultScene(void)
{
    g_eng.bodyCount = 0; // очистить сцену
    g_eng.activeBodies = 0;
    g_eng.stepCounter = 0;

    unsigned int seed = (unsigned int)GetTickCount() ^ 0xC0FFEEu; // случайность пресета
    double G = g_eng.gravityG;
    const double coreM = 8000.0; // масса центрального ядра

    AddBody(0, 0, 0, 0, coreM, 18.0f, RGB(255, 220, 100)); // неподвижное ядро в центре

    const int stars = 280; // число орбитальных «звёзд»
    for (int k = 0; k < stars; ++k) {
        double r = 40.0 + sqrt(RandUnit(&seed)) * 380.0; // радиус: плотнее к центру (sqrt)
        double a = RandUnit(&seed) * 6.28318530718; // угол 0..2π
        double px = cos(a) * r, py = sin(a) * r; // позиция на кольце
        double v = sqrt(G * (coreM + r * 2.0) / r) * (0.9 + RandUnit(&seed) * 0.15); // почти кеплерова скорость
        double vx = -sin(a) * v, vy = cos(a) * v; // касательная (прогрейд)
        double m = 1.0 + RandUnit(&seed) * 4.0; // масса звезды
        BYTE cr = (BYTE)(120 + (int)(RandUnit(&seed) * 120)); // случайный цвет
        BYTE cg = (BYTE)(140 + (int)(RandUnit(&seed) * 100));
        BYTE cb = (BYTE)(180 + (int)(RandUnit(&seed) * 70));
        AddBody(px, py, vx, vy, m, 2.5f, RGB(cr, cg, cb));
    }

    ComputeAccelerations(); // начальные ax/ay для первого VerletSubstep
}

DLL_EXPORT BOOL __cdecl PhysInit(const SimConfig *cfg)
{
    InitializeCriticalSection(&g_eng.lock); // mutex для Step/Snapshot
    g_eng.gravityG = cfg ? cfg->gravityG : 120.0; // из exe или значение по умолчанию
    g_eng.softening = cfg ? cfg->softening : 4.5;

    SIZE_T sb = Align64K(sizeof(Body) * MAX_BODIES); // регион Body
    SIZE_T sm = Align64K(sizeof(BodyMeta) * MAX_BODIES); // регион Meta
    SIZE_T sa = Align64K(sizeof(double) * MAX_BODIES * 2); // newAx + newAy
    SIZE_T total = sb + sm + sa; // один VirtualAlloc на всё

    BYTE *base = (BYTE *)VirtualAlloc(NULL, total, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!base)
        return FALSE; // не хватило памяти

    g_eng.bodies = (Body *)base; // начало блока
    g_eng.meta = (BodyMeta *)(base + sb); // следом метаданные
    g_eng.newAx = (double *)(base + sb + sm); // буферы ускорений
    g_eng.newAy = g_eng.newAx + MAX_BODIES; // вторая половина sa
    g_eng.virtualBytes = total; // для SimStats
    return TRUE;
}

DLL_EXPORT void __cdecl PhysShutdown(void)
{
    EnterCriticalSection(&g_eng.lock); // не освобождать во время Step
    if (g_eng.bodies) {
        VirtualFree(g_eng.bodies, 0, MEM_RELEASE); // весь блок одним вызовом
        g_eng.bodies = NULL;
    }
    LeaveCriticalSection(&g_eng.lock);
    DeleteCriticalSection(&g_eng.lock);
}

DLL_EXPORT void __cdecl PhysResetScene(void)
{
    EnterCriticalSection(&g_eng.lock);
    BuildDefaultScene(); // пересобрать ядро + 280 звёзд
    LeaveCriticalSection(&g_eng.lock);
}

DLL_EXPORT void __cdecl PhysStep(double dt)
{
    EnterCriticalSection(&g_eng.lock); // UI не читает во время интеграции
    double t0 = NowMs(); // замер времени шага
    g_eng.curDt = dt; // запомнить для отладки
    g_eng.stepCounter++; // +1 к счётчику кадров физики
    VerletSubstep(dt); // один подшаг Verlet в одном потоке
    g_eng.lastStepMs = NowMs() - t0; // длительность для HUD
    LeaveCriticalSection(&g_eng.lock);
}

DLL_EXPORT int __cdecl PhysSnapshotRead(Body *outBodies, BodyMeta *outMeta,
                                        int maxCount, SimStats *outStats)
{
    EnterCriticalSection(&g_eng.lock); // согласованная копия
    int n = g_eng.bodyCount;
    if (n > maxCount) n = maxCount; // не переполнить буфер exe
    if (outBodies && n > 0)
        memcpy(outBodies, g_eng.bodies, (size_t)n * sizeof(Body)); // копия тел
    if (outMeta && n > 0)
        memcpy(outMeta, g_eng.meta, (size_t)n * sizeof(BodyMeta)); // копия meta
    if (outStats) {
        outStats->activeBodies = g_eng.activeBodies;
        outStats->totalBodies = g_eng.bodyCount;
        outStats->stepCounter = g_eng.stepCounter;
        outStats->stepTimeMs = g_eng.lastStepMs;
        outStats->virtualMemBytes = g_eng.virtualBytes;
    }
    LeaveCriticalSection(&g_eng.lock);
    return n; // сколько элементов скопировано
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID reserved)
{
    (void)h; (void)reserved;
    if (reason == DLL_PROCESS_ATTACH)
        memset(&g_eng, 0, sizeof(g_eng)); // обнулить до PhysInit
    return TRUE;
}
