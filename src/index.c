#include "engine.h"
#include "native_stat.h"
#include <winioctl.h>
#include <stddef.h>
#include <stdlib.h>
#include <wchar.h>
#include <errno.h>

#define JOURNAL_BUFFER (1024 * 1024)
#define PATH_CHARS 32768
#define NAME_BLOCK_UNITS (32 * 1024)
#define SEARCH_BATCH_SIZE 512
#define SEARCH_WORKERS 4

typedef struct {
    DWORDLONG id;
    DWORDLONG parent;
    USN usn;
    wchar_t *name;
    DWORD attributes;
    DWORD name_signature;
} INDEX_ENTRY;

typedef struct NAME_BLOCK {
    struct NAME_BLOCK *next;
    size_t used;
    wchar_t text[NAME_BLOCK_UNITS];
} NAME_BLOCK;

typedef struct {
    HANDLE volume;
    DWORDLONG root;
    DWORDLONG journal;
    USN cursor;
    INDEX_ENTRY *entries;
    size_t capacity;
    size_t occupied;
    size_t live;
    INDEX_ENTRY *files;
    size_t file_capacity;
    size_t file_count;
    NAME_BLOCK *file_names;
    BYTE *buffer;
    const wchar_t *prefix;
    const wchar_t *directory;
    const wchar_t *watch_path;
    HANDLE notification;
    DWORDLONG watched_root;
    DWORD search_workers;
    TREE_RESULT totals;
} INDEX;

/* Only reject ASCII names here. Windows remains responsible for ordinal matching. */
static DWORD name_signature(const wchar_t *name, DWORD non_ascii)
{
    DWORD signature = 0, previous = 0;
    size_t i;
    for (i = 0; name[i]; ++i) {
        DWORD unit = name[i];
        if (unit > 127)
            return non_ascii;
        if (unit >= L'A' && unit <= L'Z')
            unit += L'a' - L'A';
        previous = (previous << 7) | unit;
        if (i >= 2) {
            DWORD triple = previous & 0x1fffff;
            signature |= 1UL << ((triple * 0x9e3779b1UL) >> 27);
        }
    }
    return signature;
}

static size_t entry_slot(const INDEX_ENTRY *entries, size_t capacity, DWORDLONG id)
{
    DWORDLONG hash = (id ^ (id >> 33)) * 0xff51afd7ed558ccdULL;
    size_t slot = (size_t)(hash ^ (hash >> 33)) & (capacity - 1);
    while (entries[slot].id && entries[slot].id != id)
        slot = (slot + 1) & (capacity - 1);
    return slot;
}

static void clear_entries(INDEX *index)
{
    size_t i;
    for (i = 0; i < index->capacity; ++i)
        free(index->entries[i].name);
    while (index->file_names) {
        NAME_BLOCK *block = index->file_names;
        index->file_names = block->next;
        free(block);
    }
    free(index->entries);
    free(index->files);
    index->entries = NULL;
    index->files = NULL;
    index->capacity = index->occupied = index->live = 0;
    index->file_capacity = index->file_count = 0;
}

static DWORD grow_entries(INDEX *index)
{
    size_t i, capacity = index->capacity ? index->capacity * 2 : 4096;
    INDEX_ENTRY *entries;
    if (capacity > SIZE_MAX / sizeof(*entries))
        return ERROR_NOT_ENOUGH_MEMORY;
    entries = calloc(capacity, sizeof(*entries));
    if (!entries)
        return ERROR_NOT_ENOUGH_MEMORY;
    for (i = 0; i < index->capacity; ++i) {
        INDEX_ENTRY *entry = &index->entries[i];
        if (entry->id)
            entries[entry_slot(entries, capacity, entry->id)] = *entry;
    }
    free(index->entries);
    index->entries = entries;
    index->capacity = capacity;
    return ERROR_SUCCESS;
}

static DWORD update_entry(INDEX *index, DWORDLONG id, DWORDLONG parent, USN usn,
                          DWORD attributes, const wchar_t *text, USHORT name_bytes)
{
    INDEX_ENTRY *entry;
    wchar_t *name = NULL;
    DWORD signature = 0;
    if (!id || id == index->root)
        return ERROR_SUCCESS;
    if (index->occupied >= index->capacity / 2) {
        DWORD error = grow_entries(index);
        if (error)
            return error;
    }
    entry = &index->entries[entry_slot(index->entries, index->capacity, id)];
    if (entry->id && entry->usn > usn)
        return ERROR_SUCCESS;
    if (text) {
        size_t units = name_bytes / sizeof(*name);
        if (entry->name && wcslen(entry->name) == units &&
            !memcmp(entry->name, text, name_bytes)) {
            name = entry->name;
            signature = entry->name_signature;
        } else {
            name = malloc(name_bytes + sizeof(*name));
            if (!name)
                return ERROR_NOT_ENOUGH_MEMORY;
            memcpy(name, text, name_bytes);
            name[units] = 0;
            signature = name_signature(name, MAXDWORD);
        }
    }
    if (!entry->id)
        ++index->occupied;
    if (entry->name)
        --index->live;
    if (entry->name != name)
        free(entry->name);
    if (name)
        ++index->live;
    *entry = (INDEX_ENTRY){ id, parent, usn, name, attributes, signature };
    return ERROR_SUCCESS;
}

static DWORD read_entries(INDEX *index, DWORD bytes, BOOL replay, USN limit)
{
    DWORD offset = sizeof(USN);
    if (bytes < offset)
        return ERROR_INVALID_DATA;
    while (offset < bytes) {
        const USN_RECORD_V2 *record = (const void *)(index->buffer + offset);
        const wchar_t *name;
        DWORD error;
        if (bytes - offset < offsetof(USN_RECORD_V2, FileName) ||
            record->MajorVersion != 2 || record->MinorVersion != 0 ||
            record->RecordLength < offsetof(USN_RECORD_V2, FileName) ||
            record->RecordLength > bytes - offset || (record->RecordLength & 7) ||
            record->FileNameOffset < offsetof(USN_RECORD_V2, FileName) ||
            record->FileNameOffset > record->RecordLength ||
            record->FileNameLength > record->RecordLength - record->FileNameOffset ||
            !record->FileNameLength || ((record->FileNameOffset | record->FileNameLength) & 1))
            return ERROR_INVALID_DATA;
        if (replay && record->Usn >= limit)
            return ERROR_SUCCESS;
        name = (const void *)((const BYTE *)record + record->FileNameOffset);
        if (replay && (record->Reason & USN_REASON_FILE_DELETE ||
                       ((record->Reason & USN_REASON_RENAME_OLD_NAME) &&
                        !(record->Reason & USN_REASON_RENAME_NEW_NAME))))
            name = NULL;
        error = update_entry(index, record->FileReferenceNumber, record->ParentFileReferenceNumber,
                             record->Usn, record->FileAttributes, name, record->FileNameLength);
        if (error)
            return error;
        offset += record->RecordLength;
    }
    return ERROR_SUCCESS;
}

static DWORD journal_info(INDEX *index, USN_JOURNAL_DATA_V0 *info)
{
    DWORD bytes;
    if (!DeviceIoControl(index->volume, FSCTL_QUERY_USN_JOURNAL, NULL, 0,
                         info, sizeof(*info), &bytes, NULL))
        return GetLastError();
    return bytes < sizeof(*info) ? ERROR_INVALID_DATA : ERROR_SUCCESS;
}

static DWORD refresh(INDEX *index)
{
    USN_JOURNAL_DATA_V0 info;
    DWORD error = journal_info(index, &info);
    if (error)
        return error;
    if (index->journal != info.UsnJournalID || index->cursor < info.FirstUsn ||
        index->cursor < info.LowestValidUsn || index->cursor > info.NextUsn)
        return ERROR_JOURNAL_ENTRY_DELETED;
    while (index->cursor < info.NextUsn) {
        READ_USN_JOURNAL_DATA_V1 read = { index->cursor, MAXDWORD, FALSE, 0, 0, index->journal, 2, 2 };
        USN next;
        DWORD bytes;
        if (!DeviceIoControl(index->volume, FSCTL_READ_USN_JOURNAL, &read, sizeof(read),
                             index->buffer, JOURNAL_BUFFER, &bytes, NULL))
            return GetLastError();
        error = read_entries(index, bytes, TRUE, info.NextUsn);
        if (error)
            return error;
        memcpy(&next, index->buffer, sizeof(next));
        if (next <= index->cursor)
            return ERROR_INVALID_DATA;
        index->cursor = min(next, info.NextUsn);
    }
    return ERROR_SUCCESS;
}

static DWORD rebuild(INDEX *index)
{
    USN_JOURNAL_DATA_V0 info;
    MFT_ENUM_DATA_V0 request = {0, 0, MAXLONGLONG};
    DWORD error = journal_info(index, &info);
    if (error)
        return error;
    clear_entries(index);
    index->journal = info.UsnJournalID;
    index->cursor = info.NextUsn;
    for (;;) {
        DWORD bytes;
        DWORDLONG next;
        if (!DeviceIoControl(index->volume, FSCTL_ENUM_USN_DATA, &request, sizeof(request),
                             index->buffer, JOURNAL_BUFFER, &bytes, NULL)) {
            error = GetLastError();
            if (error == ERROR_HANDLE_EOF || error == ERROR_NO_MORE_FILES)
                break;
            return error;
        }
        error = read_entries(index, bytes, FALSE, 0);
        if (error)
            return error;
        memcpy(&next, index->buffer, sizeof(next));
        if (next <= request.StartFileReferenceNumber)
            return ERROR_INVALID_DATA;
        request.StartFileReferenceNumber = next;
    }
    return refresh(index);
}

static const wchar_t *entry_path(const INDEX *index, const INDEX_ENTRY *entry, wchar_t *buffer)
{
    wchar_t *start = buffer + PATH_CHARS - 1;
    size_t depth = 0, prefix_length = wcslen(index->prefix);
    *start = 0;
    for (;;) {
        size_t length;
        if (!entry->name || ++depth > PATH_CHARS / 2)
            return NULL;
        length = wcslen(entry->name);
        if ((size_t)(start - buffer) < length + 1 + prefix_length)
            return NULL;
        start -= length;
        memcpy(start, entry->name, length * sizeof(*start));
        *--start = L'\\';
        if (entry->parent == index->root)
            break;
        entry = &index->entries[entry_slot(index->entries, index->capacity, entry->parent)];
        if ((entry->attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) !=
            FILE_ATTRIBUTE_DIRECTORY)
            return NULL;
    }
    start -= prefix_length;
    memcpy(start, index->prefix, prefix_length * sizeof(*start));
    return start;
}

static BOOL contains(const wchar_t *value, const wchar_t *needle)
{
    return FindStringOrdinal(FIND_FROMSTART, value, -1, needle, -1, TRUE) >= 0;
}

static BOOL verify_match(const INDEX_ENTRY *entry, const wchar_t *path, ULONGLONG min_size,
                         ULONGLONG max_size, ULONGLONG *size)
{
    BY_HANDLE_FILE_INFORMATION info;
    NATIVE_FILE_STAT stat;
    DWORDLONG id = 0;
    BOOL ok;
    HANDLE handle = CreateFileW(path, FILE_READ_ATTRIBUTES,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                               OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (handle == INVALID_HANDLE_VALUE)
        return FALSE;
    if (query_native_stat(handle, &stat)) {
        ok = TRUE;
        id = (DWORDLONG)stat.file_id.QuadPart;
        *size = (ULONGLONG)stat.end_of_file.QuadPart;
    } else {
        ok = GetFileInformationByHandle(handle, &info);
        if (ok) {
            id = ((DWORDLONG)info.nFileIndexHigh << 32) | info.nFileIndexLow;
            *size = ((ULONGLONG)info.nFileSizeHigh << 32) | info.nFileSizeLow;
        }
    }
    CloseHandle(handle);
    return ok && id == entry->id && *size >= min_size && *size <= max_size;
}

typedef struct {
    const INDEX *index;
    const wchar_t *path_needle;
    ULONGLONG low, high;
} SEARCH_QUERY;

static const wchar_t *lookup_match(const SEARCH_QUERY *query, const INDEX_ENTRY *entry,
                                   wchar_t *buffer, ULONGLONG *size)
{
    const wchar_t *path = entry_path(query->index, entry, buffer);
    if (!path || (query->path_needle && !contains(path + 4, query->path_needle)) ||
        !verify_match(entry, path, query->low, query->high, size))
        return NULL;
    return path;
}

typedef struct {
    const SEARCH_QUERY *query;
    const INDEX_ENTRY *const *entries;
    size_t count, matches;
    wchar_t *path;
    PTP_WORK work;
} SEARCH_WORKER;

static VOID CALLBACK count_matches(PTP_CALLBACK_INSTANCE instance, PVOID context, PTP_WORK work)
{
    SEARCH_WORKER *worker = context;
    size_t i;
    UNREFERENCED_PARAMETER(instance);
    UNREFERENCED_PARAMETER(work);
    worker->matches = 0;
    for (i = 0; i < worker->count; ++i) {
        ULONGLONG size;
        if (lookup_match(worker->query, worker->entries[i], worker->path, &size))
            ++worker->matches;
    }
}

static size_t count_batch(const SEARCH_QUERY *query, const INDEX_ENTRY *const *entries, size_t count,
                          SEARCH_WORKER *workers, DWORD worker_count)
{
    size_t i, matches = 0;
    for (i = 0; i < worker_count; ++i) {
        size_t start = i * count / worker_count;
        workers[i].query = query;
        workers[i].entries = entries + start;
        workers[i].count = (i + 1) * count / worker_count - start;
    }
    for (i = 1; i < worker_count; ++i)
        SubmitThreadpoolWork(workers[i].work);
    count_matches(NULL, workers, NULL);
    for (i = 1; i < worker_count; ++i)
        WaitForThreadpoolWorkCallbacks(workers[i].work, FALSE);
    for (i = 0; i < worker_count; ++i)
        matches += workers[i].matches;
    return matches;
}

static BOOL can_use_search_workers(void)
{
    HANDLE token;
    /* Pool callbacks use the process token. Preserve a caller's thread token. */
    if (!OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &token))
        return GetLastError() == ERROR_NO_TOKEN;
    CloseHandle(token);
    return FALSE;
}

static DWORD number(const wchar_t *value, ULONGLONG *out, wchar_t **end)
{
    if (*value < L'0' || *value > L'9')
        return ERROR_INVALID_PARAMETER;
    errno = 0;
    *out = _wcstoui64(value, end, 10);
    return errno ? ERROR_INVALID_PARAMETER : ERROR_SUCCESS;
}

static DWORD search(INDEX *index, wchar_t *mode, wchar_t *value, BOOL count_only)
{
    WCHAR *path_buffer;
    size_t i, matches = 0;
    size_t count = index->directory ? index->file_count : index->capacity;
    const INDEX_ENTRY *entries = index->directory ? index->files : index->entries;
    const INDEX_ENTRY *pending[SEARCH_BATCH_SIZE];
    SEARCH_WORKER workers[SEARCH_WORKERS] = {0};
    DWORD worker_count = 1;
    size_t pending_count = 0;
    BOOL parallel = count_only && index->search_workers > 1;
    BOOL pool_attempted = FALSE;
    BOOL by_size = !wcscmp(mode, L"size"), by_path = !wcscmp(mode, L"path");
    BOOL by_ext = !wcscmp(mode, L"ext");
    SEARCH_QUERY query = { index, by_path ? value : NULL, 0, MAXULONGLONG };
    DWORD required = 0;
    LARGE_INTEGER start, end, frequency;
    if (!*value)
        return ERROR_INVALID_PARAMETER;
    if (by_size) {
        wchar_t *end_value;
        if (number(value, &query.low, &end_value) || *end_value != L':' ||
            number(end_value + 1, &query.high, &end_value) || *end_value || query.low > query.high)
            return ERROR_INVALID_PARAMETER;
    } else if (!by_ext && !by_path && wcscmp(mode, L"name")) {
        return ERROR_INVALID_PARAMETER;
    } else if (!by_ext && !by_path) {
        required = name_signature(value, 0);
    }
    path_buffer = malloc(PATH_CHARS * sizeof(*path_buffer));
    if (!path_buffer)
        return ERROR_NOT_ENOUGH_MEMORY;
    workers[0].path = path_buffer;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start);
    for (i = 0; i < count; ++i) {
        const INDEX_ENTRY *entry = &entries[i];
        const wchar_t *path;
        ULONGLONG size;
        if (!entry->name || (entry->attributes & FILE_ATTRIBUTE_DIRECTORY))
            continue;
        if (by_ext) {
            const wchar_t *dot = wcsrchr(entry->name, L'.');
            if (!dot || CompareStringOrdinal(dot + 1, -1, value, -1, TRUE) != CSTR_EQUAL)
                continue;
        } else if (!by_path && !by_size && ((entry->name_signature & required) != required ||
                                          !contains(entry->name, value))) {
            continue;
        }
        if (parallel) {
            pending[pending_count++] = entry;
            if (pending_count == SEARCH_BATCH_SIZE) {
                if (!pool_attempted) {
                    DWORD limit = can_use_search_workers() ? index->search_workers : 1;
                    pool_attempted = TRUE;
                    while (worker_count < limit) {
                        SEARCH_WORKER *worker = &workers[worker_count];
                        worker->path = malloc(PATH_CHARS * sizeof(*worker->path));
                        if (!worker->path)
                            break;
                        worker->work = CreateThreadpoolWork(count_matches, worker, NULL);
                        if (!worker->work) {
                            free(worker->path);
                            break;
                        }
                        ++worker_count;
                    }
                }
                matches += count_batch(&query, pending, pending_count, workers, worker_count);
                pending_count = 0;
            }
            continue;
        }
        path = lookup_match(&query, entry, path_buffer, &size);
        if (!path)
            continue;
        ++matches;
        if (!count_only)
            wprintf(L"%llu\t%ls\n", size, path + 4);
    }
    if (pending_count)
        matches += count_batch(&query, pending, pending_count, workers,
                               pending_count < 32 ? 1 : worker_count);
    for (i = 1; i < worker_count; ++i) {
        CloseThreadpoolWork(workers[i].work);
        free(workers[i].path);
    }
    QueryPerformanceCounter(&end);
    wprintf(L"matches=%zu search_seconds=%.6f\n", matches,
            (double)(end.QuadPart - start.QuadPart) / (double)frequency.QuadPart);
    free(path_buffer);
    return ERROR_SUCCESS;
}

static DWORD snapshot_entry(void *context, ULONGLONG parent, const FILE_ID_EXTD_DIR_INFO *entry)
{
    INDEX *index = context;
    DWORDLONG id;
    const size_t length = entry->FileNameLength / sizeof(wchar_t);
    NAME_BLOCK *block;
    wchar_t *name;
    memcpy(&id, entry->FileId.Identifier, sizeof(id));
    if (entry->FileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        return update_entry(index, id, parent, 0, entry->FileAttributes, entry->FileName,
                            (USHORT)entry->FileNameLength);
    if (index->file_count == index->file_capacity) {
        size_t capacity = index->file_capacity ? index->file_capacity * 2 : 256;
        INDEX_ENTRY *files = capacity <= SIZE_MAX / sizeof(*files) ?
                             realloc(index->files, capacity * sizeof(*files)) : NULL;
        if (!files)
            return ERROR_NOT_ENOUGH_MEMORY;
        index->files = files;
        index->file_capacity = capacity;
    }
    if (length >= NAME_BLOCK_UNITS)
        return ERROR_INVALID_DATA;
    block = index->file_names;
    if (!block || NAME_BLOCK_UNITS - block->used <= length) {
        block = malloc(sizeof(*block));
        if (!block)
            return ERROR_NOT_ENOUGH_MEMORY;
        block->next = index->file_names;
        block->used = 0;
        index->file_names = block;
    }
    name = block->text + block->used;
    block->used += length + 1;
    memcpy(name, entry->FileName, entry->FileNameLength);
    name[length] = 0;
    index->files[index->file_count++] = (INDEX_ENTRY){
        id, parent, 0, name, entry->FileAttributes, name_signature(name, MAXDWORD)
    };
    return ERROR_SUCCESS;
}

static DWORD directory_identity(const wchar_t *path, DWORDLONG *id)
{
    BY_HANDLE_FILE_INFORMATION information;
    DWORD error = ERROR_SUCCESS;
    HANDLE handle = CreateFileW(path, FILE_READ_ATTRIBUTES,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                                OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (handle == INVALID_HANDLE_VALUE)
        return GetLastError();
    if (!GetFileInformationByHandle(handle, &information)) {
        error = GetLastError();
    } else if ((information.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) !=
               FILE_ATTRIBUTE_DIRECTORY) {
        error = ERROR_DIRECTORY;
    } else {
        *id = ((DWORDLONG)information.nFileIndexHigh << 32) | information.nFileIndexLow;
    }
    CloseHandle(handle);
    return error;
}

static DWORD snapshot_refresh(INDEX *index, BOOL initial)
{
    unsigned attempt;
    DWORDLONG current_id = 0;
    DWORD identity_error = directory_identity(index->watch_path, &current_id);
    if (identity_error)
        return identity_error;
    if (current_id != index->watched_root)
        return ERROR_RETRY;
    for (attempt = 0; attempt < 3; ++attempt) {
        DWORD state = WaitForSingleObject(index->notification, 0);
        DWORD error;
        if (state == WAIT_FAILED)
            return GetLastError();
        if (state == WAIT_OBJECT_0 && !FindNextChangeNotification(index->notification))
            return GetLastError();
        if (!initial && state == WAIT_TIMEOUT)
            return ERROR_SUCCESS;
        clear_entries(index);
        error = tree_snapshot(index->directory, snapshot_entry, index, &index->root, &index->totals);
        if (error)
            return error;
        if (index->root != index->watched_root)
            return ERROR_RETRY;
        state = WaitForSingleObject(index->notification, 0);
        if (state == WAIT_TIMEOUT)
            return ERROR_SUCCESS;
        if (state == WAIT_FAILED)
            return GetLastError();
        initial = TRUE;
    }
    return ERROR_BUSY;
}

static DWORD commands(INDEX *index, const wchar_t *storage_path)
{
    WCHAR input[4096];
    LARGE_INTEGER start, end, frequency;
    DWORD error;
    index->search_workers = min(storage_workers(storage_path), SEARCH_WORKERS);
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start);
    error = index->directory ? snapshot_refresh(index, TRUE) : rebuild(index);
    QueryPerformanceCounter(&end);
    if (!error) {
        wprintf(L"entries=%zu build_seconds=%.6f\n", index->live + index->file_count,
                (double)(end.QuadPart - start.QuadPart) / (double)frequency.QuadPart);
        fwprintf(stderr, L"name TEXT | ext EXT | path TEXT | size MIN:MAX | count MODE VALUE | %lsquit\n",
                 index->directory ? L"folder-size | " : L"");
        fflush(stdout);
    }
    while (!error && fgetws(input, ARRAYSIZE(input), stdin)) {
        wchar_t *value, *mode = input;
        BOOL count_only = FALSE, folder_size;
        size_t length = wcscspn(input, L"\r\n");
        if (!input[length] && length == ARRAYSIZE(input) - 1) {
            wint_t c;
            do {
                c = fgetwc(stdin);
            } while (c != WEOF && c != L'\n');
            print_error(L"query", ERROR_BUFFER_OVERFLOW);
            continue;
        }
        input[length] = 0;
        if (!wcscmp(input, L"quit"))
            break;
        if (!wcsncmp(mode, L"count ", 6)) {
            count_only = TRUE;
            mode += 6;
        }
        folder_size = index->directory && !count_only && !wcscmp(mode, L"folder-size");
        value = wcschr(mode, L' ');
        if (value) {
            *value++ = 0;
        } else if (!folder_size) {
            print_error(L"query", ERROR_INVALID_PARAMETER);
            continue;
        }
        if (index->directory) {
            error = snapshot_refresh(index, FALSE);
        } else {
            if (index->occupied - index->live > index->live / 2 + 65536)
                error = rebuild(index);
            else
                error = refresh(index);
            if (error == ERROR_JOURNAL_ENTRY_DELETED)
                error = rebuild(index);
        }
        if (!error) {
            if (folder_size) {
                const TREE_RESULT *totals = &index->totals;
                wprintf(L"files=%llu directories=%llu bytes=%llu\n", totals->files,
                        totals->directories, totals->bytes);
            } else {
                DWORD query_error = search(index, mode, value, count_only);
                if (query_error)
                    print_error(L"query", query_error);
            }
            fflush(stdout);
        }
    }
    return error;
}

DWORD directory_index_console(const wchar_t *input)
{
    INDEX index = {0};
    wchar_t *path = malloc(PATH_CHARS * sizeof(*path));
    DWORD length, error;
    if (!path)
        return ERROR_NOT_ENOUGH_MEMORY;
    memcpy(path, L"\\\\?\\", 4 * sizeof(*path));
    length = GetFullPathNameW(input, PATH_CHARS - 4, path + 4, NULL);
    if (!length || length >= PATH_CHARS - 4 || length < 3 || path[5] != L':' || path[6] != L'\\') {
        error = length ? ERROR_INVALID_NAME : GetLastError();
    } else {
        while (length > 3 && path[length + 3] == L'\\')
            path[--length + 4] = 0;
        index.directory = path + 4;
        index.watch_path = path;
        error = directory_identity(path, &index.watched_root);
        if (error) {
            free(path);
            return error;
        }
        index.notification = FindFirstChangeNotificationW(path, TRUE,
            FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_ATTRIBUTES |
            FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_SECURITY);
        if (index.notification == INVALID_HANDLE_VALUE) {
            error = GetLastError();
        } else {
            /* Keep the drive root valid for traversal and omit its slash only in joined paths. */
            wchar_t *prefix = _wcsdup(path);
            if (!prefix) {
                error = ERROR_NOT_ENOUGH_MEMORY;
            } else {
                while (length > 2 && prefix[length + 3] == L'\\')
                    prefix[--length + 4] = 0;
                index.prefix = prefix;
                error = commands(&index, index.directory);
                free(prefix);
            }
            FindCloseChangeNotification(index.notification);
        }
    }
    clear_entries(&index);
    free(path);
    return error;
}

DWORD index_console(const wchar_t *drive)
{
    INDEX index = {0};
    WCHAR root[] = L"X:\\", device[] = L"\\\\.\\X:", prefix[] = L"\\\\?\\X:", fs[16];
    DWORD error;
    if (wcslen(drive) != 2 || drive[1] != L':' ||
        !((drive[0] >= L'A' && drive[0] <= L'Z') || (drive[0] >= L'a' && drive[0] <= L'z')))
        return ERROR_INVALID_DRIVE;
    root[0] = device[4] = prefix[4] = drive[0];
    index.prefix = prefix;
    if (GetDriveTypeW(root) != DRIVE_FIXED)
        return ERROR_NOT_SUPPORTED;
    if (!GetVolumeInformationW(root, NULL, 0, NULL, NULL, NULL, fs, ARRAYSIZE(fs)))
        return GetLastError();
    if (_wcsicmp(fs, L"NTFS"))
        return ERROR_NOT_SUPPORTED;
    error = directory_identity(root, &index.root);
    if (error)
        return error;
    index.volume = CreateFileW(device, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_EXISTING, 0, NULL);
    if (index.volume == INVALID_HANDLE_VALUE)
        return GetLastError();
    index.buffer = malloc(JOURNAL_BUFFER);
    error = index.buffer ? commands(&index, root) : ERROR_NOT_ENOUGH_MEMORY;
    clear_entries(&index);
    free(index.buffer);
    CloseHandle(index.volume);
    return error;
}
