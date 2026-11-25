#include "io_budget.h"
#include <stdio.h>

typedef struct {
    IO_DEVICE source, destination;
    HANDLE cancel;
    DWORD result;
} REQUEST;

static DWORD WINAPI acquire(void *context)
{
    REQUEST *request = context;
    IO_PERMIT permit;
    request->result = io_budget_acquire(&permit, request->source, request->destination, request->cancel);
    if (!request->result)
        io_budget_release(&permit);
    return 0;
}

static BOOL check_request(IO_DEVICE a, IO_DEVICE b, BOOL blocked)
{
    REQUEST request = {a, b, CreateEventW(NULL, TRUE, FALSE, NULL), ERROR_INVALID_STATE};
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

int main(void)
{
    IO_DEVICE a = {1, 1, TRUE}, b = {2, 2, TRUE}, c = {3, 1, TRUE};
    IO_DEVICE unknown = {0, 1, FALSE};
    IO_PERMIT held;
    BOOL success;
    if (io_budget_acquire(&held, a, b, NULL))
        return 1;
    success = check_request(a, a, TRUE) && check_request(b, a, TRUE) &&
              check_request(b, b, FALSE) && check_request(c, c, FALSE) &&
              check_request(unknown, c, TRUE);
    io_budget_release(&held);
    if (success)
        success = check_request(a, a, FALSE);
    if (!success)
        fputs("Disk limit, cross-device permit or cancellation failed.\n", stderr);
    return success ? 0 : 1;
}
