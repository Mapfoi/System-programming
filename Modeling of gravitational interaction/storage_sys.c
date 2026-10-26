/*
 * storage_sys.c — настройки и сохранение состояния (без GUI)
 *
 * A) ConfigSetDefaults / ConfigApplyPresetPhysics — что передать в PhysInit и PhysSetParams.
 *    ConfigSolarPhysicsSubSteps — сколько раз вызвать PhysStep с малым subDt за один кадр (СС).
 *
 * B) RegistryLoadConfig: всегда сначала ConfigSetDefaults, потом перезапись полей из реестра.
 *    RegistrySaveConfig: при выходе и по F5 (вместе со снимком сцены).
 *
 * C) SceneSaveMapped: CreateFileW → CreateFileMappingW(PAGE_READWRITE) → MapViewOfFile(FILE_MAP_WRITE)
 *    → memcpy заголовка и массивов → FlushViewOfFile → UnmapViewOfFile → CloseHandle×2.
 *    SceneLoadMapped: OPEN_EXISTING, PAGE_READONLY mapping, проверка magic/version/size, memcpy в буфер exe.
 *
 * Читать после common.h.
 */

#include "common.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

/* ========== A. Конфигурация симуляции ========== */

// Путь в реестре текущего пользователя (слайд 9: HKEY_CURRENT_USER, Advapi32.dll)
static const wchar_t kRegSubKey[] = L"Software\\GravitySim";

// Стартовые значения, если реестр пуст или ключ не создан
void ConfigSetDefaults(SimConfig *cfg)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);

    int defThreads = (int)si.dwNumberOfProcessors;
    if (defThreads < 1) defThreads = 4;
    if (defThreads > 16) defThreads = 16;

    cfg->numThreads       = defThreads;
    cfg->threadPriority   = THREAD_PRIORITY_NORMAL;
    cfg->preset           = PRESET_SOLAR_SYSTEM;
    cfg->timeScale        = 1.0;
    cfg->unitSystem       = SIM_UNITS_ASTRO;
    cfg->gravityG         = SIM_G_AU_MSUN_DAY;
    cfg->softening        = SIM_SOFTENING_AU;
    cfg->dt               = SIM_SOLAR_DT;
    cfg->subSteps         = SIM_SOLAR_SUBSTEPS;
    cfg->enableCollisions = FALSE;
    cfg->showTrails       = TRUE;
    cfg->showOrbitGuides  = TRUE;
    cfg->showVectors      = FALSE;
    cfg->showGrid         = TRUE;
    cfg->paused           = FALSE;
    cfg->camX             = 0.0;
    cfg->camY             = 0.0;
    cfg->camZoom          = 22.0;
}

void ConfigApplyUnitSystem(SimConfig *cfg, int unitSystem)
{
    cfg->unitSystem = unitSystem;
    switch (unitSystem) {
    case SIM_UNITS_CHOREO:
        cfg->gravityG  = 1.0;
        cfg->softening = 0.02;
        cfg->dt        = 0.00035;
        break;
    case SIM_UNITS_GALAXY:
        // kpc, 10^10 M☉, Myr (мегалет): G подобран под орбитальную скорость ~220 km/s на 8 kpc
        cfg->gravityG  = 4.0 * 3.14159265358979323846 * 3.14159265358979323846 / 100.0;
        cfg->softening = 0.08;
        cfg->dt        = 0.002;
        break;
    case SIM_UNITS_ASTRO:
    default:
        cfg->gravityG  = SIM_G_AU_MSUN_DAY;
        cfg->softening = SIM_SOFTENING_AU;
        cfg->dt        = SIM_DEFAULT_DT_DAY;
        break;
    }
}

/*
 * Подстраивает «быстроту» галактик/восьмёрки без изменения G: dt_eff = cfg.dt * timeScale * gain.
 * gain < 1 замедляет хаотичные сцены; gain > 1 ускоряет восьмёрку. СС gain=1 — см. ConfigSolar*.
 */
double ConfigPresetTimeGain(int preset)
{
    switch (preset) {
    case PRESET_SOLAR_SYSTEM:
        return 1.0;
    case PRESET_FIGURE_EIGHT:
        return 35.0;
    case PRESET_GALAXY_CLASH:
    case PRESET_RANDOM_CLOUD:
        return 0.06;
    default:
        return 1.0;
    }
}

/*
 * Солнечная система: нельзя увеличивать dt интегратора для «ускорения времени» — орбиты развалятся.
 * Вместо этого за кадр продвигаем advance = SIM_SOLAR_FRAME_DAYS * timeScale суток модели,
 * разбивая на steps кусков по SIM_SOLAR_PHYSICS_SUBDT (PhysStep вызывается с subDt и steps).
 * Итого модельное время за кадр ≈ steps * subDt (не больше advance из-за ceil и cap).
 */
int ConfigSolarPhysicsSubSteps(double timeScale, double *outSubDt)
{
    if (outSubDt)
        *outSubDt = SIM_SOLAR_PHYSICS_SUBDT;
    if (timeScale < 0.05)
        timeScale = 0.05;
    double advance = SIM_SOLAR_FRAME_DAYS * timeScale;
    if (advance > SIM_SOLAR_MAX_DAYS_PER_FRAME)
        advance = SIM_SOLAR_MAX_DAYS_PER_FRAME;
    int steps = (int)ceil(advance / SIM_SOLAR_PHYSICS_SUBDT);
    if (steps < SIM_SOLAR_SUBSTEPS)
        steps = SIM_SOLAR_SUBSTEPS;
    if (steps > SIM_SOLAR_MAX_SUBSTEPS)
        steps = SIM_SOLAR_MAX_SUBSTEPS;
    return steps;
}

/* Вызывается из ApplyPreset: подставляет согласованные G, ε, dt, subSteps под номер сцены 1..4 */
void ConfigApplyPresetPhysics(SimConfig *cfg, int preset)
{
    switch (preset) {
    case PRESET_SOLAR_SYSTEM:
        cfg->unitSystem = SIM_UNITS_ASTRO;
        cfg->gravityG   = SIM_G_AU_MSUN_DAY;
        cfg->softening  = SIM_SOFTENING_AU;
        cfg->dt         = SIM_SOLAR_DT;
        cfg->subSteps   = SIM_SOLAR_SUBSTEPS;
        break;
    case PRESET_FIGURE_EIGHT:
        cfg->gravityG  = 1.0;
        cfg->softening = 0.02;
        cfg->dt        = 0.00035;
        cfg->subSteps  = 8;
        break;
    case PRESET_GALAXY_CLASH:
    case PRESET_RANDOM_CLOUD:
    default:
        cfg->gravityG  = 120.0;
        cfg->softening = SIM_CLUSTER_SOFTENING;
        cfg->dt        = 0.016;
        if (cfg->subSteps < 4)
            cfg->subSteps = 4;
        break;
    }
}

void ConfigUnitSystemForPreset(SimConfig *cfg, int preset)
{
    switch (preset) {
    case PRESET_GALAXY_CLASH:
        ConfigApplyUnitSystem(cfg, SIM_UNITS_GALAXY);
        break;
    case PRESET_FIGURE_EIGHT:
        ConfigApplyUnitSystem(cfg, SIM_UNITS_CHOREO);
        break;
    default:
        ConfigApplyUnitSystem(cfg, SIM_UNITS_ASTRO);
        break;
    }
}

/* ========== B. Реестр Windows ========== */

static void RegWriteDword(HKEY hKey, const wchar_t *name, DWORD val)
{
    RegSetValueExW(hKey, name, 0, REG_DWORD, (const BYTE *)&val, sizeof(val));
}

static BOOL RegReadDword(HKEY hKey, const wchar_t *name, DWORD *outVal)
{
    DWORD type = 0, size = sizeof(DWORD);
    if (RegQueryValueExW(hKey, name, NULL, &type, (BYTE *)outVal, &size) == ERROR_SUCCESS &&
        type == REG_DWORD && size == sizeof(DWORD)) {
        return TRUE;
    }
    return FALSE;
}

static void RegWriteDouble(HKEY hKey, const wchar_t *name, double val)
{
    RegSetValueExW(hKey, name, 0, REG_BINARY, (const BYTE *)&val, sizeof(val));
}

static BOOL RegReadDouble(HKEY hKey, const wchar_t *name, double *outVal)
{
    DWORD type = 0, size = sizeof(double);
    if (RegQueryValueExW(hKey, name, NULL, &type, (BYTE *)outVal, &size) == ERROR_SUCCESS &&
        type == REG_BINARY && size == sizeof(double)) {
        return TRUE;
    }
    return FALSE;
}

// При старте exe: сначала defaults, затем перекрытие полями из реестра (если ключ есть)
BOOL RegistryLoadConfig(SimConfig *cfg)
{
    HKEY hKey = NULL;
    ConfigSetDefaults(cfg);

    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegSubKey, 0, KEY_QUERY_VALUE, &hKey) != ERROR_SUCCESS)
        return FALSE;

    DWORD v = 0;
    if (RegReadDword(hKey, L"NumThreads", &v) && v >= 1 && v <= MAX_WORKERS)
        cfg->numThreads = (int)v;
    if (RegReadDword(hKey, L"ThreadPriority", &v))
        cfg->threadPriority = (int)v;
    if (RegReadDword(hKey, L"Preset", &v) && v >= 1 && v <= 4)
        cfg->preset = (int)v;
    if (RegReadDword(hKey, L"SubSteps", &v) && v >= 1 && v <= 32)
        cfg->subSteps = (int)v;
    if (RegReadDword(hKey, L"UnitSystem", &v) && v <= (DWORD)SIM_UNITS_GALAXY)
        cfg->unitSystem = (int)v;
    if (RegReadDword(hKey, L"EnableCollisions", &v))
        cfg->enableCollisions = (v != 0);
    if (RegReadDword(hKey, L"ShowTrails", &v))
        cfg->showTrails = (v != 0);
    if (RegReadDword(hKey, L"ShowOrbitGuides", &v))
        cfg->showOrbitGuides = (v != 0);
    if (RegReadDword(hKey, L"ShowVectors", &v))
        cfg->showVectors = (v != 0);
    if (RegReadDword(hKey, L"ShowGrid", &v))
        cfg->showGrid = (v != 0);

    double regDt = 0.0, regTs = 0.0, regG = 0.0, d = 0.0;
    BOOL haveDt = RegReadDouble(hKey, L"TimeStep", &regDt) && regDt >= 0.00001 && regDt <= 1.0;
    BOOL haveTs = RegReadDouble(hKey, L"TimeScale", &regTs) && regTs >= 0.05 && regTs <= 256.0;
    BOOL haveG  = RegReadDouble(hKey, L"GravityG", &regG) && regG >= 1e-8 && regG <= 1000.0;
    if (haveTs)
        cfg->timeScale = regTs;
    if (haveDt)
        cfg->dt = regDt;
    if (haveG)
        cfg->gravityG = regG;
    if (RegReadDouble(hKey, L"Softening", &d) && d >= 1e-6 && d <= 50.0)
        cfg->softening = d;
    if (RegReadDouble(hKey, L"CamZoom", &d) && d >= 0.5 && d <= 500.0)
        cfg->camZoom = d;

    RegCloseKey(hKey);
    return TRUE;  /* ключ был — даже если часть полей не прочиталась */
}

/* Записывает текущий g_cfg; camX/camY в реестр не пишем (только zoom) — см. поля RegWrite* */
BOOL RegistrySaveConfig(const SimConfig *cfg)
{
    HKEY hKey = NULL;
    DWORD disp = 0;

    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegSubKey, 0, NULL,
                        REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, NULL,
                        &hKey, &disp) != ERROR_SUCCESS) {
        return FALSE;
    }

    RegWriteDword(hKey, L"NumThreads",       (DWORD)cfg->numThreads);
    RegWriteDword(hKey, L"ThreadPriority",   (DWORD)cfg->threadPriority);
    RegWriteDword(hKey, L"Preset",           (DWORD)cfg->preset);
    RegWriteDword(hKey, L"SubSteps",         (DWORD)cfg->subSteps);
    RegWriteDword(hKey, L"UnitSystem",       (DWORD)cfg->unitSystem);
    RegWriteDword(hKey, L"EnableCollisions", (DWORD)(cfg->enableCollisions ? 1 : 0));
    RegWriteDword(hKey, L"ShowTrails",       (DWORD)(cfg->showTrails ? 1 : 0));
    RegWriteDword(hKey, L"ShowOrbitGuides",  (DWORD)(cfg->showOrbitGuides ? 1 : 0));
    RegWriteDword(hKey, L"ShowVectors",      (DWORD)(cfg->showVectors ? 1 : 0));
    RegWriteDword(hKey, L"ShowGrid",         (DWORD)(cfg->showGrid ? 1 : 0));

    RegWriteDouble(hKey, L"TimeScale", cfg->timeScale);
    RegWriteDouble(hKey, L"TimeStep",  cfg->dt);
    RegWriteDouble(hKey, L"GravityG",  cfg->gravityG);
    RegWriteDouble(hKey, L"Softening", cfg->softening);
    RegWriteDouble(hKey, L"CamZoom",   cfg->camZoom);

    RegCloseKey(hKey);
    return TRUE;
}

/* ========== C. Снимок сцены .grav (file mapping) ========== */
/*
 * Layout на диске: [SceneFileHeader][Body × N][BodyMeta × N]
 * Загрузка: MapViewOfFile → memcpy в буфер рендера → PhysLoadBodies в DLL
 */

// Проецирование файлов в память: двухуровневая схема Windows (слайды 58–61)
// Файл (CreateFileW) -> Проекция файла (CreateFileMappingW) -> Окно проекции (MapViewOfFile)
BOOL SceneSaveMapped(const wchar_t *filePath, const SimConfig *cfg,
                     const Body *bodies, const BodyMeta *meta,
                     int count, ULONGLONG stepCounter, wchar_t *statusMsg, size_t msgCap)
{
    if (count < 0) count = 0;
    if (count > MAX_BODIES) count = MAX_BODIES;

    DWORD totalBytes = (DWORD)(sizeof(SceneFileHeader) +
                               (size_t)count * sizeof(Body) +
                               (size_t)count * sizeof(BodyMeta));

    // 1. Создать объект ядра — Файл
    HANDLE hFile = CreateFileW(filePath, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        if (statusMsg)
            _snwprintf(statusMsg, msgCap, L"Ошибка CreateFileW при сохранении (код %lu)", GetLastError());
        return FALSE;
    }

    // 2. Создать объект ядра — Проекция файла (Section Object)
    HANDLE hMap = CreateFileMappingW(hFile, NULL, PAGE_READWRITE, 0, totalBytes, NULL);
    if (!hMap) {
        if (statusMsg)
            _snwprintf(statusMsg, msgCap, L"Ошибка CreateFileMappingW (код %lu)", GetLastError());
        CloseHandle(hFile);
        return FALSE;
    }

    // 3. Создать окно проекции в виртуальном адресном пространстве процесса
    BYTE *view = (BYTE *)MapViewOfFile(hMap, FILE_MAP_WRITE, 0, 0, totalBytes);
    if (!view) {
        if (statusMsg)
            _snwprintf(statusMsg, msgCap, L"Ошибка MapViewOfFile (код %lu)", GetLastError());
        CloseHandle(hMap);
        CloseHandle(hFile);
        return FALSE;
    }

    // Запись заголовка и массивов напрямую через указатели памяти
    SceneFileHeader *hdr = (SceneFileHeader *)view;
    memset(hdr, 0, sizeof(*hdr));
    hdr->magic       = GRAV_FILE_MAGIC;
    hdr->version     = GRAV_FILE_VERSION;
    hdr->bodyCount   = (DWORD)count;
    hdr->dt          = cfg->dt;
    hdr->gravityG    = cfg->gravityG;
    hdr->softening   = cfg->softening;
    hdr->camX        = cfg->camX;
    hdr->camY        = cfg->camY;
    hdr->camZoom     = cfg->camZoom;
    hdr->stepCounter = stepCounter;

    BYTE *pBodies = view + sizeof(SceneFileHeader);
    BYTE *pMeta   = pBodies + (size_t)count * sizeof(Body);

    if (count > 0) {
        memcpy(pBodies, bodies, (size_t)count * sizeof(Body));
        memcpy(pMeta,   meta,   (size_t)count * sizeof(BodyMeta));
    }

    // 4. Синхронизация страниц памяти с диском и закрытие окна проекции (слайд 61)
    FlushViewOfFile(view, totalBytes);
    UnmapViewOfFile(view);
    CloseHandle(hMap);
    CloseHandle(hFile);

    if (statusMsg) {
        _snwprintf(statusMsg, msgCap,
                   L"Снимок сохранен через File Mapping: %s (%d тел, %.1f КБ)",
                   filePath, count, (double)totalBytes / 1024.0);
    }
    return TRUE;
}

BOOL SceneLoadMapped(const wchar_t *filePath, SimConfig *cfg,
                     Body *outBodies, BodyMeta *outMeta,
                     int maxCount, int *outCount, ULONGLONG *outStepCounter,
                     wchar_t *statusMsg, size_t msgCap)
{
    HANDLE hFile = CreateFileW(filePath, GENERIC_READ, FILE_SHARE_READ, NULL,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        if (statusMsg)
            _snwprintf(statusMsg, msgCap, L"Файл снимка %s не найден (сначала сохраните по F5)", filePath);
        return FALSE;
    }

    LARGE_INTEGER fileSize;
    if (!GetFileSizeEx(hFile, &fileSize) || fileSize.QuadPart < (LONGLONG)sizeof(SceneFileHeader)) {
        if (statusMsg)
            _snwprintf(statusMsg, msgCap, L"Файл %s поврежден или слишком мал", filePath);
        CloseHandle(hFile);
        return FALSE;
    }

    HANDLE hMap = CreateFileMappingW(hFile, NULL, PAGE_READONLY, 0, 0, NULL);
    if (!hMap) {
        if (statusMsg)
            _snwprintf(statusMsg, msgCap, L"Ошибка CreateFileMappingW при чтении (код %lu)", GetLastError());
        CloseHandle(hFile);
        return FALSE;
    }

    const BYTE *view = (const BYTE *)MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
    if (!view) {
        if (statusMsg)
            _snwprintf(statusMsg, msgCap, L"Ошибка MapViewOfFile при чтении (код %lu)", GetLastError());
        CloseHandle(hMap);
        CloseHandle(hFile);
        return FALSE;
    }

    const SceneFileHeader *hdr = (const SceneFileHeader *)view;
    if (hdr->magic != GRAV_FILE_MAGIC || hdr->version != GRAV_FILE_VERSION) {
        if (statusMsg)
            _snwprintf(statusMsg, msgCap, L"Неверная сигнатура заголовка в файле %s", filePath);
        UnmapViewOfFile(view);
        CloseHandle(hMap);
        CloseHandle(hFile);
        return FALSE;
    }

    int count = (int)hdr->bodyCount;
    if (count > maxCount)
        count = maxCount;

    SIZE_T expectedSize = sizeof(SceneFileHeader) +
                          (size_t)hdr->bodyCount * sizeof(Body) +
                          (size_t)hdr->bodyCount * sizeof(BodyMeta);
    if ((ULONGLONG)fileSize.QuadPart < expectedSize) {
        if (statusMsg)
            _snwprintf(statusMsg, msgCap, L"Неполный размер данных в снимке %s", filePath);
        UnmapViewOfFile(view);
        CloseHandle(hMap);
        CloseHandle(hFile);
        return FALSE;
    }

    /* Указатели внутри отображения: не копируем весь файл, только нужные байты в буфер exe */
    const BYTE *pBodies = view + sizeof(SceneFileHeader);
    const BYTE *pMeta   = pBodies + (size_t)hdr->bodyCount * sizeof(Body);

    if (count > 0) {
        memcpy(outBodies, pBodies, (size_t)count * sizeof(Body));
        memcpy(outMeta,   pMeta,   (size_t)count * sizeof(BodyMeta));
    }

    /* Восстанавливаем параметры камеры и физики; тела в DLL — через PhysLoadBodies из WndProc F9 */
    cfg->dt        = hdr->dt;
    cfg->gravityG  = hdr->gravityG;
    cfg->softening = hdr->softening;
    cfg->camX      = hdr->camX;
    cfg->camY      = hdr->camY;
    cfg->camZoom   = hdr->camZoom;

    ULONGLONG savedStepCounter = hdr->stepCounter;
    if (outCount) *outCount = count;
    if (outStepCounter) *outStepCounter = savedStepCounter;

    UnmapViewOfFile(view);
    CloseHandle(hMap);
    CloseHandle(hFile);

    if (statusMsg) {
        _snwprintf(statusMsg, msgCap,
                   L"Снимок загружен через File Mapping: %s (%d тел, шаг %llu)",
                   filePath, count, savedStepCounter);
    }
    return TRUE;
}
