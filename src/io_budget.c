#include "io_budget.h"
#include <winioctl.h>

static SRWLOCK lock = SRWLOCK_INIT;
static CONDITION_VARIABLE available = CONDITION_VARIABLE_INIT;
static IO_PERMIT *active;

typedef struct IO_WAIT {
    struct IO_WAIT *next;
    IO_DEVICE source, destination;
    ULONGLONG queued_at;
    HANDLE cancel;
    BOOL background;
} IO_WAIT;

static IO_WAIT *waiting;
/* Once promoted, a background request has its original FIFO position. */
enum { BACKGROUND_PROMOTION_MS = 500 };

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

static BOOL overlaps(IO_DEVICE left, IO_DEVICE right)
{
    return !left.known || !right.known || left.disk == right.disk;
}

static BOOL promoted(const IO_WAIT *request, ULONGLONG now)
{
    return !request->background || now - request->queued_at >= BACKGROUND_PROMOTION_MS;
}

static BOOL has_predecessor(const IO_WAIT *request)
{
    ULONGLONG now = GetTickCount64();
    BOOL priority = promoted(request, now);
    BOOL earlier = TRUE;
    for (const IO_WAIT *other = waiting; other; other = other->next) {
        BOOL other_priority;
        if (other == request) {
            earlier = FALSE;
            continue;
        }
        other_priority = promoted(other, now);
        if ((other_priority > priority || (earlier && other_priority == priority)) &&
            (overlaps(request->source, other->source) ||
             overlaps(request->source, other->destination) ||
             overlaps(request->destination, other->source) ||
             overlaps(request->destination, other->destination)) &&
            (!other->cancel || WaitForSingleObject(other->cancel, 0) != WAIT_OBJECT_0) &&
            fits(other->source) && fits(other->destination))
            return TRUE;
    }
    return FALSE;
}

DWORD io_budget_acquire(IO_PERMIT *permit, IO_DEVICE source, IO_DEVICE destination,
                        HANDLE cancel, BOOL background)
{
    DWORD error = ERROR_SUCCESS;
    IO_WAIT request = { NULL, source, destination, 0, cancel, background };
    IO_WAIT **link;
    AcquireSRWLockExclusive(&lock);
    request.queued_at = GetTickCount64();
    for (link = &waiting; *link; link = &(*link)->next)
        ;
    *link = &request;
    for (;;) {
        if (cancel && WaitForSingleObject(cancel, 0) == WAIT_OBJECT_0) {
            error = ERROR_CANCELLED;
            break;
        }
        if (fits(source) && fits(destination) && !has_predecessor(&request)) {
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
    for (link = &waiting; *link != &request; link = &(*link)->next)
        ;
    *link = request.next;
    ReleaseSRWLockExclusive(&lock);
    /* A request elsewhere in the queue may now be eligible on a separate disk. */
    WakeAllConditionVariable(&available);
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
