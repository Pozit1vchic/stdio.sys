#pragma once
#include <windows.h>

/* Uses the caller's existing metadata handle. A failure leaves the output untouched
   so the caller can continue with its normal allocation-size calculation. */
DWORD folder_stream_allocation(HANDLE file, DWORD minimum_allocation, ULONGLONG *bytes);
