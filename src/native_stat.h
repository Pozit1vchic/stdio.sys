#pragma once
#include <windows.h>

/* FILE_STAT_INFORMATION from the WDK. The Win32 SDK omits this class. */
typedef struct {
    LARGE_INTEGER file_id, creation, access, write, change, allocation, end_of_file;
    ULONG attributes, reparse_tag, links;
    ACCESS_MASK effective_access;
} NATIVE_FILE_STAT;

BOOL query_native_stat(HANDLE handle, NATIVE_FILE_STAT *stat);
/* The caller owns a writable extended DOS path, restored before return. */
DWORD open_indexed_file(wchar_t *path, HANDLE *out);
