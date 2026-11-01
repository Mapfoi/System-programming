# Лабораторная 5 — динамически загружаемая библиотека (DLL)

Материал курса: **OSiSP.2021-09-29.pdf** — разделы о DLL (с. 46–52), виртуальной памяти (`VirtualQuery`, `VirtualProtect`, с. 118), внедрении DLL удалённым потоком (`CreateRemoteThread`, `VirtualAllocEx`, `WriteProcessMemory`, с. 156).

Открывать в **CLion**: File → Open → папка `Lab 5 DLL`.

## Задание (формулировка лабораторной)

- DLL с функцией поиска заданной строки в виртуальной памяти процесса и замены на другую (**одинаковая длина**).
- Программа со **статическим** импортом DLL, получающая указатель на экспорт.
- Программа с **динамическим** импортом (`LoadLibraryW`, `GetProcAddress`).
- Программа, **внедряющая** DLL в процесс через удалённый поток и получающая адрес функции DLL в этом процессе.

## Как это устроено

### 1. `memreplace.dll`

| Экспорт | Назначение |
|---------|------------|
| `MemReplaceStringA` | Обходит адресное пространство **текущего** процесса циклом `VirtualQuery`, в каждом переданном (`MEM_COMMIT`) и доступном для записи регионе ищет байтовую строку и заменяет её через `VirtualProtect` + `memcpy`. |
| `MemReplaceRemoteEntry` | Точка входа для второго `CreateRemoteThread` в чужом процессе: читает `MemReplaceParams` из памяти, переданной инжектором, и вызывает тот же поиск, но только в памяти **exe-мишени** и её `MEM_PRIVATE` (без сканирования образа `memreplace.dll`). |
| `DllMain` | Пустая заглушка — в курсе (с. 52) описаны сценарии `DLL_PROCESS_ATTACH` / `DLL_PROCESS_DETACH`; здесь инициализация не требуется. |

Поиск идёт по **всей** виртуальной памяти процесса (от `GetSystemInfo().lpMinimumApplicationAddress` до максимума), как требует задание. В регионы только для чтения запись не выполняется — иначе замена невозможна.

Демо-строки: `LAB5_DEMO_SEARCH` / `LAB5_DEMO_REPLACE` в `memreplace_api.h` (15 символов + `\0`).

### 2. `Lab5Static.exe` — импорт на старте (load-time)

Соответствует с. 47–48: `__declspec(dllimport)`, линковка с `memreplace.dll`, в EXE попадает таблица импорта.

Программа сохраняет указатель `fn = MemReplaceStringA`, печатает его адрес и вызывает замену в **своём** буфере `g_demoBuffer`.

### 3. `Lab5Dynamic.exe` — импорт во время работы (run-time)

Соответствует с. 50–51:

1. `LoadLibraryW` — загрузка `memreplace.dll` из каталога exe.
2. `GetProcAddress(..., "MemReplaceStringA")` — адрес экспорта в указатель `fn`.
3. Вызов через `fn`, затем `FreeLibrary`.

### 4. `Lab5Target.exe` + `Lab5Inject.exe` — удалённый поток (с. 156)

**Target** — долгоживущий процесс с тестовой строкой в блоке `VirtualAlloc` (`MEM_PRIVATE`), в консоли печатается **PID**.

**Inject** (PID мишени в аргументе):

1. `OpenProcess` с правами на поток и память.
2. **Первый удалённый поток:** в целевом процессе `VirtualAllocEx` → `WriteProcessMemory` (путь к DLL) → `CreateRemoteThread` → `LoadLibraryW` (адрес `LoadLibraryW` из локального `kernel32.dll` — общий для процессов).
3. База `memreplace.dll` в мишени — через `CreateToolhelp32Snapshot` / `Module32FirstW` (полный 64-битный адрес модуля).
4. Адрес `MemReplaceRemoteEntry` в мишени: локально `LoadLibrary` + `GetProcAddress`, смещение от базы модуля переносится на базу в целевом процессе.
5. **Второй удалённый поток:** `VirtualAllocEx` + `WriteProcessMemory` (`MemReplaceParams`) → `CreateRemoteThread` → `MemReplaceRemoteEntry`.
6. В консоли — адрес функции в целевом процессе и число замен.

**Важно:** `Lab5Dynamic` **не** принимает PID и не меняет память другого процесса. Для мишени нужен только **`Lab5Inject`**.

### Схема внедрения

```text
Lab5Inject (свой процесс)                Lab5Target (PID)
        |                                        |
        | CreateRemoteThread(LoadLibraryW)       |
        |--------------------------------------->| memreplace.dll загружена
        |                                        |
        | CreateRemoteThread(MemReplaceRemoteEntry)
        |--------------------------------------->| Scan → g_lab5Needle = "Lab5NewString!!"
```

## Сборка

1. Toolchain: MinGW-w64 или MSVC (CLion: bundled MinGW + CMake).
2. Reload CMake → Build `all`.
3. Артефакты: `build/` или `cmake-build-*` — `memreplace.dll` и все `.exe`.

```text
cmake -S "Lab 5 DLL" -B "Lab 5 DLL/build" -G Ninja
cmake --build "Lab 5 DLL/build"
```

## Проверка

```text
Lab5Static.exe
Lab5Dynamic.exe
Lab5Target.exe
Lab5Inject.exe <PID_из_Target>
```

После инъекции в окне Target строка должна стать `Lab5NewString!!`.

## Файлы

| Файл | Роль |
|------|------|
| `memreplace_api.h` | Экспорт/импорт, константы, типы |
| `memreplace_dll.c` | Ядро: поиск/замена, `DllMain` |
| `static_import.c` | Статический импорт |
| `dynamic_import.c` | `LoadLibraryW` / `GetProcAddress` |
| `inject_remote.c` | Внедрение DLL и вызов экспорта |
| `target_host.c` | Процесс-мишень |
| `build_report.py` | `otchet5_DLL.docx` |

## Отчёт

```text
pip install python-docx lxml
python build_report.py
```

В Word замените «ФИО группа» на свои данные.
