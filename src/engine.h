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
DWORD tree_snapshot_directory(const wchar_t *path, TREE_VISITOR visitor, void *context,
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
typedef enum {
    TREE_COPY_UNCHANGED,
    TREE_COPY_DIRECTORY_CREATED,
    TREE_COPY_FILE_COPIED,
    TREE_COPY_FILE_OUTCOME_UNKNOWN
} TREE_COPY_EFFECT;

typedef enum {
    TREE_COPY_COMPLETE,
    TREE_COPY_FAILED,
    TREE_COPY_SKIPPED,
    TREE_COPY_CANCELLED,
    TREE_COPY_INCOMPLETE
} TREE_COPY_STATE;

typedef struct {
    const wchar_t *source;
    const wchar_t *destination;
    const FILE_ID_128 *source_id;
    BOOL directory;
    TREE_COPY_STATE state;
    TREE_COPY_EFFECT effect;
    DWORD error;
} TREE_COPY_ITEM;

/* Paths and ID are borrowed during the callback. Callbacks run serially on copy workers.
   A report covers attempted objects and work skipped from queues already populated.
   Traversal stops on failure, so descendants of unopened folders are not enumerated. */
typedef void (*TREE_COPY_REPORT)(void *context, const TREE_COPY_ITEM *item);
typedef struct {
    TREE_COPY_REPORT report;
    void *context;
    HANDLE cancel_event; /* Optional, caller-owned manual-reset event. */
    BOOL background; /* Yield device capacity to waiting interactive operations. */
} TREE_COPY_OPTIONS;

typedef struct {
    TREE_RESULT totals;
    ULONGLONG directories_created;
    ULONGLONG files_outcome_unknown;
    ULONGLONG skipped;
    ULONGLONG cancelled;
} TREE_COPY_RESULT;

/* ERROR_BUSY means admission was denied before opening either path or creating the destination. */
DWORD copy_new_tree_report(const wchar_t *source, const wchar_t *destination, DWORD workers,
                           BOOL unbuffered_large, const TREE_COPY_OPTIONS *options,
                           TREE_COPY_RESULT *result);
DWORD copy_new_tree(const wchar_t *source, const wchar_t *destination, DWORD workers,
                    BOOL unbuffered_large, TREE_RESULT *result);
HRESULT shell_operation(const wchar_t *verb, int count, wchar_t **paths,
                        const wchar_t *destination, HWND owner, BOOL quiet);
DWORD index_console(const wchar_t *drive);
DWORD directory_index_console(const wchar_t *path);
void print_error(const wchar_t *operation, DWORD error);
