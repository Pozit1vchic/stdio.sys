#pragma once
#include <windows.h>

typedef struct {
    DWORD disk;
    DWORD limit;
    BOOL known;
} IO_DEVICE;

typedef struct IO_PERMIT {
    struct IO_PERMIT *next;
    IO_DEVICE source, destination;
} IO_PERMIT;

IO_DEVICE io_device_for_path(const wchar_t *path, DWORD limit);
DWORD io_budget_acquire(IO_PERMIT *permit, IO_DEVICE source, IO_DEVICE destination,
                        HANDLE cancel, BOOL background);
void io_budget_release(IO_PERMIT *permit);
