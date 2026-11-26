#pragma once
#include <windows.h>

typedef struct {
    ULONGLONG files, folders, bytes, allocated;
} FOLDER_WALK_TOTALS;

typedef DWORD (*FOLDER_WALK_PROGRESS)(void *context, const FOLDER_WALK_TOTALS *totals);

DWORD folder_walk_properties(PCWSTR path, const volatile LONG *running, void (*first_entry)(void),
                             FOLDER_WALK_PROGRESS progress, void *context, FOLDER_WALK_TOTALS *totals);
