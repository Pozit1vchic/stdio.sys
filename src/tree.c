#include "engine.h"
#include "native_stat.h"
#include <winternl.h>
#include <winioctl.h>
#include <stddef.h>
#include <stdlib.h>
#include <wchar.h>
#include <limits.h>

#define DIRECTORY_BUFFER (32 * 1024)
#define SHARE_READ_ONLY FILE_SHARE_READ
#define SHARE_ALL (FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE)
#define MAX_DEPTH 256
#define MAX_WORKERS 16
#define FILE_BATCH_SIZE 128
#define COPY_ROOT_BATCH_SIZE 512
#define DELETE_BUFFER (128 * 1024)
#define DELETE_BATCH_SIZE 1024
#define DELETE_WORK_CHUNK 8
#define LARGE_COPY_BYTES (512ULL * 1024 * 1024)

enum { NATIVE_FILE_STAT_CLASS = 68 };
NTSYSAPI NTSTATUS NTAPI NtQueryInformationFile(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG,
                                               FILE_INFORMATION_CLASS);

BOOL query_native_stat(HANDLE handle, NATIVE_FILE_STAT *stat)
{
    IO_STATUS_BLOCK io;
    return NtQueryInformationFile(handle, &io, stat, sizeof(*stat),
                                  (FILE_INFORMATION_CLASS)NATIVE_FILE_STAT_CLASS) >= 0 &&
           io.Information == sizeof(*stat);
}

typedef struct {
    HANDLE parent;
    const FILE_ID_EXTD_DIR_INFO *entry;
    DWORD error;
} DELETE_JOB;

typedef struct {
    TREE_RESULT *result;
    BOOL remove;
    PTP_POOL pool;
    PTP_WORK work;
    DELETE_JOB jobs[DELETE_BATCH_SIZE];
    unsigned count;
    volatile LONG next;
    const wchar_t *needle;
    ULONGLONG matches;
    TREE_VISITOR visitor;
    void *context;
} WALK;

static DWORD native_file(HANDLE parent, const wchar_t *name, USHORT bytes,
                         ACCESS_MASK access, ULONG sharing, ULONG disposition,
                         ULONG options, HANDLE *out)
{
    UNICODE_STRING path = { bytes, bytes, (PWSTR)name };
    OBJECT_ATTRIBUTES attributes;
    IO_STATUS_BLOCK io;
    NTSTATUS status;

    /* Exact spelling from NTFS is required for case-sensitive directories. */
    InitializeObjectAttributes(&attributes, &path, parent ? OBJ_DONT_REPARSE :
                               OBJ_CASE_INSENSITIVE | OBJ_DONT_REPARSE, parent, NULL);
    status = NtCreateFile(out, access, &attributes, &io, NULL, 0, sharing, disposition,
                          options | FILE_OPEN_REPARSE_POINT | FILE_SYNCHRONOUS_IO_NONALERT,
                          NULL, 0);
    return status < 0 ? RtlNtStatusToDosError(status) : ERROR_SUCCESS;
}

static DWORD native_open(HANDLE parent, const wchar_t *name, USHORT bytes,
                         ACCESS_MASK access, ULONG sharing, HANDLE *out)
{
    return native_file(parent, name, bytes, access, sharing, FILE_OPEN, 0, out);
}

static BOOL local_storage(const wchar_t *root)
{
    UINT type = GetDriveTypeW(root);
    return type == DRIVE_FIXED || type == DRIVE_REMOVABLE;
}

static DWORD open_root(const wchar_t *input, BOOL remove, BOOL stable, HANDLE *out)
{
    wchar_t *absolute;
    DWORD error, length;

    if (!input[0] || (input[0] == L'\\' && input[1] == L'\\'))
        return ERROR_INVALID_NAME;
    length = GetFullPathNameW(input, 0, NULL, NULL);
    if (!length)
        return GetLastError();
    if (length > 32760)
        return ERROR_FILENAME_EXCED_RANGE;
    absolute = malloc(((size_t)length + 4) * sizeof(*absolute));
    if (!absolute)
        return ERROR_NOT_ENOUGH_MEMORY;
    memcpy(absolute, L"\\??\\", 4 * sizeof(*absolute));
    length = GetFullPathNameW(input, length, absolute + 4, NULL);
    if (!length || length > 32760) {
        error = length ? ERROR_FILENAME_EXCED_RANGE : GetLastError();
    } else {
        while (length > 3 && absolute[3 + length] == L'\\')
            --length;
        absolute[4 + length] = 0;
        if (length < 3 || absolute[5] != L':' || absolute[6] != L'\\' ||
            wcschr(absolute + 6, L':') || (remove && length == 3)) {
            error = ERROR_INVALID_NAME;
        } else if (!local_storage((wchar_t[]){ absolute[4], L':', L'\\', 0 })) {
            error = ERROR_NOT_SUPPORTED;
        } else {
            error = native_open(NULL, absolute, (USHORT)((length + 4) * sizeof(wchar_t)),
                                FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | SYNCHRONIZE |
                                (remove ? DELETE : 0), remove || stable ? SHARE_READ_ONLY : SHARE_ALL, out);
        }
    }
    free(absolute);
    return error;
}

static DWORD remove_open(HANDLE handle)
{
    FILE_DISPOSITION_INFO disposition = { TRUE };
    return SetFileInformationByHandle(handle, FileDispositionInfo, &disposition,
                                      sizeof(disposition)) ? ERROR_SUCCESS : GetLastError();
}

static DWORD open_entry(HANDLE parent, const FILE_ID_EXTD_DIR_INFO *entry, ACCESS_MASK access,
                        ULONG sharing, HANDLE *handle)
{
    FILE_ID_INFO id;
    FILE_ATTRIBUTE_TAG_INFO tag;
    NATIVE_FILE_STAT stat;
    FILE_ID_128 stat_id = {0};
    const DWORD mask = FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT;
    DWORD error = native_open(parent, entry->FileName, (USHORT)entry->FileNameLength,
                              FILE_READ_ATTRIBUTES | SYNCHRONIZE | access, sharing, handle);
    if (error)
        return error;
    if (query_native_stat(*handle, &stat)) {
        memcpy(stat_id.Identifier, &stat.file_id.QuadPart, sizeof(stat.file_id.QuadPart));
        if (memcmp(&stat_id, &entry->FileId, sizeof(stat_id)) ||
            ((stat.attributes ^ entry->FileAttributes) & mask))
            error = ERROR_RETRY;
    } else if (!GetFileInformationByHandleEx(*handle, FileIdInfo, &id, sizeof(id)) ||
        !GetFileInformationByHandleEx(*handle, FileAttributeTagInfo, &tag, sizeof(tag))) {
        error = GetLastError();
    } else if (memcmp(&id.FileId, &entry->FileId, sizeof(id.FileId)) ||
               ((tag.FileAttributes ^ entry->FileAttributes) & mask)) {
        error = ERROR_RETRY;
    }
    if (error)
        CloseHandle(*handle);
    return error;
}

static void delete_job(DELETE_JOB *job)
{
    HANDLE file;
    job->error = open_entry(job->parent, job->entry, DELETE, SHARE_READ_ONLY, &file);
    if (!job->error) {
        job->error = remove_open(file);
        CloseHandle(file);
    }
}

static VOID CALLBACK delete_worker(PTP_CALLBACK_INSTANCE instance, PVOID context, PTP_WORK work)
{
    WALK *walk = context;
    LONG start;
    UNREFERENCED_PARAMETER(instance);
    UNREFERENCED_PARAMETER(work);
    while ((start = InterlockedExchangeAdd(&walk->next, DELETE_WORK_CHUNK)) < (LONG)walk->count) {
        LONG end = min(start + DELETE_WORK_CHUNK, (LONG)walk->count);
        LONG index;
        for (index = start; index < end; ++index)
            delete_job(&walk->jobs[index]);
    }
}

static void record_error(TREE_RESULT *result, DWORD error)
{
    if (error) {
        if (!result->error)
            result->error = error;
        ++result->errors;
    }
}

static void flush_jobs(WALK *walk)
{
    unsigned i;
    if (!walk->count)
        return;
    if (walk->work && walk->count >= 4) {
        walk->next = 0;
        for (i = 1; i < walk->result->workers && i * DELETE_WORK_CHUNK < walk->count; ++i)
            SubmitThreadpoolWork(walk->work);
        delete_worker(NULL, walk, NULL);
        WaitForThreadpoolWorkCallbacks(walk->work, FALSE);
    } else {
        for (i = 0; i < walk->count; ++i)
            delete_job(&walk->jobs[i]);
    }
    for (i = 0; i < walk->count; ++i) {
        DWORD error = walk->jobs[i].error;
        if (!error)
            ++walk->result->files;
        record_error(walk->result, error);
    }
    walk->count = 0;
}

static BOOL dot_entry(const FILE_ID_EXTD_DIR_INFO *entry)
{
    return (entry->FileNameLength == 2 && entry->FileName[0] == L'.') ||
           (entry->FileNameLength == 4 && entry->FileName[0] == L'.' && entry->FileName[1] == L'.');
}

static BOOL ordinary_directory(const FILE_ID_EXTD_DIR_INFO *entry)
{
    return (entry->FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ==
           FILE_ATTRIBUTE_DIRECTORY;
}

static FILE_ID_EXTD_DIR_INFO *next_entry(FILE_ID_EXTD_DIR_INFO *entry)
{
    return entry->NextEntryOffset ? (void *)((BYTE *)entry + entry->NextEntryOffset) : NULL;
}

static ULONGLONG ntfs_id(const FILE_ID_128 *id)
{
    ULONGLONG number;
    memcpy(&number, id->Identifier, sizeof(number));
    return number;
}

static void walk_directory(WALK *walk, HANDLE directory, unsigned depth, ULONGLONG parent)
{
    BYTE *buffer;
    DWORD buffer_size = walk->remove && !depth ? DELETE_BUFFER : DIRECTORY_BUFFER;
    TREE_RESULT *result = walk->result;
    ULONGLONG starting_errors = result->errors;
    if (depth == MAX_DEPTH) {
        record_error(result, ERROR_DIRECTORY);
        return;
    }
    buffer = malloc(buffer_size);
    if (!buffer) {
        record_error(result, ERROR_NOT_ENOUGH_MEMORY);
        return;
    }
    while (GetFileInformationByHandleEx(directory, FileIdExtdDirectoryInfo, buffer, buffer_size)) {
        FILE_ID_EXTD_DIR_INFO *entry;
        for (entry = (void *)buffer; entry; entry = next_entry(entry)) {
            if (dot_entry(entry))
                continue;
            if (walk->visitor) {
                DWORD error = walk->visitor(walk->context, parent, entry);
                if (error) {
                    record_error(result, error);
                    free(buffer);
                    return;
                }
            }
            if (ordinary_directory(entry))
                continue;
            if (walk->remove) {
                walk->jobs[walk->count++] = (DELETE_JOB){ directory, entry, 0 };
                if (walk->count == DELETE_BATCH_SIZE)
                    flush_jobs(walk);
            } else {
                ++result->files;
                if (!(entry->FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                    result->bytes += (ULONGLONG)entry->EndOfFile.QuadPart;
                if (walk->needle && FindStringOrdinal(FIND_FROMSTART, entry->FileName,
                    (int)(entry->FileNameLength / sizeof(wchar_t)), walk->needle, -1, TRUE) >= 0)
                    ++walk->matches;
            }
        }
        flush_jobs(walk);
        for (entry = (void *)buffer; entry; entry = next_entry(entry)) {
            HANDLE child;
            DWORD error;
            if (dot_entry(entry) || !ordinary_directory(entry))
                continue;
            error = open_entry(directory, entry, FILE_LIST_DIRECTORY | (walk->remove ? DELETE : 0),
                               walk->remove ? SHARE_READ_ONLY : SHARE_ALL, &child);
            if (!error) {
                walk_directory(walk, child, depth + 1, ntfs_id(&entry->FileId));
                CloseHandle(child);
            }
            record_error(result, error);
        }
    }
    {
        DWORD error = GetLastError();
        if (error != ERROR_NO_MORE_FILES && error != ERROR_HANDLE_EOF)
            record_error(result, error);
    }
    free(buffer);
    if (!walk->remove) {
        ++result->directories;
    } else if (result->errors == starting_errors) {
        DWORD error = remove_open(directory);
        if (!error)
            ++result->directories;
        record_error(result, error);
    }
}

DWORD storage_workers(const wchar_t *path)
{
    wchar_t volume[MAX_PATH], device[] = L"\\\\.\\X:";
    STORAGE_PROPERTY_QUERY query = { StorageDeviceSeekPenaltyProperty, PropertyStandardQuery, {0} };
    DEVICE_SEEK_PENALTY_DESCRIPTOR seek;
    HANDLE handle;
    DWORD bytes, workers = 1;

    if (!GetVolumePathNameW(path, volume, ARRAYSIZE(volume)) ||
        wcslen(volume) != 3 || !local_storage(volume))
        return 1;
    device[4] = volume[0];
    handle = CreateFileW(device, 0, SHARE_ALL, NULL, OPEN_EXISTING, 0, NULL);
    if (handle == INVALID_HANDLE_VALUE)
        return 1;
    if (DeviceIoControl(handle, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query),
                        &seek, sizeof(seek), &bytes, NULL) &&
        bytes >= sizeof(seek) && !seek.IncursSeekPenalty) {
        const DWORD cores = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
        union {
            STORAGE_DEVICE_DESCRIPTOR descriptor;
            BYTE buffer[1024];
        } storage;
        query.PropertyId = StorageDeviceProperty;
        workers = 2;
        if (DeviceIoControl(handle, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query),
                            &storage, sizeof(storage), &bytes, NULL) &&
            bytes >= offsetof(STORAGE_DEVICE_DESCRIPTOR, RawPropertiesLength) &&
            storage.descriptor.BusType == BusTypeNvme)
            workers = 4;
        if (cores && cores < workers)
            workers = cores;
    }
    CloseHandle(handle);
    return workers;
}

static DWORD walk_path(const wchar_t *path, BOOL remove, DWORD workers, const wchar_t *needle,
                       TREE_VISITOR visitor, void *context, ULONGLONG *root_id,
                       TREE_RESULT *result, ULONGLONG *matches)
{
    WALK walk = {0};
    HANDLE root;
    wchar_t fs[16];
    FILE_ATTRIBUTE_TAG_INFO tag;
    TP_CALLBACK_ENVIRON environment;
    DWORD error = open_root(path, remove, FALSE, &root);
    ZeroMemory(result, sizeof(*result));
    if (error) {
        record_error(result, error);
        return error;
    }
    if (!GetVolumeInformationByHandleW(root, NULL, 0, NULL, NULL, NULL, fs, ARRAYSIZE(fs)) ||
        !GetFileInformationByHandleEx(root, FileAttributeTagInfo, &tag, sizeof(tag))) {
        error = GetLastError();
    } else if (_wcsicmp(fs, L"NTFS")) {
        error = ERROR_NOT_SUPPORTED;
    } else if (!(tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
               (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        error = ERROR_DIRECTORY;
    }
    if (!error && root_id) {
        FILE_ID_INFO id;
        if (!GetFileInformationByHandleEx(root, FileIdInfo, &id, sizeof(id)))
            error = GetLastError();
        else
            *root_id = ntfs_id(&id.FileId);
    }
    if (error) {
        CloseHandle(root);
        record_error(result, error);
        return error;
    }
    walk.result = result;
    walk.remove = remove;
    walk.needle = needle;
    walk.visitor = visitor;
    walk.context = context;
    result->workers = remove ? (workers ? workers : storage_workers(path)) : 1;
    if (result->workers > MAX_WORKERS)
        result->workers = MAX_WORKERS;
    if (result->workers > 1) {
        walk.pool = CreateThreadpool(NULL);
        if (walk.pool) {
            SetThreadpoolThreadMaximum(walk.pool, result->workers - 1);
            InitializeThreadpoolEnvironment(&environment);
            SetThreadpoolCallbackPool(&environment, walk.pool);
            walk.work = CreateThreadpoolWork(delete_worker, &walk, &environment);
        }
        if (!walk.work)
            result->workers = 1;
    }
    walk_directory(&walk, root, 0, root_id ? *root_id : 0);
    CloseHandle(root);
    if (walk.work)
        CloseThreadpoolWork(walk.work);
    if (walk.pool) {
        CloseThreadpool(walk.pool);
        DestroyThreadpoolEnvironment(&environment);
    }
    if (matches)
        *matches = walk.matches;
    return result->error;
}

DWORD tree_run(const wchar_t *path, BOOL remove, DWORD workers, TREE_RESULT *result)
{
    return walk_path(path, remove, workers, NULL, NULL, NULL, NULL, result, NULL);
}

DWORD tree_find(const wchar_t *path, const wchar_t *needle, ULONGLONG *matches)
{
    TREE_RESULT result;
    *matches = 0;
    return walk_path(path, FALSE, 1, needle, NULL, NULL, NULL, &result, matches);
}

DWORD tree_snapshot(const wchar_t *path, TREE_VISITOR visitor, void *context,
                    ULONGLONG *root_id, TREE_RESULT *result)
{
    return walk_path(path, FALSE, 1, NULL, visitor, context, root_id, result, NULL);
}

typedef struct {
    size_t name_offset;
    ULONGLONG size;
    DWORD attributes;
} DIRECTORY_ITEM;

static int compare_entries(void *context, const void *left, const void *right)
{
    const DIRECTORY_ITEM *a = left, *b = right;
    const wchar_t *names = context;
    const wchar_t *first = names + a->name_offset, *second = names + b->name_offset;
    int order = CompareStringOrdinal(first, -1, second, -1, TRUE);
    return order == CSTR_EQUAL ? wcscmp(first, second) : order - CSTR_EQUAL;
}

DWORD tree_list(const wchar_t *path)
{
    HANDLE root;
    DIRECTORY_ITEM *items = NULL;
    wchar_t *names = NULL;
    size_t capacity = 0, count = 0, i;
    size_t names_capacity = 0, names_used = 0;
    BYTE *buffer;
    DWORD error = open_root(path, FALSE, FALSE, &root);
    FILE_ATTRIBUTE_TAG_INFO tag;
    if (error)
        return error;
    if (!GetFileInformationByHandleEx(root, FileAttributeTagInfo, &tag, sizeof(tag))) {
        error = GetLastError();
        CloseHandle(root);
        return error;
    }
    if ((tag.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) !=
        FILE_ATTRIBUTE_DIRECTORY) {
        CloseHandle(root);
        return ERROR_DIRECTORY;
    }
    buffer = malloc(DIRECTORY_BUFFER);
    if (!buffer) {
        CloseHandle(root);
        return ERROR_NOT_ENOUGH_MEMORY;
    }
    while (!error && GetFileInformationByHandleEx(root, FileIdExtdDirectoryInfo, buffer, DIRECTORY_BUFFER)) {
        FILE_ID_EXTD_DIR_INFO *entry;
        for (entry = (void *)buffer; entry; entry = next_entry(entry)) {
            size_t units = entry->FileNameLength / sizeof(*names);
            if (dot_entry(entry))
                continue;
            if (count == capacity) {
                size_t next = capacity ? capacity * 2 : 256;
                DIRECTORY_ITEM *grown = next <= SIZE_MAX / sizeof(*items) ?
                                    realloc(items, next * sizeof(*items)) : NULL;
                if (!grown) {
                    error = ERROR_NOT_ENOUGH_MEMORY;
                    break;
                }
                capacity = next;
                items = grown;
            }
            if (units + 1 > SIZE_MAX / sizeof(*names) - names_used) {
                error = ERROR_NOT_ENOUGH_MEMORY;
                break;
            }
            if (names_capacity < names_used + units + 1) {
                size_t next = max(names_used + units + 1, names_capacity ? names_capacity * 2 : 4096);
                wchar_t *grown = next <= SIZE_MAX / sizeof(*names) ?
                                realloc(names, next * sizeof(*names)) : NULL;
                if (!grown) {
                    error = ERROR_NOT_ENOUGH_MEMORY;
                    break;
                }
                names = grown;
                names_capacity = next;
            }
            memcpy(names + names_used, entry->FileName, entry->FileNameLength);
            names[names_used + units] = 0;
            items[count++] = (DIRECTORY_ITEM){
                names_used, (ULONGLONG)entry->EndOfFile.QuadPart, entry->FileAttributes
            };
            names_used += units + 1;
        }
    }
    if (!error) {
        error = GetLastError();
        if (error == ERROR_NO_MORE_FILES || error == ERROR_HANDLE_EOF)
            error = ERROR_SUCCESS;
    }
    if (!error && count) {
        qsort_s(items, count, sizeof(*items), compare_entries, names);
        for (i = 0; i < count; ++i)
            wprintf(L"%lc\t%llu\t%ls\n", items[i].attributes & FILE_ATTRIBUTE_DIRECTORY ? L'd' :
                    items[i].attributes & FILE_ATTRIBUTE_REPARSE_POINT ? L'r' : L'f',
                    items[i].size, names + items[i].name_offset);
    }
    free(names);
    free(items);
    free(buffer);
    CloseHandle(root);
    return error;
}

DWORD copy_file_mode(const wchar_t *source, const wchar_t *destination, BOOL unbuffered)
{
    COPYFILE2_EXTENDED_PARAMETERS options = {
        sizeof(options), COPY_FILE_FAIL_IF_EXISTS | COPY_FILE_COPY_SYMLINK, NULL, NULL, NULL
    };
    HRESULT result;
    if (unbuffered)
        options.dwCopyFlags |= COPY_FILE_NO_BUFFERING;
    result = CopyFile2(source, destination, &options);
    return FAILED(result) ? (HRESULT_FACILITY(result) == FACILITY_WIN32 ?
                            HRESULT_CODE(result) : (DWORD)result) : ERROR_SUCCESS;
}

DWORD plain_copy(const wchar_t *source, const wchar_t *destination)
{
    return copy_file_mode(source, destination, FALSE);
}

typedef struct {
    TREE_RESULT *result;
    PTP_WORK work;
    HANDLE source;
    const wchar_t *source_path;
    const wchar_t *target_path;
    size_t source_length;
    size_t target_length;
    DWORD source_flags;
    DWORD target_flags;
    BOOL legacy_source;
    BOOL fat32;
    BOOL unbuffered_large;
    const FILE_ID_EXTD_DIR_INFO *entries[COPY_ROOT_BATCH_SIZE];
    DWORD errors[COPY_ROOT_BATCH_SIZE];
    unsigned count;
    volatile LONG next;
} COPY_BATCH;

static DWORD open_copy_entry(const COPY_BATCH *batch, HANDLE parent, const FILE_ID_EXTD_DIR_INFO *entry,
                             ACCESS_MASK access, HANDLE *handle)
{
    BY_HANDLE_FILE_INFORMATION info;
    DWORD error;
    DWORDLONG expected = ntfs_id(&entry->FileId);
    const DWORD type_mask = FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT;
    if (!batch->legacy_source)
        return open_entry(parent, entry, access, SHARE_READ_ONLY, handle);
    if (!expected)
        return ERROR_NOT_SUPPORTED;
    error = native_open(parent, entry->FileName, (USHORT)entry->FileNameLength,
                        FILE_READ_ATTRIBUTES | SYNCHRONIZE | access, SHARE_READ_ONLY, handle);
    if (error)
        return error;
    if (!GetFileInformationByHandle(*handle, &info))
        error = GetLastError();
    else if (expected != (((DWORDLONG)info.nFileIndexHigh << 32) | info.nFileIndexLow) ||
             ((info.dwFileAttributes ^ entry->FileAttributes) & type_mask))
        error = ERROR_RETRY;
    if (error)
        CloseHandle(*handle);
    return error;
}

static BOOL enumerate_copy_directory(const COPY_BATCH *batch, HANDLE directory, BYTE *buffer, DWORD capacity)
{
    FILE_ID_BOTH_DIR_INFO *entry;
    if (!batch->legacy_source)
        return GetFileInformationByHandleEx(directory, FileIdExtdDirectoryInfo, buffer, capacity);
    if (!GetFileInformationByHandleEx(directory, FileIdBothDirectoryInfo, buffer, capacity))
        return FALSE;
    /* The 64-bit records have enough space for the common metadata and name. */
    _Static_assert(offsetof(FILE_ID_EXTD_DIR_INFO, FileName) <= offsetof(FILE_ID_BOTH_DIR_INFO, FileName),
                   "Directory record conversion would overwrite the next entry");
    for (entry = (void *)buffer; ; ) {
        FILE_ID_EXTD_DIR_INFO common = {0};
        common.NextEntryOffset = entry->NextEntryOffset;
        common.EndOfFile = entry->EndOfFile;
        common.FileAttributes = entry->FileAttributes;
        common.FileNameLength = entry->FileNameLength;
        common.EaSize = entry->EaSize;
        memcpy(common.FileId.Identifier, &entry->FileId.QuadPart, sizeof(entry->FileId.QuadPart));
        memmove((BYTE *)entry + offsetof(FILE_ID_EXTD_DIR_INFO, FileName),
                entry->FileName, common.FileNameLength);
        memcpy(entry, &common, offsetof(FILE_ID_EXTD_DIR_INFO, FileName));
        if (!common.NextEntryOffset)
            return TRUE;
        entry = (void *)((BYTE *)entry + common.NextEntryOffset);
    }
}

static DWORD portable_file_check(const COPY_BATCH *batch, const FILE_ID_EXTD_DIR_INFO *entry,
                                 HANDLE pinned)
{
    if (batch->fat32) {
        LARGE_INTEGER size;
        if (!GetFileSizeEx(pinned, &size))
            return GetLastError();
        if ((ULONGLONG)size.QuadPart > MAXDWORD)
            return ERROR_FILE_TOO_LARGE;
    }
    if (!(batch->target_flags & FILE_SUPPORTS_EXTENDED_ATTRIBUTES) && entry->EaSize)
        return ERROR_EAS_NOT_SUPPORTED;
    if ((batch->source_flags & FILE_NAMED_STREAMS) && !(batch->target_flags & FILE_NAMED_STREAMS)) {
        /* A too-small buffer also fails closed for a lengthy named stream. */
        union {
            FILE_STREAM_INFO stream;
            BYTE buffer[512];
        } info;
        if (!GetFileInformationByHandleEx(pinned, FileStreamInfo, &info, sizeof(info))) {
            DWORD error = GetLastError();
            return error == ERROR_HANDLE_EOF ? ERROR_SUCCESS : error;
        }
        if (info.stream.NextEntryOffset)
            return ERROR_NOT_SUPPORTED;
    }
    return ERROR_SUCCESS;
}

static VOID CALLBACK copy_worker(PTP_CALLBACK_INSTANCE instance, PVOID context, PTP_WORK work)
{
    COPY_BATCH *batch = context;
    wchar_t source[32768], target[32768];
    LONG job;
    UNREFERENCED_PARAMETER(instance);
    UNREFERENCED_PARAMETER(work);
    memcpy(source, batch->source_path, batch->source_length * sizeof(*source));
    memcpy(target, batch->target_path, batch->target_length * sizeof(*target));
    while ((job = InterlockedIncrement(&batch->next)) < (LONG)batch->count) {
        const FILE_ID_EXTD_DIR_INFO *entry = batch->entries[job];
        size_t length = entry->FileNameLength / sizeof(wchar_t);
        HANDLE pinned;
        DWORD error = ERROR_FILENAME_EXCED_RANGE;
        if (batch->source_length + length < ARRAYSIZE(source) &&
            batch->target_length + length < ARRAYSIZE(target)) {
            error = open_copy_entry(batch, batch->source, entry, 0, &pinned);
            if (!error) {
                error = portable_file_check(batch, entry, pinned);
                if (!error) {
                    BOOL unbuffered = batch->unbuffered_large &&
                                      (ULONGLONG)entry->EndOfFile.QuadPart >= LARGE_COPY_BYTES &&
                                      !(entry->FileAttributes & (FILE_ATTRIBUTE_COMPRESSED |
                                        FILE_ATTRIBUTE_ENCRYPTED | FILE_ATTRIBUTE_SPARSE_FILE));
                    memcpy(source + batch->source_length, entry->FileName, entry->FileNameLength);
                    source[batch->source_length + length] = 0;
                    memcpy(target + batch->target_length, entry->FileName, entry->FileNameLength);
                    target[batch->target_length + length] = 0;
                    error = copy_file_mode(source, target, unbuffered);
                }
                CloseHandle(pinned);
            }
        }
        batch->errors[job] = error;
    }
}

static void copy_flush(COPY_BATCH *batch)
{
    unsigned i;
    if (!batch->count)
        return;
    batch->next = -1;
    if (batch->work && batch->count >= 4) {
        for (i = 1; i < batch->result->workers && i < batch->count; ++i)
            SubmitThreadpoolWork(batch->work);
    }
    copy_worker(NULL, batch, NULL);
    if (batch->work && batch->count >= 4)
        WaitForThreadpoolWorkCallbacks(batch->work, FALSE);
    for (i = 0; i < batch->count; ++i) {
        if (!batch->errors[i]) {
            ++batch->result->files;
            batch->result->bytes += (ULONGLONG)batch->entries[i]->EndOfFile.QuadPart;
        }
        record_error(batch->result, batch->errors[i]);
    }
    batch->count = 0;
}

static DWORD create_child_directory(HANDLE parent, const wchar_t *name, USHORT bytes, HANDLE *out)
{
    return native_file(parent, name, bytes, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES |
                       FILE_WRITE_ATTRIBUTES | SYNCHRONIZE, SHARE_READ_ONLY, FILE_CREATE,
                       FILE_DIRECTORY_FILE, out);
}

static DWORD directory_path(HANDLE handle, wchar_t *path, DWORD capacity, size_t *length)
{
    DWORD count = GetFinalPathNameByHandleW(handle, path, capacity, FILE_NAME_NORMALIZED | VOLUME_NAME_GUID);
    if (!count)
        return GetLastError();
    if (count >= capacity - 1)
        return ERROR_FILENAME_EXCED_RANGE;
    if (path[count - 1] != L'\\')
        path[count++] = L'\\';
    path[count] = 0;
    *length = count;
    return ERROR_SUCCESS;
}

static DWORD copy_directory(COPY_BATCH *batch, HANDLE source, HANDLE target, unsigned depth,
                            wchar_t *source_path, size_t source_length,
                            wchar_t *target_path, size_t target_length)
{
    DWORD attributes_mask = FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM |
                            FILE_ATTRIBUTE_ARCHIVE;
    BYTE *buffer;
    DWORD capacity = depth ? DIRECTORY_BUFFER : 128 * 1024;
    unsigned batch_limit = depth ? FILE_BATCH_SIZE : COPY_ROOT_BATCH_SIZE;
    FILE_BASIC_INFO attributes;
    DWORD error = ERROR_SUCCESS;
    if (depth == MAX_DEPTH)
        return ERROR_DIRECTORY;
    if (!GetFileInformationByHandleEx(source, FileBasicInfo, &attributes, sizeof(attributes)))
        return GetLastError();
    if (attributes.FileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_ENCRYPTED |
                                     FILE_ATTRIBUTE_COMPRESSED))
        return ERROR_NOT_SUPPORTED;
    if (batch->target_flags & FILE_NAMED_STREAMS)
        attributes_mask |= FILE_ATTRIBUTE_NOT_CONTENT_INDEXED;
    buffer = malloc(capacity);
    if (!buffer)
        return ERROR_NOT_ENOUGH_MEMORY;
    while (!batch->result->error && enumerate_copy_directory(batch, source, buffer, capacity)) {
        FILE_ID_EXTD_DIR_INFO *entry;
        batch->source = source;
        batch->source_length = source_length;
        batch->target_length = target_length;
        for (entry = (void *)buffer; entry; entry = next_entry(entry)) {
            if (dot_entry(entry))
                continue;
            if (entry->FileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_OFFLINE)) {
                error = ERROR_NOT_SUPPORTED;
                break;
            }
            if (ordinary_directory(entry))
                continue;
            batch->entries[batch->count++] = entry;
            if (batch->count == batch_limit)
                copy_flush(batch);
            if (batch->result->error)
                break;
        }
        copy_flush(batch);
        if (error || batch->result->error)
            break;
        for (entry = (void *)buffer; entry; entry = next_entry(entry)) {
            HANDLE source_child, target_child;
            size_t units;
            if (dot_entry(entry) || !ordinary_directory(entry))
                continue;
            units = entry->FileNameLength / sizeof(wchar_t);
            if (source_length + units + 1 >= 32768 || target_length + units + 1 >= 32768) {
                error = ERROR_FILENAME_EXCED_RANGE;
                break;
            }
            error = open_copy_entry(batch, source, entry, FILE_LIST_DIRECTORY, &source_child);
            if (error)
                break;
            error = create_child_directory(target, entry->FileName, (USHORT)entry->FileNameLength, &target_child);
            if (!error) {
                memcpy(source_path + source_length, entry->FileName, entry->FileNameLength);
                source_path[source_length + units] = L'\\';
                memcpy(target_path + target_length, entry->FileName, entry->FileNameLength);
                target_path[target_length + units] = L'\\';
                error = copy_directory(batch, source_child, target_child, depth + 1, source_path,
                                       source_length + units + 1, target_path, target_length + units + 1);
                CloseHandle(target_child);
            }
            CloseHandle(source_child);
            if (error || batch->result->error)
                break;
        }
        if (error)
            break;
    }
    if (!error && !batch->result->error) {
        error = GetLastError();
        if (error == ERROR_NO_MORE_FILES || error == ERROR_HANDLE_EOF) {
            attributes.ChangeTime.QuadPart = 0;
            attributes.FileAttributes &= attributes_mask;
            if (!attributes.FileAttributes)
                attributes.FileAttributes = FILE_ATTRIBUTE_NORMAL;
            if (!SetFileInformationByHandle(target, FileBasicInfo, &attributes, sizeof(attributes))) {
                error = GetLastError();
            } else {
                error = ERROR_SUCCESS;
                ++batch->result->directories;
            }
        }
    }
    free(buffer);
    return error;
}

DWORD copy_new_tree(const wchar_t *input, const wchar_t *output, DWORD workers,
                    BOOL unbuffered_large, TREE_RESULT *result)
{
    COPY_BATCH batch = {0};
    HANDLE source = INVALID_HANDLE_VALUE, parent = INVALID_HANDLE_VALUE, target = INVALID_HANDLE_VALUE;
    PTP_POOL pool = NULL;
    TP_CALLBACK_ENVIRON environment;
    wchar_t *paths, *source_path, *target_path, *destination, *name;
    wchar_t source_fs[16], target_fs[16], *split;
    DWORD error = ERROR_INVALID_NAME, length;
    size_t source_length = 0, target_length = 0;
    FILE_BASIC_INFO properties;
    ZeroMemory(result, sizeof(*result));
    paths = malloc(3 * 32768 * sizeof(*paths));
    if (!paths) {
        record_error(result, ERROR_NOT_ENOUGH_MEMORY);
        return result->error;
    }
    source_path = paths;
    target_path = paths + 32768;
    destination = paths + 2 * 32768;
    length = GetFullPathNameW(output, 32768, destination, NULL);
    if (length < 4 || length >= 32768 || destination[1] != L':' ||
        destination[2] != L'\\' || wcspbrk(destination + 3, L":*?")) {
        goto done;
    }
    split = wcsrchr(destination, L'\\');
    name = split + 1;
    if (!*name || wcslen(name) > 255 || wcschr(name, L'"') ||
        name[wcslen(name) - 1] == L'.' || name[wcslen(name) - 1] == L' ')
        goto done;
    {
        wchar_t saved = *split;
        wchar_t first = *name;
        if (split == destination + 2)
            *name = 0;
        else
            *split = 0;
        error = open_root(destination, FALSE, TRUE, &parent);
        *split = saved;
        *name = first;
    }
    if (error)
        goto done;
    error = open_root(input, FALSE, TRUE, &source);
    if (error)
        goto done;
    if (!GetFileInformationByHandleEx(source, FileBasicInfo, &properties, sizeof(properties)) ||
        !GetVolumeInformationByHandleW(source, NULL, 0, NULL, NULL, &batch.source_flags,
                                       source_fs, ARRAYSIZE(source_fs)) ||
        !GetVolumeInformationByHandleW(parent, NULL, 0, NULL, NULL, &batch.target_flags,
                                       target_fs, ARRAYSIZE(target_fs))) {
        error = GetLastError();
        goto done;
    }
    if ((properties.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT |
                                      FILE_ATTRIBUTE_ENCRYPTED | FILE_ATTRIBUTE_COMPRESSED)) !=
        FILE_ATTRIBUTE_DIRECTORY ||
        (_wcsicmp(source_fs, L"NTFS") && _wcsicmp(source_fs, L"exFAT") && _wcsicmp(source_fs, L"FAT32")) ||
        (_wcsicmp(target_fs, L"NTFS") && _wcsicmp(target_fs, L"exFAT") && _wcsicmp(target_fs, L"FAT32"))) {
        error = ERROR_NOT_SUPPORTED;
        goto done;
    }
    batch.legacy_source = _wcsicmp(source_fs, L"NTFS") != 0;
    batch.fat32 = !_wcsicmp(target_fs, L"FAT32");
    batch.unbuffered_large = unbuffered_large;
    error = directory_path(source, source_path, 32768, &source_length);
    if (!error)
        error = directory_path(parent, target_path, 32768, &target_length);
    if (error)
        goto done;
    if (target_length >= source_length &&
        CompareStringOrdinal(source_path, (int)source_length, target_path, (int)source_length, TRUE) == CSTR_EQUAL) {
        error = ERROR_INVALID_PARAMETER;
        goto done;
    }
    error = create_child_directory(parent, name, (USHORT)(wcslen(name) * sizeof(*name)), &target);
    if (error)
        goto done;
    error = directory_path(target, target_path, 32768, &target_length);
    if (error)
        goto done;
    result->workers = workers ? min(workers, MAX_WORKERS) : min(storage_workers(input), storage_workers(output));
    batch.result = result;
    batch.source_path = source_path;
    batch.target_path = target_path;
    if (result->workers > 1) {
        pool = CreateThreadpool(NULL);
        if (pool) {
            SetThreadpoolThreadMaximum(pool, result->workers - 1);
            InitializeThreadpoolEnvironment(&environment);
            SetThreadpoolCallbackPool(&environment, pool);
            batch.work = CreateThreadpoolWork(copy_worker, &batch, &environment);
        }
        if (!batch.work)
            result->workers = 1;
    }
    error = copy_directory(&batch, source, target, 0, source_path, source_length, target_path, target_length);
    if (batch.work)
        CloseThreadpoolWork(batch.work);
    if (pool) {
        CloseThreadpool(pool);
        DestroyThreadpoolEnvironment(&environment);
    }
done:
    if (target != INVALID_HANDLE_VALUE)
        CloseHandle(target);
    if (source != INVALID_HANDLE_VALUE)
        CloseHandle(source);
    if (parent != INVALID_HANDLE_VALUE)
        CloseHandle(parent);
    free(paths);
    record_error(result, error);
    return result->error;
}
