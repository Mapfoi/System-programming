#ifndef GRAVITY_MVP_COMMON_H
#define GRAVITY_MVP_COMMON_H

/*
 * MVP: общие типы. Читать в порядке common.h → physics_dll.c → main_win.c
 * Body — состояние для интегратора; SimConfig — dt/G/камера; PhysicsApi — указатели из DLL.
 */

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_BODIES   1024
#define TRAIL_LENGTH 0

typedef struct DECLSPEC_ALIGN(64) {
    double x, y;
    double vx, vy;
    double ax, ay;
    double mass;
    float  radius;
    DWORD  color;
} Body;

typedef struct {
    LONG alive;
} BodyMeta;

typedef struct {
    double gravityG;
    double softening;
    double dt;
    BOOL   paused;
    double camX, camY, camZoom;
} SimConfig;

typedef struct {
    int        activeBodies;
    int        totalBodies;
    ULONGLONG  stepCounter;
    double     stepTimeMs;
    SIZE_T     virtualMemBytes;
} SimStats;

typedef BOOL (__cdecl *PFN_PhysInit)(const SimConfig *cfg);
typedef void (__cdecl *PFN_PhysShutdown)(void);
typedef void (__cdecl *PFN_PhysResetScene)(void);
typedef void (__cdecl *PFN_PhysStep)(double dt);
typedef int  (__cdecl *PFN_PhysSnapshotRead)(Body *outBodies, BodyMeta *outMeta,
                                             int maxCount, SimStats *outStats);

typedef struct {
    HMODULE              hModule;
    PFN_PhysInit         Init;
    PFN_PhysShutdown     Shutdown;
    PFN_PhysResetScene   ResetScene;
    PFN_PhysStep         Step;
    PFN_PhysSnapshotRead SnapshotRead;
} PhysicsApi;

#ifdef __cplusplus
}
#endif

#endif
