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
    const volatile LONG *running;
    BOOL background;
    BOOL prefer_native;
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

static BOOL fits(IO_DEVICE device, BOOL yield_native, BOOL *yielded_to_native)
{
    DWORD used = 0, limit = device.limit;
    for (const IO_PERMIT *item = active; item; item = item->next) {
        if (!device.known || !item->source.known || !item->destination.known)
            return FALSE;
        if (item->source.disk == device.disk || item->destination.disk == device.disk) {
            if (yield_native && item->native) {
                if (yielded_to_native)
                    *yielded_to_native = TRUE;
                return FALSE;
            }
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

static BOOL cancelled(const IO_WAIT *request)
{
    return (request->running && !ReadAcquire(request->running)) ||
           (request->cancel && WaitForSingleObject(request->cancel, 0) == WAIT_OBJECT_0);
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
            !cancelled(other) && fits(other->source, other->prefer_native && !other_priority, NULL) &&
            fits(other->destination, other->prefer_native && !other_priority, NULL))
            return TRUE;
    }
    return FALSE;
}

static DWORD acquire(IO_PERMIT *permit, IO_WAIT *request, BOOL *yielded_to_native)
{
    DWORD error = ERROR_SUCCESS;
    IO_WAIT **link;
    if (yielded_to_native)
        *yielded_to_native = FALSE;
    AcquireSRWLockExclusive(&lock);
    request->queued_at = GetTickCount64();
    for (link = &waiting; *link; link = &(*link)->next)
        ;
    *link = request;
    for (;;) {
        BOOL low_priority = !promoted(request, GetTickCount64());
        BOOL yield_native = request->prefer_native && low_priority;
        if (cancelled(request)) {
            error = ERROR_CANCELLED;
            break;
        }
        if (fits(request->source, yield_native, yielded_to_native) &&
            fits(request->destination, yield_native, yielded_to_native) && !has_predecessor(request)) {
            *permit = (IO_PERMIT){ active, request->source, request->destination, FALSE };
            active = permit;
            break;
        }
        if (!SleepConditionVariableSRW(&available, &lock,
                                       request->cancel || request->running || low_priority ? 25 : INFINITE, 0) &&
            GetLastError() != ERROR_TIMEOUT) {
            error = GetLastError();
            break;
        }
    }
    for (link = &waiting; *link != request; link = &(*link)->next)
        ;
    *link = request->next;
    ReleaseSRWLockExclusive(&lock);
    /* A request elsewhere in the queue may now be eligible on a separate disk. */
    WakeAllConditionVariable(&available);
    return error;
}

DWORD io_budget_acquire(IO_PERMIT *permit, IO_DEVICE source, IO_DEVICE destination,
                        HANDLE cancel, BOOL background)
{
    IO_WAIT request = { .source = source, .destination = destination, .cancel = cancel,
                        .background = background, .prefer_native = background };
    return acquire(permit, &request, NULL);
}

DWORD io_budget_acquire_background(IO_PERMIT *permit, IO_DEVICE device, const volatile LONG *running,
                                   BOOL prefer_native, BOOL *yielded_to_native)
{
    IO_WAIT request = { .source = device, .destination = device, .running = running,
                        .background = TRUE, .prefer_native = prefer_native };
    return acquire(permit, &request, yielded_to_native);
}

void io_budget_account_native(IO_PERMIT *permit, IO_DEVICE source, IO_DEVICE destination)
{
    AcquireSRWLockExclusive(&lock);
    *permit = (IO_PERMIT){ active, source, destination, TRUE };
    active = permit;
    ReleaseSRWLockExclusive(&lock);
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
