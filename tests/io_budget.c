#include "io_budget.h"
#include <stdio.h>

typedef struct {
    IO_DEVICE source, destination;
    HANDLE cancel;
    DWORD result;
    BOOL background;
    volatile LONG *order;
    LONG position;
    const volatile LONG *running;
    BOOL yielded;
} REQUEST;

static DWORD WINAPI acquire(void *context)
{
    REQUEST *request = context;
    IO_PERMIT permit;
    if (request->running)
        request->result = io_budget_acquire_background(&permit, request->source, request->running,
                                                       &request->yielded);
    else
        request->result = io_budget_acquire(&permit, request->source, request->destination,
                                            request->cancel, request->background);
    if (!request->result) {
        if (request->order)
            request->position = InterlockedIncrement(request->order);
        io_budget_release(&permit);
    }
    return 0;
}

static BOOL check_native_priority(IO_DEVICE device, BOOL age)
{
    IO_PERMIT native;
    volatile LONG running = TRUE;
    REQUEST request = { .source = device, .running = &running };
    HANDLE thread;
    DWORD observed;
    io_budget_account_native(&native, device, device);
    thread = CreateThread(NULL, 0, acquire, &request, 0, NULL);
    if (!thread) {
        io_budget_release(&native);
        return FALSE;
    }
    observed = WaitForSingleObject(thread, age ? 2000 : 100);
    InterlockedExchange(&running, FALSE);
    WaitForSingleObject(thread, INFINITE);
    io_budget_release(&native);
    CloseHandle(thread);
    return request.yielded && (age ? observed == WAIT_OBJECT_0 && request.result == ERROR_SUCCESS :
                               observed == WAIT_TIMEOUT && request.result == ERROR_CANCELLED);
}

static BOOL check_request(IO_DEVICE a, IO_DEVICE b, BOOL blocked)
{
    REQUEST request = { .source = a, .destination = b,
                        .cancel = CreateEventW(NULL, TRUE, FALSE, NULL),
                        .result = ERROR_INVALID_STATE };
    HANDLE thread;
    DWORD observed;
    BOOL success;
    if (!request.cancel)
        return FALSE;
    thread = CreateThread(NULL, 0, acquire, &request, 0, NULL);
    if (!thread) {
        CloseHandle(request.cancel);
        return FALSE;
    }
    observed = WaitForSingleObject(thread, blocked ? 100 : 2000);
    SetEvent(request.cancel);
    WaitForSingleObject(thread, INFINITE);
    success = blocked ? observed == WAIT_TIMEOUT && request.result == ERROR_CANCELLED :
                        observed == WAIT_OBJECT_0 && request.result == ERROR_SUCCESS;
    CloseHandle(thread);
    CloseHandle(request.cancel);
    return success;
}

static BOOL check_priority(IO_DEVICE disk, BOOL aged)
{
    IO_PERMIT held;
    volatile LONG order = 0;
    REQUEST background = { .source = disk, .destination = disk, .background = TRUE, .order = &order };
    REQUEST interactive = { .source = disk, .destination = disk, .order = &order };
    HANDLE low, high;
    BOOL success = FALSE;
    if (io_budget_acquire(&held, disk, disk, NULL, FALSE))
        return FALSE;
    low = CreateThread(NULL, 0, acquire, &background, 0, NULL);
    if (low)
        WaitForSingleObject(low, aged ? 650 : 50);
    high = CreateThread(NULL, 0, acquire, &interactive, 0, NULL);
    if (high)
        WaitForSingleObject(high, 50);
    io_budget_release(&held);
    if (high) {
        WaitForSingleObject(high, INFINITE);
        CloseHandle(high);
    }
    if (low) {
        WaitForSingleObject(low, INFINITE);
        CloseHandle(low);
    }
    success = background.result == ERROR_SUCCESS && interactive.result == ERROR_SUCCESS &&
              background.position == (aged ? 1 : 2) && interactive.position == (aged ? 2 : 1);
    return success;
}

int main(void)
{
    IO_DEVICE a = {1, 1, TRUE}, b = {2, 2, TRUE}, c = {3, 1, TRUE};
    IO_DEVICE unknown = {0, 1, FALSE};
    IO_PERMIT held, native;
    BOOL success;
    if (io_budget_acquire(&held, a, b, NULL, FALSE))
        return 1;
    success = check_request(a, a, TRUE) && check_request(b, a, TRUE) &&
              check_request(b, b, FALSE) && check_request(c, c, FALSE) &&
              check_request(unknown, c, TRUE);
    io_budget_release(&held);
    if (success) {
        io_budget_account_native(&native, a, a);
        io_budget_account_native(&held, a, a);
        success = check_request(a, a, TRUE) && check_request(c, c, FALSE);
        io_budget_release(&native);
        success = success && check_request(a, a, TRUE);
        io_budget_release(&held);
    }
    if (success)
        success = check_request(a, a, FALSE) && check_priority(a, FALSE) && check_priority(a, TRUE) &&
                  check_native_priority(b, FALSE) && check_native_priority(b, TRUE);
    if (!success)
        fputs("Disk limit, cross-device permit or cancellation failed.\n", stderr);
    return success ? 0 : 1;
}
