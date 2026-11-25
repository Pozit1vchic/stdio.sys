#include "native_stat.h"
#include <winternl.h>
#include <stddef.h>
#include <stdlib.h>

NTSYSAPI NTSTATUS NTAPI NtQueryInformationFile(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG,
                                               FILE_INFORMATION_CLASS);

enum { FILE_HARD_LINK_INFORMATION_CLASS = 46, MAX_LINK_BYTES = 1024 * 1024 };

typedef struct {
    ULONG next;
    ULONGLONG parent;
    ULONG units;
    wchar_t name[1];
} LINK_ENTRY;

typedef struct {
    ULONG needed, count;
    LINK_ENTRY entries[1];
} LINKS;

static DWORD visit(const LINKS *links, size_t bytes, NTFS_LINK_VISITOR visitor, void *context)
{
    const size_t header = offsetof(LINK_ENTRY, name);
    size_t offset = offsetof(LINKS, entries);
    ULONG i;
    if (bytes < offset || !links->count)
        return ERROR_INVALID_DATA;
    /* Validate the entire response before exposing entries to the caller. */
    for (i = 0; i < links->count; ++i) {
        const LINK_ENTRY *entry = (const void *)((const BYTE *)links + offset);
        const size_t remaining = bytes - offset;
        size_t record;
        if (remaining < header)
            return ERROR_INVALID_DATA;
        record = entry->next ? entry->next : remaining;
        if (record < header || record > remaining ||
            (!!entry->next != (i + 1 < links->count)) ||
            (entry->next % _Alignof(LINK_ENTRY)) ||
            !entry->parent || !entry->units || entry->units > 255 ||
            entry->units > (record - header) / sizeof(wchar_t))
            return ERROR_INVALID_DATA;
        offset += record;
    }
    offset = offsetof(LINKS, entries);
    for (i = 0; i < links->count; ++i) {
        const LINK_ENTRY *entry = (const void *)((const BYTE *)links + offset);
        DWORD error = visitor(context, entry->parent, entry->name, entry->units);
        if (error)
            return error;
        offset += entry->next;
    }
    return ERROR_SUCCESS;
}

DWORD ntfs_visit_links(HANDLE volume_hint, ULONGLONG id, NTFS_LINK_VISITOR visitor, void *context)
{
    FILE_ID_DESCRIPTOR descriptor = { .dwSize = sizeof(descriptor), .Type = FileIdType };
    _Alignas(LINKS) BYTE local[1024];
    LINKS *links = (void *)local;
    DWORD capacity = sizeof(local), error;
    HANDLE file;
    descriptor.FileId.QuadPart = (LONGLONG)id;
    file = OpenFileById(volume_hint, &descriptor, FILE_READ_ATTRIBUTES,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        NULL, FILE_FLAG_OPEN_REPARSE_POINT);
    if (file == INVALID_HANDLE_VALUE)
        return GetLastError();
    for (;;) {
        IO_STATUS_BLOCK io = {0};
        NTSTATUS status = NtQueryInformationFile(file, &io, links, capacity,
                                               (FILE_INFORMATION_CLASS)FILE_HARD_LINK_INFORMATION_CLASS);
        if (status >= 0) {
            if (io.Information <= capacity)
                error = visit(links, io.Information, visitor, context);
            else
                error = ERROR_INVALID_DATA;
            break;
        }
        error = RtlNtStatusToDosError(status);
        if (error != ERROR_MORE_DATA && error != ERROR_INSUFFICIENT_BUFFER)
            break;
        if (capacity == MAX_LINK_BYTES)
            break;
        if (links != (void *)local)
            free(links);
        capacity *= 2;
        links = malloc(capacity);
        if (!links) {
            error = ERROR_NOT_ENOUGH_MEMORY;
            break;
        }
    }
    if (links != (void *)local)
        free(links);
    CloseHandle(file);
    return error;
}
