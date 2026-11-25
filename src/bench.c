#include "bench.h"
#include <bcrypt.h>
#include <objbase.h>
#include <psapi.h>
#include <winternl.h>
#include <errno.h>
#include <stdlib.h>
#include <wchar.h>

#define BENCH_PATH 32768

BOOL parse_unsigned(const wchar_t *text, ULONGLONG *value)
{
    wchar_t *end;
    if (*text < L'0' || *text > L'9')
        return FALSE;
    errno = 0;
    *value = _wcstoui64(text, &end, 10);
    return !errno && !*end;
}

static DWORD make_file(const wchar_t *path, const BYTE *buffer, DWORD capacity,
                       ULONGLONG length, BOOL flush)
{
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD written, error = ERROR_SUCCESS;
    if (file == INVALID_HANDLE_VALUE)
        return GetLastError();
    while (length) {
        DWORD chunk = (DWORD)min(length, capacity);
        if (!WriteFile(file, buffer, chunk, &written, NULL))
            error = GetLastError();
        else if (written != chunk)
            error = ERROR_WRITE_FAULT;
        if (error)
            break;
        length -= chunk;
    }
    if (!error && flush && !FlushFileBuffers(file))
        error = GetLastError();
    if (!CloseHandle(file) && !error)
        error = GetLastError();
    return error;
}

static DWORD prepare(const wchar_t *parent, ULONGLONG count, const wchar_t *layout,
                     ULONGLONG large_bytes)
{
    enum { DATA_SIZE = 1024 * 1024, SHARDS = 64, MANY_SHARDS = 4096, DEPTH = 192 };
    static const DWORD small_sizes[] = {0, 64, 512, 1024, 4096, 8192};
    wchar_t parent_path[MAX_PATH], root[MAX_PATH], directory[MAX_PATH + DEPTH + 16];
    wchar_t file[MAX_PATH + DEPTH + 64], id[40];
    size_t i, created_dirs = 1;
    GUID guid;
    BYTE *data;
    DWORD seed = 0x19274123, error = ERROR_SUCCESS, length;
    BOOL deep = !wcscmp(layout, L"deep"), fanout = !wcscmp(layout, L"fanout");
    BOOL mixed = fanout || !wcscmp(layout, L"mixed");
    size_t shards = fanout ? MANY_SHARDS : SHARDS;
    HRESULT hr;
    if (!count || count > 1000000 || (!large_bytes && !deep && !mixed && wcscmp(layout, L"flat")))
        return ERROR_INVALID_PARAMETER;
    length = GetFullPathNameW(parent, ARRAYSIZE(parent_path), parent_path, NULL);
    if (!length)
        return GetLastError();
    if (length >= ARRAYSIZE(parent_path) || length < 3 || parent_path[1] != L':')
        return ERROR_INVALID_NAME;
    if (large_bytes) {
        ULARGE_INTEGER available;
        if (!GetDiskFreeSpaceExW(parent_path, &available, NULL, NULL))
            return GetLastError();
        if (available.QuadPart < large_bytes)
            return ERROR_DISK_FULL;
    }
    hr = CoCreateGuid(&guid);
    if (FAILED(hr))
        return (DWORD)hr;
    StringFromGUID2(&guid, id, ARRAYSIZE(id));
    if (_snwprintf_s(root, ARRAYSIZE(root), _TRUNCATE, L"%ls%lsstdio-bench-%ls", parent_path,
                     parent_path[length - 1] == L'\\' ? L"" : L"\\", id) < 0)
        return ERROR_FILENAME_EXCED_RANGE;
    data = malloc(DATA_SIZE);
    if (!data)
        return ERROR_NOT_ENOUGH_MEMORY;
    for (i = 0; i < DATA_SIZE; ++i) {
        seed = seed * 1664525u + 1013904223u;
        data[i] = (BYTE)(seed >> 24);
    }
    if (!CreateDirectoryW(root, NULL)) {
        free(data);
        return GetLastError();
    }
    wprintf(L"path=%ls\n", root);
    fflush(stdout);
    swprintf_s(directory, ARRAYSIZE(directory), L"\\\\?\\%ls", root);
    for (i = 0; i < (size_t)count; ++i) {
        ULONGLONG file_size = large_bytes;
        if (deep && i && i <= DEPTH / 2) {
            wcscat_s(directory, ARRAYSIZE(directory), L"\\d");
            if (!CreateDirectoryW(directory, NULL)) {
                error = GetLastError();
                break;
            }
            ++created_dirs;
        } else if (mixed && i % ((count + shards - 1) / shards) == 0) {
            swprintf_s(directory, ARRAYSIZE(directory), L"\\\\?\\%ls\\part-%03zu", root, created_dirs);
            if (!CreateDirectoryW(directory, NULL)) {
                error = GetLastError();
                break;
            }
            ++created_dirs;
        }
        if (mixed)
            file_size = i % 4096 == 0 ? DATA_SIZE : small_sizes[i % ARRAYSIZE(small_sizes)];
        swprintf_s(file, ARRAYSIZE(file), L"%ls\\file-%07zu.%ls", directory, i,
                   i % 127 == 0 ? L"needle" : L"bin");
        error = make_file(file, data, DATA_SIZE, file_size, large_bytes != 0);
        if (error)
            break;
    }
    wprintf(L"files=%zu directories=%zu layout=%ls completed=%d\n", i, created_dirs, layout, !error);
    free(data);
    return error;
}

DWORD benchmark_prepare(const wchar_t *parent, ULONGLONG count, const wchar_t *layout)
{
    return prepare(parent, count, layout, 0);
}

DWORD benchmark_prepare_large(const wchar_t *parent, ULONGLONG mib)
{
    if (!mib || mib > 32768)
        return ERROR_INVALID_PARAMETER;
    return prepare(parent, 1, L"large", mib * 1024 * 1024);
}

#define HASH_BUFFER (1024 * 1024)
#define SHA256_BYTES 32

static DWORD hash_file(HANDLE file, BYTE *buffer, BYTE digest[SHA256_BYTES])
{
    BCRYPT_HASH_HANDLE hash;
    NTSTATUS status = BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &hash, NULL, 0, NULL, 0, 0);
    DWORD error = ERROR_SUCCESS;
    if (status < 0)
        return RtlNtStatusToDosError(status);
    for (;;) {
        DWORD bytes;
        if (!ReadFile(file, buffer, HASH_BUFFER, &bytes, NULL)) {
            error = GetLastError();
            break;
        }
        if (!bytes) {
            status = BCryptFinishHash(hash, digest, SHA256_BYTES, 0);
            break;
        }
        status = BCryptHashData(hash, buffer, bytes, 0);
        if (status < 0)
            break;
    }
    BCryptDestroyHash(hash);
    return error ? error : status < 0 ? RtlNtStatusToDosError(status) : ERROR_SUCCESS;
}

DWORD benchmark_copy(const wchar_t *source, const wchar_t *destination, STDIO_COPY_MODE mode,
                     BOOL hash_after_copy)
{
    HANDLE input, output = INVALID_HANDLE_VALUE;
    FILE_ATTRIBUTE_TAG_INFO attributes;
    BYTE *buffer = NULL;
    BYTE expected[SHA256_BYTES], actual[SHA256_BYTES];
    LARGE_INTEGER length, copied_length, frequency, start, copied, flushed;
    DWORD error;
    unsigned i;
    const wchar_t *mode_name = L"buffered";

    input = CreateFileW(source, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                       FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (input == INVALID_HANDLE_VALUE)
        return GetLastError();
    if (!GetFileInformationByHandleEx(input, FileAttributeTagInfo, &attributes, sizeof(attributes)) ||
        !GetFileSizeEx(input, &length)) {
        error = GetLastError();
        goto done;
    }
    if (attributes.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT |
                                     FILE_ATTRIBUTE_READONLY)) {
        error = ERROR_NOT_SUPPORTED;
        goto done;
    }
    buffer = malloc(HASH_BUFFER);
    if (!buffer) {
        error = ERROR_NOT_ENOUGH_MEMORY;
        goto done;
    }
    if (!hash_after_copy) {
        error = hash_file(input, buffer, expected);
        if (error)
            goto done;
    }
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start);
    error = copy_file_mode(source, destination, mode);
    QueryPerformanceCounter(&copied);
    if (error)
        goto done;
    output = CreateFileW(destination, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, NULL,
                         OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (output == INVALID_HANDLE_VALUE || !FlushFileBuffers(output)) {
        error = GetLastError();
        goto done;
    }
    QueryPerformanceCounter(&flushed);
    if (!GetFileSizeEx(output, &copied_length)) {
        error = GetLastError();
        goto done;
    }
    if (length.QuadPart != copied_length.QuadPart) {
        error = ERROR_CRC;
        goto done;
    }
    if (hash_after_copy) {
        error = hash_file(input, buffer, expected);
        if (error)
            goto done;
    }
    error = hash_file(output, buffer, actual);
    if (error)
        goto done;
    if (memcmp(expected, actual, sizeof(expected))) {
        error = ERROR_CRC;
        goto done;
    }
    if (mode == STDIO_COPY_UNBUFFERED)
        mode_name = L"unbuffered";
    else if (mode == STDIO_COPY_NO_PREALLOCATION)
        mode_name = L"no-preallocation";
    wprintf(L"bytes=%llu mode=%ls copy_seconds=%.6f copy_and_flush_seconds=%.6f sha256=",
            (ULONGLONG)length.QuadPart, mode_name,
            (double)(copied.QuadPart - start.QuadPart) / (double)frequency.QuadPart,
            (double)(flushed.QuadPart - start.QuadPart) / (double)frequency.QuadPart);
    for (i = 0; i < SHA256_BYTES; ++i)
        wprintf(L"%02x", (unsigned)actual[i]);
    wprintf(L"\n");

done:
    if (output != INVALID_HANDLE_VALUE)
        CloseHandle(output);
    CloseHandle(input);
    free(buffer);
    return error;
}

static DWORD baseline_walk(wchar_t *path, size_t length, unsigned depth, TREE_RESULT *result,
                           const wchar_t *needle, ULONGLONG *matches)
{
    WIN32_FIND_DATAW file;
    HANDLE query;
    DWORD error = ERROR_SUCCESS;
    if (depth == 256)
        return ERROR_DIRECTORY;
    if (length + 3 >= BENCH_PATH)
        return ERROR_FILENAME_EXCED_RANGE;
    wcscpy_s(path + length, BENCH_PATH - length, L"\\*");
    query = FindFirstFileExW(path, FindExInfoBasic, &file, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
    if (query == INVALID_HANDLE_VALUE) {
        error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND ? ERROR_SUCCESS : error;
    }
    ++result->directories;
    do {
        if (!wcscmp(file.cFileName, L".") || !wcscmp(file.cFileName, L".."))
            continue;
        if ((file.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ==
            FILE_ATTRIBUTE_DIRECTORY) {
            size_t name_length = wcslen(file.cFileName);
            if (length + 1 + name_length + 1 >= BENCH_PATH) {
                error = ERROR_FILENAME_EXCED_RANGE;
                break;
            }
            wcscpy_s(path + length + 1, BENCH_PATH - length - 1, file.cFileName);
            error = baseline_walk(path, length + 1 + name_length, depth + 1, result, needle, matches);
            if (error)
                break;
        } else {
            ++result->files;
            if (!(file.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                result->bytes += ((ULONGLONG)file.nFileSizeHigh << 32) | file.nFileSizeLow;
            if (needle && FindStringOrdinal(FIND_FROMSTART, file.cFileName, -1, needle, -1, TRUE) >= 0)
                ++*matches;
        }
    } while (FindNextFileW(query, &file));
    if (!error) {
        error = GetLastError();
        if (error == ERROR_NO_MORE_FILES)
            error = ERROR_SUCCESS;
    }
    FindClose(query);
    return error;
}

DWORD benchmark_baseline(const wchar_t *input, const wchar_t *needle)
{
    wchar_t *path = malloc(BENCH_PATH * sizeof(*path));
    DWORD length, error;
    TREE_RESULT result = {0};
    ULONGLONG matches = 0;
    if (!path)
        return ERROR_NOT_ENOUGH_MEMORY;
    wcscpy_s(path, BENCH_PATH, L"\\\\?\\");
    length = GetFullPathNameW(input, BENCH_PATH - 4, path + 4, NULL);
    if (length < 3 || length >= BENCH_PATH - 4 || path[5] != L':') {
        free(path);
        return ERROR_INVALID_NAME;
    }
    length += 4;
    if (path[length - 1] == L'\\')
        --length;
    error = baseline_walk(path, length, 0, &result, needle, &matches);
    free(path);
    wprintf(L"files=%llu directories=%llu bytes=%llu matches=%llu\n",
            result.files, result.directories, result.bytes, matches);
    return error;
}

static BOOL quote_argument(wchar_t *line, size_t *length, const wchar_t *arg)
{
    size_t units;
    if (*length >= BENCH_PATH - 3)
        return FALSE;
    if (*length)
        line[(*length)++] = L' ';
    if (*arg && !wcspbrk(arg, L" \t\"")) {
        units = wcslen(arg);
        if (units >= BENCH_PATH - *length)
            return FALSE;
        memcpy(line + *length, arg, (units + 1) * sizeof(*line));
        *length += units;
        return TRUE;
    }
    line[(*length)++] = L'"';
    for (;;) {
        size_t slashes = 0, i;
        while (*arg == L'\\') {
            ++slashes;
            ++arg;
        }
        if (!*arg || *arg == L'"')
            slashes *= 2;
        if (*arg == L'"')
            ++slashes;
        if (slashes >= BENCH_PATH - *length - 3)
            return FALSE;
        for (i = 0; i < slashes; ++i)
            line[(*length)++] = L'\\';
        if (!*arg)
            break;
        line[(*length)++] = *arg++;
    }
    line[(*length)++] = L'"';
    line[*length] = 0;
    return TRUE;
}

static ULONGLONG ticks(FILETIME time)
{
    return ((ULONGLONG)time.dwHighDateTime << 32) | time.dwLowDateTime;
}

DWORD benchmark_measure(const wchar_t *label, ULONGLONG files, const wchar_t *cache,
                         const wchar_t *exit_policy, int argc, wchar_t **argv)
{
    wchar_t full[MAX_PATH], *line;
    size_t length = 0;
    int i;
    DWORD error = ERROR_SUCCESS, exit_code;
    DWORD full_length = GetFullPathNameW(argv[0], ARRAYSIZE(full), full, NULL);
    BOOL robocopy = !wcscmp(exit_policy, L"robocopy");
    LARGE_INTEGER start, end, frequency;
    FILETIME created, exited, kernel, user;
    IO_COUNTERS io;
    PROCESS_MEMORY_COUNTERS_EX memory = {sizeof(memory)};
    STARTUPINFOW startup = {sizeof(startup)};
    PROCESS_INFORMATION child;
    double seconds, cpu;
    if (!full_length || full_length >= ARRAYSIZE(full) || !*label ||
        wcspbrk(label, L"\t\r\n") || !files || (!robocopy && wcscmp(exit_policy, L"zero")) ||
        (wcscmp(cache, L"cold") && wcscmp(cache, L"warm") && wcscmp(cache, L"as-created")))
        return ERROR_INVALID_PARAMETER;
    line = malloc(BENCH_PATH * sizeof(*line));
    if (!line)
        return ERROR_NOT_ENOUGH_MEMORY;
    for (i = 0; i < argc; ++i) {
        if (!quote_argument(line, &length, i ? argv[i] : full)) {
            free(line);
            return ERROR_FILENAME_EXCED_RANGE;
        }
    }
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start);
    if (!CreateProcessW(full, line, NULL, NULL, TRUE, 0, NULL, NULL, &startup, &child)) {
        error = GetLastError();
        free(line);
        return error;
    }
    free(line);
    CloseHandle(child.hThread);
    if (WaitForSingleObject(child.hProcess, INFINITE) != WAIT_OBJECT_0) {
        error = GetLastError();
        CloseHandle(child.hProcess);
        return error;
    }
    QueryPerformanceCounter(&end);
    if (!GetExitCodeProcess(child.hProcess, &exit_code) ||
        !GetProcessTimes(child.hProcess, &created, &exited, &kernel, &user) ||
        !GetProcessIoCounters(child.hProcess, &io) ||
        !GetProcessMemoryInfo(child.hProcess, (PROCESS_MEMORY_COUNTERS *)&memory, sizeof(memory))) {
        error = GetLastError();
        CloseHandle(child.hProcess);
        return error;
    }
    CloseHandle(child.hProcess);
    seconds = (double)(end.QuadPart - start.QuadPart) / (double)frequency.QuadPart;
    cpu = (double)(ticks(kernel) + ticks(user)) / 10000000.0;
    wprintf(L"metric\tlabel\tcache\tfiles\tseconds\tfiles_per_second\tcpu_s\tcpu_pct_one_core"
            L"\tpeak_ws_bytes\tread_ops\twrite_ops\tother_ops\tread_bytes\twrite_bytes\tother_bytes\texit\n");
    wprintf(L"result\t%ls\t%ls\t%llu\t%.6f\t%.2f\t%.6f\t%.2f\t%zu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%lu\n",
            label, cache, files, seconds, (double)files / seconds, cpu, cpu * 100 / seconds,
            (size_t)memory.PeakWorkingSetSize, io.ReadOperationCount, io.WriteOperationCount,
            io.OtherOperationCount, io.ReadTransferCount, io.WriteTransferCount, io.OtherTransferCount, exit_code);
    return exit_code >= (robocopy ? 8u : 1u) ? ERROR_PROCESS_ABORTED : ERROR_SUCCESS;
}
