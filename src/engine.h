#pragma once
#include <windows.h>
#include <stdio.h>

#ifndef _M_X64
#error stdio.sys requires x64
#endif

typedef struct {
    ULONGLONG files;
    ULONGLONG directories;
    ULONGLONG bytes;
    ULONGLONG errors;
    DWORD error;
    DWORD workers;
} TREE_RESULT;

/* A zero worker count selects the storage-aware default. */
DWORD tree_run(const wchar_t *path, BOOL remove, DWORD workers, TREE_RESULT *result);
DWORD tree_list(const wchar_t *path);
DWORD tree_find(const wchar_t *path, const wchar_t *needle, ULONGLONG *matches);
typedef DWORD (*TREE_VISITOR)(void *context, ULONGLONG parent, const FILE_ID_EXTD_DIR_INFO *entry);
DWORD tree_snapshot(const wchar_t *path, TREE_VISITOR visitor, void *context,
                    ULONGLONG *root_id, TREE_RESULT *result);
DWORD storage_workers(const wchar_t *path);
DWORD storage_info(const wchar_t *path);
DWORD plain_copy(const wchar_t *source, const wchar_t *destination);
typedef enum {
    STDIO_COPY_BUFFERED,
    STDIO_COPY_UNBUFFERED,
    STDIO_COPY_NO_PREALLOCATION
} STDIO_COPY_MODE;
DWORD copy_file_mode(const wchar_t *source, const wchar_t *destination, STDIO_COPY_MODE mode);
DWORD copy_new_tree(const wchar_t *source, const wchar_t *destination, DWORD workers,
                    BOOL unbuffered_large, TREE_RESULT *result);
HRESULT shell_operation(const wchar_t *verb, int count, wchar_t **paths,
                        const wchar_t *destination, HWND owner, BOOL quiet);
DWORD index_console(const wchar_t *drive);
DWORD directory_index_console(const wchar_t *path);
void print_error(const wchar_t *operation, DWORD error);
