#define COBJMACROS
#include "engine.h"
#include "directory_search.h"
#include "exact_name_query.h"
#include "native_stat.h"
#include <shlobj.h>
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
#define EXACT_INDEX_BYTES (8 * 1024 * 1024)
#define NOTIFICATION_BYTES (64 * 1024)
#define MAX_DIRTY_DIRECTORIES 16
#define SHELL_SNAPSHOT_ENTRIES 100000

typedef struct {
    DWORDLONG id;
    DWORDLONG parent;
    union {
        USN usn;
        ULONGLONG size;
    };
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
    const INDEX_ENTRY **exact_names;
    size_t exact_count;
    BOOL exact_attempted;
    NAME_BLOCK *file_names;
    size_t file_name_units, stale_name_units;
    BYTE *buffer;
    const wchar_t *prefix;
    const wchar_t *directory;
    const wchar_t *watch_path;
    HANDLE directory_watch;
    OVERLAPPED notification;
    BOOL watch_pending;
    DWORDLONG watched_root;
    DWORD watched_volume;
    ULONGLONG full_refreshes, refreshed_directories;
    DWORD search_workers;
    BOOL shell_scope;
    TREE_RESULT totals;
} INDEX;

struct DIRECTORY_SEARCH {
    INDEX index;
    wchar_t *path, *prefix;
    BOOL ready;
};

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

static void clear_exact_names(INDEX *index)
{
    free(index->exact_names);
    index->exact_names = NULL;
    index->exact_count = 0;
    index->exact_attempted = FALSE;
}

static void clear_entries(INDEX *index)
{
    size_t i;
    clear_exact_names(index);
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
    index->file_name_units = index->stale_name_units = 0;
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
    clear_exact_names(index);
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
    if (index->exact_attempted &&
        (name != entry->name || ((attributes ^ entry->attributes) & FILE_ATTRIBUTE_DIRECTORY)))
        clear_exact_names(index);
    if (entry->name != name)
        free(entry->name);
    if (name)
        ++index->live;
    *entry = (INDEX_ENTRY){ .id = id, .parent = parent, .usn = usn, .name = name,
                            .attributes = attributes, .name_signature = signature };
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

typedef struct {
    ULONGLONG parent;
    size_t length;
    wchar_t path[1024];
} PARENT_PATH;

static wchar_t *entry_path_cached(const INDEX *index, const INDEX_ENTRY *entry,
                                  wchar_t *buffer, PARENT_PATH *cache)
{
    wchar_t *start = buffer + PATH_CHARS - 1;
    size_t depth = 0, prefix_length = wcslen(index->prefix);
    size_t name_length;
    ULONGLONG parent = entry->parent;
    if (!entry->name)
        return NULL;
    name_length = wcslen(entry->name);
    if (cache && cache->length && cache->parent == parent) {
        if (cache->length + 1 + name_length >= PATH_CHARS)
            return NULL;
        memcpy(buffer, cache->path, cache->length * sizeof(*buffer));
        buffer[cache->length] = L'\\';
        memcpy(buffer + cache->length + 1, entry->name, (name_length + 1) * sizeof(*buffer));
        return buffer;
    }
    *start = 0;
    for (;;) {
        size_t length;
        if (!entry->name || ++depth > PATH_CHARS / 2)
            return NULL;
        length = depth == 1 ? name_length : wcslen(entry->name);
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
    if (cache) {
        size_t length = (size_t)(buffer + PATH_CHARS - 1 - start) - name_length - 1;
        if (length <= ARRAYSIZE(cache->path)) {
            memcpy(cache->path, start, length * sizeof(*start));
            cache->parent = parent;
            cache->length = length;
        }
    }
    return start;
}

static const wchar_t *entry_path(const INDEX *index, const INDEX_ENTRY *entry, wchar_t *buffer)
{
    return entry_path_cached(index, entry, buffer, NULL);
}

static BOOL contains(const wchar_t *value, const wchar_t *needle)
{
    return FindStringOrdinal(FIND_FROMSTART, value, -1, needle, -1, TRUE) >= 0;
}

static BOOL verify_match(const INDEX_ENTRY *entry, wchar_t *path, ULONGLONG min_size,
                         ULONGLONG max_size, ULONGLONG *size, DWORD *attributes)
{
    BY_HANDLE_FILE_INFORMATION info;
    NATIVE_FILE_STAT stat;
    DWORDLONG id = 0;
    BOOL ok;
    HANDLE handle;
    if (open_indexed_file(path, &handle))
        return FALSE;
    if (query_native_stat(handle, &stat)) {
        ok = TRUE;
        id = (DWORDLONG)stat.file_id.QuadPart;
        *size = (ULONGLONG)stat.end_of_file.QuadPart;
        if (attributes)
            *attributes = stat.attributes;
    } else {
        ok = GetFileInformationByHandle(handle, &info);
        if (ok) {
            id = ((DWORDLONG)info.nFileIndexHigh << 32) | info.nFileIndexLow;
            *size = ((ULONGLONG)info.nFileSizeHigh << 32) | info.nFileSizeLow;
            if (attributes)
                *attributes = info.dwFileAttributes;
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
                                   wchar_t *buffer, PARENT_PATH *cache, ULONGLONG *size)
{
    wchar_t *path = entry_path_cached(query->index, entry, buffer, cache);
    if (!path || (query->path_needle && !contains(path + 4, query->path_needle)) ||
        !verify_match(entry, path, query->low, query->high, size, NULL))
        return NULL;
    return path;
}

typedef struct {
    const SEARCH_QUERY *query;
    const INDEX_ENTRY *const *entries;
    size_t count, matches;
    wchar_t *path;
    PARENT_PATH parent_path;
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
        if (lookup_match(worker->query, worker->entries[i], worker->path, &worker->parent_path, &size))
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

static BOOL uses_process_token(void)
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

static int compare_exact_names(const void *left, const void *right)
{
    const INDEX_ENTRY *const *first = left, *const *second = right;
    return CompareStringOrdinal((*first)->name, -1, (*second)->name, -1, TRUE) - CSTR_EQUAL;
}

static void build_exact_names(INDEX *index, const INDEX_ENTRY *entries, size_t count)
{
    const size_t maximum = index->directory ? index->file_count + (index->shell_scope ? index->live : 0) :
                                             index->live;
    size_t i;
    if (index->exact_attempted)
        return;
    index->exact_attempted = TRUE;
    if (!maximum || maximum > EXACT_INDEX_BYTES / sizeof(*index->exact_names))
        return;
    index->exact_names = malloc(maximum * sizeof(*index->exact_names));
    if (!index->exact_names)
        return;
    for (i = 0; i < count; ++i) {
        if (entries[i].name && !(entries[i].attributes & FILE_ATTRIBUTE_DIRECTORY))
            index->exact_names[index->exact_count++] = &entries[i];
    }
    if (index->shell_scope) {
        for (i = 0; i < index->capacity; ++i) {
            if (index->entries[i].name)
                index->exact_names[index->exact_count++] = &index->entries[i];
        }
    }
    qsort(index->exact_names, index->exact_count, sizeof(*index->exact_names), compare_exact_names);
}

static size_t exact_name_bound(const INDEX *index, const wchar_t *name, BOOL upper)
{
    size_t first = 0, end = index->exact_count;
    while (first < end) {
        size_t middle = first + (end - first) / 2;
        int order = CompareStringOrdinal(index->exact_names[middle]->name, -1, name, -1, TRUE);
        if (order == CSTR_LESS_THAN || (upper && order == CSTR_EQUAL))
            first = middle + 1;
        else
            end = middle;
    }
    return first;
}

static DWORD search(INDEX *index, wchar_t *mode, wchar_t *value, BOOL count_only)
{
    WCHAR *path_buffer;
    size_t i, first = 0, matches = 0;
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
    BOOL by_exact = !wcscmp(mode, L"name-exact");
    BOOL exact_sorted = FALSE;
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
    } else if (!by_ext && !by_path && !by_exact && wcscmp(mode, L"name")) {
        return ERROR_INVALID_PARAMETER;
    } else if (!by_ext && !by_path && !by_exact) {
        required = name_signature(value, 0);
    }
    path_buffer = malloc(PATH_CHARS * sizeof(*path_buffer));
    if (!path_buffer)
        return ERROR_NOT_ENOUGH_MEMORY;
    workers[0].path = path_buffer;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start);
    if (by_exact) {
        build_exact_names(index, entries, count);
        if (index->exact_names) {
            first = exact_name_bound(index, value, FALSE);
            count = exact_name_bound(index, value, TRUE);
            exact_sorted = TRUE;
        }
    }
    for (i = first; i < count; ++i) {
        const INDEX_ENTRY *entry = exact_sorted ? index->exact_names[i] : &entries[i];
        const wchar_t *path;
        ULONGLONG size;
        if (!entry->name || (entry->attributes & FILE_ATTRIBUTE_DIRECTORY))
            continue;
        if (by_ext) {
            const wchar_t *dot = wcsrchr(entry->name, L'.');
            if (!dot || CompareStringOrdinal(dot + 1, -1, value, -1, TRUE) != CSTR_EQUAL)
                continue;
        } else if (by_exact) {
            if (!exact_sorted && CompareStringOrdinal(entry->name, -1, value, -1, TRUE) != CSTR_EQUAL)
                continue;
        } else if (!by_path && !by_size && ((entry->name_signature & required) != required ||
                                          !contains(entry->name, value))) {
            continue;
        }
        if (parallel) {
            pending[pending_count++] = entry;
            if (pending_count == SEARCH_BATCH_SIZE) {
                if (!pool_attempted) {
                    DWORD limit = uses_process_token() ? index->search_workers : 1;
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
        path = lookup_match(&query, entry, path_buffer, &workers[0].parent_path, &size);
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

static BOOL snapshot_entry_supported(const INDEX *index, DWORD attributes)
{
    return !index->shell_scope ||
           (index->file_count + index->live < SHELL_SNAPSHOT_ENTRIES &&
            !(attributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_OFFLINE)));
}

static DWORD snapshot_file(INDEX *index, ULONGLONG parent, DWORDLONG id, const FILE_ID_EXTD_DIR_INFO *entry)
{
    const size_t length = entry->FileNameLength / sizeof(wchar_t);
    ULONGLONG size = entry->FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT ?
                     0 : (ULONGLONG)entry->EndOfFile.QuadPart;
    NAME_BLOCK *block;
    wchar_t *name;
    if (!snapshot_entry_supported(index, entry->FileAttributes))
        return ERROR_NOT_SUPPORTED;
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
    index->file_name_units += length + 1;
    memcpy(name, entry->FileName, entry->FileNameLength);
    name[length] = 0;
    index->files[index->file_count++] = (INDEX_ENTRY){
        .id = id, .parent = parent, .size = size, .name = name, .attributes = entry->FileAttributes,
        .name_signature = name_signature(name, MAXDWORD)
    };
    return ERROR_SUCCESS;
}

static DWORD snapshot_entry(void *context, HANDLE directory, ULONGLONG parent,
                            const FILE_ID_EXTD_DIR_INFO *entry)
{
    UNREFERENCED_PARAMETER(directory);
    INDEX *index = context;
    DWORDLONG id;
    memcpy(&id, entry->FileId.Identifier, sizeof(id));
    if (entry->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        if (!snapshot_entry_supported(index, entry->FileAttributes))
            return ERROR_NOT_SUPPORTED;
        return update_entry(index, id, parent, 0, entry->FileAttributes, entry->FileName,
                            (USHORT)entry->FileNameLength);
    }
    return snapshot_file(index, parent, id, entry);
}

static DWORD directory_identity(const wchar_t *path, DWORDLONG *id, DWORD *serial)
{
    BY_HANDLE_FILE_INFORMATION information;
    DWORD error = ERROR_SUCCESS;
    HANDLE handle = CreateFileW(path, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
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
        if (serial)
            *serial = information.dwVolumeSerialNumber;
    }
    CloseHandle(handle);
    return error;
}

typedef struct {
    DWORDLONG directories[MAX_DIRTY_DIRECTORIES];
    DWORDLONG objects[MAX_DIRTY_DIRECTORIES];
    size_t directory_count, object_count;
    BOOL rebuild;
} DIRECTORY_CHANGES;

static BOOL add_changed_id(DWORDLONG *values, size_t *count, DWORDLONG id)
{
    size_t i;
    for (i = 0; i < *count; ++i) {
        if (values[i] == id)
            return TRUE;
    }
    if (!id || *count == MAX_DIRTY_DIRECTORIES)
        return FALSE;
    values[(*count)++] = id;
    return TRUE;
}

static void read_directory_changes(const BYTE *buffer, DWORD bytes, DIRECTORY_CHANGES *changes)
{
    const DWORD header = offsetof(FILE_NOTIFY_EXTENDED_INFORMATION, FileName);
    DWORD offset = 0;
    while (bytes - offset >= header) {
        FILE_NOTIFY_EXTENDED_INFORMATION record;
        memcpy(&record, buffer + offset, header);
        const DWORD remaining = bytes - offset;
        const DWORD length = record.NextEntryOffset ? record.NextEntryOffset : remaining;
        if (length < header || length > remaining || !record.FileNameLength ||
            (record.FileNameLength % sizeof(wchar_t)) || record.FileNameLength > length - header ||
            record.Action < FILE_ACTION_ADDED || record.Action > FILE_ACTION_RENAMED_NEW_NAME ||
            (record.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
            !add_changed_id(changes->directories, &changes->directory_count, record.ParentFileId.QuadPart) ||
            (record.Action == FILE_ACTION_MODIFIED &&
             !add_changed_id(changes->objects, &changes->object_count, record.FileId.QuadPart)))
            break;
        if (!record.NextEntryOffset)
            return;
        offset += length;
    }
    changes->rebuild = TRUE;
}

static DWORD arm_directory_watch(INDEX *index)
{
    HANDLE event = index->notification.hEvent;
    ZeroMemory(&index->notification, sizeof(index->notification));
    index->notification.hEvent = event;
    if (!ResetEvent(event) ||
        !ReadDirectoryChangesExW(index->directory_watch, index->buffer, NOTIFICATION_BYTES, TRUE,
                                 FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                                 FILE_NOTIFY_CHANGE_ATTRIBUTES | FILE_NOTIFY_CHANGE_SIZE |
                                 FILE_NOTIFY_CHANGE_SECURITY,
                                 NULL, &index->notification, NULL, ReadDirectoryNotifyExtendedInformation))
        return GetLastError();
    index->watch_pending = TRUE;
    return ERROR_SUCCESS;
}

static DWORD finish_watch_read(INDEX *index, DIRECTORY_CHANGES *changes)
{
    DWORD bytes, error = ERROR_SUCCESS;
    if (!GetOverlappedResult(index->directory_watch, &index->notification, &bytes, FALSE)) {
        error = GetLastError();
        if (error == ERROR_IO_INCOMPLETE)
            return error;
    }
    index->watch_pending = FALSE;
    if (error && error != ERROR_NOTIFY_ENUM_DIR)
        return error;
    if (error || bytes > NOTIFICATION_BYTES)
        changes->rebuild = TRUE;
    else
        read_directory_changes(index->buffer, bytes, changes);
    return arm_directory_watch(index);
}

typedef struct {
    INDEX *index;
    size_t directories_seen;
} DIRECTORY_REFRESH;

static DWORD refresh_child_entry(void *context, HANDLE directory, ULONGLONG parent,
                                 const FILE_ID_EXTD_DIR_INFO *entry)
{
    UNREFERENCED_PARAMETER(directory);
    DIRECTORY_REFRESH *update = context;
    INDEX *index = update->index;
    DWORDLONG id;
    memcpy(&id, entry->FileId.Identifier, sizeof(id));
    if (entry->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        const INDEX_ENTRY *previous;
        if (!index->capacity)
            return ERROR_RETRY;
        previous = &index->entries[entry_slot(index->entries, index->capacity, id)];
        if (!previous->name || previous->parent != parent || previous->attributes != entry->FileAttributes ||
            wcslen(previous->name) * sizeof(wchar_t) != entry->FileNameLength ||
            memcmp(previous->name, entry->FileName, entry->FileNameLength))
            return ERROR_RETRY;
        ++update->directories_seen;
        return ERROR_SUCCESS;
    }
    return snapshot_file(index, parent, id, entry);
}

static DWORD refresh_files_in_directory(INDEX *index, DWORDLONG id, wchar_t *path_buffer)
{
    DIRECTORY_REFRESH update = { .index = index };
    size_t before = index->file_count, known_directories = 0, read, write = 0;
    const wchar_t *path = index->directory;
    DWORDLONG actual_id;
    TREE_RESULT totals;
    DWORD error;
    if (id != index->root) {
        const INDEX_ENTRY *entry;
        if (!index->capacity)
            return ERROR_RETRY;
        entry = &index->entries[entry_slot(index->entries, index->capacity, id)];
        if ((entry->attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) !=
            FILE_ATTRIBUTE_DIRECTORY)
            return ERROR_RETRY;
        path = entry_path(index, entry, path_buffer);
        if (!path)
            return ERROR_RETRY;
        path += 4;
    }
    for (read = 0; read < index->capacity; ++read) {
        if (index->entries[read].name && index->entries[read].parent == id)
            ++known_directories;
    }
    error = tree_snapshot_directory(path, refresh_child_entry, &update, &actual_id, &totals);
    if (error || actual_id != id || update.directories_seen != known_directories)
        return ERROR_RETRY;
    for (read = 0; read < before; ++read) {
        INDEX_ENTRY *entry = &index->files[read];
        if (entry->parent == id) {
            --index->totals.files;
            index->totals.bytes -= entry->size;
            index->stale_name_units += wcslen(entry->name) + 1;
        } else {
            if (write != read)
                index->files[write] = *entry;
            ++write;
        }
    }
    index->totals.files += index->file_count - before;
    index->totals.bytes += totals.bytes;
    if (write != before)
        memmove(index->files + write, index->files + before, (index->file_count - before) * sizeof(*index->files));
    index->file_count = write + index->file_count - before;
    return ERROR_SUCCESS;
}

static DWORD refresh_changed_directories(INDEX *index, DIRECTORY_CHANGES *changes)
{
    wchar_t *path;
    size_t i, j;
    DWORD error = ERROR_SUCCESS;
    if (!index->live || index->stale_name_units >=
        max(NAME_BLOCK_UNITS, (index->file_name_units - index->stale_name_units) / 4))
        return ERROR_RETRY;
    /* A size change through one hardlink also affects its other names in the snapshot. */
    for (i = 0; changes->object_count && i < index->file_count; ++i) {
        for (j = 0; j < changes->object_count; ++j) {
            if (index->files[i].id == changes->objects[j] &&
                !add_changed_id(changes->directories, &changes->directory_count, index->files[i].parent))
                return ERROR_RETRY;
        }
    }
    path = malloc(PATH_CHARS * sizeof(*path));
    if (!path)
        return ERROR_RETRY;
    clear_exact_names(index);
    for (i = 0; i < changes->directory_count && !error; ++i) {
        error = refresh_files_in_directory(index, changes->directories[i], path);
        if (!error)
            ++index->refreshed_directories;
    }
    free(path);
    return error;
}

static DWORD snapshot_refresh(INDEX *index, BOOL initial)
{
    unsigned attempt;
    DWORDLONG current_id;
    DWORD current_volume;
    DWORD error = directory_identity(index->watch_path, &current_id, &current_volume);
    if (error)
        return error;
    if (current_id != index->watched_root || current_volume != index->watched_volume)
        return ERROR_RETRY;
    for (attempt = 0; attempt < 3; ++attempt) {
        DIRECTORY_CHANGES changes = {0};
        DWORD state = WaitForSingleObject(index->notification.hEvent, 0);
        if (state == WAIT_FAILED)
            return GetLastError();
        if (state == WAIT_OBJECT_0) {
            error = finish_watch_read(index, &changes);
            if (error)
                return error;
        } else if (!initial) {
            return ERROR_SUCCESS;
        }
        if (initial || changes.rebuild || refresh_changed_directories(index, &changes)) {
            clear_entries(index);
            error = tree_snapshot(index->directory, snapshot_entry, index, &index->root, &index->totals);
            if (error)
                return error;
            if (index->root != index->watched_root)
                return ERROR_RETRY;
            ++index->full_refreshes;
        }
        initial = FALSE;
    }
    return WaitForSingleObject(index->notification.hEvent, 0) == WAIT_TIMEOUT ? ERROR_SUCCESS : ERROR_BUSY;
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
        fwprintf(stderr, L"name TEXT | name-exact NAME | ext EXT | path TEXT | size MIN:MAX | "
                 L"count MODE VALUE | %lsquit\n",
                 index->directory ? L"folder-size | snapshot-stats | " : L"");
        fflush(stdout);
    }
    while (!error && fgetws(input, ARRAYSIZE(input), stdin)) {
        wchar_t *value, *mode = input;
        BOOL count_only = FALSE, folder_size, snapshot_stats;
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
        snapshot_stats = index->directory && !count_only && !wcscmp(mode, L"snapshot-stats");
        value = wcschr(mode, L' ');
        if (value) {
            *value++ = 0;
        } else if (!folder_size && !snapshot_stats) {
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
            } else if (snapshot_stats) {
                wprintf(L"full_refreshes=%llu refreshed_directories=%llu\n",
                        index->full_refreshes, index->refreshed_directories);
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

static DWORD open_directory_scope(DIRECTORY_SEARCH *scope, const wchar_t *input)
{
    INDEX *index = &scope->index;
    wchar_t *path = scope->path = malloc(PATH_CHARS * sizeof(*path));
    BY_HANDLE_FILE_INFORMATION identity;
    DWORD length, error;
    if (!path)
        return ERROR_NOT_ENOUGH_MEMORY;
    memcpy(path, L"\\\\?\\", 4 * sizeof(*path));
    length = GetFullPathNameW(input, PATH_CHARS - 4, path + 4, NULL);
    if (!length || length >= PATH_CHARS - 4 || length < 3 || path[5] != L':' || path[6] != L'\\')
        return length ? ERROR_INVALID_NAME : GetLastError();
    while (length > 3 && path[length + 3] == L'\\')
        path[--length + 4] = 0;
    index->directory = path + 4;
    index->watch_path = path;
    if (index->shell_scope) {
        wchar_t fs[16];
        HANDLE handle;
        UINT type = GetDriveTypeW((wchar_t[]){path[4], L':', L'\\', 0});
        if ((type != DRIVE_FIXED && type != DRIVE_REMOVABLE) || wcschr(path + 7, L':'))
            return ERROR_NOT_SUPPORTED;
        error = open_indexed_file(path, &handle);
        if (error)
            return error;
        if (!GetVolumeInformationByHandleW(handle, NULL, 0, NULL, NULL, NULL, fs, ARRAYSIZE(fs)))
            error = GetLastError();
        else if (wcscmp(fs, L"NTFS"))
            error = ERROR_NOT_SUPPORTED;
        CloseHandle(handle);
        if (error)
            return error;
    }
    error = directory_identity(path, &index->watched_root, &index->watched_volume);
    if (error)
        return error;
    index->directory_watch = CreateFileW(path, FILE_LIST_DIRECTORY,
                                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                                         OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT |
                                         FILE_FLAG_OVERLAPPED, NULL);
    if (index->directory_watch == INVALID_HANDLE_VALUE ||
        !GetFileInformationByHandle(index->directory_watch, &identity))
        return GetLastError();
    if ((((DWORDLONG)identity.nFileIndexHigh << 32) | identity.nFileIndexLow) != index->watched_root ||
        identity.dwVolumeSerialNumber != index->watched_volume)
        return ERROR_RETRY;
    index->notification.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!index->notification.hEvent)
        return GetLastError();
    scope->prefix = _wcsdup(path);
    index->buffer = malloc(NOTIFICATION_BYTES);
    if (!scope->prefix || !index->buffer)
        return ERROR_NOT_ENOUGH_MEMORY;
    /* Omit the drive root's trailing slash when composing paths. */
    if (length == 3)
        scope->prefix[6] = 0;
    index->prefix = scope->prefix;
    return arm_directory_watch(index);
}

static void close_directory_scope(DIRECTORY_SEARCH *scope)
{
    INDEX *index = &scope->index;
    if (index->watch_pending) {
        DWORD bytes;
        CancelIoEx(index->directory_watch, &index->notification);
        GetOverlappedResult(index->directory_watch, &index->notification, &bytes, TRUE);
    }
    if (index->directory_watch != INVALID_HANDLE_VALUE)
        CloseHandle(index->directory_watch);
    if (index->notification.hEvent)
        CloseHandle(index->notification.hEvent);
    clear_entries(index);
    free(index->buffer);
    free(scope->prefix);
    free(scope->path);
}

DWORD directory_index_console(const wchar_t *input)
{
    DIRECTORY_SEARCH scope = { .index = { .directory_watch = INVALID_HANDLE_VALUE } };
    DWORD error = open_directory_scope(&scope, input);
    if (!error)
        error = commands(&scope.index, scope.index.directory);
    close_directory_scope(&scope);
    return error;
}

HRESULT directory_search_open(PCWSTR path, DIRECTORY_SEARCH **search)
{
    DIRECTORY_SEARCH *scope;
    DWORD error;
    if (!path || !*path || !search || path[1] != L':' || path[2] != L'\\')
        return E_INVALIDARG;
    if (!uses_process_token())
        return E_ACCESSDENIED;
    scope = calloc(1, sizeof(*scope));
    if (!scope)
        return E_OUTOFMEMORY;
    scope->index.directory_watch = INVALID_HANDLE_VALUE;
    scope->index.shell_scope = TRUE;
    error = open_directory_scope(scope, path);
    if (error) {
        directory_search_close(scope);
        return HRESULT_FROM_WIN32(error);
    }
    *search = scope;
    return S_OK;
}

void directory_search_close(DIRECTORY_SEARCH *search)
{
    if (search) {
        close_directory_scope(search);
        free(search);
    }
}

static HRESULT verified_item_id(const INDEX *index, const INDEX_ENTRY *entry,
                                wchar_t *buffer, PIDLIST_ABSOLUTE *id)
{
    wchar_t *path = entry_path_cached(index, entry, buffer, NULL), *actual = NULL;
    ULONGLONG size;
    DWORD attributes;
    HRESULT hr;
    if (!path || !verify_match(entry, path, 0, MAXULONGLONG, &size, &attributes) ||
        ((attributes ^ entry->attributes) & FILE_ATTRIBUTE_DIRECTORY) ||
        (attributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_OFFLINE)))
        return HRESULT_FROM_WIN32(ERROR_RETRY);
    hr = SHParseDisplayName(path + 4, NULL, id, 0, NULL);
    if (SUCCEEDED(hr))
        hr = SHGetNameFromIDList(*id, SIGDN_FILESYSPATH, &actual);
    if (SUCCEEDED(hr) && wcscmp(path + 4, actual))
        hr = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    CoTaskMemFree(actual);
    if (FAILED(hr)) {
        CoTaskMemFree(*id);
        *id = NULL;
    }
    return hr;
}

static HRESULT search_exact_name(DIRECTORY_SEARCH *search, PCWSTR name, IShellItemArray **items)
{
    INDEX *index;
    PIDLIST_ABSOLUTE *ids = NULL;
    IShellItemArray *array = NULL;
    wchar_t *buffer = NULL;
    size_t first, count, parsed = 0;
    DWORD error;
    HRESULT hr = S_FALSE;
    if (!uses_process_token())
        return E_ACCESSDENIED;
    index = &search->index;
    error = snapshot_refresh(index, !search->ready);
    search->ready = !error;
    if (error || !index->watch_pending)
        return HRESULT_FROM_WIN32(error ? error : ERROR_RETRY);
    build_exact_names(index, index->files, index->file_count);
    if ((index->file_count || index->live) && !index->exact_names)
        return E_OUTOFMEMORY;
    first = exact_name_bound(index, name, FALSE);
    count = exact_name_bound(index, name, TRUE) - first;
    if (count) {
        ids = calloc(count, sizeof(*ids));
        buffer = malloc(PATH_CHARS * sizeof(*buffer));
        if (!ids || !buffer) {
            hr = E_OUTOFMEMORY;
            goto done;
        }
        for (; parsed < count; ++parsed) {
            const INDEX_ENTRY *entry = index->exact_names[first + parsed];
            hr = verified_item_id(index, entry, buffer, &ids[parsed]);
            if (FAILED(hr))
                goto done;
        }
        hr = SHCreateShellItemArrayFromIDLists((UINT)count, (PCIDLIST_ABSOLUTE *)ids, &array);
    }
    if (SUCCEEDED(hr)) {
        DWORD state = WaitForSingleObject(index->notification.hEvent, 0);
        if (state == WAIT_TIMEOUT) {
            *items = array;
            array = NULL;
        } else {
            hr = HRESULT_FROM_WIN32(state == WAIT_FAILED ? GetLastError() : ERROR_RETRY);
        }
    }
done:
    if (array)
        IShellItemArray_Release(array);
    while (parsed)
        CoTaskMemFree(ids[--parsed]);
    free(ids);
    free(buffer);
    return hr;
}


static BOOL condition_matches_input(ICondition *condition, PCWSTR component, size_t length)
{
    IRichChunk *term = NULL;
    PWSTR text = NULL;
    BOOL matches = FALSE;
    if (SUCCEEDED(ICondition_GetInputTerms(condition, NULL, NULL, &term)) && term) {
        /* The comparison value can carry Shell word-break metadata. */
        if (SUCCEEDED(IRichChunk_GetData(term, NULL, NULL, &text, NULL)) && text)
            matches = wcslen(text) == length && !wmemcmp(component, text, length);
        CoTaskMemFree(text);
        IRichChunk_Release(term);
    }
    return matches;
}

HRESULT directory_search_condition(DIRECTORY_SEARCH *search, PCWSTR query, ICondition *condition,
                                   IShellItemArray **items)
{
    PCWSTR component;
    WCHAR name[EXACT_NAME_LIMIT + 1];
    CONDITION_TYPE type;
    CONDITION_OPERATION operation;
    PWSTR property = NULL;
    size_t name_length;
    HRESULT hr;
    if (!search || !query || !condition || !items)
        return E_INVALIDARG;
    if (!exact_name_query(query, &component, &name_length))
        return E_NOTIMPL;
    hr = ICondition_GetConditionType(condition, &type);
    if (FAILED(hr) || type != CT_LEAF_CONDITION)
        return FAILED(hr) ? hr : E_NOTIMPL;
    hr = ICondition_GetComparisonInfo(condition, &property, &operation, NULL);
    if (SUCCEEDED(hr)) {
        if (operation == COP_EQUAL && property &&
            CompareStringOrdinal(property, -1, L"System.FileName", -1, TRUE) == CSTR_EQUAL &&
            condition_matches_input(condition, component, name_length)) {
            wmemcpy(name, component, name_length);
            name[name_length] = 0;
            hr = search_exact_name(search, name, items);
        } else {
            hr = E_NOTIMPL;
        }
    }
    CoTaskMemFree(property);
    return hr;
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
    error = directory_identity(root, &index.root, NULL);
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
