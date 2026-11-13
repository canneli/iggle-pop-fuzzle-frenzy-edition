#include <windows.h>
#include <shlobj.h>
#include <wchar.h>
#include "iggle_custom_levels.h"
#include "config.h"

#define MAX_LEVELS 512
typedef struct {
    wchar_t file[MAX_PATH];
    BOOL in_game;
    char label[64];
} LevelEntry;

static LevelEntry levels[MAX_LEVELS];
static int count;
static wchar_t selected_path[MAX_PATH];

BOOL iggle_custom_directory(wchar_t path[MAX_PATH])
{
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA | CSIDL_FLAG_CREATE, NULL, 0, path)))
        return FALSE;
    const wchar_t folder[] = L"\\IgglePopHD\\Levels";
    if (wcslen(path) + ARRAYSIZE(folder) >= MAX_PATH) return FALSE;
    wcscat(path, folder);
    int status = SHCreateDirectoryExW(NULL, path, NULL);
    return status == ERROR_SUCCESS || status == ERROR_ALREADY_EXISTS || status == ERROR_FILE_EXISTS;
}

static int compare_levels(const void *left, const void *right)
{
    return _wcsicmp(((const LevelEntry *)left)->file, ((const LevelEntry *)right)->file);
}

static BOOL game_directory(wchar_t path[MAX_PATH])
{
    wchar_t base[MAX_PATH];
    DWORD length = GetModuleFileNameW(NULL, base, ARRAYSIZE(base));
    if (!length || length >= ARRAYSIZE(base))
        return FALSE;
    wchar_t *filename = wcsrchr(base, L'\\');
    const wchar_t subdirectory[] = L"CustomLevels";
    if (!filename || filename - base + ARRAYSIZE(subdirectory) >= MAX_PATH) return FALSE;
    wcscpy(filename+1, subdirectory);
    wcscpy(path, base);
    return TRUE;
}

static void collect_levels(const wchar_t *directory, BOOL in_game)
{
    wchar_t wildcard[MAX_PATH];
    if (count >= MAX_LEVELS || swprintf(wildcard, MAX_PATH, L"%ls\\*.xml", directory) < 0) return;
    WIN32_FIND_DATAW data;
    HANDLE search = FindFirstFileW(wildcard, &data);
    if (search == INVALID_HANDLE_VALUE) return;
    do {
        DWORD excluded = FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT;
        if ((data.dwFileAttributes & excluded) || data.nFileSizeHigh ||
            data.nFileSizeLow < 32 || data.nFileSizeLow > 4000000) continue;
        size_t length = wcslen(data.cFileName);
        if (length <= 4 || _wcsicmp(data.cFileName + length - 4, L".xml")) continue;
        LevelEntry *entry = &levels[count++];
        entry->in_game = in_game;
        wcscpy(entry->file, data.cFileName);
        wchar_t label[43];
        size_t len = length - 4;
        size_t limit = len >= ARRAYSIZE(label) ? ARRAYSIZE(label) - 4 : len;
        wmemcpy(label, data.cFileName, limit);
        wcscpy(label + limit, limit == len ? L"" : L"...");
        WideCharToMultiByte(1252, 0, label, -1, entry->label, sizeof entry->label, "?", NULL);
    } while (count < MAX_LEVELS && FindNextFileW(search, &data));
    FindClose(search);
}

int iggle_custom_refresh(void)
{
    wchar_t directory[MAX_PATH];
    count = 0;
    if (game_directory(directory)) collect_levels(directory, TRUE);
    if (iggle_custom_directory(directory)) collect_levels(directory, FALSE);
    qsort(levels, count, sizeof levels[0], compare_levels);
    return count;
}

BOOL iggle_custom_path_at(int index, wchar_t path[MAX_PATH])
{
    wchar_t directory[MAX_PATH];
    if (index < 0 || index >= count) return FALSE;
    LevelEntry *entry = &levels[index];
    if (!(entry->in_game ? game_directory(directory) : iggle_custom_directory(directory))) return FALSE;
    int written = swprintf(path, MAX_PATH, L"%ls\\%ls", directory, entry->file);
    return written > 0 && written < MAX_PATH;
}

BOOL iggle_custom_in_game(int index)
{
    return index >= 0 && index < count && levels[index].in_game;
}

BOOL iggle_custom_new_path(wchar_t path[MAX_PATH])
{
    wchar_t directory[MAX_PATH];
    if (!game_directory(directory)) return FALSE;
    if (!CreateDirectoryW(directory, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) return FALSE;
    const wchar_t *names[] = { L"Level", L"Fase", L"Nivel" };
    for (int i = 1; i <= 9999; ++i) {
        if (swprintf(path, MAX_PATH, L"%ls\\%ls %03d.xml", directory, names[iggle_current_language()], i) < 0)
            return FALSE;
        if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
            DWORD error = GetLastError();
            return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
        }
    }
    return FALSE;
}

BOOL iggle_custom_select_path(const wchar_t *path)
{
    if (wcslen(path) >= MAX_PATH || !path[0]) return FALSE;
    wcscpy(selected_path, path);
    return TRUE;
}

const char *iggle_custom_name(int index)
{
    return index >= 0 && index < count ? levels[index].label : "";
}

BOOL iggle_custom_select(int index)
{
    selected_path[0] = 0;
    return iggle_custom_path_at(index, selected_path);
}

BOOL iggle_custom_get_path(char path[MAX_PATH])
{
    if (!selected_path[0]) return FALSE;
    wchar_t short_path[MAX_PATH];
    DWORD n = GetShortPathNameW(selected_path, short_path, ARRAYSIZE(short_path));
    const wchar_t *source = n && n < ARRAYSIZE(short_path) ? short_path : selected_path;
    BOOL substituted = FALSE;
    UINT code_page = GetACP();
    BOOL ok = WideCharToMultiByte(code_page, 0, source, -1, path, MAX_PATH, NULL,
        code_page == CP_UTF8 ? NULL : &substituted) > 0;
    selected_path[0] = 0;
    return ok && !substituted;
}
