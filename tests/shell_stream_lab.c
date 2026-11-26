#include "folder_streams.h"
#include "folder_walk_lab.h"
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

static HRESULT walk_folder(void *shell_context, PCWSTR path)
{
    BYTE *state = shell_context;
    volatile LONG *running = (volatile LONG *)(state + 0x08);
    FOLDER_WALK_TOTALS totals, zero = {0};
    WALK_PROGRESS progress = {
        .state = state,
        .baseline = { *(DWORD *)(state + 0x20), *(DWORD *)(state + 0x24),
                      *(ULONGLONG *)(state + 0x10), *(ULONGLONG *)(state + 0x18) }
    };
    DWORD error;
    if (ReadAcquire(&passthrough) || !wcsstr(path, L"\\stdio-properties-"))
        return original_walk(shell_context, path);
    error = folder_walk_properties(path, running, resume_first && !pause_after_progress ? pause_walk_entry : NULL,
                                   report_walk, &progress, &totals);
    if (!error)
        error = publish_walk(&progress, &totals);
    if (error == ERROR_CANCELLED || !ReadAcquire(running)) {
        InterlockedIncrement(&walk_cancelled);
        return E_ABORT;
    }
    if (error) {
        if (publish_walk(&progress, &zero) == ERROR_CANCELLED) {
            InterlockedIncrement(&walk_cancelled);
            return E_ABORT;
        }
        InterlockedIncrement(&walk_fallbacks);
        return original_walk(shell_context, path);
    }
    InterlockedIncrement(&hits);
    return S_OK;
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

static BOOL expected_image(HMODULE shell)
{
    static const BYTE digest[32] = {
        0xC8,0x3C,0xC7,0xEE,0x08,0xE1,0xB3,0x38,0x0E,0x53,0xF8,0xA7,0xB0,0x0A,0x0E,0xA8,
        0x4A,0xCD,0x2B,0xBD,0xDE,0x24,0x02,0x23,0xF8,0x4B,0xD3,0x2A,0xE8,0x68,0xA0,0x9F
    };
    wchar_t path[MAX_PATH];
    BYTE sum[sizeof(digest)], buffer[8192];
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
            }
        }
    }
    return CallNextHookEx(NULL, code, wparam, lparam);
}
