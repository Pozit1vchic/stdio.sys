#pragma once
#include <windows.h>

/* Reserve capacity before allocating traversal state or creating the destination.
   The caller releases exactly once after closing the operation's handles. */
BOOL copy_budget_try_acquire(void);
void copy_budget_release(void);
