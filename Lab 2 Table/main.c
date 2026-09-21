#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN

#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

#define WINDOW_CLASS_NAME L"Lab2_Table"
#define WINDOW_TITLE      L"Table"

/* Пределы размерности, которую можно ввести */
#define MAX_ROWS 32
#define MAX_COLS 32

/* Минимальная высота шрифта при подгонке, px */
#define MIN_FONT_PX 8

/* Максимальная высота шрифта, px */
#define MAX_FONT_PX 36

/* Отступ текста от границ ячейки, px */
#define CELL_PAD 4

/* Макс. длина текста одной ячейки (автозаполнение) */
#define CELL_TEXT_MAX 192

/* Число строк и столбцов (задаёт пользователь) */
static int g_rows;
static int g_cols;

/* Буфер ячеек. Заполняется InitCells() после ввода размеров. */
static wchar_t g_cells[MAX_ROWS][MAX_COLS][CELL_TEXT_MAX];

/* Ширина клиентской области окна */
static int g_clientW;

/* Высота клиентской области */
static int g_clientH;

/* Текущая высота шрифта в пикселях (после FitFontToClient) */
static int g_fontPx = 16;

static int ReadDimensions(void)
{
    AllocConsole();
    freopen("CONIN$", "r", stdin);
    freopen("CONOUT$", "w", stdout);
    freopen("CONOUT$", "w", stderr);

    for (;;) {
        int n;

        wprintf(L"Enter number of rows (1..%d): ", MAX_ROWS);
        n = wscanf(L"%d", &g_rows);
        while (getwchar() != L'\n' && !feof(stdin))
            ;

        if (n != 1 || g_rows < 1 || g_rows > MAX_ROWS) {
            wprintf(L"Invalid input. Try again.\n");
            continue;
        }
        break;
    }

    for (;;) {
        int n;

        wprintf(L"Enter number of columns (1..%d): ", MAX_COLS);
        n = wscanf(L"%d", &g_cols);
        while (getwchar() != L'\n' && !feof(stdin))
            ;

        if (n != 1 || g_cols < 1 || g_cols > MAX_COLS) {
            wprintf(L"Invalid input. Try again.\n");
            continue;
        }
        break;
    }

    wprintf(L"Table %d x %d. Opening window...\n", g_rows, g_cols);
    FreeConsole();
    return 1;
}

/* Заполняет все ячейки: координата + lorem разной длины. */
static void InitCells(void)
{
    static const wchar_t lorem[] =
        L"Lorem ipsum dolor sit amet, consectetur adipiscing elit, "
        L"sed do eiusmod tempor incididunt ut labore et dolore magna aliqua. ";
    const int loremLen = (int)(sizeof(lorem) / sizeof(lorem[0]) - 1);
    int r, c;

    for (r = 0; r < g_rows; ++r) {
        for (c = 0; c < g_cols; ++c) {
            int idx = r * g_cols + c;
            int bodyLen = 12 + (idx * 23) % 100;
            int prefixLen;
            int i;
            wchar_t* dst = g_cells[r][c];

            prefixLen = wsprintfW(dst, L"[%d,%d] ", r + 1, c + 1);
            if (prefixLen < 0)
                prefixLen = 0;

            for (i = 0; i < bodyLen && prefixLen + i < CELL_TEXT_MAX - 1; ++i)
                dst[prefixLen + i] = lorem[i % loremLen];

            dst[prefixLen + i] = L'\0';
        }
    }
}

/* Создаёт логический шрифт заданной высоты в пикселях. */
static HFONT CreateTableFont(int heightPx)
{
    LOGFONTW lf = { 0 };

    /* Отрицательный lfHeight — высота символа в пикселях */
    lf.lfHeight = -heightPx;
    lf.lfWeight = FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    lstrcpyW(lf.lfFaceName, L"Segoe UI");
    return CreateFontIndirectW(&lf);
}

/* Высота ячейки: DrawText с DT_CALCRECT не рисует, а считает нужный RECT. */
static int MeasureCellHeight(HDC hdc, const wchar_t* text, int colWidth)
{
    RECT rc;
    int textWidth = colWidth - 2 * CELL_PAD;
    if (textWidth < 1)
        textWidth = 1;

    SetRect(&rc, 0, 0, textWidth, 0);
    DrawTextW(hdc, text, -1, &rc,
              DT_CALCRECT | DT_WORDBREAK | DT_LEFT | DT_TOP | DT_NOPREFIX);
    return (rc.bottom - rc.top) + 2 * CELL_PAD;
}

/* Высота строки = max по ячейкам строки; суммируем - высота всей таблицы. */
static void ComputeRowHeights(HDC hdc, int colWidth, int* rowHeights, int* totalHeight)
{
    int r, c;
    *totalHeight = 0;
    for (r = 0; r < g_rows; ++r) {
        int maxH = 0;
        for (c = 0; c < g_cols; ++c) {
            int h = MeasureCellHeight(hdc, g_cells[r][c], colWidth);
            if (h > maxH)
                maxH = h;
        }
        rowHeights[r] = maxH;
        *totalHeight += maxH;
    }
}

/*
 * Бинарный поиск высоты шрифта в [MIN_FONT_PX; MAX_FONT_PX]:
 * ищем максимальный размер, при котором высота таблицы ≤ высоты окна.
 * mid — кандидат; влезло - пробуем крупнее (lo), иначе — мельче (hi).
 * MIN/MAX — границы поиска, не фиксированный размер шрифта в рантайме.
 */
static int FitFontToClient(HWND hwnd)
{
    /* Нижняя граница диапазона */
    int lo = MIN_FONT_PX;

    /* Верхняя граница диапазона */
    int hi = MAX_FONT_PX;

    int best = MIN_FONT_PX;
    int colWidth;
    HDC hdc;
    int* rowHeights;

    if (g_clientW <= 0 || g_clientH <= 0)
        return MIN_FONT_PX;

    colWidth = g_clientW / g_cols;
    if (colWidth < 1)
        colWidth = 1;

    rowHeights = (int*)malloc((size_t)g_rows * sizeof(int));
    if (!rowHeights)
        return MIN_FONT_PX;

    hdc = GetDC(hwnd);

    while (lo <= hi) {
        /* Середина текущего [lo; hi] */
        int mid = (lo + hi) / 2;

        HFONT font = CreateTableFont(mid);
        HFONT old = (HFONT)SelectObject(hdc, font);
        int total = 0;

        ComputeRowHeights(hdc, colWidth, rowHeights, &total);

        SelectObject(hdc, old);
        DeleteObject(font);

        if (total <= g_clientH) {
            /* mid подходит — запоминаем */
            best = mid;

            /* Ищем ещё крупнее */
            lo = mid + 1;
        } else {
            /* mid слишком большой — сужаем сверху */
            hi = mid - 1;
        }
    }

    ReleaseDC(hwnd, hdc);
    free(rowHeights);

    /* Наибольшая высота шрифта, при которой таблица ещё влезала */
    return best;
}

/* Рисует сетку и текст: равные столбцы, высоты строк уже подобраны. */
static void DrawTable(HWND hwnd, HDC hdc)
{
    HFONT font;
    HFONT oldFont;
    HPEN pen;
    HPEN oldPen;
    int colWidth;
    int* rowHeights;
    int totalHeight;
    int r, c;
    int y;
    int remain;

    (void)hwnd;

    if (g_clientW <= 0 || g_clientH <= 0)
        return;

    /* Ширина столбца */
    colWidth = g_clientW / g_cols;
    if (colWidth < 1)
        colWidth = 1;

    /* Остаток от деления ширины - последнему столбцу, чтобы закрыть окно */
    remain = g_clientW - colWidth * g_cols;

    rowHeights = (int*)malloc((size_t)g_rows * sizeof(int));
    if (!rowHeights)
        return;

    font = CreateTableFont(g_fontPx);
    oldFont = (HFONT)SelectObject(hdc, font);

    ComputeRowHeights(hdc, colWidth, rowHeights, &totalHeight);

    pen = CreatePen(PS_SOLID, 1, RGB(60, 60, 60));
    oldPen = (HPEN)SelectObject(hdc, pen);

    /* Рамки (Rectangle) и текст (DrawText с DT_WORDBREAK) */
    y = 0;
    for (r = 0; r < g_rows; ++r) {
        int x = 0;
        for (c = 0; c < g_cols; ++c) {
            int w = colWidth + (c == g_cols - 1 ? remain : 0);
            RECT cell = { x, y, x + w, y + rowHeights[r] };
            RECT textRc = {
                x + CELL_PAD,
                y + CELL_PAD,
                x + w - CELL_PAD,
                y + rowHeights[r] - CELL_PAD
            };

            Rectangle(hdc, cell.left, cell.top, cell.right, cell.bottom);
            DrawTextW(hdc, g_cells[r][c], -1, &textRc,
                      DT_WORDBREAK | DT_LEFT | DT_TOP | DT_NOPREFIX);

            x += w;
        }
        y += rowHeights[r];
    }

    SelectObject(hdc, oldPen);
    DeleteObject(pen);
    SelectObject(hdc, oldFont);
    DeleteObject(font);
    free(rowHeights);
}

/* Диспетчер сообщений окна. */
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_SIZE:
        /* Новые размеры клиента - пересчёт шрифта - запрос перерисовки */
        g_clientW = LOWORD(lParam);
        g_clientH = HIWORD(lParam);

        /* Подбирает максимальную высоту шрифта */
        g_fontPx = FitFontToClient(hwnd);

        /* Ставит в очередь WM_PAINT */
        InvalidateRect(hwnd, NULL, TRUE);

        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        DrawTable(hwnd, hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_DESTROY:
        /* Выходим из цикла GetMessage */
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

/* Точка входа GUI: ввод размеров - окно - цикл сообщений. */
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                    PWSTR pCmdLine, int nCmdShow)
{
    WNDCLASSEXW wc = { 0 };
    HWND hwnd;
    MSG msg;

    (void)hPrevInstance;
    (void)pCmdLine;

    if (!ReadDimensions())
        return 1;

    InitCells();

    wc.cbSize = sizeof(wc);

    /* Полный invalidate при ресайзе */
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = WINDOW_CLASS_NAME;

    if (!RegisterClassExW(&wc))
        return 1;

    hwnd = CreateWindowExW(
        0,
        WINDOW_CLASS_NAME,
        WINDOW_TITLE,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        900, 600,
        NULL, NULL, hInstance, NULL);

    if (!hwnd)
        return 1;

    ShowWindow(hwnd, nCmdShow);

    /* GetMessage - Dispatch - WndProc */
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        DispatchMessageW(&msg);
    }

    return (int)msg.wParam;
}
