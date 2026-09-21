# Лабораторная 2 — текстовая таблица (WinAPI / C)

Открывать в **CLion**: File - Open - папка `Lab 2 Table` (подхватит `CMakeLists.txt`).

## Задание

- Таблица **N×M** в окне (макросы `ROWS` / `COLS` в `main.c`)
- Столбцы равномерно по ширине
- Высота строк по содержимому ячеек (`DrawText` + `DT_CALCRECT` + `DT_WORDBREAK`)
- При `WM_SIZE` — пересчёт и перерисовка

## Сборка в CLion

1. Toolchain: MinGW-w64 или MSVC
2. Reload CMake Project
3. Run configuration: `Lab2Table`
