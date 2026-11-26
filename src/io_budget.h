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
    BOOL native;
} IO_PERMIT;

IO_DEVICE io_device_for_path(const wchar_t *path, DWORD limit);
DWORD io_budget_acquire(IO_PERMIT *permit, IO_DEVICE source, IO_DEVICE destination,
                        HANDLE cancel, BOOL background);
/* The running flag is owned by the caller and remains valid while waiting. */
DWORD io_budget_acquire_background(IO_PERMIT *permit, IO_DEVICE device, const volatile LONG *running,
                                   BOOL prefer_native, BOOL *yielded_to_native);
/* Observe work already admitted by the caller. Native callbacks must stay
   free to run even when the device is full. Release as for a regular permit. */
void io_budget_account_native(IO_PERMIT *permit, IO_DEVICE source, IO_DEVICE destination);
void io_budget_release(IO_PERMIT *permit);
