#define COBJMACROS
#include <shlobj.h>
#include <shellapi.h>
#include "engine.h"
#include <wchar.h>

HRESULT shell_operation(const wchar_t *verb, int count, wchar_t **paths,
                        const wchar_t *destination, HWND owner, BOOL quiet)
{
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    IFileOperation *operation = NULL;
    IShellItem *target = NULL;
    BOOL aborted = FALSE;
    BOOL recycle = !wcscmp(verb, L"recycle");
    BOOL remove = recycle || !wcscmp(verb, L"delete");
    BOOL move = !wcscmp(verb, L"move");
    DWORD flags = recycle ? FOFX_RECYCLEONDELETE | FOFX_ADDUNDORECORD | FOF_WANTNUKEWARNING : 0;
    int i;

    if (FAILED(hr))
        return hr;
    if (quiet)
        flags |= FOF_SILENT | FOF_NOERRORUI | FOF_NOCONFIRMATION | FOF_NOCONFIRMMKDIR |
                 FOFX_EARLYFAILURE;
    hr = CoCreateInstance(&CLSID_FileOperation, NULL, CLSCTX_INPROC_SERVER,
                          &IID_IFileOperation, (void **)&operation);
    if (SUCCEEDED(hr))
        hr = IFileOperation_SetOperationFlags(operation, flags);
    if (SUCCEEDED(hr) && owner)
        hr = IFileOperation_SetOwnerWindow(operation, owner);
    if (SUCCEEDED(hr) && destination)
        hr = SHCreateItemFromParsingName(destination, NULL, &IID_IShellItem, (void **)&target);
    for (i = 0; SUCCEEDED(hr) && i < count; ++i) {
        IShellItem *item;
        hr = SHCreateItemFromParsingName(paths[i], NULL, &IID_IShellItem, (void **)&item);
        if (SUCCEEDED(hr)) {
            if (remove)
                hr = IFileOperation_DeleteItem(operation, item, NULL);
            else if (move)
                hr = IFileOperation_MoveItem(operation, item, target, NULL, NULL);
            else
                hr = IFileOperation_CopyItem(operation, item, target, NULL, NULL);
            IShellItem_Release(item);
        }
    }
    if (SUCCEEDED(hr))
        hr = IFileOperation_PerformOperations(operation);
    if (SUCCEEDED(hr)) {
        hr = IFileOperation_GetAnyOperationsAborted(operation, &aborted);
        if (SUCCEEDED(hr) && aborted)
            hr = HRESULT_FROM_WIN32(ERROR_CANCELLED);
    }
    if (target)
        IShellItem_Release(target);
    if (operation)
        IFileOperation_Release(operation);
    CoUninitialize();
    return hr;
}

void print_error(const wchar_t *operation, DWORD error)
{
    wchar_t message[512];
    DWORD chars = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                NULL, error, 0, message, ARRAYSIZE(message), NULL);
    if (chars)
        fwprintf(stderr, L"%ls: 0x%08lX %ls", operation, error, message);
    else
        fwprintf(stderr, L"%ls: 0x%08lX\n", operation, error);
}
