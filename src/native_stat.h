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

typedef DWORD (*NTFS_LINK_VISITOR)(void *context, ULONGLONG parent, const wchar_t *name, DWORD units);
/* Names are borrowed during the callback. The caller must validate each path before publishing it. */
DWORD ntfs_visit_links(HANDLE volume_hint, ULONGLONG id, NTFS_LINK_VISITOR visitor, void *context);
