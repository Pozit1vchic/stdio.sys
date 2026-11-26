#include "engine.h"
#include "folder_streams.h"

typedef struct {
    DWORD cluster;
    ULONGLONG allocated;
} WALK_PROPERTIES;

static DWORD collect_entry(void *context, HANDLE directory, ULONGLONG parent,
                           const FILE_ID_EXTD_DIR_INFO *entry)
{
    WALK_PROPERTIES *properties = context;
    HANDLE file;
    ULONGLONG size, allocation = 0;
    DWORD error;
    UNREFERENCED_PARAMETER(parent);
    /* The Shell's reparse and provider rules are independent of the engine walk. */
    if (entry->FileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_OFFLINE))
        return ERROR_NOT_SUPPORTED;
    if (entry->FileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        return ERROR_SUCCESS;
    error = tree_open_snapshot_entry(directory, entry, &file, &size);
    if (error)
        return error;
    if (size != (ULONGLONG)entry->EndOfFile.QuadPart)
        error = ERROR_RETRY;
    else
        error = folder_stream_allocation(file, properties->cluster, &allocation);
    CloseHandle(file);
    if (error)
        return error;
    if (allocation > MAXULONGLONG - properties->allocated)
        return ERROR_ARITHMETIC_OVERFLOW;
    properties->allocated += allocation;
    return ERROR_SUCCESS;
}

/* Read-only comparison in the host process. The native property worker owns its result. */
__declspec(dllexport) DWORD WINAPI ShellFolderWalkCompare(PCWSTR path, ULONGLONG *files,
                                                          ULONGLONG *folders, ULONGLONG *bytes,
                                                          ULONGLONG *allocated)
{
    wchar_t volume[MAX_PATH];
    DWORD sectors, sector_bytes, free_clusters, clusters;
    WALK_PROPERTIES properties = {0};
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
    if (error)
        return error;
    *files = totals.files;
    *folders = totals.directories - 1;
    *bytes = totals.bytes;
    *allocated = properties.allocated;
    return ERROR_SUCCESS;
}
