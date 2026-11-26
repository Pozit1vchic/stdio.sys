#include "folder_streams.h"
#include "folder_walk_lab.h"
#include "exact_name_query.h"
#include "engine.h"
#include "io_budget.h"
#include <bcrypt.h>
#include <stdint.h>
#include <string.h>
#include <wchar.h>

/* shell32.dll 10.0.28000.1643, x64. Private ABI verified for this image only. */
enum { CALL_RVA = 0x35BF7, STREAM_RVA = 0x1E0BCC, WALK_CALL_RVA = 0x34D816, WALK_RVA = 0x44AE38 };
typedef struct {
    ULONGLONG *total;
    DWORD *minimum;
    HRESULT *status;
} SHELL_STREAM_VISITOR;
typedef HRESULT (*SHELL_STREAMS)(PCWSTR, SHELL_STREAM_VISITOR *);

static SHELL_STREAMS original;
static HRESULT (*original_walk)(void *, PCWSTR);
static volatile LONG hits;
static volatile LONG walk_fallbacks;
static volatile LONG walk_cancelled;
enum { WALK_BUDGET_OFF, WALK_BUDGET_STRICT, WALK_BUDGET_ADAPTIVE };
static volatile LONG walk_yields, budgeted_walk, walk_device_limit;
static DWORD budget_control_thread;
static volatile LONG64 walk_started;
static volatile LONG64 walk_finished;
static volatile LONG requests;
static volatile LONG pause_claimed;
static volatile LONG passthrough;
static HANDLE resume_first;
static HANDLE size_worker;
static BOOL explorer_host;
static BOOL pause_after_progress;

static void pause_walk_entry(void)
{
    if (InterlockedExchange(&pause_claimed, 1) == 0) {
        InterlockedIncrement(&requests);
        WaitForSingleObject(resume_first, 10000);
    }
}

typedef struct {
    BYTE *state;
    FOLDER_WALK_TOTALS baseline;
    ULONGLONG next_update;
} WALK_PROGRESS;

static DWORD publish_walk(WALK_PROGRESS *progress, const FOLDER_WALK_TOTALS *totals)
{
    BYTE *state = progress->state;
    const FOLDER_WALK_TOTALS *base = &progress->baseline;
    if (!ReadAcquire((volatile LONG *)(state + 0x08)))
        return ERROR_CANCELLED;
    if (totals->files > MAXDWORD - base->files || totals->folders > MAXDWORD - base->folders ||
        totals->bytes > MAXULONGLONG - base->bytes || totals->allocated > MAXULONGLONG - base->allocated)
        return ERROR_ARITHMETIC_OVERFLOW;
    *(ULONGLONG *)(state + 0x10) = base->bytes + totals->bytes;
    *(ULONGLONG *)(state + 0x18) = base->allocated + totals->allocated;
    *(DWORD *)(state + 0x20) = (DWORD)(base->files + totals->files);
    *(DWORD *)(state + 0x24) = (DWORD)(base->folders + totals->folders);
    return ERROR_SUCCESS;
}

static DWORD report_walk(void *context, const FOLDER_WALK_TOTALS *totals)
{
    WALK_PROGRESS *progress = context;
    ULONGLONG now = GetTickCount64();
    DWORD error;
    if (now < progress->next_update &&
        !(pause_after_progress && totals->files && !ReadAcquire(&pause_claimed)))
        return ERROR_SUCCESS;
    progress->next_update = now + 100;
    error = publish_walk(progress, totals);
    if (!error && pause_after_progress && totals->files)
        pause_walk_entry();
    return error;
}

static HRESULT finish_walk(HRESULT result)
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    InterlockedExchange64(&walk_finished, now.QuadPart);
    return result;
}

static HRESULT walk_folder(void *shell_context, PCWSTR path)
{
    LARGE_INTEGER now;
    BYTE *state = shell_context;
    volatile LONG *running = (volatile LONG *)(state + 0x08);
    FOLDER_WALK_TOTALS totals, zero = {0};
    FOLDER_WALK_BUDGET budget = {0};
    WALK_PROGRESS progress = {
        .state = state,
        .baseline = { *(DWORD *)(state + 0x20), *(DWORD *)(state + 0x24),
                      *(ULONGLONG *)(state + 0x10), *(ULONGLONG *)(state + 0x18) }
    };
    DWORD error;
    LONG budget_mode;
    if (!wcsstr(path, L"\\stdio-properties-"))
        return original_walk(shell_context, path);
    QueryPerformanceCounter(&now);
    InterlockedExchange64(&walk_finished, 0);
    InterlockedExchange64(&walk_started, now.QuadPart);
    if (ReadAcquire(&passthrough))
        return finish_walk(original_walk(shell_context, path));
    budget_mode = ReadAcquire(&budgeted_walk);
    if (budget_mode != WALK_BUDGET_OFF && GetCurrentThreadId() != budget_control_thread) {
        budget.device = io_device_for_path(path, storage_workers(path));
        budget.prefer_native = budget_mode == WALK_BUDGET_STRICT || budget.device.limit == 1;
    }
    InterlockedExchange(&walk_device_limit, budget.device.known ? (LONG)budget.device.limit : 0);
    error = folder_walk_properties(path, running, resume_first && !pause_after_progress ? pause_walk_entry : NULL,
                                   report_walk, &progress, budget.device.known ? &budget : NULL, &totals);
    InterlockedExchange(&walk_yields, (LONG)budget.yielded);
    if (!error)
        error = publish_walk(&progress, &totals);
    if (error == ERROR_CANCELLED || !ReadAcquire(running)) {
        InterlockedIncrement(&walk_cancelled);
        return finish_walk(E_ABORT);
    }
    if (error) {
        if (publish_walk(&progress, &zero) == ERROR_CANCELLED) {
            InterlockedIncrement(&walk_cancelled);
            return finish_walk(E_ABORT);
        }
        InterlockedIncrement(&walk_fallbacks);
        return finish_walk(original_walk(shell_context, path));
    }
    InterlockedIncrement(&hits);
    return finish_walk(S_OK);
}

static HRESULT read_streams(PCWSTR path, SHELL_STREAM_VISITOR *visitor)
{
    ULONGLONG bytes;
    DWORD error;
    HANDLE file;
    if (resume_first && InterlockedExchange(&pause_claimed, 1) == 0) {
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
                        &size_worker, SYNCHRONIZE, FALSE, 0);
        InterlockedIncrement(&requests);
        WaitForSingleObject(resume_first, 10000);
    } else {
        InterlockedIncrement(&requests);
    }
    if (InterlockedCompareExchange(&passthrough, 0, 0) ||
        (explorer_host && !wcsstr(path, L"\\stdio-properties-")))
        return original(path, visitor);
    file = CreateFileW(path, FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS | FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return original(path, visitor);
    error = folder_stream_allocation(file, *visitor->minimum, &bytes);
    CloseHandle(file);
    if (error)
        return original(path, visitor);
    if (SUCCEEDED(*visitor->status)) {
        if (bytes > MAXULONGLONG - *visitor->total) {
            *visitor->total = MAXULONGLONG;
            *visitor->status = HRESULT_FROM_WIN32(ERROR_ARITHMETIC_OVERFLOW);
        } else {
            *visitor->total += bytes;
        }
    }
    InterlockedIncrement(&hits);
    return S_OK;
}

static BOOL image_matches(HMODULE shell, const BYTE digest[32])
{
    wchar_t path[MAX_PATH];
    BYTE sum[32], buffer[8192];
    BCRYPT_ALG_HANDLE algorithm = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    HANDLE file;
    DWORD count, length = GetModuleFileNameW(shell, path, ARRAYSIZE(path));
    BOOL valid = FALSE;
    if (!length || length >= ARRAYSIZE(path))
        return FALSE;
    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL,
                       OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return FALSE;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, NULL, 0) >= 0 &&
        BCryptCreateHash(algorithm, &hash, NULL, 0, NULL, 0, 0) >= 0) {
        while (ReadFile(file, buffer, sizeof(buffer), &count, NULL)) {
            if (!count) {
                valid = BCryptFinishHash(hash, sum, sizeof(sum), 0) >= 0 &&
                        !memcmp(sum, digest, sizeof(sum));
                break;
            }
            if (BCryptHashData(hash, buffer, count, 0) < 0)
                break;
        }
    }
    if (hash)
        BCryptDestroyHash(hash);
    if (algorithm)
        BCryptCloseAlgorithmProvider(algorithm, 0);
    CloseHandle(file);
    return valid;
}

static BOOL expected_image(HMODULE shell)
{
    static const BYTE digest[32] = {
        0xC8,0x3C,0xC7,0xEE,0x08,0xE1,0xB3,0x38,0x0E,0x53,0xF8,0xA7,0xB0,0x0A,0x0E,0xA8,
        0x4A,0xCD,0x2B,0xBD,0xDE,0x24,0x02,0x23,0xF8,0x4B,0xD3,0x2A,0xE8,0x68,0xA0,0x9F
    };
    return image_matches(shell, digest);
}

typedef HRESULT (*SEARCH_NOTIFY)(void *, PCWSTR, PCWSTR, DWORD);
typedef UINT_PTR (WINAPI *SEARCH_TIMER)(HWND, UINT_PTR, UINT, TIMERPROC);
enum { SEARCH_NOTIFY_CALL = 0x2ABA7C, SEARCH_NOTIFY_TARGET = 0x502C10,
       SEARCH_TIMER_CALL = 0x180EB4, SEARCH_TIMER_SLOT = 0x600A78,
       SEARCH_TIMER_POINTER = 32, SEARCH_STOCK_NOTIFY = 64 };
static const BYTE search_notify_code[] = {0xE8, 0x8F, 0x71, 0x25, 0x00};
static const BYTE search_timer_code[] = {0x48, 0xFF, 0x15, 0xBD, 0xFB, 0x47, 0x00};
static SEARCH_NOTIFY stock_search_notify;
static SEARCH_TIMER stock_search_timer;
static HWND search_window;
static BYTE *search_shell;
static int32_t search_notify_operand, search_timer_operand;
static DWORD search_thread;
static volatile LONG search_timer_delay, search_timer_hits;
static _Thread_local BOOL exact_search;

static HRESULT search_notify(void *view, PCWSTR text, PCWSTR stripped, DWORD flags)
{
    BOOL previous = exact_search;
    HRESULT result;
    exact_search = ReadAcquire(&search_timer_delay) && GetCurrentThreadId() == search_thread &&
                   flags == 1 && exact_name_query(text, NULL, NULL) && stripped && !wcscmp(text, stripped);
    result = stock_search_notify(view, text, stripped, flags);
    exact_search = previous;
    return result;
}

static UINT_PTR WINAPI search_timer(HWND window, UINT_PTR id, UINT delay, TIMERPROC callback)
{
    UINT accelerated = 0;
    if (exact_search && id == 5 && delay == 500 && !callback && GetAncestor(window, GA_ROOT) == search_window)
        accelerated = (UINT)ReadAcquire(&search_timer_delay);
    UINT_PTR timer = stock_search_timer(window, id, accelerated ? accelerated : delay, callback);
    if (accelerated && timer)
        InterlockedIncrement(&search_timer_hits);
    return timer;
}

static DWORD write_search_operands(BYTE *notify, BYTE *timer, int32_t notify_operand, int32_t timer_operand)
{
    DWORD notify_protection, timer_protection, ignore;
    BOOL notify_flushed, timer_flushed, notify_restored, timer_restored;
    if (!VirtualProtect(notify, sizeof(search_notify_code), PAGE_EXECUTE_READWRITE, &notify_protection))
        return GetLastError();
    if (!VirtualProtect(timer, sizeof(search_timer_code), PAGE_EXECUTE_READWRITE, &timer_protection)) {
        DWORD error = GetLastError();
        VirtualProtect(notify, sizeof(search_notify_code), notify_protection, &ignore);
        return error;
    }
    /* Both replaced x64 displacement operands are contained in one cache line. */
    _InterlockedExchange((volatile LONG *)(notify + 1), notify_operand);
    _InterlockedExchange((volatile LONG *)(timer + 3), timer_operand);
    notify_flushed = FlushInstructionCache(GetCurrentProcess(), notify, sizeof(search_notify_code));
    timer_flushed = FlushInstructionCache(GetCurrentProcess(), timer, sizeof(search_timer_code));
    notify_restored = VirtualProtect(notify, sizeof(search_notify_code), notify_protection, &ignore);
    timer_restored = VirtualProtect(timer, sizeof(search_timer_code), timer_protection, &ignore);
    return notify_flushed && timer_flushed && notify_restored && timer_restored ?
           ERROR_SUCCESS : ERROR_INVALID_STATE;
}

static DWORD remove_search_timer(void);

static DWORD install_search_timer(HWND window)
{
    static const BYTE indirect_jump[] = {0xFF, 0x25, 0, 0, 0, 0};
    wchar_t process[MAX_PATH];
    const wchar_t *name;
    DWORD length = GetModuleFileNameW(NULL, process, ARRAYSIZE(process));
    BYTE *shell = (BYTE *)GetModuleHandleW(L"shell32.dll"), *notify, *timer, *stub;
    DWORD ignore, error;
    HMODULE pinned;
    uintptr_t low, high, notify_target = (uintptr_t)search_notify, timer_target = (uintptr_t)search_timer;
    MEM_ADDRESS_REQUIREMENTS range = {0};
    MEM_EXTENDED_PARAMETER parameter = {0};
    int32_t relative;
    if (search_window)
        return ERROR_ALREADY_EXISTS;
    if (!length || length >= ARRAYSIZE(process) ||
        !(name = wcsrchr(process, L'\\')) || _wcsicmp(name + 1, L"explorer.exe"))
        return ERROR_NOT_SUPPORTED;
    if (!shell || !expected_image((HMODULE)shell))
        return ERROR_REVISION_MISMATCH;
    notify = shell + SEARCH_NOTIFY_CALL;
    timer = shell + SEARCH_TIMER_CALL;
    stock_search_timer = *(SEARCH_TIMER *)(shell + SEARCH_TIMER_SLOT);
    if (memcmp(notify, search_notify_code, sizeof(search_notify_code)) ||
        memcmp(timer, search_timer_code, sizeof(search_timer_code)) ||
        stock_search_timer != (SEARCH_TIMER)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetTimer"))
        return ERROR_REVISION_MISMATCH;

    low = (uintptr_t)(shell + SEARCH_NOTIFY_TARGET) - INT32_MAX + 0xFFFF;
    high = (uintptr_t)timer + INT32_MAX;
    range.LowestStartingAddress = (void *)(low & ~(uintptr_t)0xFFFF);
    range.HighestEndingAddress = (void *)((high & ~(uintptr_t)0xFFFF) - 1);
    parameter.Type = MemExtendedParameterAddressRequirements;
    parameter.Pointer = &range;
    stub = VirtualAlloc2(GetCurrentProcess(), NULL, 4096, MEM_COMMIT | MEM_RESERVE,
                         PAGE_READWRITE, &parameter, 1);
    if (!stub)
        return GetLastError();
    memcpy(stub, indirect_jump, sizeof(indirect_jump));
    memcpy(stub + sizeof(indirect_jump), &notify_target, sizeof(notify_target));
    memcpy(stub + SEARCH_TIMER_POINTER, &timer_target, sizeof(timer_target));
    stub[SEARCH_STOCK_NOTIFY] = 0xE9;
    relative = (int32_t)((intptr_t)(shell + SEARCH_NOTIFY_TARGET) - (intptr_t)(stub + SEARCH_STOCK_NOTIFY + 5));
    memcpy(stub + SEARCH_STOCK_NOTIFY + 1, &relative, sizeof(relative));
    if (!VirtualProtect(stub, 4096, PAGE_EXECUTE_READ, &ignore) ||
        !FlushInstructionCache(GetCurrentProcess(), stub, 4096) ||
        !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           (PCWSTR)search_notify, &pinned)) {
        error = GetLastError();
        VirtualFree(stub, 0, MEM_RELEASE);
        return error;
    }
    stock_search_notify = (SEARCH_NOTIFY)(stub + SEARCH_STOCK_NOTIFY);
    search_window = window;
    search_thread = GetCurrentThreadId();
    search_shell = shell;
    search_notify_operand = (int32_t)((intptr_t)stub - (intptr_t)(notify + sizeof(search_notify_code)));
    search_timer_operand = (int32_t)((intptr_t)(stub + SEARCH_TIMER_POINTER) -
                                     (intptr_t)(timer + sizeof(search_timer_code)));
    error = write_search_operands(notify, timer, search_notify_operand, search_timer_operand);
    if (error)
        remove_search_timer();
    return error;
}

static DWORD remove_search_timer(void)
{
    BYTE *notify = search_shell + SEARCH_NOTIFY_CALL, *timer = search_shell + SEARCH_TIMER_CALL;
    int32_t current_notify, current_timer, original_notify, original_timer;
    InterlockedExchange(&search_timer_delay, 0);
    if (!memcmp(notify, search_notify_code, sizeof(search_notify_code)) &&
        !memcmp(timer, search_timer_code, sizeof(search_timer_code)))
        return ERROR_SUCCESS;
    memcpy(&current_notify, notify + 1, sizeof(current_notify));
    memcpy(&current_timer, timer + 3, sizeof(current_timer));
    if (notify[0] != search_notify_code[0] || memcmp(timer, search_timer_code, 3) ||
        current_notify != search_notify_operand || current_timer != search_timer_operand)
        return ERROR_REVISION_MISMATCH;
    memcpy(&original_notify, search_notify_code + 1, sizeof(original_notify));
    memcpy(&original_timer, search_timer_code + 3, sizeof(original_timer));
    /* Keep the pinned stub valid for any call already in progress. */
    return write_search_operands(notify, timer, original_notify, original_timer);
}

typedef HRESULT (WINAPI *NATIVE_COPY)(PCWSTR, PCWSTR, COPYFILE2_EXTENDED_PARAMETERS *);
enum { COPY_CALL = 0x34AE60, COPY_SLOT = 0x6E50D8 };
static const BYTE copy_code[] = {0x48, 0xFF, 0x15, 0x71, 0xA2, 0x39, 0x00};
static NATIVE_COPY native_copy;
static BYTE *copy_call;
static int32_t copy_operand;
static HWND copy_window;
static DWORD copy_ui_thread;
static volatile LONG copy_enabled, copy_accounted, copy_active;
static volatile LONG64 copy_finished;
static _Thread_local struct {
    wchar_t root[MAX_PATH];
    IO_DEVICE device;
} copy_scope;

static BOOL copy_fixture_device(PCWSTR source, PCWSTR destination, IO_DEVICE *device)
{
    const wchar_t *marker = wcsstr(source, L"\\stdio-transfer-");
    const wchar_t *end = marker ? wcschr(marker + 1, L'\\') : NULL;
    size_t length;
    if (!end)
        return FALSE;
    length = (size_t)(end - source);
    if (length >= ARRAYSIZE(copy_scope.root) || source[1] != L':' || source[2] != L'\\' ||
        wcsncmp(source, destination, length) || destination[length] != L'\\')
        return FALSE;
    if (wcsncmp(copy_scope.root, source, length) || copy_scope.root[length]) {
        wmemcpy(copy_scope.root, source, length);
        copy_scope.root[length] = 0;
        copy_scope.device = io_device_for_path(copy_scope.root, storage_workers(copy_scope.root));
    }
    *device = copy_scope.device;
    return device->known;
}

static HRESULT WINAPI account_copy(PCWSTR source, PCWSTR destination, COPYFILE2_EXTENDED_PARAMETERS *parameters)
{
    IO_DEVICE device;
    IO_PERMIT permit;
    HRESULT result;
    DWORD last = GetLastError();
    if (!ReadAcquire(&copy_enabled) || GetCurrentThreadId() == copy_ui_thread || !source || !destination ||
        !copy_fixture_device(source, destination, &device)) {
        SetLastError(last);
        return native_copy(source, destination, parameters);
    }
    io_budget_account_native(&permit, device, device);
    InterlockedIncrement(&copy_active);
    SetLastError(last);
    result = native_copy(source, destination, parameters);
    last = GetLastError();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    InterlockedExchange64(&copy_finished, now.QuadPart);
    io_budget_release(&permit);
    InterlockedDecrement(&copy_active);
    InterlockedIncrement(&copy_accounted);
    SetLastError(last);
    return result;
}

static DWORD write_copy_operand(int32_t value)
{
    DWORD old, ignore;
    BOOL flushed, restored;
    if (!VirtualProtect(copy_call, sizeof(copy_code), PAGE_EXECUTE_READWRITE, &old))
        return GetLastError();
    _InterlockedExchange((volatile LONG *)(copy_call + 3), value);
    flushed = FlushInstructionCache(GetCurrentProcess(), copy_call, sizeof(copy_code));
    restored = VirtualProtect(copy_call, sizeof(copy_code), old, &ignore);
    return flushed && restored ? ERROR_SUCCESS : ERROR_INVALID_STATE;
}

static DWORD remove_copy_accounting(void)
{
    int32_t current, stock_operand;
    InterlockedExchange(&copy_enabled, FALSE);
    if (!memcmp(copy_call, copy_code, sizeof(copy_code)))
        return ERROR_SUCCESS;
    memcpy(&current, copy_call + 3, sizeof(current));
    if (memcmp(copy_call, copy_code, 3) || current != copy_operand)
        return ERROR_REVISION_MISMATCH;
    memcpy(&stock_operand, copy_code + 3, sizeof(stock_operand));
    return write_copy_operand(stock_operand);
}

static DWORD install_copy_accounting(HWND window)
{
    static const BYTE digest[32] = {
        0x81,0x39,0x1D,0x8A,0x95,0x41,0xDA,0x55,0x85,0x7F,0x81,0x1C,0x0C,0xAA,0xA4,0x8D,
        0x28,0x9F,0xC9,0x16,0x1A,0xED,0x8E,0x05,0x36,0xD4,0xAD,0x58,0x56,0x02,0xCF,0x27
    };
    wchar_t process[MAX_PATH];
    const wchar_t *name;
    BYTE *module = (BYTE *)GetModuleHandleW(L"windows.storage.dll"), *call;
    DWORD length = GetModuleFileNameW(NULL, process, ARRAYSIZE(process)), old, error;
    MEM_ADDRESS_REQUIREMENTS range = {0};
    MEM_EXTENDED_PARAMETER parameter = {0};
    HMODULE pinned;
    NATIVE_COPY *slot;
    uintptr_t next;
    if (copy_window)
        return ERROR_ALREADY_EXISTS;
    if (!length || length >= ARRAYSIZE(process) ||
        !(name = wcsrchr(process, L'\\')) || _wcsicmp(name + 1, L"explorer.exe"))
        return ERROR_NOT_SUPPORTED;
    if (!module || !image_matches((HMODULE)module, digest))
        return ERROR_REVISION_MISMATCH;
    call = module + COPY_CALL;
    native_copy = *(NATIVE_COPY *)(module + COPY_SLOT);
    if (memcmp(call, copy_code, sizeof(copy_code)) ||
        native_copy != (NATIVE_COPY)GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), "CopyFile2"))
        return ERROR_REVISION_MISMATCH;
    next = (uintptr_t)(call + sizeof(copy_code));
    range.LowestStartingAddress = (void *)((next - INT32_MAX + 0xFFFF) & ~(uintptr_t)0xFFFF);
    range.HighestEndingAddress = (void *)(((next + INT32_MAX) & ~(uintptr_t)0xFFFF) - 1);
    parameter.Type = MemExtendedParameterAddressRequirements;
    parameter.Pointer = &range;
    slot = VirtualAlloc2(GetCurrentProcess(), NULL, 4096, MEM_COMMIT | MEM_RESERVE,
                         PAGE_READWRITE, &parameter, 1);
    if (!slot)
        return GetLastError();
    *slot = account_copy;
    if (!VirtualProtect(slot, 4096, PAGE_READONLY, &old) ||
        !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           (PCWSTR)account_copy, &pinned)) {
        error = GetLastError();
        VirtualFree(slot, 0, MEM_RELEASE);
        return error;
    }
    copy_call = call;
    copy_operand = (int32_t)((intptr_t)slot - (intptr_t)next);
    copy_window = window;
    copy_ui_thread = GetCurrentThreadId();
    error = write_copy_operand(copy_operand);
    if (error)
        remove_copy_accounting();
    else
        InterlockedExchange(&copy_enabled, TRUE);
    return error;
}

__declspec(dllexport) LONG WINAPI ShellStreamLabHits(void)
{
    return InterlockedCompareExchange(&hits, 0, 0);
}

__declspec(dllexport) LONG WINAPI ShellStreamLabRequests(void)
{
    return InterlockedCompareExchange(&requests, 0, 0);
}

__declspec(dllexport) void WINAPI ShellStreamLabResume(void)
{
    if (resume_first)
        SetEvent(resume_first);
}

__declspec(dllexport) DWORD WINAPI ShellStreamLabWaitForWorker(void)
{
    DWORD result;
    /* Called after releasing the first request's event. */
    if (!size_worker)
        return ERROR_INVALID_HANDLE;
    result = WaitForSingleObject(size_worker, 5000);
    if (result == WAIT_OBJECT_0) {
        CloseHandle(size_worker);
        size_worker = NULL;
    }
    return result;
}

static DWORD install(BOOL stock_only, BOOL pause_first, BOOL for_explorer, BOOL full_walk)
{
    const BYTE stream_expected[] = {0xE8, 0xD0, 0xAF, 0x1A, 0x00};
    const BYTE walk_expected[] = {0xE8, 0x1D, 0xD6, 0x0F, 0x00};
    const BYTE *expected = full_walk ? walk_expected : stream_expected;
    const SIZE_T call_size = sizeof(stream_expected);
    const unsigned original_rva = full_walk ? WALK_RVA : STREAM_RVA;
    const BYTE jump[] = {0xFF, 0x25, 0, 0, 0, 0};
    const unsigned fallback_offset = 32;
    const uintptr_t target = full_walk ? (uintptr_t)walk_folder : (uintptr_t)read_streams;
    wchar_t process[MAX_PATH];
    HMODULE pinned;
    BYTE *shell, *call, *stub;
    uintptr_t from;
    intptr_t delta;
    int32_t relative;
    DWORD old, ignore;
    MEM_ADDRESS_REQUIREMENTS range = {0};
    MEM_EXTENDED_PARAMETER parameter = {0};
    DWORD length = GetModuleFileNameW(NULL, process, ARRAYSIZE(process));
    const wchar_t *name;
    if (!length || length >= ARRAYSIZE(process))
        return ERROR_NOT_SUPPORTED;
    name = wcsrchr(process, L'\\');
    if (!name || _wcsicmp(name + 1, for_explorer ? L"explorer.exe" : L"powershell.exe"))
        return ERROR_NOT_SUPPORTED;
    shell = (BYTE *)GetModuleHandleW(L"shell32.dll");
    if (!shell || !expected_image((HMODULE)shell))
        return ERROR_REVISION_MISMATCH;
    call = shell + (full_walk ? WALK_CALL_RVA : CALL_RVA);
    if (memcmp(call, expected, call_size))
        return ERROR_REVISION_MISMATCH;
    from = (uintptr_t)(call + call_size);
    range.LowestStartingAddress = (void *)(((uintptr_t)(shell + original_rva) -
                                             INT32_MAX + 0xFFFF) & ~(uintptr_t)0xFFFF);
    range.HighestEndingAddress = (void *)(((from + INT32_MAX) & ~(uintptr_t)0xFFFF) - 1);
    parameter.Type = MemExtendedParameterAddressRequirements;
    parameter.Pointer = &range;
    stub = VirtualAlloc2(GetCurrentProcess(), NULL, 4096, MEM_RESERVE | MEM_COMMIT,
                         PAGE_READWRITE, &parameter, 1);
    if (!stub)
        return GetLastError();
    delta = (intptr_t)((uintptr_t)stub - from);
    if (delta < INT32_MIN || delta > INT32_MAX) {
        VirtualFree(stub, 0, MEM_RELEASE);
        return ERROR_ARITHMETIC_OVERFLOW;
    }
    memcpy(stub, jump, sizeof(jump));
    memcpy(stub + sizeof(jump), &target, sizeof(target));
    /* The original helper is a direct-call target in Shell, so use a direct
       branch to it from the aligned fallback entry. */
    delta = (intptr_t)((uintptr_t)(shell + original_rva) -
                      (uintptr_t)(stub + fallback_offset + 5));
    if (delta < INT32_MIN || delta > INT32_MAX) {
        VirtualFree(stub, 0, MEM_RELEASE);
        return ERROR_ARITHMETIC_OVERFLOW;
    }
    stub[fallback_offset] = 0xE9;
    relative = (int32_t)delta;
    memcpy(stub + fallback_offset + 1, &relative, sizeof(relative));
    if (pause_first) {
        resume_first = CreateEventW(NULL, TRUE, FALSE, NULL);
        if (!resume_first) {
            DWORD error = GetLastError();
            VirtualFree(stub, 0, MEM_RELEASE);
            return error;
        }
    }
    if (!VirtualProtect(stub, 4096, PAGE_EXECUTE_READ, &old) ||
        !FlushInstructionCache(GetCurrentProcess(), stub, fallback_offset + 5) ||
        !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           (PCWSTR)read_streams, &pinned) ||
        !VirtualProtect(call, call_size, PAGE_EXECUTE_READWRITE, &old)) {
        DWORD error = GetLastError();
        if (resume_first) {
            CloseHandle(resume_first);
            resume_first = NULL;
        }
        VirtualFree(stub, 0, MEM_RELEASE);
        return error;
    }
    if (full_walk)
        original_walk = (HRESULT (*)(void *, PCWSTR))(stub + fallback_offset);
    else
        original = (SHELL_STREAMS)(stub + fallback_offset);
    passthrough = stock_only;
    explorer_host = for_explorer;
    relative = (int32_t)((uintptr_t)stub - from);
    /* This x64-only test changes an operand contained in one cache line. */
    _InterlockedExchange((volatile LONG *)(call + 1), relative);
    {
        BOOL flushed = FlushInstructionCache(GetCurrentProcess(), call, call_size);
        BOOL restored = VirtualProtect(call, call_size, old, &ignore);
        if (!flushed || !restored)
            return ERROR_INVALID_STATE;
    }
    return ERROR_SUCCESS;
}

/* Call before starting any property worker in the disposable test process. */
__declspec(dllexport) DWORD WINAPI ShellStreamLabInstall(BOOL stock_only, BOOL pause_first)
{
    return install(stock_only, pause_first, FALSE, FALSE);
}

__declspec(dllexport) LRESULT CALLBACK ShellStreamLabWindowHook(int code, WPARAM wparam, LPARAM lparam)
{
    if (code == HC_ACTION) {
        const CWPSTRUCT *message = (const CWPSTRUCT *)lparam;
        if (message->message == RegisterWindowMessageW(L"stdio.shell.stream.lab.2026") &&
            message->wParam == GetCurrentProcessId() && message->hwnd != GetShellWindow()) {
            if (message->lParam == 1 || message->lParam == 2 ||
                message->lParam == 5 || message->lParam == 6 ||
                message->lParam == 8 || message->lParam == 12) {
                pause_after_progress = message->lParam == 12;
                DWORD result = install(message->lParam == 2 || message->lParam == 6,
                                       message->lParam == 8 || message->lParam == 12, TRUE,
                                       message->lParam >= 5);
                SetPropW(message->hwnd, L"stdio.lab.install", (HANDLE)((UINT_PTR)result + 1));
            } else if (message->lParam == 3) {
                SetPropW(message->hwnd, L"stdio.lab.hits",
                         (HANDLE)((UINT_PTR)ShellStreamLabHits() + 1));
            } else if (message->lParam == 4) {
                InterlockedExchange(&passthrough, TRUE);
                SetPropW(message->hwnd, L"stdio.lab.disabled", (HANDLE)(UINT_PTR)1);
            } else if (message->lParam == 7) {
                SetPropW(message->hwnd, L"stdio.lab.walk-fallbacks",
                         (HANDLE)((UINT_PTR)ReadAcquire(&walk_fallbacks) + 1));
            } else if (message->lParam == 9) {
                SetPropW(message->hwnd, L"stdio.lab.paused", (HANDLE)((UINT_PTR)ReadAcquire(&requests) + 1));
            } else if (message->lParam == 10) {
                ShellStreamLabResume();
                SetPropW(message->hwnd, L"stdio.lab.resumed", (HANDLE)(UINT_PTR)1);
            } else if (message->lParam == 11) {
                SetPropW(message->hwnd, L"stdio.lab.cancelled", (HANDLE)((UINT_PTR)ReadAcquire(&walk_cancelled) + 1));
            } else if (message->lParam == 13) {
                SetPropW(message->hwnd, L"stdio.lab.walk-started", (HANDLE)((UINT_PTR)ReadAcquire64(&walk_started) + 1));
            } else if (message->lParam == 14) {
                SetPropW(message->hwnd, L"stdio.lab.walk-finished", (HANDLE)((UINT_PTR)ReadAcquire64(&walk_finished) + 1));
            } else if (message->lParam == 15) {
                DWORD result = install_search_timer(message->hwnd);
                SetPropW(message->hwnd, L"stdio.lab.search-install", (HANDLE)((UINT_PTR)result + 1));
            } else if (message->hwnd == search_window &&
                       (message->lParam == 16 || message->lParam == 17 || message->lParam == 29)) {
                InterlockedExchange(&search_timer_delay, message->lParam == 16 ? 100 :
                                                         message->lParam == 29 ? 10 : 0);
                SetPropW(message->hwnd, L"stdio.lab.search-mode", (HANDLE)(UINT_PTR)1);
            } else if (message->hwnd == search_window && message->lParam == 18) {
                SetPropW(message->hwnd, L"stdio.lab.search-hits",
                         (HANDLE)((UINT_PTR)ReadAcquire(&search_timer_hits) + 1));
            } else if (message->hwnd == search_window && message->lParam == 19) {
                DWORD result = remove_search_timer();
                SetPropW(message->hwnd, L"stdio.lab.search-remove", (HANDLE)((UINT_PTR)result + 1));
            } else if (message->lParam == 20) {
                DWORD result = install_copy_accounting(message->hwnd);
                SetPropW(message->hwnd, L"stdio.lab.copy-install", (HANDLE)((UINT_PTR)result + 1));
            } else if (message->hwnd == copy_window && message->lParam == 21) {
                SetPropW(message->hwnd, L"stdio.lab.copy-accounted",
                         (HANDLE)((UINT_PTR)ReadAcquire(&copy_accounted) + 1));
            } else if (message->hwnd == copy_window && message->lParam == 22) {
                DWORD result = remove_copy_accounting();
                SetPropW(message->hwnd, L"stdio.lab.copy-remove", (HANDLE)((UINT_PTR)result + 1));
            } else if (message->hwnd == copy_window && (message->lParam == 23 || message->lParam == 27)) {
                budget_control_thread = GetCurrentThreadId();
                InterlockedExchange(&budgeted_walk, message->lParam == 23 ? WALK_BUDGET_STRICT : WALK_BUDGET_ADAPTIVE);
                SetPropW(message->hwnd, L"stdio.lab.walk-budget", (HANDLE)(UINT_PTR)1);
            } else if (message->hwnd == copy_window && message->lParam == 24) {
                SetPropW(message->hwnd, L"stdio.lab.walk-yields",
                         (HANDLE)((UINT_PTR)ReadAcquire(&walk_yields) + 1));
            } else if (message->hwnd == copy_window && message->lParam == 25) {
                SetPropW(message->hwnd, L"stdio.lab.copy-active",
                         (HANDLE)((UINT_PTR)ReadAcquire(&copy_active) + 1));
            } else if (message->hwnd == copy_window && message->lParam == 26) {
                SetPropW(message->hwnd, L"stdio.lab.copy-finished",
                         (HANDLE)((UINT_PTR)ReadAcquire64(&copy_finished) + 1));
            } else if (message->hwnd == copy_window && message->lParam == 28) {
                SetPropW(message->hwnd, L"stdio.lab.walk-device-limit",
                         (HANDLE)((UINT_PTR)ReadAcquire(&walk_device_limit) + 1));
            }
        }
    }
    return CallNextHookEx(NULL, code, wparam, lparam);
}
