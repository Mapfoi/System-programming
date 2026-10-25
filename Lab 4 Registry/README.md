# Лабораторная 4 — реестр (WinAPI / C)

Открывать в **CLion**: File - Open - папка `Lab 4 Registry` (подхватит `CMakeLists.txt`).

## Задание

- Добавление, изменение и удаление ключа `HKEY_CURRENT_USER\Software\Lab4Registry`
- Чтение флагов доступа к своему ключу и попытка прочитать их у `HKEY_LOCAL_MACHINE\SAM`
- Отслеживание изменения ключа
- Размер файлов реестра на диске

## Сборка в CLion

1. Toolchain: MinGW-w64 или MSVC
2. Reload CMake Project
3. Run configuration: `Lab4Registry`
