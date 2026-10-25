#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>

// Реестр хранит настройки ОС и программ в одном месте: разделы, ключи и значения
// Реестр есть в Windows; в других ОС такого реестра нет, настройки лежат в отдельных файлах
// Главные разделы: HKEY_LOCAL_MACHINE, HKEY_USERS
// Короткие пути: HKEY_CURRENT_USER, HKEY_CLASSES_ROOT, HKEY_CURRENT_CONFIG
// Функции реестра — в Advapi32.dll
// Править реестр можно с помощью regedit.exe, reg.exe и PowerShell
// Вес реестра — размер его файлов на диске

static const wchar_t kKey[] = L"Software\\Lab4Registry";
static const wchar_t kValue[] = L"Sample";

static void PrintResult(const wchar_t *what, LONG status)
{
    if (status == ERROR_SUCCESS)
        wprintf(L"%ls\n", what);
    else
        wprintf(L"%ls, ошибка %ld\n", what, status);
}

static void PrintMegabytes(const wchar_t *path, unsigned long long bytes)
{
    wprintf(L"%ls  %.2f МБ\n", path, (double)bytes / (1024.0 * 1024.0));
}

// Размер файлов реестра
static void PrintRegistrySize(void)
{
    static const wchar_t *hives[] = {
        L"SYSTEM", L"SOFTWARE", L"SAM", L"SECURITY", L"DEFAULT"
    };
    wchar_t root[MAX_PATH];
    wchar_t profile[MAX_PATH];
    wchar_t path[MAX_PATH];
    WIN32_FILE_ATTRIBUTE_DATA info;
    unsigned long long total = 0;
    int i;

    if (!GetEnvironmentVariableW(L"SystemRoot", root, MAX_PATH) ||
        !GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH)) {
        wprintf(L"не найден путь к файлам реестра\n");
        return;
    }

    wprintf(L"Вес реестра:\n");
    // SYSTEM, SOFTWARE, SAM, SECURITY, DEFAULT — файлы раздела HKEY_LOCAL_MACHINE
    for (i = 0; i < 5; ++i) {
        unsigned long long size;
        swprintf(path, MAX_PATH, L"%ls\\System32\\config\\%ls", root, hives[i]);
        if (!GetFileAttributesExW(path, GetFileExInfoStandard, &info)) {
            DWORD err = GetLastError();
            wprintf(L"%ls  %ls\n", path,
                    err == ERROR_ACCESS_DENIED ? L"нет доступа" : L"не удалось прочитать размер");
            continue;
        }
        size = ((unsigned long long)info.nFileSizeHigh << 32) | info.nFileSizeLow;
        total += size;
        PrintMegabytes(path, size);
    }

    // NTUSER.DAT — файл текущего пользователя, короткий путь HKEY_CURRENT_USER
    swprintf(path, MAX_PATH, L"%ls\\NTUSER.DAT", profile);
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &info)) {
        DWORD err = GetLastError();
        wprintf(L"%ls  %ls\n", path,
                err == ERROR_ACCESS_DENIED ? L"нет доступа" : L"не удалось прочитать размер");
    } else {
        unsigned long long size =
            ((unsigned long long)info.nFileSizeHigh << 32) | info.nFileSizeLow;
        total += size;
        PrintMegabytes(path, size);
    }
    wprintf(L"всего:  %.2f МБ\n", (double)total / (1024.0 * 1024.0));
}

static void PrintMask(DWORD mask)
{
    static const struct {
        DWORD flag;
        const wchar_t *name;
    } flags[] = {
        { KEY_QUERY_VALUE, L"чтение значения" },
        { KEY_SET_VALUE, L"запись значения" },
        { KEY_CREATE_SUB_KEY, L"создание подраздела" },
        { KEY_ENUMERATE_SUB_KEYS, L"перечень подразделов" },
        { KEY_NOTIFY, L"слежение за изменениями" },
        { READ_CONTROL, L"чтение прав" },
        { WRITE_DAC, L"изменение прав" },
        { WRITE_OWNER, L"смена владельца" },
    };
    int i, first = 1;
    wprintf(L"0x%08lX  ", mask);
    for (i = 0; i < 8; ++i) {
        if (!(mask & flags[i].flag))
            continue;
        if (!first)
            wprintf(L", ");
        first = 0;
        wprintf(L"%ls", flags[i].name);
    }
}

// Прочитать флаги доступа к ключу реестра
static void PrintAccess(HKEY root, const wchar_t *path, const wchar_t *title)
{
    HKEY key;
    LONG status;
    DWORD bytes = 256;
    BYTE stack[256];
    BYTE *heap = NULL;
    PSECURITY_DESCRIPTOR sd = stack;
    BOOL present = FALSE;
    BOOL defaulted = FALSE;
    PACL dacl = NULL;
    ACL_SIZE_INFORMATION info;
    DWORD i;

    wprintf(L"\nФлаги доступа %ls\n", title);
    status = RegOpenKeyExW(root, path, 0, READ_CONTROL, &key);
    if (status != ERROR_SUCCESS) {
        PrintResult(L"открыть ключ не удалось", status);
        return;
    }
    status = RegGetKeySecurity(key, DACL_SECURITY_INFORMATION, sd, &bytes);
    if (status == ERROR_INSUFFICIENT_BUFFER) {
        heap = (BYTE *)malloc(bytes);
        if (!heap) {
            RegCloseKey(key);
            return;
        }
        sd = heap;
        status = RegGetKeySecurity(key, DACL_SECURITY_INFORMATION, sd, &bytes);
    }
    RegCloseKey(key);
    if (status != ERROR_SUCCESS) {
        PrintResult(L"прочитать флаги не удалось", status);
        free(heap);
        return;
    }
    if (!GetSecurityDescriptorDacl(sd, &present, &dacl, &defaulted) || !present || !dacl) {
        wprintf(L"список доступа отсутствует\n");
        free(heap);
        return;
    }
    if (!GetAclInformation(dacl, &info, sizeof info, AclSizeInformation)) {
        wprintf(L"GetAclInformation: %lu\n", GetLastError());
        free(heap);
        return;
    }
    for (i = 0; i < info.AceCount; ++i) {
        void *ace = NULL;
        ACE_HEADER *header;
        if (!GetAce(dacl, i, &ace))
            continue;
        header = (ACE_HEADER *)ace;
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE &&
            header->AceType != ACCESS_DENIED_ACE_TYPE)
            continue;
        wprintf(L"  %lu. %ls  ", i + 1,
                header->AceType == ACCESS_DENIED_ACE_TYPE ? L"запрет" : L"разрешение");
        PrintMask(((ACCESS_ALLOWED_ACE *)ace)->Mask);
        wprintf(L"\n");
    }
    free(heap);
}

int main(void)
{
    HKEY key = NULL;
    HANDLE changed = NULL;
    LONG status;
    wchar_t data[64];
    DWORD size;
    DWORD type;
    const wchar_t *added = L"добавлено";
    const wchar_t *edited = L"изменено";
    const wchar_t *watched = L"слежение";

    // Вывод в консоль
    _setmode(_fileno(stdout), _O_U16TEXT);
    PrintRegistrySize();

    wprintf(L"\nКлюч HKEY_CURRENT_USER\\%ls\n", kKey);
    // Добавить ключ реестра
    status = RegCreateKeyExW(HKEY_CURRENT_USER, kKey, 0, NULL, 0,
                              KEY_READ | KEY_WRITE | KEY_NOTIFY, NULL, &key, NULL);
    PrintResult(L"ключ создан", status);
    if (status != ERROR_SUCCESS)
        return 1;

    // Добавить значение
    status = RegSetValueExW(key, kValue, 0, REG_SZ, (const BYTE *)added,
                             (DWORD)((wcslen(added) + 1) * sizeof(wchar_t)));
    PrintResult(L"значение Sample добавлено", status);

    // Изменить значение
    status = RegSetValueExW(key, kValue, 0, REG_SZ, (const BYTE *)edited,
                             (DWORD)((wcslen(edited) + 1) * sizeof(wchar_t)));
    PrintResult(L"значение Sample изменено", status);

    size = sizeof data;
    status = RegQueryValueExW(key, kValue, NULL, &type, (BYTE *)data, &size);
    if (status == ERROR_SUCCESS)
        wprintf(L"прочитано значение: %ls\n", data);
    else
        PrintResult(L"прочитать значение не удалось", status);

    // Свой ключ и раздел HKEY_LOCAL_MACHINE\SAM
    PrintAccess(HKEY_CURRENT_USER, kKey, L"HKEY_CURRENT_USER\\Software\\Lab4Registry");
    PrintAccess(HKEY_LOCAL_MACHINE, L"SAM", L"HKEY_LOCAL_MACHINE\\SAM");

    // Отследить изменение ключа
    wprintf(L"\nСлежение за ключом:\n");
    changed = CreateEventW(NULL, FALSE, FALSE, NULL);
    status = RegNotifyChangeKeyValue(key, FALSE, REG_NOTIFY_CHANGE_LAST_SET, changed, TRUE);
    PrintResult(L"ожидание изменения установлено", status);
    status = RegSetValueExW(key, kValue, 0, REG_SZ, (const BYTE *)watched,
                             (DWORD)((wcslen(watched) + 1) * sizeof(wchar_t)));
    PrintResult(L"значение изменено ещё раз", status);
    if (WaitForSingleObject(changed, 5000) == WAIT_OBJECT_0)
        wprintf(L"ключ изменился\n");
    else
        wprintf(L"изменение ключа не пришло\n");

    // Удалить значение и ключ
    wprintf(L"\nУдаление:\n");
    status = RegDeleteValueW(key, kValue);
    PrintResult(L"значение удалено", status);
    RegCloseKey(key);
    status = RegDeleteKeyW(HKEY_CURRENT_USER, kKey);
    PrintResult(L"ключ удалён", status);
    CloseHandle(changed);
    fflush(stdout);
    system("pause");
    return 0;
}
