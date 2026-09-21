/*
 * Версия main.c с укороченными строками
 * (Courier New 11, лист Word / A4).
 * Не подключается к сборке — только для отчёта.
 */

#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN

#include <windows.h>

/* Размерность таблицы: N строк x M столбцов */
#define ROWS 4
#define COLS 3

#define WINDOW_CLASS_NAME L"OSiSP_Lab2_Table"
#define WINDOW_TITLE \
    L"Лабораторная 2 — текстовая таблица"

/* Минимальный кегль при подгонке */
#define MIN_FONT_PT 6

/* Максимальный кегль */
#define MAX_FONT_PT 28

/* Отступ текста от границ ячейки, px */
#define CELL_PAD 4

/*
 * Содержимое ячеек.
 * Меняя ROWS/COLS — синхронно меняй этот массив.
 */
static const wchar_t* g_cells[ROWS][COLS] = {
    {
        L"Имя",
        L"Город",
        L"Описание"
    },
    {
        L"Иванов И.И.",
        L"Москва",
        L"Lorem ipsum dolor sit amet, "
        L"consectetur adipiscing elit."
    },
    {
        L"Петров П.П.",
        L"Санкт-Петербург",
        L"Sed do eiusmod tempor incididunt "
        L"ut labore et dolore magna aliqua."
    },
    {
        L"Сидоров С.С.",
        L"Новосибирск",
        L"Ut enim ad minim veniam, quis "
        L"nostrud exercitation ullamco "
        L"laboris nisi ut aliquip ex ea "
        L"commodo consequat."
    }
};

/* Ширина клиентской области окна */
static int g_clientW;

/* Высота клиентской области */
static int g_clientH;

/* Текущий кегль (после FitFontToClient) */
static int g_fontPt = 14;

/* Создаёт логический шрифт заданного кегля. */
static HFONT CreateTableFont(int pointSize)
{
    LOGFONTW lf = { 0 };
    HDC hdc = GetDC(NULL);

    /*
     * Отрицательный lfHeight — высота в
     * пикселях «по кеглю», без internal leading.
     */
    lf.lfHeight = -MulDiv(
        pointSize,
        GetDeviceCaps(hdc, LOGPIXELSY),
        72);
    ReleaseDC(NULL, hdc);
    lf.lfWeight = FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    lstrcpyW(lf.lfFaceName, L"Segoe UI");
    return CreateFontIndirectW(&lf);
}

/*
 * Высота ячейки: DrawText с DT_CALCRECT
 * не рисует, а считает нужный RECT.
 */
static int MeasureCellHeight(
    HDC hdc,
    const wchar_t* text,
    int colWidth)
{
    RECT rc;
    int textWidth = colWidth - 2 * CELL_PAD;
    if (textWidth < 1)
        textWidth = 1;

    SetRect(&rc, 0, 0, textWidth, 0);
    DrawTextW(
        hdc,
        text,
        -1,
        &rc,
        DT_CALCRECT | DT_WORDBREAK |
            DT_LEFT | DT_TOP | DT_NOPREFIX);
    return (rc.bottom - rc.top) + 2 * CELL_PAD;
}

/*
 * Высота строки = max по ячейкам строки;
 * суммируем - высота всей таблицы.
 */
static void ComputeRowHeights(
    HDC hdc,
    int colWidth,
    int* rowHeights,
    int* totalHeight)
{
    int r, c;
    *totalHeight = 0;
    for (r = 0; r < ROWS; ++r) {
        int maxH = 0;
        for (c = 0; c < COLS; ++c) {
            int h = MeasureCellHeight(
                hdc, g_cells[r][c], colWidth);
            if (h > maxH)
                maxH = h;
        }
        rowHeights[r] = maxH;
        *totalHeight += maxH;
    }
}

/*
 * Бинарный поиск кегля в
 * [MIN_FONT_PT; MAX_FONT_PT]:
 * ищем максимальный размер шрифта,
 * при котором высота таблицы ≤ высоты окна.
 * mid — кандидат; влезло - крупнее (lo),
 * иначе — мельче (hi).
 * MIN/MAX — границы поиска, не фиксированный
 * размер шрифта в рантайме.
 */
static int FitFontToClient(HWND hwnd)
{
    /* Нижняя граница диапазона */
    int lo = MIN_FONT_PT;

    /* Верхняя граница диапазона */
    int hi = MAX_FONT_PT;

    int best = MIN_FONT_PT;
    int colWidth;
    HDC hdc;

    if (g_clientW <= 0 || g_clientH <= 0)
        return MIN_FONT_PT;

    colWidth = g_clientW / COLS;
    if (colWidth < 1)
        colWidth = 1;

    hdc = GetDC(hwnd);

    while (lo <= hi) {
        /* Середина текущего [lo; hi] */
        int mid = (lo + hi) / 2;

        HFONT font = CreateTableFont(mid);
        HFONT old =
            (HFONT)SelectObject(hdc, font);
        int rowHeights[ROWS];
        int total = 0;

        ComputeRowHeights(
            hdc, colWidth, rowHeights, &total);

        SelectObject(hdc, old);
        DeleteObject(font);

        if (total <= g_clientH) {
            /* mid подходит — запоминаем */
            best = mid;

            /* Ищем ещё крупнее */
            lo = mid + 1;
        } else {
            /* mid слишком большой */
            hi = mid - 1;
        }
    }

    ReleaseDC(hwnd, hdc);

    /* Наибольший подходящий кегль */
    return best;
}

/*
 * Рисует сетку и текст: равные столбцы,
 * высоты строк уже подобраны.
 */
static void DrawTable(HWND hwnd, HDC hdc)
{
    HFONT font;
    HFONT oldFont;
    HPEN pen;
    HPEN oldPen;
    int colWidth;
    int rowHeights[ROWS];
    int totalHeight;
    int r, c;
    int y;
    int remain;

    (void)hwnd;

    if (g_clientW <= 0 || g_clientH <= 0)
        return;

    /* Ширина столбца */
    colWidth = g_clientW / COLS;
    if (colWidth < 1)
        colWidth = 1;

    /*
     * Остаток от деления ширины —
     * последнему столбцу, чтобы закрыть окно.
     */
    remain = g_clientW - colWidth * COLS;

    font = CreateTableFont(g_fontPt);
    oldFont = (HFONT)SelectObject(hdc, font);

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(20, 20, 20));

    ComputeRowHeights(
        hdc, colWidth, rowHeights, &totalHeight);

    pen = CreatePen(PS_SOLID, 1, RGB(60, 60, 60));
    oldPen = (HPEN)SelectObject(hdc, pen);

    /*
     * Рамки (Rectangle) и текст
     * (DrawText с DT_WORDBREAK).
     */
    y = 0;
    for (r = 0; r < ROWS; ++r) {
        int x = 0;
        for (c = 0; c < COLS; ++c) {
            int w = colWidth +
                (c == COLS - 1 ? remain : 0);
            RECT cell = {
                x,
                y,
                x + w,
                y + rowHeights[r]
            };
            RECT textRc;

            Rectangle(
                hdc,
                cell.left,
                cell.top,
                cell.right,
                cell.bottom);

            textRc = cell;
            InflateRect(
                &textRc, -CELL_PAD, -CELL_PAD);
            DrawTextW(
                hdc,
                g_cells[r][c],
                -1,
                &textRc,
                DT_WORDBREAK | DT_LEFT |
                    DT_TOP | DT_NOPREFIX);

            x += w;
        }
        y += rowHeights[r];
    }

    SelectObject(hdc, oldPen);
    DeleteObject(pen);
    SelectObject(hdc, oldFont);
    DeleteObject(font);
}

/* Диспетчер сообщений окна. */
static LRESULT CALLBACK WndProc(
    HWND hwnd,
    UINT msg,
    WPARAM wParam,
    LPARAM lParam)
{
    switch (msg) {
    case WM_SIZE:
        /*
         * Новые размеры клиента —
         * пересчёт кегля — запрос перерисовки.
         */
        g_clientW = LOWORD(lParam);
        g_clientH = HIWORD(lParam);

        /* Подбирает максимальный кегль */
        g_fontPt = FitFontToClient(hwnd);

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

    case WM_ERASEBKGND: {
        /* Фон сами — меньше мерцания */
        RECT rc;
        HDC hdc = (HDC)wParam;
        GetClientRect(hwnd, &rc);
        FillRect(
            hdc,
            &rc,
            (HBRUSH)(COLOR_WINDOW + 1));
        return 1;
    }

    case WM_DESTROY:
        /* Выходим из цикла GetMessage */
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(
        hwnd, msg, wParam, lParam);
}

/*
 * Точка входа GUI:
 * класс окна - CreateWindow - цикл сообщений.
 */
int WINAPI wWinMain(
    HINSTANCE hInstance,
    HINSTANCE hPrevInstance,
    PWSTR pCmdLine,
    int nCmdShow)
{
    WNDCLASSEXW wc = { 0 };
    HWND hwnd;
    MSG msg;

    (void)hPrevInstance;
    (void)pCmdLine;

    wc.cbSize = sizeof(wc);

    /* Полный invalidate при ресайзе */
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground =
        (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = WINDOW_CLASS_NAME;

    if (!RegisterClassExW(&wc))
        return 1;

    hwnd = CreateWindowExW(
        0,
        WINDOW_CLASS_NAME,
        WINDOW_TITLE,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        900,
        600,
        NULL,
        NULL,
        hInstance,
        NULL);

    if (!hwnd)
        return 1;

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    /*
     * Классический цикл:
     * GetMessage - Translate - Dispatch.
     */
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    return (int)msg.wParam;
}
