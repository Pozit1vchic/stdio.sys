#include "copy_budget.h"

static volatile LONG active_copies;

BOOL copy_budget_try_acquire(void)
{
    MEMORYSTATUSEX memory = { .dwLength = sizeof(memory) };
    LONG observed, limit = 1;

    if (GlobalMemoryStatusEx(&memory) && memory.ullAvailPhys >= 256ULL * 1024 * 1024)
        limit = memory.ullTotalPhys <= 8ULL * 1024 * 1024 * 1024 ? 2 : 4;
    observed = InterlockedCompareExchange(&active_copies, 0, 0);
    while (observed < limit) {
        LONG previous = InterlockedCompareExchange(&active_copies, observed + 1, observed);
        if (previous == observed)
            return TRUE;
        observed = previous;
    }
    return FALSE;
}

void copy_budget_release(void)
{
    InterlockedDecrement(&active_copies);
}
