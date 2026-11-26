#pragma once
#include <windows.h>
#include "io_budget.h"

typedef struct {
    ULONGLONG files, folders, bytes, allocated;
} FOLDER_WALK_TOTALS;

typedef DWORD (*FOLDER_WALK_PROGRESS)(void *context, const FOLDER_WALK_TOTALS *totals);

typedef struct {
    IO_DEVICE device;
    BOOL prefer_native;
    DWORD yielded;
} FOLDER_WALK_BUDGET;

DWORD folder_walk_properties(PCWSTR path, const volatile LONG *running, void (*first_entry)(void),
                             FOLDER_WALK_PROGRESS progress, void *context, FOLDER_WALK_BUDGET *budget,
                             FOLDER_WALK_TOTALS *totals);
