#include "folder_streams.h"
#include <stddef.h>

#define STREAM_FIRST_BYTES 1056
#define STREAM_MAX_BYTES (64 * 1024)

static DWORD sum_streams(const BYTE *buffer, DWORD capacity, DWORD minimum, ULONGLONG *bytes)
{
    const DWORD header = (DWORD)offsetof(FILE_STREAM_INFO, StreamName);
    ULONGLONG total = 0;
    for (;;) {
        const FILE_STREAM_INFO *stream = (const void *)buffer;
        DWORD next;
        ULONGLONG allocation;
        if (capacity < header || stream->StreamNameLength > capacity - header ||
            stream->StreamAllocationSize.QuadPart < 0)
            return ERROR_INVALID_DATA;
        allocation = (ULONGLONG)stream->StreamAllocationSize.QuadPart;
        if (allocation >= minimum) {
            if (allocation > MAXULONGLONG - total)
                return ERROR_ARITHMETIC_OVERFLOW;
            total += allocation;
        }
        next = stream->NextEntryOffset;
        if (!next)
            break;
        if (next % _Alignof(FILE_STREAM_INFO) || next < header + stream->StreamNameLength ||
            next > capacity - header)
            return ERROR_INVALID_DATA;
        buffer += next;
        capacity -= next;
    }
    *bytes = total;
    return ERROR_SUCCESS;
}

DWORD folder_stream_allocation(HANDLE file, DWORD minimum_allocation, ULONGLONG *bytes)
{
    _Alignas(FILE_STREAM_INFO) BYTE first[STREAM_FIRST_BYTES];
    DWORD error;
    DWORD capacity;

    if (GetFileInformationByHandleEx(file, FileStreamInfo, first, sizeof(first)))
        return sum_streams(first, sizeof(first), minimum_allocation, bytes);
    error = GetLastError();
    for (capacity = 2 * STREAM_FIRST_BYTES; error == ERROR_MORE_DATA &&
         capacity <= STREAM_MAX_BYTES; capacity *= 2) {
        BYTE *buffer = HeapAlloc(GetProcessHeap(), 0, capacity);
        if (!buffer)
            return ERROR_NOT_ENOUGH_MEMORY;
        if (GetFileInformationByHandleEx(file, FileStreamInfo, buffer, capacity))
            error = sum_streams(buffer, capacity, minimum_allocation, bytes);
        else
            error = GetLastError();
        HeapFree(GetProcessHeap(), 0, buffer);
    }
    return error;
}
