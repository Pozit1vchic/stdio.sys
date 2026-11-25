#define COBJMACROS
#include <shlobj.h>
#include <winioctl.h>
#include <wchar.h>
#include "../src/engine.h"
#include "../src/copy_budget.h"

static BOOL make_file(const wchar_t *path, const char *data, DWORD length)
{
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL);
    DWORD written;
    BOOL ok;
    if (file == INVALID_HANDLE_VALUE)
        return FALSE;
    ok = WriteFile(file, data, length, &written, NULL) && written == length;
    return CloseHandle(file) && ok;
}

typedef struct {
    unsigned complete_files;
    unsigned failed_files;
    unsigned skipped_files;
    unsigned cancelled_files;
    unsigned incomplete_directories;
    unsigned other;
    HANDLE cancel_after_copy;
} COPY_TEST_REPORT;

static void count_copy_items(void *context, const TREE_COPY_ITEM *item)
{
    COPY_TEST_REPORT *report = context;
    if (!item->source || !item->destination || !item->source_id) {
        ++report->other;
    } else if (!item->directory && item->state == TREE_COPY_COMPLETE &&
               item->effect == TREE_COPY_FILE_COPIED && !item->error) {
        ++report->complete_files;
        if (report->cancel_after_copy)
            SetEvent(report->cancel_after_copy);
    } else if (!item->directory && item->state == TREE_COPY_FAILED &&
               item->effect == TREE_COPY_UNCHANGED && item->error == ERROR_SHARING_VIOLATION) {
        ++report->failed_files;
    } else if (!item->directory && item->state == TREE_COPY_SKIPPED &&
               item->effect == TREE_COPY_UNCHANGED && item->error == ERROR_SHARING_VIOLATION) {
        ++report->skipped_files;
    } else if (!item->directory && item->state == TREE_COPY_CANCELLED &&
               item->effect == TREE_COPY_UNCHANGED && item->error == ERROR_CANCELLED) {
        ++report->cancelled_files;
    } else if (item->directory && item->state == TREE_COPY_INCOMPLETE &&
               item->effect == TREE_COPY_DIRECTORY_CREATED &&
               (item->error == ERROR_SHARING_VIOLATION || item->error == ERROR_CANCELLED)) {
        ++report->incomplete_directories;
    } else {
        ++report->other;
    }
}

static BOOL partial_copy_test(const wchar_t *fixture)
{
    wchar_t source[MAX_PATH], target[MAX_PATH], cancelled[MAX_PATH], path[MAX_PATH];
    TREE_COPY_RESULT summary;
    TREE_RESULT cleanup;
    COPY_TEST_REPORT report = {0};
    TREE_COPY_OPTIONS options = { .report = count_copy_items, .context = &report };
    HANDLE lock = INVALID_HANDLE_VALUE;
    BOOL ok = FALSE;
    swprintf_s(source, ARRAYSIZE(source), L"%ls\\partial", fixture);
    swprintf_s(target, ARRAYSIZE(target), L"%ls\\result", fixture);
    swprintf_s(cancelled, ARRAYSIZE(cancelled), L"%ls\\cancelled", fixture);
    if (!CreateDirectoryW(source, NULL))
        goto done;
    swprintf_s(path, ARRAYSIZE(path), L"%ls\\01.bin", source);
    if (!make_file(path, "completed", 9))
        goto done;
    swprintf_s(path, ARRAYSIZE(path), L"%ls\\02.bin", source);
    if (!make_file(path, "locked", 6))
        goto done;
    lock = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (lock == INVALID_HANDLE_VALUE)
        goto done;
    swprintf_s(path, ARRAYSIZE(path), L"%ls\\03.bin", source);
    if (!make_file(path, "untouched", 9))
        goto done;
    if (copy_new_tree_report(source, target, 1, FALSE, &options, &summary) != ERROR_SHARING_VIOLATION ||
        summary.totals.files != 1 || summary.totals.bytes != 9 || summary.totals.errors != 1 ||
        summary.totals.directories != 0 || summary.directories_created != 1 ||
        summary.files_outcome_unknown || summary.skipped != 1 || summary.cancelled ||
        report.complete_files != 1 || report.failed_files != 1 || report.skipped_files != 1 ||
        report.incomplete_directories != 1 || report.other)
        goto done;
    if (!tree_run(target, FALSE, 1, &cleanup) && cleanup.files == 1 && cleanup.bytes == 9) {
        CloseHandle(lock);
        lock = INVALID_HANDLE_VALUE;
    } else {
        goto done;
    }
    if (copy_new_tree_report(source, target, 1, FALSE, NULL, &summary) != ERROR_ALREADY_EXISTS ||
        summary.directories_created || summary.totals.files)
        goto done;
    ZeroMemory(&report, sizeof(report));
    options.cancel_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!options.cancel_event)
        goto done;
    report.cancel_after_copy = options.cancel_event;
    if (copy_new_tree_report(source, cancelled, 1, FALSE, &options, &summary) != ERROR_CANCELLED ||
        summary.totals.files != 1 || summary.totals.errors || summary.directories_created != 1 ||
        summary.cancelled != 2 || report.complete_files != 1 || report.cancelled_files != 2 ||
        report.incomplete_directories != 1 || report.other)
        goto done;
    if (!tree_run(cancelled, TRUE, 1, &cleanup) &&
        copy_new_tree_report(source, cancelled, 1, FALSE, &options, &summary) == ERROR_CANCELLED &&
        summary.cancelled == 1 && !summary.directories_created &&
        GetFileAttributesW(cancelled) == INVALID_FILE_ATTRIBUTES &&
        copy_new_tree_report(source, cancelled, 1, FALSE, NULL, &summary) == ERROR_SUCCESS &&
        summary.totals.files == 3 && summary.totals.directories == 1 &&
        summary.directories_created == 1)
        ok = TRUE;
done:
    if (options.cancel_event)
        CloseHandle(options.cancel_event);
    if (lock != INVALID_HANDLE_VALUE)
        CloseHandle(lock);
    tree_run(source, TRUE, 1, &cleanup);
    tree_run(target, TRUE, 1, &cleanup);
    tree_run(cancelled, TRUE, 1, &cleanup);
    return ok;
}

static BOOL copy_admission_test(const wchar_t *source, const wchar_t *target)
{
    TREE_COPY_RESULT summary;
    TREE_RESULT cleanup;
    TREE_COPY_OPTIONS options = {0};
    unsigned reservations = 0;
    BOOL ok;
    while (reservations < 4 && copy_budget_try_acquire())
        ++reservations;
    options.cancel_event = CreateEventW(NULL, TRUE, TRUE, NULL);
    ok = reservations && options.cancel_event &&
         copy_new_tree_report(source, target, 1, FALSE, &options, &summary) == ERROR_CANCELLED &&
         !summary.directories_created &&
         copy_new_tree_report(source, target, 1, FALSE, NULL, &summary) == ERROR_BUSY &&
         !summary.directories_created &&
         GetFileAttributesW(target) == INVALID_FILE_ATTRIBUTES;
    if (options.cancel_event)
        CloseHandle(options.cancel_event);
    while (reservations) {
        copy_budget_release();
        --reservations;
    }
    if (ok) {
        TREE_RESULT expected;
        if (tree_run(source, FALSE, 1, &expected))
            return FALSE;
        ok = !copy_new_tree_report(source, target, 1, FALSE, NULL, &summary) &&
             summary.totals.files == expected.files && summary.totals.bytes == expected.bytes;
        if (summary.directories_created && tree_run(target, TRUE, 1, &cleanup))
            ok = FALSE;
    }
    return ok;
}

static BOOL directory_stream_test(const wchar_t *fixture)
{
    wchar_t source[MAX_PATH], child[MAX_PATH], target[MAX_PATH], stream[MAX_PATH];
    TREE_COPY_RESULT summary;
    TREE_RESULT cleanup;
    BOOL ok = FALSE;
    swprintf_s(source, ARRAYSIZE(source), L"%ls\\stream-source", fixture);
    swprintf_s(child, ARRAYSIZE(child), L"%ls\\child", source);
    swprintf_s(target, ARRAYSIZE(target), L"%ls\\stream-target", fixture);
    swprintf_s(stream, ARRAYSIZE(stream), L"%ls:directory-data", source);
    if (!CreateDirectoryW(source, NULL) || !CreateDirectoryW(child, NULL) ||
        !make_file(stream, "dir", 3))
        goto done;
    if (copy_new_tree_report(source, target, 1, FALSE, NULL, &summary) != ERROR_NOT_SUPPORTED ||
        summary.directories_created || GetFileAttributesW(target) != INVALID_FILE_ATTRIBUTES ||
        !DeleteFileW(stream))
        goto done;
    swprintf_s(stream, ARRAYSIZE(stream), L"%ls:directory-data", child);
    if (make_file(stream, "child", 5) &&
        copy_new_tree_report(source, target, 4, FALSE, NULL, &summary) == ERROR_NOT_SUPPORTED &&
        summary.directories_created == 1 && !summary.totals.directories && summary.totals.errors == 1 &&
        !tree_run(target, FALSE, 1, &cleanup) && cleanup.directories == 1)
        ok = TRUE;
done:
    tree_run(source, TRUE, 1, &cleanup);
    tree_run(target, TRUE, 1, &cleanup);
    return ok;
}

static BOOL sparse_copy_test(const wchar_t *root, const wchar_t *tree_copy)
{
    wchar_t source[MAX_PATH], direct[MAX_PATH], nested[MAX_PATH];
    LARGE_INTEGER tail = { .QuadPart = 4 * 1024 * 1024 };
    WIN32_FILE_ATTRIBUTE_DATA info;
    HANDLE file;
    DWORD bytes;
    TREE_RESULT result;
    BOOL prepared;
    swprintf_s(source, ARRAYSIZE(source), L"%ls\\sparse.bin", root);
    swprintf_s(direct, ARRAYSIZE(direct), L"%ls\\sparse-copy.bin", root);
    swprintf_s(nested, ARRAYSIZE(nested), L"%ls\\sparse.bin", tree_copy);
    file = CreateFileW(source, GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return FALSE;
    prepared = DeviceIoControl(file, FSCTL_SET_SPARSE, NULL, 0, NULL, 0, &bytes, NULL) &&
               SetFilePointerEx(file, tail, NULL, FILE_BEGIN) &&
               WriteFile(file, "end", 3, &bytes, NULL) && bytes == 3;
    CloseHandle(file);
    return prepared && !plain_copy(source, direct) &&
           GetFileAttributesExW(direct, GetFileExInfoStandard, &info) &&
           (info.dwFileAttributes & FILE_ATTRIBUTE_SPARSE_FILE) && info.nFileSizeLow == tail.LowPart + 3 &&
           !copy_new_tree(root, tree_copy, 4, FALSE, &result) &&
           GetFileAttributesExW(nested, GetFileExInfoStandard, &info) &&
           (info.dwFileAttributes & FILE_ATTRIBUTE_SPARSE_FILE) && info.nFileSizeLow == tail.LowPart + 3;
}

static HRESULT test_command(const wchar_t *dll, const wchar_t *path)
{
    static const CLSID id = {0xf64c0d11, 0xf4c2, 0x4358, {0x80,0x85,0x93,0x10,0x59,0xe2,0x84,0xf6}};
    typedef HRESULT (WINAPI *GET_CLASS_OBJECT)(REFCLSID, REFIID, void **);
    GET_CLASS_OBJECT get_class;
    FARPROC proc;
    HMODULE module;
    IClassFactory *factory = NULL;
    IExplorerCommand *command = NULL;
    IShellItem *item = NULL;
    IShellItemArray *array = NULL;
    LPWSTR title = NULL;
    EXPCMDSTATE state = ECS_HIDDEN;
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(hr))
        return hr;
    module = LoadLibraryExW(dll, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) {
        hr = HRESULT_FROM_WIN32(GetLastError());
        CoUninitialize();
        return hr;
    }
    proc = GetProcAddress(module, "DllGetClassObject");
    memcpy(&get_class, &proc, sizeof(proc));
    hr = get_class ? get_class(&id, &IID_IClassFactory, (void **)&factory) : E_NOINTERFACE;
    if (SUCCEEDED(hr))
        hr = IClassFactory_CreateInstance(factory, NULL, &IID_IExplorerCommand, (void **)&command);
    if (SUCCEEDED(hr))
        hr = SHCreateItemFromParsingName(path, NULL, &IID_IShellItem, (void **)&item);
    if (SUCCEEDED(hr))
        hr = SHCreateShellItemArrayFromShellItem(item, &IID_IShellItemArray, (void **)&array);
    if (SUCCEEDED(hr))
        hr = IExplorerCommand_GetState(command, array, FALSE, &state);
    if (SUCCEEDED(hr) && state != ECS_ENABLED)
        hr = E_FAIL;
    if (SUCCEEDED(hr))
        hr = IExplorerCommand_GetTitle(command, array, &title);
    if (SUCCEEDED(hr) && (!title || !wcsstr(title, L"stdio.sys")))
        hr = E_FAIL;
    CoTaskMemFree(title);
    if (array)
        IShellItemArray_Release(array);
    if (item)
        IShellItem_Release(item);
    if (command)
        IExplorerCommand_Release(command);
    if (factory)
        IClassFactory_Release(factory);
    FreeLibrary(module);
    CoUninitialize();
    return hr;
}

int wmain(int argc, wchar_t **argv)
{
    wchar_t cwd[MAX_PATH], root[MAX_PATH], id[40], source[MAX_PATH], copy[MAX_PATH], stream[MAX_PATH];
    wchar_t relative[MAX_PATH], hardlink[MAX_PATH], tree_copy[MAX_PATH], nested[MAX_PATH];
    wchar_t left[MAX_PATH], right[MAX_PATH], payload[MAX_PATH];
    TREE_RESULT result;
    GUID guid;
    HANDLE lock = INVALID_HANDLE_VALUE;
    DWORD chars, collision;
    int status = 1;
    if (argc != 2 || FAILED(CoCreateGuid(&guid)))
        return 1;
    chars = GetCurrentDirectoryW(ARRAYSIZE(cwd), cwd);
    if (!chars || chars >= ARRAYSIZE(cwd))
        return 1;
    StringFromGUID2(&guid, id, ARRAYSIZE(id));
    if (swprintf_s(root, ARRAYSIZE(root), L"%ls\\stdio-test-%ls", cwd, id) < 0 ||
        wcslen(root) > MAX_PATH - 40 || !CreateDirectoryW(root, NULL))
        return 1;
    swprintf_s(source, ARRAYSIZE(source), L"%ls\\тест.txt", root);
    swprintf_s(copy, ARRAYSIZE(copy), L"%ls\\copy.txt", root);
    swprintf_s(hardlink, ARRAYSIZE(hardlink), L"%ls\\hardlink.txt", root);
    swprintf_s(tree_copy, ARRAYSIZE(tree_copy), L"%ls-copy", root);
    swprintf_s(nested, ARRAYSIZE(nested), L"%ls\\inside", root);
    swprintf_s(stream, ARRAYSIZE(stream), L"%ls:meta", source);
    if (!make_file(source, "contents", 8) || !make_file(stream, "ads", 3) ||
        !CreateHardLinkW(hardlink, source, NULL) ||
        tree_run(root, FALSE, 0, &result) || result.files != 2 || result.bytes != 16 ||
        FAILED(test_command(argv[1], root))) {
        fputws(L"Enumeration, hard link or Explorer command failed.\n", stderr);
        goto cleanup;
    }
    if (plain_copy(source, copy)) {
        fputws(L"Copy failed.\n", stderr);
        goto cleanup;
    }
    collision = plain_copy(source, copy);
    if (collision != ERROR_FILE_EXISTS && collision != ERROR_ALREADY_EXISTS) {
        fputws(L"Copy or collision handling failed.\n", stderr);
        goto cleanup;
    }
    swprintf_s(stream, ARRAYSIZE(stream), L"%ls:meta", copy);
    lock = CreateFileW(stream, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    char ads[4];
    DWORD read;
    if (lock == INVALID_HANDLE_VALUE || !ReadFile(lock, ads, sizeof(ads), &read, NULL) ||
        read != 3 || memcmp(ads, "ads", 3)) {
        fputws(L"Copy lost the alternate data stream.\n", stderr);
        goto cleanup;
    }
    CloseHandle(lock);
    lock = INVALID_HANDLE_VALUE;
    swprintf_s(left, ARRAYSIZE(left), L"%ls\\left", root);
    swprintf_s(right, ARRAYSIZE(right), L"%ls\\right", root);
    if (!CreateDirectoryW(left, NULL) || !CreateDirectoryW(right, NULL))
        goto cleanup;
    swprintf_s(payload, ARRAYSIZE(payload), L"%ls\\payload.txt", left);
    swprintf_s(stream, ARRAYSIZE(stream), L"%ls:meta", payload);
    if (!make_file(payload, "left", 4) || !make_file(stream, "ads", 3))
        goto cleanup;
    swprintf_s(payload, ARRAYSIZE(payload), L"%ls\\payload.txt", right);
    if (!make_file(payload, "right", 5))
        goto cleanup;
    if (copy_new_tree(root, nested, 4, FALSE, &result) != ERROR_INVALID_PARAMETER ||
        GetFileAttributesW(nested) != INVALID_FILE_ATTRIBUTES ||
        copy_new_tree(root, tree_copy, 4, FALSE, &result) ||
        tree_run(tree_copy, FALSE, 0, &result) ||
        result.files != 5 || result.directories != 3 || result.bytes != 33) {
        fputws(L"Tree copy failed or accepted a destination inside its source.\n", stderr);
        goto cleanup;
    }
    if (!copy_new_tree(root, tree_copy, 1, FALSE, &result)) {
        fputws(L"Tree copy accepted an existing destination.\n", stderr);
        goto cleanup;
    }
    swprintf_s(stream, ARRAYSIZE(stream), L"%ls\\left\\payload.txt:meta", tree_copy);
    lock = CreateFileW(stream, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (lock == INVALID_HANDLE_VALUE || !ReadFile(lock, ads, sizeof(ads), &read, NULL) ||
        read != 3 || memcmp(ads, "ads", 3)) {
        fputws(L"Tree copy lost the alternate data stream.\n", stderr);
        goto cleanup;
    }
    CloseHandle(lock);
    lock = INVALID_HANDLE_VALUE;
    if (tree_run(tree_copy, TRUE, 4, &result)) {
        fputws(L"Tree copy cleanup failed.\n", stderr);
        goto cleanup;
    }
    if (!sparse_copy_test(root, tree_copy) || tree_run(tree_copy, TRUE, 4, &result)) {
        fputws(L"Sparse file copy failed.\n", stderr);
        goto cleanup;
    }
    if (!copy_admission_test(root, tree_copy)) {
        fputws(L"Copy admission or retry failed.\n", stderr);
        goto cleanup;
    }
    if (!partial_copy_test(root) || !directory_stream_test(root)) {
        fputws(L"Partial copy, cancellation or directory stream handling failed.\n", stderr);
        goto cleanup;
    }
    lock = CreateFileW(copy, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (lock == INVALID_HANDLE_VALUE || !tree_run(root, TRUE, 4, &result) ||
        GetFileAttributesW(copy) == INVALID_FILE_ATTRIBUTES) {
        fputws(L"Sharing violation was not preserved.\n", stderr);
        goto cleanup;
    }
    CloseHandle(lock);
    lock = INVALID_HANDLE_VALUE;
    if (!SetFileAttributesW(copy, FILE_ATTRIBUTE_READONLY) || !tree_run(root, TRUE, 1, &result) ||
        GetFileAttributesW(copy) == INVALID_FILE_ATTRIBUTES) {
        fputws(L"Read-only file was not preserved.\n", stderr);
        goto cleanup;
    }
    swprintf_s(relative, ARRAYSIZE(relative), L".\\stdio-test-%ls", id);
    if (!SetFileAttributesW(copy, FILE_ATTRIBUTE_NORMAL) ||
        tree_run(relative, TRUE, 4, &result) || GetFileAttributesW(root) != INVALID_FILE_ATTRIBUTES) {
        fputws(L"Retry did not remove the fixture.\n", stderr);
        goto cleanup;
    }
    status = 0;
cleanup:
    if (lock != INVALID_HANDLE_VALUE)
        CloseHandle(lock);
    if (status) {
        SetFileAttributesW(copy, FILE_ATTRIBUTE_NORMAL);
        tree_run(root, TRUE, 1, &result);
        tree_run(tree_copy, TRUE, 1, &result);
    }
    return status;
}
