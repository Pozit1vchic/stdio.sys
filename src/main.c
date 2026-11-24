#include "engine.h"
#include <shellapi.h>
#include <shlobj.h>
#include <wchar.h>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR line, int show)
{
    int argc;
    wchar_t **argv;
    DWORD error = ERROR_INVALID_PARAMETER;
    TREE_RESULT result = {0};
    wchar_t message[256];
    BOOL remove;
    UNREFERENCED_PARAMETER(instance);
    UNREFERENCED_PARAMETER(previous);
    UNREFERENCED_PARAMETER(line);
    UNREFERENCED_PARAMETER(show);

    argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv)
        return 1;
    if (argc != 3 || (wcscmp(argv[1], L"size") && wcscmp(argv[1], L"delete"))) {
        LocalFree(argv);
        return 1;
    }
    remove = !wcscmp(argv[1], L"delete");
    if (remove && MessageBoxW(NULL, argv[2], L"stdio.sys — Delete permanently?",
                              MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2 | MB_SETFOREGROUND) != IDYES) {
        LocalFree(argv);
        return 0;
    }
    error = tree_run(argv[2], remove, 0, &result);
    if (!remove && error) {
        HRESULT apartment = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
        BOOL opened = SHObjectProperties(NULL, SHOP_FILEPATH, argv[2], NULL);
        if (SUCCEEDED(apartment))
            CoUninitialize();
        if (opened) {
            LocalFree(argv);
            return 0;
        }
    }
    if (error) {
        _snwprintf_s(message, ARRAYSIZE(message), _TRUNCATE,
                     L"Operation incomplete (Windows error %lu).\nFiles processed: %llu\nFolders processed: %llu\nErrors: %llu",
                     error, result.files, result.directories, result.errors);
    } else {
        _snwprintf_s(message, ARRAYSIZE(message), _TRUNCATE, L"Files: %llu\nFolders: %llu\nBytes: %llu",
                     result.files, result.directories, result.bytes);
    }
    if (error || !remove)
        MessageBoxW(NULL, message, L"stdio.sys", MB_OK | MB_SETFOREGROUND |
                    (error ? MB_ICONERROR : MB_ICONINFORMATION));
    LocalFree(argv);
    return error ? 1 : 0;
}
