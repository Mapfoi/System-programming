#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string.h>

// Строки для демонстрации (длины search и replace должны совпадать)
#define LAB5_SEARCH "Lab5OldString!!"
#define LAB5_REPLACE "Lab5NewString!!"

// Можно ли писать в регион с такой маской защиты страниц
static BOOL PageWritable(DWORD protect)
{
    if (protect & (PAGE_GUARD | PAGE_NOACCESS))
        return FALSE;
    switch (protect & 0xFF) {
    case PAGE_READWRITE:
    case PAGE_WRITECOPY:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
        return TRUE;
    default:
        return FALSE;
    }
}

// Поиск search по виртуальной памяти текущего процесса и замена на replace
__declspec(dllexport) int __cdecl MemReplaceStringA(const char *search, const char *replace)
{
    SYSTEM_INFO si;
    unsigned char *addr;
    unsigned char *limit;
    size_t len;
    int count = 0;

    if (!search || !replace)
        return -1;
    len = strlen(search);
    if (len == 0 || len != strlen(replace))
        return -1;

    // Границы пользовательского адресного пространства процесса
    GetSystemInfo(&si);
    addr = (unsigned char *)si.lpMinimumApplicationAddress;
    limit = (unsigned char *)si.lpMaximumApplicationAddress;

    // Обход регионов памяти через VirtualQuery
    while (addr < limit) {
        MEMORY_BASIC_INFORMATION mbi;
        SIZE_T pos;

        if (VirtualQuery(addr, &mbi, sizeof(mbi)) != sizeof(mbi))
            break;

        // Только переданные в память регионы, куда разрешена запись
        if (mbi.State == MEM_COMMIT && PageWritable(mbi.Protect) && mbi.RegionSize >= len) {
            unsigned char *base = (unsigned char *)mbi.BaseAddress;
            for (pos = 0; pos + len <= mbi.RegionSize; ++pos) {
                if (memcmp(base + pos, search, len) != 0)
                    continue;
                {
                    DWORD old = 0;
                    // Временно разрешаем запись, подменяем байты, восстанавливаем права
                    if (!VirtualProtect(base + pos, len, PAGE_READWRITE, &old))
                        continue;
                    memcpy(base + pos, replace, len);
                    VirtualProtect(base + pos, len, old, &old);
                    count++;
                }
            }
        }

        addr = (unsigned char *)mbi.BaseAddress + mbi.RegionSize;
    }

    return count;
}

// Точка входа для CreateRemoteThread в чужом процессе (после LoadLibraryW)
__declspec(dllexport) DWORD WINAPI MemReplaceRemoteEntry(LPVOID lpParameter)
{
    int count;

    (void)lpParameter;
    count = MemReplaceStringA(LAB5_SEARCH, LAB5_REPLACE);
    return (count < 0) ? (DWORD)-1 : (DWORD)count;
}

// Точка входа DLL (здесь инициализация не требуется)
BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved)
{
    (void)hinst;
    (void)reason;
    (void)reserved;
    return TRUE;
}
