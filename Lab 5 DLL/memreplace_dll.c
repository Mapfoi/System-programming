#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string.h>

#define LAB5_PAIR_MAX 260

typedef struct {
    char find[LAB5_PAIR_MAX];
    char replace[LAB5_PAIR_MAX];
} MemReplaceRemoteParams;

static BOOL RegionWritable(DWORD protect)
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

    GetSystemInfo(&si);
    addr = (unsigned char *)si.lpMinimumApplicationAddress;
    limit = (unsigned char *)si.lpMaximumApplicationAddress;

    while (addr < limit) {
        MEMORY_BASIC_INFORMATION mbi;
        SIZE_T pos;

        if (VirtualQuery(addr, &mbi, sizeof(mbi)) != sizeof(mbi))
            break;

        if (mbi.State == MEM_COMMIT && RegionWritable(mbi.Protect) && mbi.RegionSize >= len) {
            unsigned char *base = (unsigned char *)mbi.BaseAddress;
            for (pos = 0; pos + len <= mbi.RegionSize; ++pos) {
                if (memcmp(base + pos, search, len) != 0)
                    continue;
                memcpy(base + pos, replace, len);
                count++;
            }
        }

        addr = (unsigned char *)mbi.BaseAddress + mbi.RegionSize;
    }

    return count;
}

// lpParameter — указатель на MemReplaceRemoteParams в памяти мишени (find / replace)
__declspec(dllexport) DWORD WINAPI MemReplaceRemoteEntry(LPVOID lpParameter)
{
    MemReplaceRemoteParams *params = (MemReplaceRemoteParams *)lpParameter;
    int count;

    if (!params || !params->find[0])
        return (DWORD)-1;
    count = MemReplaceStringA(params->find, params->replace);
    return (count < 0) ? (DWORD)-1 : (DWORD)count;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID reserved)
{
    (void)h;
    (void)reason;
    (void)reserved;
    return TRUE;
}
