#ifndef GRAVITY_COMMON_H
#define GRAVITY_COMMON_H

/*
 * common.h — общий контракт между gravity_sim.exe и gravity_physics.dll
 *
 * Порядок чтения: common.h → storage_sys.c → physics_dll.c → main_win.c
 *
 * --- Как связаны процессы ---
 * exe НЕ линкуется с DLL на этапе сборки: в рантайме LoadLibraryW("gravity_physics.dll"),
 * затем GetProcAddress("PhysInit") и т.д. Указатели складываются в PhysicsApi (main_win.c).
 *
 * --- Один кадр симуляции (главный цикл в WinMain) ---
 * 1. PeekMessageW — обработать клавиши/мышь (WndProc может вызвать PhysAddBody, ApplyPreset).
 * 2. PhysStep(dt, subSteps) — в DLL под CRITICAL_SECTION: несколько подшагов Verlet.
 * 3. PhysSnapshotRead — memcpy тел из DLL в g_renderBodies (отдельный буфер exe).
 * 4. InvalidateRect + WM_PAINT → RenderScene в offscreen DC → BitBlt на экран.
 *
 * --- Почему два буфера тел ---
 * Физика пишет в массив внутри DLL (потоки + lock). Рендер читает копию без удержания lock
 * на время GDI — иначе UI зависал бы на весь PhysStep.
 *
 * --- Body vs BodyMeta ---
 * Body — всё, что нужно интегратору (r, v, a, mass). BodyMeta — UI/игровая логика: alive,
 * pinned, кольцевой буфер trail[] для Polyline (индекс trailHead — куда писать следующую точку).
 */

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602 // Windows 8+ (для SYNCHRONIZATION_BARRIER и системных примитивов)
#endif

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- Лимиты и константы файла снимка --- */
// Максимальное количество тел и длина истории орбитальных треков
#define MAX_BODIES      4096
#define TRAIL_LENGTH    64
#define MAX_WORKERS     64
#define CHUNK_SIZE      16

// Сигнатура бинарного файла снимка симуляции (.grav) для File Mapping
#define GRAV_FILE_MAGIC   0x56415247u // 'GRAV'
#define GRAV_FILE_VERSION 1u

// --- Система единиц (астрономическая, согласованная с законами Кеплера) ---
// Длина: 1 AU (астрономическая единица ≈ 149.6 млн км)
// Масса: 1 M☉ (масса Солнца)
// Время: 1 сутки (day)
// G из T² = (4π²/GM) r³ при r = 1 AU, T = 365.25 суток, M = 1 M☉:
//   G = 4π² / (365.25²)  [AU³ / (M☉·сут²)]
#define SIM_DAYS_PER_YEAR 365.25
#define SIM_G_AU_MSUN_DAY (4.0 * 3.14159265358979323846 * 3.14159265358979323846 \
                           / (SIM_DAYS_PER_YEAR * SIM_DAYS_PER_YEAR))
#define SIM_DEFAULT_DT_DAY    0.004   // суток за кадр при timeScale=1 (точнее для N тел)
#define SIM_SOFTENING_AU      1.2e-5  // ε << орбиты Луны (~0.0026 AU), иначе спутник «плывёт»
#define SIM_LEGACY_G_THRESHOLD 1.0    // значения G из старых сохранений реестра

// Солнечная система: за кадр продвигаем много модельных суток, но малым фиксированным subDt
#define SIM_SOLAR_FRAME_DAYS   0.06    // суток модели на кадр при timeScale=1
#define SIM_SOLAR_PHYSICS_SUBDT 2.5e-4 // суток на один подшаг Verlet (~21.6 с модели)
#define SIM_SOLAR_SUBSTEPS     10      // минимум подшагов за кадр (реальный счёт выше)
#define SIM_SOLAR_MAX_SUBSTEPS 512
#define SIM_SOLAR_MAX_DAYS_PER_FRAME 6.0
#define SIM_SOLAR_DT           SIM_SOLAR_FRAME_DAYS
// Масштаб GDI: 1 AU = 100 единиц мира (дробные AU не теряются при (LONG) для Polyline)
#define SIM_SOLAR_DRAW_AU      100.0
#define SIM_CLUSTER_SOFTENING  4.5     // плотные демо-сцены (галактика, облако)

/* --- Пресеты и системы единиц (влияют на G, dt, подписи HUD) --- */
typedef enum {
    SIM_UNITS_ASTRO   = 0, // AU, M☉, сутки (Солнечная система, облако)
    SIM_UNITS_CHOREO  = 1, // безразмерные единицы (восьмёрка)
    SIM_UNITS_GALAXY  = 2  // kpc, 10¹⁰ M☉, Myr (столкновение галактик)
} SimUnitSystem;

// Пресеты астрофизических сцен
typedef enum {
    PRESET_SOLAR_SYSTEM  = 1, // Солнечная система (Солнце, планеты, Луна и спутник Юпитера)
    PRESET_GALAXY_CLASH  = 2, // Столкновение двух спиральных галактик
    PRESET_FIGURE_EIGHT  = 3, // Задача трех тел (хореография Восьмерки + пробные тела)
    PRESET_RANDOM_CLOUD  = 4  // Случайное гравитационное облако
} SimPreset;

/* --- Состояние одного тела и сцены --- */
// Точка орбитального трека в мировых координатах
typedef struct {
    float x;
    float y;
} TrailPoint;

// Выровненная структура тела (64 байта — 1 кэш-линия CPU для исключения false sharing)
typedef struct DECLSPEC_ALIGN(64) {
    double x, y;         // Положение в мировых единицах (AU для СС, безразм. для восьмёрки)
    double vx, vy;       // Скорость; при Verlet на шаге v обновляется из a_old и a_new
    double ax, ay;       // Ускорение на текущем шаге; после фазы 2 равно newAx/newAy
    double mass;         // Масса тела (8 байт)
    float  radius;       // Визуальный радиус в мировых единицах (4 байта)
    DWORD  color;        // Цвет GDI COLORREF (4 байта)
} Body;

// Дополнительное состояние тела (флаги и кольцевой буфер трека)
typedef struct {
    LONG       alive;               // 1 — тело активно, 0 — поглощено при столкновении
    LONG       pinned;              // 1 — фиксированное тело (опционально)
    LONG       trailHead;           // Текущий индекс записи в кольцевой буфер
    LONG       trailCount;          // Число накопленных точек трека (0..TRAIL_LENGTH)
    TrailPoint trail[TRAIL_LENGTH]; // История координат для отрисовки орбит
} BodyMeta;

// Параметры симуляции (сохраняются в реестре HKCU\Software\GravitySim)
typedef struct {
    int    numThreads;       // Число рабочих потоков в пуле (1..MAX_WORKERS)
    int    threadPriority;   // Относительный приоритет (THREAD_PRIORITY_*)
    int    preset;           // Активный пресет (SimPreset)
    int    subSteps;         // Число подшагов интегратора за кадр (вычислительная нагрузка CPU: 1..32)
    double timeScale;        // Множитель скорости времени симуляции (0.1x .. 10.0x)
    double dt;               // Шаг модельного времени за кадр [сутки / Myr / безразм. — см. unitSystem]
    double gravityG;         // Гравитационная постоянная G в выбранных единицах
    double softening;        // Параметр смягчения потенциала ε [AU / kpc / ...]
    int    unitSystem;       // SimUnitSystem — подписи осей и HUD
    BOOL   enableCollisions; // Неупругое слияние тел при столкновении
    BOOL   showTrails;       // Отрисовка орбитальных следов
    BOOL   showOrbitGuides;  // Справочные круговые орбиты (кеплеровские радиусы)
    BOOL   showVectors;      // Отрисовка векторов скорости
    BOOL   showGrid;         // Отрисовка координатной сетки мирового пространства
    BOOL   paused;           // Пауза физического интегрирования
    double camX, camY;       // Смещение камеры в мировых координатах
    double camZoom;          // Масштаб камеры (XFORM zoom)
} SimConfig;

// Телеметрия и статистика производительности (для вывода в HUD)
typedef struct {
    int        activeBodies;      // Текущее число живых тел
    int        totalBodies;       // Общее число слотов в массиве
    int        mergersCount;      // Количество слияний за сцену
    int        workerThreads;     // Активное число потоков в пуле
    int        workerPriority;    // Текущий приоритет потоков
    double     stepTimeMs;        // Время последнего шага физики (мс)
    double     singleThreadMs;    // Эталонное время на 1 потоке для оценки speedup
    double     kineticEnergy;     // Полная кинетическая энергия системы
    double     potentialEnergy;   // Полная потенциальная энергия системы
    double     totalEnergy;       // Полная механическая энергия E = K + U
    ULONGLONG  stepCounter;       // Счетчик выполненных шагов симуляции
    double     simTime;           // Накопленное модельное время (в единицах unitSystem)
    double     energyDriftRel;    // (E-E0)/|E0| относительно E на старте сцены
    SIZE_T     virtualMemBytes;   // Объем выделенной через VirtualAlloc памяти
    LONG       dllThreadsAttached;// Счетчик потоков, прошедших через DllMain
} SimStats;

// Заголовок бинарного файла снимка симуляции (.grav) для проецирования в память
// Выровнен по 64 байтам, чтобы массив Body после заголовка был выровнен для SSE/AVX (DECLSPEC_ALIGN(64))
typedef struct DECLSPEC_ALIGN(64) {
    DWORD     magic;            // GRAV_FILE_MAGIC ('GRAV')
    DWORD     version;          // GRAV_FILE_VERSION
    DWORD     bodyCount;        // Количество сохраненных тел
    DWORD     reserved;
    double    dt;
    double    gravityG;
    double    softening;
    double    camX;
    double    camY;
    double    camZoom;
    ULONGLONG stepCounter;
} SceneFileHeader;

/* --- Динамический импорт gravity_physics.dll (main_win.c → LoadPhysicsDll) --- */
// Типы экспортируемых функций из gravity_physics.dll
typedef BOOL (__cdecl *PFN_PhysInit)(const SimConfig *cfg);
typedef void (__cdecl *PFN_PhysShutdown)(void);
typedef void (__cdecl *PFN_PhysResetPreset)(int preset, int customCount);
typedef void (__cdecl *PFN_PhysStep)(double dt, int subSteps);
typedef int  (__cdecl *PFN_PhysAddBody)(double x, double y, double vx, double vy,
                                        double mass, float radius, DWORD color);
typedef void (__cdecl *PFN_PhysSetThreads)(int numThreads, int priority);
typedef void (__cdecl *PFN_PhysSetParams)(double gravityG, double softening, BOOL enableCollisions);
typedef int  (__cdecl *PFN_PhysSnapshotRead)(Body *outBodies, BodyMeta *outMeta,
                                             int maxCount, SimStats *outStats);
typedef int  (__cdecl *PFN_PhysLoadBodies)(const Body *inBodies, const BodyMeta *inMeta,
                                           int count, ULONGLONG stepCounter);

typedef void (__cdecl *PFN_PhysAddCluster)(double cx, double cy, int count);

// Таблица динамического импорта функций DLL (заполняется через GetProcAddress)
typedef struct {
    HMODULE              hModule;
    PFN_PhysInit         Init;
    PFN_PhysShutdown     Shutdown;
    PFN_PhysResetPreset  ResetPreset;
    PFN_PhysStep         Step;
    PFN_PhysAddBody      AddBody;
    PFN_PhysAddCluster   AddCluster;
    PFN_PhysSetThreads   SetThreads;
    PFN_PhysSetParams    SetParams;
    PFN_PhysSnapshotRead SnapshotRead;
    PFN_PhysLoadBodies   LoadBodies;
} PhysicsApi;

/* --- storage_sys.c: конфигурация и персистентность --- */
// Функции модуля storage_sys.c (Реестр Windows + Проецирование файлов в память)
void ConfigSetDefaults(SimConfig *cfg);
void ConfigApplyUnitSystem(SimConfig *cfg, int unitSystem);
void ConfigUnitSystemForPreset(SimConfig *cfg, int preset);
void ConfigApplyPresetPhysics(SimConfig *cfg, int preset);
double ConfigPresetTimeGain(int preset);
int ConfigSolarPhysicsSubSteps(double timeScale, double *outSubDt);
BOOL RegistryLoadConfig(SimConfig *cfg);
BOOL RegistrySaveConfig(const SimConfig *cfg);

BOOL SceneSaveMapped(const wchar_t *filePath, const SimConfig *cfg,
                     const Body *bodies, const BodyMeta *meta,
                     int count, ULONGLONG stepCounter, wchar_t *statusMsg, size_t msgCap);

BOOL SceneLoadMapped(const wchar_t *filePath, SimConfig *cfg,
                     Body *outBodies, BodyMeta *outMeta,
                     int maxCount, int *outCount, ULONGLONG *outStepCounter,
                     wchar_t *statusMsg, size_t msgCap);

#ifdef __cplusplus
}
#endif

#endif // GRAVITY_COMMON_H
