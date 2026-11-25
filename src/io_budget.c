#include "io_budget.h"
#include <winioctl.h>

static SRWLOCK lock = SRWLOCK_INIT;
static CONDITION_VARIABLE available = CONDITION_VARIABLE_INIT;
static IO_PERMIT *active;

IO_DEVICE io_device_for_path(const wchar_t *path, DWORD limit)
{
    wchar_t volume[MAX_PATH], device[MAX_PATH];
    VOLUME_DISK_EXTENTS extents;
    DWORD bytes;
    HANDLE handle;
    IO_DEVICE result = { .limit = 1 };
    if (!GetVolumePathNameW(path, volume, ARRAYSIZE(volume)) ||
        !GetVolumeNameForVolumeMountPointW(volume, device, ARRAYSIZE(device)))
        return result;
    device[wcslen(device) - 1] = 0;
    handle = CreateFileW(device, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         NULL, OPEN_EXISTING, 0, NULL);
    if (handle == INVALID_HANDLE_VALUE)
        return result;
    if (DeviceIoControl(handle, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, NULL, 0,
                        &extents, sizeof(extents), &bytes, NULL) &&
        bytes >= sizeof(extents) && extents.NumberOfDiskExtents == 1)
        result = (IO_DEVICE){ extents.Extents[0].DiskNumber, limit ? limit : 1, TRUE };
    CloseHandle(handle);
    return result;
}

static BOOL fits(IO_DEVICE device)
{
    DWORD used = 0, limit = device.limit;
    for (const IO_PERMIT *item = active; item; item = item->next) {
        if (!device.known || !item->source.known || !item->destination.known)
            return FALSE;
        if (item->source.disk == device.disk || item->destination.disk == device.disk) {
            ++used;
            if (item->source.disk == device.disk)
                limit = min(limit, item->source.limit);
            if (item->destination.disk == device.disk)
                limit = min(limit, item->destination.limit);
        }
    }
    return used < limit;
}

DWORD io_budget_acquire(IO_PERMIT *permit, IO_DEVICE source, IO_DEVICE destination, HANDLE cancel)
{
    DWORD error = ERROR_SUCCESS;
    AcquireSRWLockExclusive(&lock);
    for (;;) {
        if (cancel && WaitForSingleObject(cancel, 0) == WAIT_OBJECT_0) {
            error = ERROR_CANCELLED;
            break;
        }
        if (fits(source) && fits(destination)) {
            *permit = (IO_PERMIT){ active, source, destination };
            active = permit;
            break;
        }
        if (!SleepConditionVariableSRW(&available, &lock, cancel ? 25 : INFINITE, 0) &&
            GetLastError() != ERROR_TIMEOUT) {
            error = GetLastError();
            break;
        }
    }
    ReleaseSRWLockExclusive(&lock);
    return error;
}

void io_budget_release(IO_PERMIT *permit)
{
    IO_PERMIT **link;
    AcquireSRWLockExclusive(&lock);
    for (link = &active; *link != permit; link = &(*link)->next)
        ;
    *link = permit->next;
    ReleaseSRWLockExclusive(&lock);
    WakeAllConditionVariable(&available);
}
