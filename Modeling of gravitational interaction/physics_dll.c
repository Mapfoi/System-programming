#include "common.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define DLL_EXPORT __declspec(dllexport)

// Внутренний контекст вычислительного ядра в gravity_physics.dll
typedef struct {
    Body                   *bodies;       // Выделяется через VirtualAlloc
    BodyMeta               *meta;         // Выделяется через VirtualAlloc
    double                 *newAx;        // Буфер новых ускорений X (VirtualAlloc)
    double                 *newAy;        // Буфер новых ускорений Y (VirtualAlloc)
    SIZE_T                  virtualBytes; // Общий объем выделенной страничной памяти

    int                     bodyCount;
    int                     activeBodies;
    int                     mergersCount;
    ULONGLONG               stepCounter;
    double                  simTime;

    double                  gravityG;
    double                  softening;
    double                  curDt;
    BOOL                    enableCollisions;
    BOOL                    recordTrailThisStep;

    // Многопоточный пул и синхронизация
    int                     numWorkers;
    int                     workerPriority;
    HANDLE                  workers[MAX_WORKERS];
    SYNCHRONIZATION_BARRIER barrier;
    CRITICAL_SECTION        dataLock;
    volatile LONG           stopWorkers;
    volatile LONG           nextIndexPhase1;
    volatile LONG           nextIndexPhase2;

    // Энергия и профилирование
    double                  partialK[MAX_WORKERS];
    double                  partialU[MAX_WORKERS];
    double                  kineticEnergy;
    double                  potentialEnergy;
    double                  lastStepMs;
    double                  singleThreadBaselineMs;
    double                  energyBaseline;
    LARGE_INTEGER           qpcFreq;
} PhysicsEngine;

static PhysicsEngine g_eng = {0};
static volatile LONG g_dllThreadsAttached = 0;

// Генератор псевдослучайных чисел [0..1] для пресетов
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

static float RadiusFromMass(double mass)
{
    float r = (float)(2.0 + pow(mass, 0.28) * 1.15);
    if (r < 2.0f) r = 2.0f;
    if (r > 95.0f) r = 95.0f;
    return r;
}

// Округление размера вверх до гранулярности виртуальной памяти (64 КБ, слайд 118)
static SIZE_T AlignTo64K(SIZE_T size)
{
    const SIZE_T gran = 65536;
    return (size + gran - 1) & ~(gran - 1);
}

// Запись текущей позиции тела в кольцевой буфер истории орбиты
static void PushTrailPoint(BodyMeta *m, double x, double y)
{
    LONG head = m->trailHead;
    m->trail[head].x = (float)x;
    m->trail[head].y = (float)y;
    m->trailHead = (head + 1) % TRAIL_LENGTH;
    if (m->trailCount < TRAIL_LENGTH)
        m->trailCount++;
}

// Фаза 1: параллельный расчет новых ускорений и потенциальной энергии (Velocity Verlet)
static void ExecutePhase1_Forces(int workerIdx)
{
    const int n = g_eng.bodyCount;
    const double G = g_eng.gravityG;
    const double eps2 = g_eng.softening * g_eng.softening;
    const double dt = g_eng.curDt;
    const double halfDt2 = 0.5 * dt * dt;
    Body *bodies = g_eng.bodies;
    BodyMeta *meta = g_eng.meta;
    double localU = 0.0;

    for (;;) {
        LONG start = InterlockedExchangeAdd(&g_eng.nextIndexPhase1, CHUNK_SIZE);
        if (start >= n)
            break;
        LONG end = start + CHUNK_SIZE;
        if (end > n)
            end = n;

        for (LONG i = start; i < end; ++i) {
            if (!meta[i].alive)
                continue;

            // Шаг 1 интегратора Velocity Verlet: r(t + dt) = r(t) + v(t)*dt + 0.5*a(t)*dt^2
            if (!meta[i].pinned) {
                bodies[i].x += bodies[i].vx * dt + bodies[i].ax * halfDt2;
                bodies[i].y += bodies[i].vy * dt + bodies[i].ay * halfDt2;
            }
        }
    }

    // Ждем, пока все потоки обновят координаты r(t + dt), прежде чем считать новые силы F(t + dt)
    if (g_eng.numWorkers > 1) {
        EnterSynchronizationBarrier(&g_eng.barrier, 0);
    }

    for (;;) {
        LONG start = InterlockedExchangeAdd(&g_eng.nextIndexPhase2, CHUNK_SIZE);
        if (start >= n)
            break;
        LONG end = start + CHUNK_SIZE;
        if (end > n)
            end = n;

        for (LONG i = start; i < end; ++i) {
            if (!meta[i].alive) {
                g_eng.newAx[i] = 0.0;
                g_eng.newAy[i] = 0.0;
                continue;
            }

            double xi = bodies[i].x;
            double yi = bodies[i].y;
            double mi = bodies[i].mass;
            double axi = 0.0;
            double ayi = 0.0;

            for (int j = 0; j < n; ++j) {
                if (i == j || !meta[j].alive)
                    continue;
                double dx = bodies[j].x - xi;
                double dy = bodies[j].y - yi;
                double dist2 = dx * dx + dy * dy + eps2;
                double invDist = 1.0 / sqrt(dist2);
                double invDist3 = invDist * invDist * invDist;
                double s = G * bodies[j].mass * invDist3;
                axi += dx * s;
                ayi += dy * s;

                if (j > i) {
                    localU -= G * mi * bodies[j].mass * invDist;
                }
            }

            g_eng.newAx[i] = axi;
            g_eng.newAy[i] = ayi;
        }
    }

    g_eng.partialU[workerIdx] = localU;
}

// Фаза 2: параллельное обновление скоростей v(t + dt) = v(t) + 0.5*(a(t) + a(t+dt))*dt и треков
static void ExecutePhase2_Integrate(int workerIdx)
{
    const int n = g_eng.bodyCount;
    const double halfDt = 0.5 * g_eng.curDt;
    const BOOL recTrail = g_eng.recordTrailThisStep;
    Body *bodies = g_eng.bodies;
    BodyMeta *meta = g_eng.meta;
    double localK = 0.0;

    // Равномерное разбиение диапазона по индексу потока
    int totalThreads = g_eng.numWorkers;
    if (totalThreads < 1)
        totalThreads = 1;
    int perThread = (n + totalThreads - 1) / totalThreads;
    int start = workerIdx * perThread;
    int end = start + perThread;
    if (end > n) end = n;

    for (int i = start; i < end; ++i) {
        if (!meta[i].alive)
            continue;
        if (!meta[i].pinned) {
            bodies[i].vx += (bodies[i].ax + g_eng.newAx[i]) * halfDt;
            bodies[i].vy += (bodies[i].ay + g_eng.newAy[i]) * halfDt;
            bodies[i].ax = g_eng.newAx[i];
            bodies[i].ay = g_eng.newAy[i];
        }
        double v2 = bodies[i].vx * bodies[i].vx + bodies[i].vy * bodies[i].vy;
        localK += 0.5 * bodies[i].mass * v2;

        if (recTrail) {
            PushTrailPoint(&meta[i], bodies[i].x, bodies[i].y);
        }
    }

    g_eng.partialK[workerIdx] = localK;
}

// Обработка неупругих столкновений (слияние тел с сохранением импульса и центра масс)
static void ProcessCollisionsSingleThread(void)
{
    if (!g_eng.enableCollisions)
        return;

    int n = g_eng.bodyCount;
    Body *b = g_eng.bodies;
    BodyMeta *m = g_eng.meta;

    for (int i = 0; i < n; ++i) {
        if (!m[i].alive)
            continue;
        for (int j = i + 1; j < n; ++j) {
            if (!m[j].alive)
                continue;

            double dx = b[j].x - b[i].x;
            double dy = b[j].y - b[i].y;
            double minDist = (double)(b[i].radius + b[j].radius) * 0.65;
            if (dx * dx + dy * dy <= minDist * minDist) {
                // Более массивное тело поглощает менее массивное
                int keep = (b[i].mass >= b[j].mass) ? i : j;
                int absorb = (keep == i) ? j : i;

                double m1 = b[keep].mass;
                double m2 = b[absorb].mass;
                double totalM = m1 + m2;

                b[keep].vx = (b[keep].vx * m1 + b[absorb].vx * m2) / totalM;
                b[keep].vy = (b[keep].vy * m1 + b[absorb].vy * m2) / totalM;
                b[keep].x  = (b[keep].x  * m1 + b[absorb].x  * m2) / totalM;
                b[keep].y  = (b[keep].y  * m1 + b[absorb].y  * m2) / totalM;
                b[keep].mass = totalM;
                b[keep].radius = RadiusFromMass(totalM);

                m[absorb].alive = 0;
                g_eng.activeBodies--;
                g_eng.mergersCount++;

                if (absorb == i)
                    break;
            }
        }
    }
}

// Функция рабочего потока пула (слайд 70: CreateThread + SYNCHRONIZATION_BARRIER)
static DWORD WINAPI PhysicsWorkerProc(LPVOID param)
{
    int workerIdx = (int)(INT_PTR)param;

    for (;;) {
        // Ожидание начала нового шага от координатора (барьер #1)
        EnterSynchronizationBarrier(&g_eng.barrier, 0);
        if (InterlockedCompareExchange(&g_eng.stopWorkers, 0, 0) != 0)
            break;

        // Фаза 1: сдвиг координат + барьер внутри + расчет ускорений
        ExecutePhase1_Forces(workerIdx);

        // Ожидание окончания расчета ускорений всеми потоками (барьер #3)
        EnterSynchronizationBarrier(&g_eng.barrier, 0);

        // Фаза 2: обновление скоростей и подсчет кинетической энергии
        ExecutePhase2_Integrate(workerIdx);

        // Сигнал координатору о завершении шага (барьер #4)
        EnterSynchronizationBarrier(&g_eng.barrier, 0);
    }
    return 0;
}

static void TerminateWorkerHandles(int fromIdx, int toIdx)
{
    for (int i = fromIdx; i < toIdx; ++i) {
        if (g_eng.workers[i]) {
            TerminateThread(g_eng.workers[i], 0);
            CloseHandle(g_eng.workers[i]);
            g_eng.workers[i] = NULL;
        }
    }
}

// Запуск пула из N потоков (1 поток = сам координатор, + N-1 фоновых потоков CreateThread)
static void StartWorkerPool(int numThreads, int priority)
{
    if (numThreads < 1) numThreads = 1;
    if (numThreads > MAX_WORKERS) numThreads = MAX_WORKERS;

    g_eng.workerPriority = priority;
    g_eng.stopWorkers = 0;
    g_eng.numWorkers = 1;

    memset(g_eng.workers, 0, sizeof(g_eng.workers));

    if (numThreads <= 1)
        return;

    // Потоки создаём приостановленными: иначе они успевают дойти до барьера до его инициализации.
    int i;
    for (i = 1; i < numThreads; ++i) {
        g_eng.workers[i] = CreateThread(NULL, 0, PhysicsWorkerProc, (LPVOID)(INT_PTR)i,
                                        CREATE_SUSPENDED, NULL);
        if (!g_eng.workers[i])
            break;
        SetThreadPriority(g_eng.workers[i], priority);
    }

    if (i < numThreads) {
        TerminateWorkerHandles(1, i);
        return;
    }

    if (!InitializeSynchronizationBarrier(&g_eng.barrier, (LONG)numThreads, 2000)) {
        TerminateWorkerHandles(1, numThreads);
        return;
    }

    g_eng.numWorkers = numThreads;

    for (i = 1; i < numThreads; ++i)
        ResumeThread(g_eng.workers[i]);
}

// Остановка и закрытие дескрипторов рабочих потоков
static void StopWorkerPool(void)
{
    if (g_eng.numWorkers <= 0)
        return;

    if (g_eng.numWorkers > 1) {
        InterlockedExchange(&g_eng.stopWorkers, 1);
        EnterSynchronizationBarrier(&g_eng.barrier, 0);

        WaitForMultipleObjects((DWORD)(g_eng.numWorkers - 1), &g_eng.workers[1], TRUE, INFINITE);
        for (int i = 1; i < g_eng.numWorkers; ++i) {
            if (g_eng.workers[i]) {
                CloseHandle(g_eng.workers[i]);
                g_eng.workers[i] = NULL;
            }
        }
        DeleteSynchronizationBarrier(&g_eng.barrier);
    }

    g_eng.numWorkers = 0;
}

// Начальный расчет ускорений при генерации новой сцены
static void ComputeInitialAccelerations(void)
{
    int n = g_eng.bodyCount;
    double G = g_eng.gravityG;
    double eps2 = g_eng.softening * g_eng.softening;
    Body *b = g_eng.bodies;
    BodyMeta *m = g_eng.meta;
    double totalK = 0.0, totalU = 0.0;

    for (int i = 0; i < n; ++i) {
        if (!m[i].alive) continue;
        double axi = 0.0, ayi = 0.0;
        for (int j = 0; j < n; ++j) {
            if (i == j || !m[j].alive) continue;
            double dx = b[j].x - b[i].x;
            double dy = b[j].y - b[i].y;
            double dist2 = dx * dx + dy * dy + eps2;
            double invDist = 1.0 / sqrt(dist2);
            double s = G * b[j].mass * invDist * invDist * invDist;
            axi += dx * s;
            ayi += dy * s;
            if (j > i)
                totalU -= G * b[i].mass * b[j].mass * invDist;
        }
        b[i].ax = axi;
        b[i].ay = ayi;
        totalK += 0.5 * b[i].mass * (b[i].vx * b[i].vx + b[i].vy * b[i].vy);
    }
    g_eng.kineticEnergy = totalK;
    g_eng.potentialEnergy = totalU;
}

// Вспомогательная вставка тела во внутренний массив
static int InternalAddBody(double x, double y, double vx, double vy,
                           double mass, float radius, DWORD color)
{
    if (g_eng.bodyCount >= MAX_BODIES)
        return -1;
    int idx = g_eng.bodyCount++;
    Body *b = &g_eng.bodies[idx];
    BodyMeta *m = &g_eng.meta[idx];

    memset(b, 0, sizeof(*b));
    memset(m, 0, sizeof(*m));

    b->x = x;
    b->y = y;
    b->vx = vx;
    b->vy = vy;
    b->mass = mass;
    b->radius = (radius > 0.0f) ? radius : RadiusFromMass(mass);
    b->color = color;

    m->alive = 1;
    PushTrailPoint(m, x, y);
    g_eng.activeBodies++;
    return idx;
}

// Кеплерова орбита вокруг массивного центра (Солнце): v = sqrt(G*M/r), направление ⊥ к радиусу
static int AddHeliocentricBody(double r, double mass, float rad, DWORD col,
                               double angle, double centralMass)
{
    double G = g_eng.gravityG;
    double v = sqrt(G * centralMass / r);
    double px = cos(angle) * r;
    double py = sin(angle) * r;
    double vx = -sin(angle) * v;
    double vy =  cos(angle) * v;
    return InternalAddBody(px, py, vx, vy, mass, rad, col);
}

// Спутник на круговой орбите вокруг уже добавленной планеты (скорость в системе отсчёта инерциальной)
static void AddSatellitePrograde(int parentIdx, double orbitR, double satMass, float satRad,
                                 DWORD col, unsigned int *seed)
{
    if (parentIdx < 0 || parentIdx >= g_eng.bodyCount)
        return;
    const Body *p = &g_eng.bodies[parentIdx];
    double G = g_eng.gravityG;
    double phase = RandUnit(seed) * 6.28318530718;
    double ux = cos(phase);
    double uy = sin(phase);
    double tx = -uy;
    double ty =  ux;
    double vmag = sqrt(G * p->mass / orbitR);
    InternalAddBody(p->x + orbitR * ux, p->y + orbitR * uy,
                    p->vx + tx * vmag, p->vy + ty * vmag,
                    satMass, satRad, col);
}

// Экспортируемые функции DLL
DLL_EXPORT BOOL __cdecl PhysInit(const SimConfig *cfg)
{
    InitializeCriticalSectionAndSpinCount(&g_eng.dataLock, 4000);
    QueryPerformanceFrequency(&g_eng.qpcFreq);

    // Выделение страничной виртуальной памяти (слайд 118: VirtualAlloc, MEM_RESERVE | MEM_COMMIT)
    SIZE_T szBodies = AlignTo64K(sizeof(Body) * MAX_BODIES);
    SIZE_T szMeta   = AlignTo64K(sizeof(BodyMeta) * MAX_BODIES);
    SIZE_T szAccel  = AlignTo64K(sizeof(double) * MAX_BODIES * 2);
    SIZE_T totalBytes = szBodies + szMeta + szAccel;

    BYTE *base = (BYTE *)VirtualAlloc(NULL, totalBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!base)
        return FALSE;

    g_eng.bodies = (Body *)base;
    g_eng.meta   = (BodyMeta *)(base + szBodies);
    g_eng.newAx  = (double *)(base + szBodies + szMeta);
    g_eng.newAy  = g_eng.newAx + MAX_BODIES;
    g_eng.virtualBytes = totalBytes;

    g_eng.gravityG = cfg ? cfg->gravityG : SIM_G_AU_MSUN_DAY;
    g_eng.softening = cfg ? cfg->softening : SIM_SOFTENING_AU;
    g_eng.enableCollisions = cfg ? cfg->enableCollisions : FALSE;

    int threads = cfg ? cfg->numThreads : 4;
    int prio = cfg ? cfg->threadPriority : THREAD_PRIORITY_NORMAL;
    StartWorkerPool(threads, prio);

    return TRUE;
}

DLL_EXPORT void __cdecl PhysShutdown(void)
{
    StopWorkerPool();
    if (g_eng.bodies) {
        VirtualFree(g_eng.bodies, 0, MEM_RELEASE);
        g_eng.bodies = NULL;
        g_eng.meta = NULL;
        g_eng.newAx = NULL;
        g_eng.newAy = NULL;
    }
    DeleteCriticalSection(&g_eng.dataLock);
}

DLL_EXPORT void __cdecl PhysSetThreads(int numThreads, int priority)
{
    EnterCriticalSection(&g_eng.dataLock);
    StopWorkerPool();
    StartWorkerPool(numThreads, priority);
    LeaveCriticalSection(&g_eng.dataLock);
}

DLL_EXPORT void __cdecl PhysSetParams(double gravityG, double softening, BOOL enableCollisions)
{
    EnterCriticalSection(&g_eng.dataLock);
    g_eng.gravityG = gravityG;
    g_eng.softening = softening;
    g_eng.enableCollisions = enableCollisions;
    LeaveCriticalSection(&g_eng.dataLock);
}

DLL_EXPORT int __cdecl PhysAddBody(double x, double y, double vx, double vy,
                                   double mass, float radius, DWORD color)
{
    EnterCriticalSection(&g_eng.dataLock);
    int idx = InternalAddBody(x, y, vx, vy, mass, radius, color);
    LeaveCriticalSection(&g_eng.dataLock);
    return idx;
}

DLL_EXPORT void __cdecl PhysAddCluster(double cx, double cy, int count)
{
    EnterCriticalSection(&g_eng.dataLock);
    unsigned int seed = (unsigned int)GetTickCount() ^ (unsigned int)(g_eng.bodyCount * 2654435761u);
    double G = g_eng.gravityG;
    const BOOL astro = (g_eng.gravityG < 1.0);
    const double centerMass = astro ? 1.0 : 12000.0;

    for (int i = 0; i < count && g_eng.bodyCount < MAX_BODIES; ++i) {
        double r = astro ? (0.4 + sqrt(RandUnit(&seed)) * 2.5)
                         : (40.0 + sqrt(RandUnit(&seed)) * 420.0);
        double a = RandUnit(&seed) * 6.28318530718;
        double px = cx + cos(a) * r;
        double py = cy + sin(a) * r;
        double soften = astro ? 0.02 : 15.0;
        double v = sqrt(G * centerMass / (r + soften)) * (0.95 + RandUnit(&seed) * 0.1);
        double vx = -sin(a) * v;
        double vy =  cos(a) * v;
        double m  = astro ? (1e-9 + RandUnit(&seed) * 3e-7) : (0.5 + RandUnit(&seed) * 2.5);
        BYTE cR = (BYTE)(110 + (int)(RandUnit(&seed) * 140));
        BYTE cG = (BYTE)(150 + (int)(RandUnit(&seed) * 100));
        BYTE cB = (BYTE)(200 + (int)(RandUnit(&seed) * 55));
        InternalAddBody(px, py, vx, vy, m, RadiusFromMass(m), RGB(cR, cG, cB));
    }
    ComputeInitialAccelerations();
    LeaveCriticalSection(&g_eng.dataLock);
}

DLL_EXPORT void __cdecl PhysResetPreset(int preset, int customCount)
{
    EnterCriticalSection(&g_eng.dataLock);

    g_eng.bodyCount = 0;
    g_eng.activeBodies = 0;
    g_eng.mergersCount = 0;
    g_eng.stepCounter = 0;
    g_eng.simTime = 0.0;
    g_eng.singleThreadBaselineMs = 0.0;

    unsigned int seed = (unsigned int)GetTickCount() ^ 0xA5A55A5Au;
    double G = g_eng.gravityG;

    if (preset == PRESET_SOLAR_SYSTEM) {
        // 2D копланарная модель: длина AU, масса M☉, время сутки, G = 4π²/365.25²
        const double sunMass = 1.0;
        int sunIdx = InternalAddBody(0.0, 0.0, 0.0, 0.0, sunMass, 0.22f, RGB(255, 215, 60));

        const struct {
            double r_au;
            double mass_msun;
            float  vis_rad_au;
            DWORD  col;
        } planets[] = {
            { 0.387, 1.66e-7, 0.028f, RGB(180, 175, 165) },
            { 0.723, 2.45e-6, 0.032f, RGB(235, 190, 110) },
            { 1.000, 3.00e-6, 0.034f, RGB( 70, 160, 255) },
            { 1.524, 3.23e-7, 0.030f, RGB(235,  95,  65) },
            { 5.203, 9.55e-4, 0.055f, RGB(230, 170, 120) },
            { 9.537, 2.86e-4, 0.048f, RGB(220, 200, 135) },
            { 19.19, 4.37e-5, 0.040f, RGB(110, 215, 235) },
            { 30.07, 5.15e-5, 0.038f, RGB( 75, 120, 245) }
        };

        const double layoutTurn = RandUnit(&seed) * 6.28318530718;
        int earthIdx = -1;

        for (int i = 0; i < 8; ++i) {
            double angle = layoutTurn + (double)i * (6.28318530718 / 8.0);
            int idx = AddHeliocentricBody(planets[i].r_au, planets[i].mass_msun,
                                          planets[i].vis_rad_au, planets[i].col,
                                          angle, sunMass + planets[i].mass_msun);
            if (i == 2)
                earthIdx = idx;
        }

        // Луна: ~0.00257 AU, ~1.2% массы Земли (в M☉)
        AddSatellitePrograde(earthIdx, 0.00257, 3.7e-8, 0.014f, RGB(220, 225, 235), &seed);

        // Солнце в покое в барицентрической СО: v☉ = −Σ mᵢvᵢ (M☉ = 1)
        {
            double Px = 0.0, Py = 0.0;
            for (int i = 0; i < g_eng.bodyCount; ++i) {
                if (!g_eng.meta[i].alive || i == sunIdx)
                    continue;
                Px += g_eng.bodies[i].mass * g_eng.bodies[i].vx;
                Py += g_eng.bodies[i].mass * g_eng.bodies[i].vy;
            }
            g_eng.bodies[sunIdx].vx = -Px;
            g_eng.bodies[sunIdx].vy = -Py;
        }
        (void)customCount;
    } else if (preset == PRESET_GALAXY_CLASH) {
        const double coreMass = 12000.0;
        const double gx[2]   = { -270.0,  270.0 };
        const double gy[2]   = {  -90.0,   90.0 };
        const double gvx[2]  = {   22.0,  -22.0 };
        const double gvy[2]  = {   14.0,  -14.0 };
        const double spin[2] = {    1.0,   -0.85 };

        for (int g = 0; g < 2; ++g) {
            DWORD coreCol = (g == 0) ? RGB(255, 230, 140) : RGB(255, 160, 220);
            InternalAddBody(gx[g], gy[g], gvx[g], gvy[g], coreMass, 14.0f, coreCol);

            int stars = 550;
            for (int s = 0; s < stars; ++s) {
                double u = RandUnit(&seed);
                double r = 28.0 + u * 230.0;
                int arm = s % 2;
                double baseAngle = arm * 3.14159265 + (r * 0.022 * spin[g]) + (RandUnit(&seed) - 0.5) * 0.55;
                double px = gx[g] + cos(baseAngle) * r;
                double py = gy[g] + sin(baseAngle) * r;

                double enclosedM = coreMass + s * 1.2;
                double vOrb = sqrt(G * enclosedM / sqrt(r * r + 25.0));
                double vx = gvx[g] - sin(baseAngle) * vOrb * spin[g];
                double vy = gvy[g] + cos(baseAngle) * vOrb * spin[g];

                DWORD starCol;
                if (g == 0) {
                    int b = 210 + (int)(RandUnit(&seed) * 45);
                    int gr = 140 + (int)((1.0 - u) * 90);
                    starCol = RGB(90, gr, b);
                } else {
                    int rCol = 220 + (int)(RandUnit(&seed) * 35);
                    int gr = 100 + (int)((1.0 - u) * 100);
                    starCol = RGB(rCol, gr, 165);
                }
                InternalAddBody(px, py, vx, vy, 1.2 + RandUnit(&seed) * 1.8, 2.3f, starCol);
            }
        }
    } else if (preset == PRESET_FIGURE_EIGHT) {
        double L = 240.0;
        double M = 4000.0;
        double vScale = sqrt(G * M / L);

        double x1 = 0.97000436 * L;
        double y1 = -0.24308753 * L;
        double vx3 = -0.93240737 * vScale;
        double vy3 = -0.86473146 * vScale;

        InternalAddBody( x1,  y1, -0.5 * vx3, -0.5 * vy3, M, 12.0f, RGB(255, 110,  90));
        InternalAddBody(-x1, -y1, -0.5 * vx3, -0.5 * vy3, M, 12.0f, RGB( 80, 195, 255));
        InternalAddBody(0.0, 0.0,        vx3,        vy3, M, 12.0f, RGB(140, 255, 130));

        int dustCount = 650;
        double totalCenterM = 3.0 * M;
        for (int i = 0; i < dustCount; ++i) {
            double r = 390.0 + (RandUnit(&seed) - 0.5) * 140.0;
            double a = (double)i * 6.28318530718 / dustCount + (RandUnit(&seed) - 0.5) * 0.05;
            double v = sqrt(G * totalCenterM / r);
            InternalAddBody(cos(a) * r, sin(a) * r, -sin(a) * v, cos(a) * v,
                            0.05, 2.0f, RGB(145, 155, 185));
        }
    } else {
        int count = (customCount > 0) ? customCount : 1100;
        InternalAddBody(0.0, 0.0, 0.0, 0.0, 6500.0, 13.0f, RGB(255, 225, 120));

        for (int i = 0; i < count; ++i) {
            double r = 35.0 + sqrt(RandUnit(&seed)) * 460.0;
            double a = RandUnit(&seed) * 6.28318530718;
            double px = cos(a) * r;
            double py = sin(a) * r;
            double v = sqrt(G * (6500.0 + r * 4.5) / r) * (0.85 + RandUnit(&seed) * 0.3);
            double vx = -sin(a) * v;
            double vy =  cos(a) * v;
            double m = 1.5 + RandUnit(&seed) * 8.5;
            BYTE cR = (BYTE)(110 + (int)(RandUnit(&seed) * 140));
            BYTE cG = (BYTE)(130 + (int)(RandUnit(&seed) * 110));
            BYTE cB = (BYTE)(180 + (int)(RandUnit(&seed) * 75));
            InternalAddBody(px, py, vx, vy, m, RadiusFromMass(m), RGB(cR, cG, cB));
        }
    }

    if (preset != PRESET_SOLAR_SYSTEM) {
        double sumM = 0.0, sumPx = 0.0, sumPy = 0.0;
        for (int i = 0; i < g_eng.bodyCount; ++i) {
            if (!g_eng.meta[i].alive) continue;
            sumM  += g_eng.bodies[i].mass;
            sumPx += g_eng.bodies[i].mass * g_eng.bodies[i].vx;
            sumPy += g_eng.bodies[i].mass * g_eng.bodies[i].vy;
        }
        if (sumM > 0.0) {
            double vcmX = sumPx / sumM;
            double vcmY = sumPy / sumM;
            for (int i = 0; i < g_eng.bodyCount; ++i) {
                g_eng.bodies[i].vx -= vcmX;
                g_eng.bodies[i].vy -= vcmY;
            }
        }
    }

    ComputeInitialAccelerations();
    g_eng.energyBaseline = g_eng.kineticEnergy + g_eng.potentialEnergy;
    LeaveCriticalSection(&g_eng.dataLock);
}

DLL_EXPORT void __cdecl PhysStep(double dt, int subSteps)
{
    if (subSteps < 1) subSteps = 1;
    if (subSteps > SIM_SOLAR_MAX_SUBSTEPS) subSteps = SIM_SOLAR_MAX_SUBSTEPS;

    EnterCriticalSection(&g_eng.dataLock);

    double tStart = NowMs();
    double subDt = dt / (double)subSteps;
    g_eng.curDt = subDt;

    for (int s = 0; s < subSteps; ++s) {
        g_eng.stepCounter++;
        g_eng.recordTrailThisStep = (s == subSteps - 1) && ((g_eng.stepCounter % 2) == 0);
        g_eng.nextIndexPhase1 = 0;
        g_eng.nextIndexPhase2 = 0;

        // Барьер #1: пробуждаем фоновые рабочие потоки
        if (g_eng.numWorkers > 1)
            EnterSynchronizationBarrier(&g_eng.barrier, 0);

        // Поток 0 также участвует в вычислениях наравне с фоновыми потоками
        ExecutePhase1_Forces(0);

        // Барьер #3: все потоки завершили расчет ускорений
        if (g_eng.numWorkers > 1)
            EnterSynchronizationBarrier(&g_eng.barrier, 0);

        ExecutePhase2_Integrate(0);

        // Барьер #4: все потоки завершили обновление скоростей
        if (g_eng.numWorkers > 1)
            EnterSynchronizationBarrier(&g_eng.barrier, 0);

        // Неупругие столкновения (выполняются координатором между шагами)
        if (g_eng.enableCollisions) {
            ProcessCollisionsSingleThread();
        }
    }

    // Суммирование полной механической энергии по всем рабочим потокам
    double sumK = 0.0, sumU = 0.0;
    for (int w = 0; w < g_eng.numWorkers; ++w) {
        sumK += g_eng.partialK[w];
        sumU += g_eng.partialU[w];
    }
    g_eng.kineticEnergy = sumK;
    g_eng.potentialEnergy = sumU;

    double elapsed = NowMs() - tStart;
    // Экспоненциальное сглаживание времени шага для стабильного отображения в HUD
    if (g_eng.lastStepMs <= 0.0001)
        g_eng.lastStepMs = elapsed;
    else
        g_eng.lastStepMs = g_eng.lastStepMs * 0.85 + elapsed * 0.15;

    if (g_eng.numWorkers == 1) {
        g_eng.singleThreadBaselineMs = g_eng.lastStepMs;
    } else if (g_eng.singleThreadBaselineMs <= 0.0001) {
        g_eng.singleThreadBaselineMs = elapsed * (double)g_eng.numWorkers * 0.72;
    }

    g_eng.simTime += dt;

    LeaveCriticalSection(&g_eng.dataLock);
}

DLL_EXPORT int __cdecl PhysSnapshotRead(Body *outBodies, BodyMeta *outMeta,
                                        int maxCount, SimStats *outStats)
{
    EnterCriticalSection(&g_eng.dataLock);

    int count = g_eng.bodyCount;
    if (count > maxCount)
        count = maxCount;

    if (outBodies && count > 0)
        memcpy(outBodies, g_eng.bodies, (size_t)count * sizeof(Body));
    if (outMeta && count > 0)
        memcpy(outMeta, g_eng.meta, (size_t)count * sizeof(BodyMeta));

    if (outStats) {
        outStats->activeBodies       = g_eng.activeBodies;
        outStats->totalBodies        = g_eng.bodyCount;
        outStats->mergersCount       = g_eng.mergersCount;
        outStats->workerThreads      = g_eng.numWorkers;
        outStats->workerPriority     = g_eng.workerPriority;
        outStats->stepTimeMs         = g_eng.lastStepMs;
        outStats->singleThreadMs     = g_eng.singleThreadBaselineMs;
        outStats->kineticEnergy      = g_eng.kineticEnergy;
        outStats->potentialEnergy    = g_eng.potentialEnergy;
        outStats->totalEnergy        = g_eng.kineticEnergy + g_eng.potentialEnergy;
        outStats->stepCounter        = g_eng.stepCounter;
        outStats->simTime            = g_eng.simTime;
        if (fabs(g_eng.energyBaseline) > 1e-30) {
            double eNow = g_eng.kineticEnergy + g_eng.potentialEnergy;
            outStats->energyDriftRel = (eNow - g_eng.energyBaseline) / fabs(g_eng.energyBaseline);
        } else {
            outStats->energyDriftRel = 0.0;
        }
        outStats->virtualMemBytes    = g_eng.virtualBytes;
        outStats->dllThreadsAttached = InterlockedCompareExchange(&g_dllThreadsAttached, 0, 0);
    }

    LeaveCriticalSection(&g_eng.dataLock);
    return count;
}

DLL_EXPORT int __cdecl PhysLoadBodies(const Body *inBodies, const BodyMeta *inMeta,
                                      int count, ULONGLONG stepCounter)
{
    if (!inBodies || !inMeta || count < 0)
        return 0;
    if (count > MAX_BODIES)
        count = MAX_BODIES;

    EnterCriticalSection(&g_eng.dataLock);

    memcpy(g_eng.bodies, inBodies, (size_t)count * sizeof(Body));
    memcpy(g_eng.meta, inMeta, (size_t)count * sizeof(BodyMeta));
    g_eng.bodyCount = count;
    g_eng.stepCounter = stepCounter;
    g_eng.simTime = 0.0;
    g_eng.mergersCount = 0;

    int alive = 0;
    for (int i = 0; i < count; ++i) {
        if (g_eng.meta[i].alive)
            alive++;
    }
    g_eng.activeBodies = alive;
    ComputeInitialAccelerations();

    LeaveCriticalSection(&g_eng.dataLock);
    return count;
}

// Точка входа-выхода динамической библиотеки (слайд 52: DllMain)
BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
    (void)hinstDLL;
    (void)lpvReserved;

    switch (fdwReason) {
    case DLL_PROCESS_ATTACH:
        InterlockedIncrement(&g_dllThreadsAttached);
        break;
    case DLL_THREAD_ATTACH:
        InterlockedIncrement(&g_dllThreadsAttached);
        break;
    case DLL_THREAD_DETACH:
        InterlockedDecrement(&g_dllThreadsAttached);
        break;
    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}
