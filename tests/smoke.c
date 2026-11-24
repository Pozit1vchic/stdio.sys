#define COBJMACROS
#include <shlobj.h>
#include <wchar.h>
#include "../src/engine.h"

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
    if (lock == INVALID_HANDLE_VALUE || GetFileSize(lock, NULL) != 3) {
        fputws(L"Copy lost the alternate data stream.\n", stderr);
        goto cleanup;
    }
    CloseHandle(lock);
    lock = INVALID_HANDLE_VALUE;
    if (copy_new_tree(root, nested, 4, FALSE, &result) != ERROR_INVALID_PARAMETER ||
        GetFileAttributesW(nested) != INVALID_FILE_ATTRIBUTES ||
        copy_new_tree(root, tree_copy, 4, FALSE, &result) ||
        tree_run(tree_copy, FALSE, 0, &result) || result.files != 3 || result.bytes != 24) {
        fputws(L"Tree copy failed or accepted a destination inside its source.\n", stderr);
        goto cleanup;
    }
    if (!copy_new_tree(root, tree_copy, 1, FALSE, &result)) {
        fputws(L"Tree copy accepted an existing destination.\n", stderr);
        goto cleanup;
    }
    swprintf_s(stream, ARRAYSIZE(stream), L"%ls\\тест.txt:meta", tree_copy);
    lock = CreateFileW(stream, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (lock == INVALID_HANDLE_VALUE || GetFileSize(lock, NULL) != 3) {
        fputws(L"Tree copy lost the alternate data stream.\n", stderr);
        goto cleanup;
    }
    CloseHandle(lock);
    lock = INVALID_HANDLE_VALUE;
    if (tree_run(tree_copy, TRUE, 4, &result)) {
        fputws(L"Tree copy cleanup failed.\n", stderr);
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
