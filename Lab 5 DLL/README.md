# Лабораторная 5 — динамически загружаемая библиотека (DLL)

Открывать в **CLion**: File → Open → папка `Lab 5 DLL`.

## Задание

- DLL с функцией поиска строки по виртуальной памяти процесса и замены на другую (одинаковая длина).
- Программа со **статическим** импортом DLL (`Lab5Static`).
- Программа с **динамическим** импортом (`LoadLibraryW` / `GetProcAddress`, `Lab5Dynamic`).
- Программа **внедрения** DLL в процесс через удалённый поток (`Lab5Inject` + `Lab5Target`).

## Сборка

1. Toolchain: MinGW-w64 или MSVC (в CLion: bundled MinGW + CMake).
2. Reload CMake Project → Build `all`.
3. Артефакты: `cmake-build-*` или локальный каталог `build/` — там `memreplace.dll` и все `.exe`.

Пример из командной строки (CLion):

```text
cmake -S "Lab 5 DLL" -B "Lab 5 DLL/build" -G Ninja ^
  -DCMAKE_C_COMPILER=".../mingw/bin/gcc.exe" ^
  -DCMAKE_MAKE_PROGRAM=ninja
cmake --build "Lab 5 DLL/build"
```

## Проверка

### Статический и динамический импорт

```text
Lab5Static.exe
Lab5Dynamic.exe
```

В выводе: адрес экспортируемой функции и замена `Lab5OldString!!` → `Lab5NewString!!` в локальном буфере.

### Внедрение (remote thread)

1. Запустить **`Lab5Target.exe`** — в консоли **PID** и строка `Lab5OldString!!`.
2. В **другой** консоли из каталога `build`:

```text
Lab5Inject.exe <PID>
```

`Lab5Dynamic.exe` **не принимает PID** — он работает только в своём процессе. Для мишени используйте **Lab5Inject**.

Инжектор:

1. `OpenProcess` + при необходимости `SeDebugPrivilege`
2. `VirtualAllocEx` / `WriteProcessMemory` — путь к DLL
3. `CreateRemoteThread` → `LoadLibraryW`
4. Вычисляет адрес `MemReplaceRemoteEntry` в целевом процессе (смещение от базы модуля)
5. Второй `CreateRemoteThread` вызывает замену строк в памяти **мишени**

В окне `Lab5Target` строка должна смениться на `Lab5NewString!!`.

## Файлы

| Файл | Назначение |
|------|------------|
| `memreplace_dll.c` | DLL: обход регионов `VirtualQuery`, `MemReplaceStringA`, `MemReplaceRemoteEntry` |
| `memreplace_api.h` | Экспорт / импорт и типы указателей на функции |
| `static_import.c` | `__declspec(dllimport)`, вызов через указатель на экспорт |
| `dynamic_import.c` | `LoadLibraryW` + `GetProcAddress` |
| `inject_remote.c` | LoadLibraryW и вызов функции DLL в чужом процессе |
| `target_host.c` | Долгоживущий процесс с тестовой строкой в RW-памяти |
| `build_report.py` | Генерация отчёта `otchet5_DLL.docx` |

## Отчёт

```text
pip install python-docx lxml
python build_report.py
```

Файл **`otchet5_DLL.docx`**: шапка «ФИО группа», затем код всех модулей; имена WinAPI и ключевых функций выделены полужирным (как в лаб. 4).
