#include "engine.h"
#include "folder_streams.h"
#include "folder_walk_lab.h"

typedef struct {
    DWORD cluster;
    FOLDER_WALK_TOTALS totals;
    const volatile LONG *running;
    void (*first_entry)(void);
    FOLDER_WALK_PROGRESS progress;
    void *progress_context;
    FOLDER_WALK_BUDGET *budget;
    IO_PERMIT permit;
    unsigned remaining;
} WALK_PROPERTIES;

enum { BUDGET_BATCH_FILES = 32 };

static DWORD collect_entry(void *context, HANDLE directory, ULONGLONG parent,
                           const FILE_ID_EXTD_DIR_INFO *entry)
{
    WALK_PROPERTIES *properties = context;
    HANDLE file;
    ULONGLONG size, allocation = 0;
    DWORD error;
    UNREFERENCED_PARAMETER(parent);
    if (properties->first_entry) {
        properties->first_entry();
        properties->first_entry = NULL;
    }
    if (properties->running && !ReadAcquire(properties->running))
        return ERROR_CANCELLED;
    /* The Shell's reparse and provider rules are independent of the engine walk. */
    if (entry->FileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_OFFLINE))
        return ERROR_NOT_SUPPORTED;
    if (entry->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        ++properties->totals.folders;
        return properties->progress ? properties->progress(properties->progress_context, &properties->totals) :
               ERROR_SUCCESS;
    }
    if (properties->budget && !properties->remaining) {
        BOOL yielded;
        error = io_budget_acquire_background(&properties->permit, properties->budget->device,
                                             properties->running, properties->budget->prefer_native, &yielded);
        if (yielded)
            ++properties->budget->yielded;
        if (error)
            return error;
        properties->remaining = BUDGET_BATCH_FILES;
    }
    error = tree_open_snapshot_entry(directory, entry, &file, &size);
    if (!error) {
        if (size != (ULONGLONG)entry->EndOfFile.QuadPart)
            error = ERROR_RETRY;
        else
            error = folder_stream_allocation(file, properties->cluster, &allocation);
        CloseHandle(file);
    }
    if (properties->remaining && !--properties->remaining)
        io_budget_release(&properties->permit);
    if (error)
        return error;
    if (allocation > MAXULONGLONG - properties->totals.allocated ||
        size > MAXULONGLONG - properties->totals.bytes)
        return ERROR_ARITHMETIC_OVERFLOW;
    properties->totals.allocated += allocation;
    properties->totals.bytes += size;
    ++properties->totals.files;
    return properties->progress ? properties->progress(properties->progress_context, &properties->totals) :
           ERROR_SUCCESS;
}

DWORD folder_walk_properties(PCWSTR path, const volatile LONG *running, void (*first_entry)(void),
                             FOLDER_WALK_PROGRESS progress, void *context, FOLDER_WALK_BUDGET *budget,
                             FOLDER_WALK_TOTALS *result)
{
    wchar_t volume[MAX_PATH];
    DWORD sectors, sector_bytes, free_clusters, clusters;
    WALK_PROPERTIES properties = { .running = running, .first_entry = first_entry,
                                   .progress = progress, .progress_context = context, .budget = budget };
    ULONGLONG id;
    TREE_RESULT totals;
    DWORD error;
    if (!GetVolumePathNameW(path, volume, ARRAYSIZE(volume)) ||
        !GetDiskFreeSpaceW(volume, &sectors, &sector_bytes, &free_clusters, &clusters))
        return GetLastError();
    if (!sectors || !sector_bytes || sectors > MAXDWORD / sector_bytes)
        return ERROR_INVALID_DATA;
    properties.cluster = sectors * sector_bytes;
    error = tree_snapshot(path, collect_entry, &properties, &id, &totals);
    if (properties.remaining)
        io_budget_release(&properties.permit);
    if (error)
        return error;
    if (running && !ReadAcquire(running))
        return ERROR_CANCELLED;
    if (totals.files != properties.totals.files ||
        totals.directories != properties.totals.folders + 1 ||
        totals.bytes != properties.totals.bytes)
        return ERROR_INVALID_DATA;
    *result = properties.totals;
    return ERROR_SUCCESS;
}

/* Read-only comparison in the host process. The native property worker owns its result. */
__declspec(dllexport) DWORD WINAPI ShellFolderWalkCompare(PCWSTR path, ULONGLONG *files,
                                                          ULONGLONG *folders, ULONGLONG *bytes,
                                                          ULONGLONG *allocated)
{
    FOLDER_WALK_TOTALS totals;
    DWORD error = folder_walk_properties(path, NULL, NULL, NULL, NULL, NULL, &totals);
    if (!error) {
        *files = totals.files;
        *folders = totals.folders;
        *bytes = totals.bytes;
        *allocated = totals.allocated;
    }
    return error;
}
