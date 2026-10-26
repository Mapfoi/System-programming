/*
 * MVP physics_dll.c — однопоточный Verlet, VirtualAlloc, одна демо-сцена.
 * PhysStep: lock → VerletSubstep → unlock. PhysSnapshotRead копирует массив в exe.
 */
#include "common.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define DLL_EXPORT __declspec(dllexport)

typedef struct {
    Body    *bodies;
    BodyMeta *meta;
    double  *newAx;
    double  *newAy;
    SIZE_T   virtualBytes;
    int      bodyCount;
    int      activeBodies;
    ULONGLONG stepCounter;
    double   gravityG;
    double   softening;
    double   curDt;
    double   lastStepMs;
    CRITICAL_SECTION lock;
    LARGE_INTEGER qpcFreq;
} Engine;

static Engine g_eng;

static double RandUnit(unsigned int *seed)
{
    *seed = (*seed * 1664525u + 1013904223u);
    return (double)(*seed & 0x00FFFFFFu) / (double)0x01000000u;
}

static double NowMs(void)
{
    LARGE_INTEGER t;
    if (!g_eng.qpcFreq.QuadPart)
        QueryPerformanceFrequency(&g_eng.qpcFreq);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)g_eng.qpcFreq.QuadPart;
}

static SIZE_T Align64K(SIZE_T n)
{
    const SIZE_T g = 65536;
    return (n + g - 1) & ~(g - 1);
}

static int AddBody(double x, double y, double vx, double vy, double mass, float rad, DWORD col)
{
    if (g_eng.bodyCount >= MAX_BODIES)
        return -1;
    int i = g_eng.bodyCount++;
    Body *b = &g_eng.bodies[i];
    b->x = x; b->y = y;
    b->vx = vx; b->vy = vy;
    b->ax = 0; b->ay = 0;
    b->mass = mass;
    b->radius = rad;
    b->color = col;
    g_eng.meta[i].alive = 1;
    g_eng.activeBodies++;
    return i;
}

static void ComputeAccelerations(void)
{
    int n = g_eng.bodyCount;
    double G = g_eng.gravityG;
    double eps2 = g_eng.softening * g_eng.softening;

    for (int i = 0; i < n; ++i) {
        if (!g_eng.meta[i].alive) {
            g_eng.newAx[i] = g_eng.newAy[i] = 0.0;
            continue;
        }
        double xi = g_eng.bodies[i].x, yi = g_eng.bodies[i].y;
        double axi = 0, ayi = 0;
        for (int j = 0; j < n; ++j) {
            if (i == j || !g_eng.meta[j].alive)
                continue;
            double dx = g_eng.bodies[j].x - xi;
            double dy = g_eng.bodies[j].y - yi;
            double d2 = dx * dx + dy * dy + eps2;
            double inv = 1.0 / sqrt(d2);
            double s = G * g_eng.bodies[j].mass * inv * inv * inv;
            axi += dx * s;
            ayi += dy * s;
        }
        g_eng.newAx[i] = axi;
        g_eng.newAy[i] = ayi;
    }
}

/* Один подшаг Verlet в одном потоке: сдвиг всех r → пересчёт всех a → обновление всех v */
static void VerletSubstep(double dt)
{
    int n = g_eng.bodyCount;
    double h2 = 0.5 * dt * dt;
    Body *b = g_eng.bodies;
    BodyMeta *m = g_eng.meta;

    for (int i = 0; i < n; ++i) {
        if (!m[i].alive) continue;
        b[i].x += b[i].vx * dt + b[i].ax * h2;
        b[i].y += b[i].vy * dt + b[i].ay * h2;
    }

    ComputeAccelerations();

    double half = 0.5 * dt;
    for (int i = 0; i < n; ++i) {
        if (!m[i].alive) continue;
        b[i].vx += (b[i].ax + g_eng.newAx[i]) * half;
        b[i].vy += (b[i].ay + g_eng.newAy[i]) * half;
        b[i].ax = g_eng.newAx[i];
        b[i].ay = g_eng.newAy[i];
    }
}

/* Ядро + звёзды на кеплеровских скоростях вокруг (0,0) */
static void BuildDefaultScene(void)
{
    g_eng.bodyCount = 0;
    g_eng.activeBodies = 0;
    g_eng.stepCounter = 0;

    unsigned int seed = (unsigned int)GetTickCount() ^ 0xC0FFEEu;
    double G = g_eng.gravityG;
    const double coreM = 8000.0;

    AddBody(0, 0, 0, 0, coreM, 18.0f, RGB(255, 220, 100));

    const int stars = 280;
    for (int k = 0; k < stars; ++k) {
        double r = 40.0 + sqrt(RandUnit(&seed)) * 380.0;
        double a = RandUnit(&seed) * 6.28318530718;
        double px = cos(a) * r, py = sin(a) * r;
        double v = sqrt(G * (coreM + r * 2.0) / r) * (0.9 + RandUnit(&seed) * 0.15);
        double vx = -sin(a) * v, vy = cos(a) * v;
        double m = 1.0 + RandUnit(&seed) * 4.0;
        BYTE cr = (BYTE)(120 + (int)(RandUnit(&seed) * 120));
        BYTE cg = (BYTE)(140 + (int)(RandUnit(&seed) * 100));
        BYTE cb = (BYTE)(180 + (int)(RandUnit(&seed) * 70));
        AddBody(px, py, vx, vy, m, 2.5f, RGB(cr, cg, cb));
    }

    ComputeAccelerations();
}

DLL_EXPORT BOOL __cdecl PhysInit(const SimConfig *cfg)
{
    InitializeCriticalSection(&g_eng.lock);
    g_eng.gravityG = cfg ? cfg->gravityG : 120.0;
    g_eng.softening = cfg ? cfg->softening : 4.5;

    SIZE_T sb = Align64K(sizeof(Body) * MAX_BODIES);
    SIZE_T sm = Align64K(sizeof(BodyMeta) * MAX_BODIES);
    SIZE_T sa = Align64K(sizeof(double) * MAX_BODIES * 2);
    SIZE_T total = sb + sm + sa;

    BYTE *base = (BYTE *)VirtualAlloc(NULL, total, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!base)
        return FALSE;

    g_eng.bodies = (Body *)base;
    g_eng.meta = (BodyMeta *)(base + sb);
    g_eng.newAx = (double *)(base + sb + sm);
    g_eng.newAy = g_eng.newAx + MAX_BODIES;
    g_eng.virtualBytes = total;
    return TRUE;
}

DLL_EXPORT void __cdecl PhysShutdown(void)
{
    EnterCriticalSection(&g_eng.lock);
    if (g_eng.bodies) {
        VirtualFree(g_eng.bodies, 0, MEM_RELEASE);
        g_eng.bodies = NULL;
    }
    LeaveCriticalSection(&g_eng.lock);
    DeleteCriticalSection(&g_eng.lock);
}

DLL_EXPORT void __cdecl PhysResetScene(void)
{
    EnterCriticalSection(&g_eng.lock);
    BuildDefaultScene();
    LeaveCriticalSection(&g_eng.lock);
}

DLL_EXPORT void __cdecl PhysStep(double dt)
{
    EnterCriticalSection(&g_eng.lock);
    double t0 = NowMs();
    g_eng.curDt = dt;
    g_eng.stepCounter++;
    VerletSubstep(dt);
    g_eng.lastStepMs = NowMs() - t0;
    LeaveCriticalSection(&g_eng.lock);
}

DLL_EXPORT int __cdecl PhysSnapshotRead(Body *outBodies, BodyMeta *outMeta,
                                        int maxCount, SimStats *outStats)
{
    EnterCriticalSection(&g_eng.lock);
    int n = g_eng.bodyCount;
    if (n > maxCount) n = maxCount;
    if (outBodies && n > 0)
        memcpy(outBodies, g_eng.bodies, (size_t)n * sizeof(Body));
    if (outMeta && n > 0)
        memcpy(outMeta, g_eng.meta, (size_t)n * sizeof(BodyMeta));
    if (outStats) {
        outStats->activeBodies = g_eng.activeBodies;
        outStats->totalBodies = g_eng.bodyCount;
        outStats->stepCounter = g_eng.stepCounter;
        outStats->stepTimeMs = g_eng.lastStepMs;
        outStats->virtualMemBytes = g_eng.virtualBytes;
    }
    LeaveCriticalSection(&g_eng.lock);
    return n;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID reserved)
{
    (void)h; (void)reserved;
    if (reason == DLL_PROCESS_ATTACH)
        memset(&g_eng, 0, sizeof(g_eng));
    return TRUE;
}
