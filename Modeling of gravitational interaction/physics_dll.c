/*
 * physics_dll.c — вычислительное ядро (gravity_physics.dll)
 *
 * Все экспорты обёрнуты в EnterCriticalSection(&g_eng.dataLock), кроме того что воркеры
 * работают только пока lock держит главный поток внутри PhysStep — SnapshotRead тоже под lock.
 *
 * PhysResetPreset: заполняет bodies[], вызывает ComputeInitialAccelerations, energyBaseline.
 * PhysStep(dt, subSteps): dt делится на subSteps → curDt; цикл subSteps × (фаза1+фаза2[+столкновения]).
 * PhysLoadBodies: memcpy из снимка + пересчёт ускорений (после F9 в exe).
 *
 * Параллелизм: numWorkers потоков на барьере; поток 0 = тот, кто вызвал PhysStep из exe.
 * CREATE_SUSPENDED при CreateThread — чтобы InitializeSynchronizationBarrier успел до первого Enter.
 *
 * Читать после common.h; до main_win.c.
 */

#include "common.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define DLL_EXPORT __declspec(dllexport)

/* ========== Глобальное состояние движка ========== */
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
    *seed = (*seed * 1664525u + 1013904223u); // шаг LCG, обновляет seed на месте
    return (double)(*seed & 0x00FFFFFFu) / (double)0x01000000u; // 24 бита → дробь [0,1)
}

static double NowMs(void)
{
    LARGE_INTEGER t; // текущий тик счётчика производительности
    if (!g_eng.qpcFreq.QuadPart) // ленивая инициализация частоты QPC
        QueryPerformanceFrequency(&g_eng.qpcFreq); // тиков в секунду
    QueryPerformanceCounter(&t); // снять значение счётчика
    return (double)t.QuadPart * 1000.0 / (double)g_eng.qpcFreq.QuadPart; // перевод в миллисекунды
}

static float RadiusFromMass(double mass)
{
    float r = (float)(2.0 + pow(mass, 0.28) * 1.15); // радиус от массы (демо-масштаб)
    if (r < 2.0f) r = 2.0f; // не рисовать слишком мелкими
    if (r > 95.0f) r = 95.0f; // ограничить гигантами
    return r; // визуальный радиус для GDI
}

// Округление размера вверх до гранулярности виртуальной памяти (64 КБ, слайд 118)
static SIZE_T AlignTo64K(SIZE_T size)
{
    const SIZE_T gran = 65536; // 64 КБ — гранула VirtualAlloc
    return (size + gran - 1) & ~(gran - 1); // округление размера региона вверх
}

// Кольцевой буфер трека: trailHead — слот следующей записи; trailCount растёт до TRAIL_LENGTH
static void PushTrailPoint(BodyMeta *m, double x, double y)
{
    LONG head = m->trailHead; // куда писать в кольцевом буфере
    m->trail[head].x = (float)x; // координата X следа
    m->trail[head].y = (float)y; // координата Y следа
    m->trailHead = (head + 1) % TRAIL_LENGTH; // следующий слот (по кругу)
    if (m->trailCount < TRAIL_LENGTH) // пока не набрали TRAIL_LENGTH точек
        m->trailCount++; // увеличить длину валидной истории
}

/* ========== Интегратор: один подшаг Verlet (фазы 1 и 2) ========== */
/*
 * Velocity Verlet для пары (r, v) при фиксированном шаге curDt:
 *
 *   1) r' = r + v*dt + 0.5*a*dt²          (сначала для всех тел)
 *   2) вычислить a' из r' (силы от всех j: F_ij ∝ m_j * (r_j-r_i) / |r_ij|³, a_i = Σ F_ij/m_i)
 *   3) v' = v + 0.5*(a + a')*dt,  a := a'
 *
 * Смягчение: |r_ij|² заменяется на |r_ij|² + ε² — убирает бесконечность при r→0 и стабилизирует Луну.
 *
 * Параллелизм: индексы тел раздаются через InterlockedExchangeAdd(&nextIndexPhase1, CHUNK_SIZE) —
 * каждый поток берёт следующий кусок [start..end). После сдвига r ОБЯЗАТЕЛЕН барьер: иначе поток A
 * читает r_j, пока поток B ещё не дописал r_j. Вторая фаза сил использует nextIndexPhase2 так же.
 * Фаза 2 (скорости) режет массив статически: workerIdx получает свой диапазон i.
 */
// Фаза 1: параллельный расчет новых ускорений и потенциальной энергии (Velocity Verlet)
static void ExecutePhase1_Forces(int workerIdx)
{
    const int n = g_eng.bodyCount; // число слотов тел в массиве
    const double G = g_eng.gravityG; // гравитационная постоянная сцены
    const double eps2 = g_eng.softening * g_eng.softening; // ε² для сглаживания расстояния
    const double dt = g_eng.curDt; // шаг интегратора текущего подшага
    const double halfDt2 = 0.5 * dt * dt; // 0.5·dt² для члена ускорения в r
    Body *bodies = g_eng.bodies; // SoA: координаты, скорости, ускорения
    BodyMeta *meta = g_eng.meta; // флаги alive/pinned и следы
    double localU = 0.0; // локальный вклад в потенциальную энергию потока

    for (;;) { // динамическая раздача чанков фазы «сдвиг r»
        LONG start = InterlockedExchangeAdd(&g_eng.nextIndexPhase1, CHUNK_SIZE); // атомарно взять следующий кусок индексов
        if (start >= n) // чанки закончились — выходим из цикла раздачи
            break;
        LONG end = start + CHUNK_SIZE; // правый конец чанка (не включительно)
        if (end > n) // обрезать последний чанк по n
            end = n;

        for (LONG i = start; i < end; ++i) { // обработать все индексы в [start, end)
            if (!meta[i].alive) // мёртвое тело не двигаем
                continue;

            if (!meta[i].pinned) { // закреплённое (если есть) остаётся на месте
                bodies[i].x += bodies[i].vx * dt + bodies[i].ax * halfDt2; // Verlet: x += v·dt + 0.5·a·dt²
                bodies[i].y += bodies[i].vy * dt + bodies[i].ay * halfDt2; // то же по Y
            }
        }
    }

    if (g_eng.numWorkers > 1) { // при одном потоке барьер не нужен
        EnterSynchronizationBarrier(&g_eng.barrier, 0); // все потоки видят обновлённые r перед силами
    }

    for (;;) { // вторая раздача: суммирование ускорений и U
        LONG start = InterlockedExchangeAdd(&g_eng.nextIndexPhase2, CHUNK_SIZE); // счётчик для фазы сил
        if (start >= n)
            break;
        LONG end = start + CHUNK_SIZE;
        if (end > n)
            end = n;

        for (LONG i = start; i < end; ++i) {
            if (!meta[i].alive) { // для удалённых тел ускорение ноль
                g_eng.newAx[i] = 0.0;
                g_eng.newAy[i] = 0.0;
                continue;
            }

            double xi = bodies[i].x; // позиция i после первого полушага
            double yi = bodies[i].y;
            double mi = bodies[i].mass; // масса i для пары в U
            double axi = 0.0; // накапливаем a_x от всех j
            double ayi = 0.0; // накапливаем a_y от всех j

            for (int j = 0; j < n; ++j) { // полный O(n²) по соседям
                if (i == j || !meta[j].alive) // не тянем себя и мёртвых
                    continue;
                double dx = bodies[j].x - xi; // вектор от i к j
                double dy = bodies[j].y - yi;
                double dist2 = dx * dx + dy * dy + eps2; // |r|² + ε²
                double invDist = 1.0 / sqrt(dist2); // 1/|r|
                double invDist3 = invDist * invDist * invDist; // 1/|r|³
                double s = G * bodies[j].mass * invDist3; // масштаб силы на единицу dx,dy
                axi += dx * s; // проекция ускорения на X
                ayi += dy * s; // проекция ускорения на Y

                if (j > i) { // каждую пару (i,j) учитываем в U один раз
                    localU -= G * mi * bodies[j].mass * invDist; // U_ij = -G m_i m_j / r
                }
            }

            g_eng.newAx[i] = axi; // записать новое ускорение для фазы 2
            g_eng.newAy[i] = ayi;
        }
    }

    g_eng.partialU[workerIdx] = localU; // сложить partialU на главном после барьера
}

// Фаза 2: параллельное обновление скоростей v(t + dt) = v(t) + 0.5*(a(t) + a(t+dt))*dt и треков
static void ExecutePhase2_Integrate(int workerIdx)
{
    const int n = g_eng.bodyCount; // размер массива тел
    const double halfDt = 0.5 * g_eng.curDt; // половина шага для v = v + 0.5(a+a')dt
    const BOOL recTrail = g_eng.recordTrailThisStep; // писать след только на некоторых кадрах
    Body *bodies = g_eng.bodies;
    BodyMeta *meta = g_eng.meta;
    double localK = 0.0; // локальная кинетическая энергия потока

    int totalThreads = g_eng.numWorkers; // число воркеров пула
    if (totalThreads < 1)
        totalThreads = 1; // защита от нуля
    int perThread = (n + totalThreads - 1) / totalThreads; // ceil(n / threads)
    int start = workerIdx * perThread; // первый индекс этого воркера
    int end = start + perThread; // конец диапазона (может быть > n)
    if (end > n) end = n; // обрезка хвоста

    for (int i = start; i < end; ++i) { // статическое разбиение без Interlocked
        if (!meta[i].alive)
            continue;
        if (!meta[i].pinned) {
            bodies[i].vx += (bodies[i].ax + g_eng.newAx[i]) * halfDt; // Verlet: обновить v
            bodies[i].vy += (bodies[i].ay + g_eng.newAy[i]) * halfDt;
            bodies[i].ax = g_eng.newAx[i]; // a(t+dt) становится a для следующего шага
            bodies[i].ay = g_eng.newAy[i];
        }
        double v2 = bodies[i].vx * bodies[i].vx + bodies[i].vy * bodies[i].vy; // |v|²
        localK += 0.5 * bodies[i].mass * v2; // K_i = ½ m v²

        if (recTrail) {
            PushTrailPoint(&meta[i], bodies[i].x, bodies[i].y); // точка следа в кольце
        }
    }

    g_eng.partialK[workerIdx] = localK; // суммирование K на главном потоке
}

/*
 * Столкновения: O(n²) только на главном потоке после барьера (без гонок с воркерами).
 * При пересечении радиусов — слияние в более массивное тело; v и r центра масс, absorb помечается !alive.
 */
static void ProcessCollisionsSingleThread(void)
{
    if (!g_eng.enableCollisions) // столкновения выключены в настройках
        return;

    int n = g_eng.bodyCount; // перебор всех пар (i,j), i<j
    Body *b = g_eng.bodies;
    BodyMeta *m = g_eng.meta;

    for (int i = 0; i < n; ++i) {
        if (!m[i].alive) // пропуск поглощённых
            continue;
        for (int j = i + 1; j < n; ++j) {
            if (!m[j].alive)
                continue;

            double dx = b[j].x - b[i].x; // расстояние между центрами
            double dy = b[j].y - b[i].y;
            double minDist = (double)(b[i].radius + b[j].radius) * 0.65; // порог слияния (чуть меньше суммы радиусов)
            if (dx * dx + dy * dy <= minDist * minDist) { // пересечение «шаров»
                int keep = (b[i].mass >= b[j].mass) ? i : j; // выживает более тяжёлое
                int absorb = (keep == i) ? j : i; // индекс поглощаемого

                double m1 = b[keep].mass; // масса оставшегося
                double m2 = b[absorb].mass; // масса жертвы
                double totalM = m1 + m2; // суммарная масса после слияния

                b[keep].vx = (b[keep].vx * m1 + b[absorb].vx * m2) / totalM; // сохранение импульса по X
                b[keep].vy = (b[keep].vy * m1 + b[absorb].vy * m2) / totalM; // по Y
                b[keep].x  = (b[keep].x  * m1 + b[absorb].x  * m2) / totalM; // центр масс X
                b[keep].y  = (b[keep].y  * m1 + b[absorb].y  * m2) / totalM; // центр масс Y
                b[keep].mass = totalM; // новая масса
                b[keep].radius = RadiusFromMass(totalM); // пересчёт визуального радиуса

                m[absorb].alive = 0; // второе тело исчезает из симуляции
                g_eng.activeBodies--; // счётчик живых для HUD
                g_eng.mergersCount++; // статистика слияний

                if (absorb == i) // если поглотили i, внутренний j-цикл для i бессмысленен
                    break;
            }
        }
    }
}

/* ========== Пул потоков (CreateThread + барьеры) ========== */
// Функция рабочего потока пула (слайд 70: CreateThread + SYNCHRONIZATION_BARRIER)
static DWORD WINAPI PhysicsWorkerProc(LPVOID param)
{
    int workerIdx = (int)(INT_PTR)param; // 1..N-1; поток 0 — координатор в PhysStep

    for (;;) { // бесконечный цикл воркера до stopWorkers
        EnterSynchronizationBarrier(&g_eng.barrier, 0); // синхронный старт подшага с координатором
        if (InterlockedCompareExchange(&g_eng.stopWorkers, 0, 0) != 0) // чтение флага остановки без изменения
            break; // выход из потока при Shutdown

        ExecutePhase1_Forces(workerIdx); // Verlet: r, затем a' и partialU

        EnterSynchronizationBarrier(&g_eng.barrier, 0); // все закончили фазу 1 до фазы 2

        ExecutePhase2_Integrate(workerIdx); // v, a←a', partialK, следы

        EnterSynchronizationBarrier(&g_eng.barrier, 0); // координатор может суммировать энергии и столкновения
    }
    return 0; // код завершения потока
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

/*
 * После сборки пресета: заполняем ax/ay и energyBaseline для HUD (дрейф E).
 * Логика сил идентична фазе 1, но без сдвига r (только один проход i,j).
 */
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

/*
 * Добавление тела в конец массива (индекс = bodyCount до инкремента).
 * ax/ay обнуляются — вызывающий код должен вызвать ComputeInitialAccelerations или следующий PhysStep.
 * PhysAddBody (экспорт) оборачивает это в dataLock.
 */
static int InternalAddBody(double x, double y, double vx, double vy,
                           double mass, float radius, DWORD color)
{
    if (g_eng.bodyCount >= MAX_BODIES) // массив заполнен
        return -1;
    int idx = g_eng.bodyCount++; // новый индекс и увеличение счётчика
    Body *b = &g_eng.bodies[idx]; // указатель на слот Body
    BodyMeta *m = &g_eng.meta[idx]; // метаданные того же индекса

    memset(b, 0, sizeof(*b)); // обнулить физику (ax/ay тоже)
    memset(m, 0, sizeof(*m)); // сброс trail и флагов

    b->x = x; // начальная позиция
    b->y = y;
    b->vx = vx; // начальная скорость
    b->vy = vy;
    b->mass = mass; // масса в единицах сцены
    b->radius = (radius > 0.0f) ? radius : RadiusFromMass(mass); // явный радиус или из mass
    b->color = color; // COLORREF для GDI

    m->alive = 1; // тело участвует в интеграторе
    PushTrailPoint(m, x, y); // первая точка следа
    g_eng.activeBodies++; // для статистики
    return idx; // индекс добавленного тела
}

// Начальные условия: планета на окружности r под углом angle; v = sqrt(G*M/r) по касательной
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

// Луна: позиция = позиция Земли + orbitR * unit; скорость = v_Земли + v_орбиты вокруг Земли (про grad)
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
    double tx = -uy;  /* касательная к орбите спутника */
    double ty =  ux;
    double vmag = sqrt(G * p->mass / orbitR);
    InternalAddBody(p->x + orbitR * ux, p->y + orbitR * uy,
                    p->vx + tx * vmag, p->vy + ty * vmag,
                    satMass, satRad, col);
}

/* ========== Экспорт DLL (вызывается из gravity_sim.exe) ========== */

// PhysInit: VirtualAlloc под тела, CRITICAL_SECTION, запуск пула потоков
DLL_EXPORT BOOL __cdecl PhysInit(const SimConfig *cfg)
{
    InitializeCriticalSectionAndSpinCount(&g_eng.dataLock, 4000);
    QueryPerformanceFrequency(&g_eng.qpcFreq);

    /*
     * Один вызов VirtualAlloc на весь пул: [Body×MAX][Meta×MAX][newAx×MAX][newAy×MAX].
     * AlignTo64K — размер округляется до гранулы резервирования ВМ (64 КБ), как в лекции.
     * newAx/newAy — отдельные массивы, чтобы фаза 1 писала ускорения без гонки с чтением ax в теле.
     */
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

// PhysResetPreset: очистка сцены и сборка одного из четырёх пресетов (см. ветки if ниже)
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
        /* Пресет 1: кеплеровские круговые орбиты, барицентрический импульс на Солнце,
         * Луна — спутник Земли (AddSatellitePrograde). Единицы: AU, M☉, сутки. */
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

        /* Импульс планет не нулевой (разные орбиты). Чтобы Σ m v = 0, Солнцу задаём v = −Σ mᵢvᵢ (M☉=1) */
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

// PhysStep: subSteps раз повторить барьер → фаза1 → барьер → фаза2; dt делится на subSteps
DLL_EXPORT void __cdecl PhysStep(double dt, int subSteps)
{
    if (subSteps < 1) subSteps = 1; // минимум один подшаг
    if (subSteps > SIM_SOLAR_MAX_SUBSTEPS) subSteps = SIM_SOLAR_MAX_SUBSTEPS; // потолок для СС

    EnterCriticalSection(&g_eng.dataLock); // эксклюзивный доступ к g_eng

    double tStart = NowMs(); // старт таймера шага
    double subDt = dt / (double)subSteps; // dt одного подшага Verlet
    g_eng.curDt = subDt; // глобально для воркеров

    for (int s = 0; s < subSteps; ++s) { // цикл подшагов за один кадр
        g_eng.stepCounter++; // счётчик интегратора +1
        g_eng.recordTrailThisStep = (s == subSteps - 1) && ((g_eng.stepCounter % 2) == 0); // след на последнем sub и чётном шаге
        g_eng.nextIndexPhase1 = 0; // сброс атомарной раздачи r
        g_eng.nextIndexPhase2 = 0; // сброс раздачи сил

        /*
         * Схема барьеров на один подшаг (numWorkers = N):
         *   Главный поток входит в барьер #1 → все N потоков просыпаются и вызывают ExecutePhase1_Forces.
         *   Внутри фазы 1 есть ещё барьер между «сдвиг r» и «счёт сил».
         *   Барьер #3 — все закончили фазу 1; затем ExecutePhase2_Integrate.
         *   Барьер #4 — готовы к следующему подшагу или к столкновениям.
         * Воркеры крутятся в PhysicsWorkerProc в бесконечном цикле с теми же барьерами.
         */
        if (g_eng.numWorkers > 1)
            EnterSynchronizationBarrier(&g_eng.barrier, 0); // барьер #1: старт подшага

        ExecutePhase1_Forces(0); // координатор — workerIdx 0

        if (g_eng.numWorkers > 1)
            EnterSynchronizationBarrier(&g_eng.barrier, 0); // барьер #3: после фазы 1

        ExecutePhase2_Integrate(0); // координатор — фаза 2

        if (g_eng.numWorkers > 1)
            EnterSynchronizationBarrier(&g_eng.barrier, 0); // барьер #4: конец подшага

        if (g_eng.enableCollisions) { // слияния только на главном потоке
            ProcessCollisionsSingleThread();
        }
    }

    double sumK = 0.0, sumU = 0.0; // сумма partialK/U по воркерам
    for (int w = 0; w < g_eng.numWorkers; ++w) {
        sumK += g_eng.partialK[w];
        sumU += g_eng.partialU[w];
    }
    g_eng.kineticEnergy = sumK; // обновить K для SnapshotRead
    g_eng.potentialEnergy = sumU; // обновить U

    double elapsed = NowMs() - tStart; // длительность PhysStep в мс
    if (g_eng.lastStepMs <= 0.0001)
        g_eng.lastStepMs = elapsed;
    else
        g_eng.lastStepMs = g_eng.lastStepMs * 0.85 + elapsed * 0.15; // сглаживание для HUD

    if (g_eng.numWorkers == 1) {
        g_eng.singleThreadBaselineMs = g_eng.lastStepMs;
    } else if (g_eng.singleThreadBaselineMs <= 0.0001) {
        g_eng.singleThreadBaselineMs = elapsed * (double)g_eng.numWorkers * 0.72; // оценка «как 1 поток»
    }

    g_eng.simTime += dt; // модельное время

    LeaveCriticalSection(&g_eng.dataLock);
}

// Копия состояния под CRITICAL_SECTION — вызывается из UI-потока после PhysStep
DLL_EXPORT int __cdecl PhysSnapshotRead(Body *outBodies, BodyMeta *outMeta,
                                        int maxCount, SimStats *outStats)
{
    EnterCriticalSection(&g_eng.dataLock); // согласованная копия для UI-потока

    int count = g_eng.bodyCount; // сколько слотов реально занято
    if (count > maxCount)
        count = maxCount; // не переполнить буфер рендера exe

    if (outBodies && count > 0)
        memcpy(outBodies, g_eng.bodies, (size_t)count * sizeof(Body)); // снимок массива Body
    if (outMeta && count > 0)
        memcpy(outMeta, g_eng.meta, (size_t)count * sizeof(BodyMeta)); // снимок метаданных

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
        InterlockedIncrement(&g_dllThreadsAttached); // процесс загрузил DLL
        break;
    case DLL_THREAD_ATTACH:
        InterlockedIncrement(&g_dllThreadsAttached); // новый поток в процессе с DLL
        break;
    case DLL_THREAD_DETACH:
        InterlockedDecrement(&g_dllThreadsAttached); // поток завершился
        break;
    case DLL_PROCESS_DETACH:
        break; // выгрузка DLL (PhysShutdown уже должен был остановить воркеров)
    }
    return TRUE; // успешная обработка DllMain
}
