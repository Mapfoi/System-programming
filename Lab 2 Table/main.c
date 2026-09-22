#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// Число строк таблицы
#define ROWS 4

// Число столбцов таблицы
#define COLS 3

// https://ru.wikipedia.org/wiki/Шрифт
static const wchar_t *cellText[ROWS][COLS] = {
    {L"Шрифт", L"Рисованный и наборный", L"Гарнитура"},
    {L"Рисунок букв и знаков",
     L"Шрифты делятся на рисованные (англ. Lettering) и наборные (англ. Type).",
     L"Группа шрифтов разных начертаний и кеглей, имеющих одинаковый стиль, называется гарнитурой."},
    {L"Пиктография",
     L"Первой письменной формой передачи мысли была пиктография — рисунки на стенах пещер и на скалах.",
     L"Первый алфавит литературно-фонетического письма создали финикийцы. Этот алфавит стал первоисточником большинства алфавитов мира — греческого, латинского, кириллического и прочих."},
    {L"Унциал",
     L"В VI веке появляется новый стиль письма — унциал.",
     L"В XV веке типографы изготовили новые печатные шрифты. Среди пионеров были Николя Жансон, Альд Мануций и Клод Гарамон. Шрифт Гарамона стал основой для множества современных шрифтов."}
};

// Гарнитуры ячеек: Roman, Swiss, Modern, Script
static const wchar_t *fontNames[] = {
    L"Times New Roman", L"Arial", L"Courier New", L"Segoe Script"
};

// Семейство шрифта для той же ячейки
static const DWORD fontFamilies[] = {FF_ROMAN, FF_SWISS, FF_MODERN, FF_SCRIPT};

// Ширина и высота клиента, текущий кегль в пикселях
static int clientWidth, clientHeight, fontSizePx = 16;

// Логический шрифт ячейки: своя гарнитура и своё начертание
static HFONT CellFont(int row, int column, int fontSize)
{

    // Индекс ячейки выбирает гарнитуру, жирность и курсив
    int cellIndex = row * COLS + column;

    // Отрицательная высота — размер символа в пикселях, контур TrueType
    return CreateFontW(-fontSize, 0, 0, 0,
        (cellIndex % 2) ? FW_BOLD : FW_NORMAL, (cellIndex % 3) == 0,
        0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | fontFamilies[cellIndex % 4],
        fontNames[cellIndex % 4]);
}

// Высота текста ячейки с переносом по словам, без рисования
static int MeasureCellHeight(HDC deviceContext, int row, int column,
                             int columnWidth, int fontSize)
{

    // Считать тем же шрифтом, которым ячейка будет нарисована
    HFONT font = CellFont(row, column, fontSize);
    HFONT previousFont = (HFONT)SelectObject(deviceContext, font);

    // Ширина столбца без полей; высоту заполнит DrawText
    RECT measuredRect = {0, 0, columnWidth > 8 ? columnWidth - 8 : 1, 0};

    // DT_CALCRECT только измеряет прямоугольник, DT_WORDBREAK переносит слова
    DrawTextW(deviceContext, cellText[row][column], -1, &measuredRect,
              DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX);

    // Вернуть контексту прежний шрифт и удалить созданный
    SelectObject(deviceContext, previousFont);
    DeleteObject(font);

    // Высота текста плюс поля сверху и снизу
    return measuredRect.bottom + 8;
}

// Высота строки — максимум среди ячеек этой строки
static int MeasureRowHeight(HDC deviceContext, int row, int columnWidth, int fontSize)
{
    int column;
    int maxCellHeight = 1;
    int cellHeight;

    // Строка должна вместить самую высокую ячейку
    for (column = 0; column < COLS; ++column)
        if ((cellHeight = MeasureCellHeight(deviceContext, row, column,
                                            columnWidth, fontSize)) > maxCellHeight)
            maxCellHeight = cellHeight;
    return maxCellHeight;
}

// Суммарная высота таблицы при заданном кегле
static int MeasureTableHeight(HDC deviceContext, int columnWidth, int fontSize)
{
    int row;
    int totalHeight = 0;

    for (row = 0; row < ROWS; ++row)
        totalHeight += MeasureRowHeight(deviceContext, row, columnWidth, fontSize);
    return totalHeight;
}

// Наибольший кегль, при котором таблица ещё помещается в окно
static int FitFontSize(HDC deviceContext, int columnWidth)
{

    // Границы поиска кегля и лучший подходящий размер
    int minFontSize = 8, maxFontSize = 36, bestFontSize = 8;

    while (minFontSize <= maxFontSize) {

        // Середина текущего диапазона
        int middleFontSize = (minFontSize + maxFontSize) / 2;

        // Таблица влезает — запоминаем кегль и пробуем крупнее
        if (MeasureTableHeight(deviceContext, columnWidth, middleFontSize) <= clientHeight) {
            bestFontSize = middleFontSize;
            minFontSize = middleFontSize + 1;
        } else {

            // Не влезает — ищем меньший кегль
            maxFontSize = middleFontSize - 1;
        }
    }
    return bestFontSize;
}

// Рамка и текст: столбцы одной ширины, высота строки уже посчитана
static void DrawTable(HDC deviceContext)
{

    // Базовая ширина столбца и остаток пикселей от деления ширины окна
    int baseColumnWidth = clientWidth / COLS;
    int remainderPixels = clientWidth % COLS;
    int rowTop = 0;
    int row;
    int column;

    // Столбец уже одного пикселя — рисовать нечего
    if (baseColumnWidth < 1)
        return;

    for (row = 0; row < ROWS; ++row) {

        // Высота строки и левый край первой ячейки
        int rowHeight = MeasureRowHeight(deviceContext, row, baseColumnWidth, fontSizePx);
        int cellLeft = 0;

        for (column = 0; column < COLS; ++column) {

            // Первым столбцам отдаётся по одному пикселю остатка
            int cellWidth = baseColumnWidth + (column < remainderPixels);

            // Текст с отступом от рамки ячейки
            RECT textRect = {cellLeft + 4, rowTop + 4,
                             cellLeft + cellWidth - 4, rowTop + rowHeight - 4};

            // Шрифт этой ячейки выбирается в контекст устройства
            HFONT font = CellFont(row, column, fontSizePx);
            HFONT previousFont = (HFONT)SelectObject(deviceContext, font);

            // Рамка ячейки
            Rectangle(deviceContext, cellLeft, rowTop,
                      cellLeft + cellWidth + 1, rowTop + rowHeight + 1);

            // Текст с переносом по словам
            DrawTextW(deviceContext, cellText[row][column], -1, &textRect,
                      DT_WORDBREAK | DT_LEFT | DT_TOP | DT_NOPREFIX);

            // Вернуть прежний шрифт и удалить созданный
            SelectObject(deviceContext, previousFont);
            DeleteObject(font);

            // Следующий столбец
            cellLeft += cellWidth;
        }

        // Следующая строка
        rowTop += rowHeight;
    }
}

// Оконная процедура: перерисовка таблицы и закрытие окна
static LRESULT CALLBACK WndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    (void)wParam;
    (void)lParam;

    switch (message) {

    // Размер окна изменился — класс с CS_HREDRAW | CS_VREDRAW уже запросил перерисовку
    case WM_PAINT: {
        PAINTSTRUCT paintStruct;
        RECT clientRect;

        // Контекст на время рисования, при необходимости стирается фон
        HDC deviceContext = BeginPaint(window, &paintStruct);

        // Актуальный размер клиента: от него зависят столбцы и кегль
        GetClientRect(window, &clientRect);
        clientWidth = clientRect.right;
        clientHeight = clientRect.bottom;

        // Подбираем кегль, только если в окне помещаются все столбцы
        if (clientWidth >= COLS && clientHeight > 0)
            fontSizePx = FitFontSize(deviceContext, clientWidth / COLS);

        // Сетка и текст
        DrawTable(deviceContext);

        // Закончить рисование и снять область обновления
        EndPaint(window, &paintStruct);
        return 0;
    }

    // Закрытие окна завершает цикл сообщений
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }

    // Остальные сообщения обрабатывает система
    return DefWindowProcW(window, message, wParam, lParam);
}

// Точка входа: регистрация класса, создание окна, цикл сообщений
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE prevInstance, PWSTR commandLine, int showCommand)
{
    WNDCLASSW windowClass = {0};
    HWND window;
    MSG message;

    (void)prevInstance;
    (void)commandLine;

    // Полная перерисовка при изменении ширины или высоты окна
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = WndProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursor(NULL, IDC_ARROW);
    windowClass.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    windowClass.lpszClassName = L"Lab2Table";

    // Без зарегистрированного класса окно создать нельзя
    if (!RegisterClassW(&windowClass))
        return 1;

    // Окно с заголовком и рамкой
    window = CreateWindowW(L"Lab2Table", L"Таблица", WS_OVERLAPPEDWINDOW,
                            CW_USEDEFAULT, CW_USEDEFAULT, 900, 640,
                            NULL, NULL, instance, NULL);

    if (!window)
        return 1;

    // Показать окно и разбирать очередь, пока не придёт WM_QUIT
    ShowWindow(window, showCommand);
    while (GetMessageW(&message, NULL, 0, 0) > 0)
        DispatchMessageW(&message);
    return (int)message.wParam;
}
