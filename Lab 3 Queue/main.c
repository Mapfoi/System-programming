#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wingdi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Кадр 1920×1080 режется на тайлы 64×64 — сотни заданий в очередь
#define WIDTH 1920
#define HEIGHT 1080
#define TILE 64
#define MAX_ITER 800
#define MAX_THREADS 64
#define TILES_X ((WIDTH + TILE - 1) / TILE)
#define TILES_Y ((HEIGHT + TILE - 1) / TILE)
#define MAX_TILES (TILES_X * TILES_Y)

// Сколько раз прогонять каждый режим и брать минимум времени
#define REPS 3

// Задание очереди: тайл кадра; SLIST_ENTRY выровнен для Interlocked SList
typedef struct DECLSPEC_ALIGN(MEMORY_ALLOCATION_ALIGNMENT) {
    SLIST_ENTRY link;
    int x, y, w, h;
} Tile;

// Атомарная очередь заданий: lock-free SLIST, вставка несколькими потоками
DECLSPEC_ALIGN(MEMORY_ALLOCATION_ALIGNMENT) static SLIST_HEADER g_queue;
static Tile g_tiles[MAX_TILES];
static BYTE g_pixels[WIDTH * HEIGHT * 3];
static BYTE g_reference[WIDTH * HEIGHT * 3];
static HANDLE g_sem, g_allDone;
static LONG g_done;

static double Now(void)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER t;
    if (!freq.QuadPart)
        QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)freq.QuadPart;
}

// Итерации Мандельброта для пикселя, RGB в BGR (как в BMP)
static void PutPixel(BYTE *img, int x, int y)
{
    double cr = (x - WIDTH * 0.5) * (3.0 / WIDTH) - 0.55;
    double ci = (y - HEIGHT * 0.5) * (3.0 / WIDTH);
    double zr = 0, zi = 0, t;
    int n = 0;
    BYTE *p = img + ((size_t)y * WIDTH + (size_t)x) * 3;
    while (n < MAX_ITER && zr * zr + zi * zi <= 4.0) {
        t = zr * zr - zi * zi + cr;
        zi = 2.0 * zr * zi + ci;
        zr = t;
        ++n;
    }
    if (n == MAX_ITER)
        p[0] = p[1] = p[2] = 0;
    else {
        p[0] = (BYTE)(n * 9);
        p[1] = (BYTE)(n * 5);
        p[2] = (BYTE)(255 - n);
    }
}

static void FillTile(BYTE *img, int x0, int y0, int tw, int th)
{
    int x, y;
    for (y = y0; y < y0 + th; ++y)
        for (x = x0; x < x0 + tw; ++x)
            PutPixel(img, x, y);
}

// Вставка в очередь атомарна: InterlockedPushEntrySList
static void Enqueue(Tile *tile)
{
    InterlockedPushEntrySList(&g_queue, &tile->link);
    ReleaseSemaphore(g_sem, 1, NULL);
}

// Извлечение задания атомарно: InterlockedPopEntrySList
static Tile *Dequeue(void)
{
    PSLIST_ENTRY e = InterlockedPopEntrySList(&g_queue);
    return e ? CONTAINING_RECORD(e, Tile, link) : NULL;
}

// Входной поток: делит кадр на тайлы 64×64 и кладёт задания в очередь
static DWORD WINAPI InputProc(void *unused)
{
    int x, y, n = 0;
    (void)unused;
    for (y = 0; y < HEIGHT; y += TILE)
        for (x = 0; x < WIDTH; x += TILE) {
            Tile *t = &g_tiles[n++];
            t->x = x;
            t->y = y;
            t->w = WIDTH - x < TILE ? WIDTH - x : TILE;
            t->h = HEIGHT - y < TILE ? HEIGHT - y : TILE;
            Enqueue(t);
        }
    return 0;
}

// Рабочий поток: извлекает тайл, считает итерации пикселей, пишет RGB
static DWORD WINAPI WorkerProc(void *unused)
{
    Tile *t;
    (void)unused;
    for (;;) {
        WaitForSingleObject(g_sem, INFINITE);
        t = Dequeue();
        if (!t)
            break;
        FillTile(g_pixels, t->x, t->y, t->w, t->h);
        if (InterlockedIncrement(&g_done) == MAX_TILES)
            SetEvent(g_allDone);
    }
    return 0;
}

// Выходной поток: ждёт все тайлы и собирает BMP через CreateFile + WriteFile
static DWORD WINAPI OutputProc(void *unused)
{
    BITMAPFILEHEADER fh = {0};
    BITMAPINFOHEADER ih = {0};
    HANDLE file;
    DWORD written;
    int y;
    (void)unused;
    WaitForSingleObject(g_allDone, INFINITE);
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + WIDTH * HEIGHT * 3;
    ih.biSize = sizeof(ih);
    ih.biWidth = WIDTH;
    ih.biHeight = HEIGHT;
    ih.biPlanes = 1;
    ih.biBitCount = 24;
    file = CreateFileW(L"mandelbrot.bmp", GENERIC_WRITE, 0, NULL,
                       CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return 1;
    WriteFile(file, &fh, sizeof(fh), &written, NULL);
    WriteFile(file, &ih, sizeof(ih), &written, NULL);
    for (y = HEIGHT - 1; y >= 0; --y)
        WriteFile(file, g_pixels + (size_t)y * WIDTH * 3, WIDTH * 3, &written, NULL);
    CloseHandle(file);
    return 0;
}

// Обработчик очереди: создаёт N рабочих потоков и раздаёт им задания
static double RunParallel(int nThreads)
{
    HANDLE workers[MAX_THREADS], hIn, hOut;
    int i;
    double t0, dt;
    if (nThreads < 1)
        nThreads = 1;
    if (nThreads > MAX_THREADS)
        nThreads = MAX_THREADS;
    memset(g_pixels, 0, sizeof(g_pixels));
    InitializeSListHead(&g_queue);
    g_done = 0;
    ResetEvent(g_allDone);
    for (i = 0; i < nThreads; ++i)
        workers[i] = CreateThread(NULL, 0, WorkerProc, NULL, 0, NULL);
    hOut = CreateThread(NULL, 0, OutputProc, NULL, 0, NULL);
    t0 = Now();
    hIn = CreateThread(NULL, 0, InputProc, NULL, 0, NULL);
    WaitForSingleObject(hIn, INFINITE);
    CloseHandle(hIn);
    for (i = 0; i < nThreads; ++i)
        ReleaseSemaphore(g_sem, 1, NULL);
    WaitForSingleObject(g_allDone, INFINITE);
    dt = Now() - t0;
    WaitForSingleObject(hOut, INFINITE);
    CloseHandle(hOut);
    WaitForMultipleObjects((DWORD)nThreads, workers, TRUE, INFINITE);
    for (i = 0; i < nThreads; ++i)
        CloseHandle(workers[i]);
    return dt;
}

/* === FIX 2: несколько прогонов, берём минимум ===
 * Один замер — это шум (планировщик, турбо-буст, другие процессы).
 * Для CPU-задач минимум из N прогонов — лучшая оценка «чистого» времени. */
static double MeasureParallel(int nThreads, int reps)
{
    double best = 1e9, t;
    int r;
    for (r = 0; r < reps; ++r) {
        t = RunParallel(nThreads);
        if (t < best)
            best = t;
    }
    return best;
}

int main(int argc, char **argv)
{
    SYSTEM_INFO si;
    int n, counts[8], nCounts = 0, i, match;
    double tSeq, tPar[8];
    GetSystemInfo(&si);
    g_sem = CreateSemaphoreW(NULL, 0, MAX_TILES + MAX_THREADS, NULL);
    g_allDone = CreateEventW(NULL, TRUE, FALSE, NULL);

    /* === FIX 1 v2: прогрев + REPS прогонов эталона, берём минимум ===
    * Первый замер ловит холодный CPU на базовой частоте (~1.2 ГГц),
    * а параллель идёт на турбо (~3 ГГц). Гоним эталон REPS раз —
    * к REPS-му прогону CPU уже на турбо, и мы меряем честное время. */
    FillTile(g_reference, 0, 0, WIDTH, HEIGHT);       // прогрев кэша

    {
        int r;
        double best = 1e9;
        for (r = 0; r < REPS; ++r) {
            double t0 = Now();
            FillTile(g_reference, 0, 0, WIDTH, HEIGHT);
            double dt = Now() - t0;
            if (dt < best) best = dt;
        }
        tSeq = best;
    }
    printf("sequential  1 thread  %.3f s  (reference)\n", tSeq);

    if (argc > 1) {
        counts[0] = atoi(argv[1]);
        nCounts = 1;
    } else {
        /* === FIX 3: 8 потоков оставлены, но добавлен комментарий ===
         * На i3-1005G1 (2C/4T) больше 4 потоков смысла нет — упрёмся
         * в физику. Но строку оставляем для наглядной демонстрации
         * плато на графике speedup(threads). */
        counts[0] = 1;
        counts[1] = 2;
        counts[2] = 4;
        counts[3] = 8;
        nCounts = 4;
    }

    // Параллельный рендер: очередь + N рабочих; speedup относительно 1 потока
    printf("\nthreads    time     speedup\n");
    for (i = 0; i < nCounts; ++i) {
        n = counts[i];
        tPar[i] = MeasureParallel(n, REPS);
        match = memcmp(g_pixels, g_reference, sizeof(g_pixels)) == 0;
        printf("%7d  %6.3f s  %6.2fx  %s\n", n, tPar[i], tSeq / tPar[i],
               match ? "pixel-match OK" : "MISMATCH");
    }
    printf("\nspeedup(threads)\n");
    for (i = 0; i < nCounts; ++i) {
        int bar = (int)((tSeq / tPar[i]) * 8 + 0.5);
        printf("%2d | ", counts[i]);
        while (bar-- > 0)
            putchar('#');
        printf("  %.2fx\n", tSeq / tPar[i]);
    }
    printf("\nCPU: %u logical processors\n", si.dwNumberOfProcessors);
    printf("BMP: mandelbrot.bmp  (%dx%d, %d tiles)\n", WIDTH, HEIGHT, MAX_TILES);
    CloseHandle(g_sem);
    CloseHandle(g_allDone);
    system("pause");
    return 0;
}