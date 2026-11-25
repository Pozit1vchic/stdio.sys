#include "folder_streams.h"
#include <stdio.h>

static BOOL write_stream(const wchar_t *path, const BYTE *buffer, DWORD size)
{
    DWORD written;
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    BOOL success;
    if (file == INVALID_HANDLE_VALUE)
        return FALSE;
    success = WriteFile(file, buffer, size, &written, NULL) && written == size;
    CloseHandle(file);
    return success;
}

int main(void)
{
    static const BYTE data[8192] = {1};
    static const wchar_t stream_prefix[] = L":long_stream_name_to_exercise_buffer_growth";
    wchar_t directory[MAX_PATH], path[MAX_PATH], stream[MAX_PATH + 100];
    HANDLE file = INVALID_HANDLE_VALUE;
    ULONGLONG bytes = 0, ordinary = 0, named = 0;
    _Alignas(FILE_STREAM_INFO) BYTE small_buffer[1024];
    DWORD i;
    int status = 1;
    DWORD length = GetTempPathW(ARRAYSIZE(directory), directory);
    if (!length || length >= ARRAYSIZE(directory) || !GetTempFileNameW(directory, L"sts", 0, path))
        return 1;

    if (!write_stream(path, data, sizeof(data)))
        goto done;
    file = CreateFileW(path, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
                       NULL, OPEN_EXISTING, 0, NULL);
    if (file == INVALID_HANDLE_VALUE ||
        folder_stream_allocation(file, 1, &ordinary) || ordinary < sizeof(data))
        goto done;
    bytes = 1;
    if (folder_stream_allocation(file, MAXDWORD, &bytes) || bytes != 0)
        goto done;
    CloseHandle(file);
    file = INVALID_HANDLE_VALUE;

    for (i = 0; i < 32; ++i) {
        if (swprintf_s(stream, ARRAYSIZE(stream), L"%ls%ls%lu", path, stream_prefix, i) < 0 ||
            !write_stream(stream, data, sizeof(data)))
            goto done;
    }
    file = CreateFileW(stream, FILE_READ_ATTRIBUTES, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (file == INVALID_HANDLE_VALUE)
        goto done;
    {
        FILE_STANDARD_INFO info;
        if (!GetFileInformationByHandleEx(file, FileStandardInfo, &info, sizeof(info)) ||
            info.AllocationSize.QuadPart < sizeof(data))
            goto done;
        named = (ULONGLONG)info.AllocationSize.QuadPart;
    }
    CloseHandle(file);
    file = CreateFileW(path, FILE_READ_ATTRIBUTES, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (file == INVALID_HANDLE_VALUE ||
        GetFileInformationByHandleEx(file, FileStreamInfo, small_buffer, sizeof(small_buffer)) ||
        GetLastError() != ERROR_MORE_DATA ||
        folder_stream_allocation(file, 1, &bytes) || bytes != ordinary + 32 * named)
        goto done;

    bytes = 73;
    if (folder_stream_allocation(INVALID_HANDLE_VALUE, 1, &bytes) != ERROR_INVALID_HANDLE || bytes != 73)
        goto done;
    status = 0;

done:
    if (file != INVALID_HANDLE_VALUE)
        CloseHandle(file);
    DeleteFileW(path);
    if (status)
        fputs("Could not verify stream allocation or error handling.\n", stderr);
    return status;
}
