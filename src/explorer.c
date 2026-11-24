#define COBJMACROS
#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <stdlib.h>
#include <wchar.h>

static const CLSID size_clsid = {0xf64c0d11, 0xf4c2, 0x4358, {0x80,0x85,0x93,0x10,0x59,0xe2,0x84,0xf6}};
static const CLSID delete_clsid = {0x05305729, 0xbfad, 0x4019, {0xa8,0x51,0xdb,0x75,0x54,0x4e,0x6b,0xc0}};
static HINSTANCE module;
static volatile LONG objects;

typedef struct {
    IExplorerCommand iface;
    LONG references;
    BOOL remove;
} COMMAND;

typedef struct {
    IClassFactory iface;
    LONG references;
    BOOL remove;
} FACTORY;

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    UNREFERENCED_PARAMETER(reserved);
    if (reason == DLL_PROCESS_ATTACH)
        module = instance;
    return TRUE;
}

static const wchar_t *title(BOOL remove)
{
    return remove ? L"Delete permanently (stdio.sys)" : L"Folder size (stdio.sys)";
}

static HRESULT WINAPI command_query(IExplorerCommand *self, REFIID iid, void **out)
{
    *out = NULL;
    if (!IsEqualIID(iid, &IID_IUnknown) && !IsEqualIID(iid, &IID_IExplorerCommand))
        return E_NOINTERFACE;
    *out = self;
    IExplorerCommand_AddRef(self);
    return S_OK;
}

static ULONG WINAPI command_add(IExplorerCommand *self)
{
    return (ULONG)InterlockedIncrement(&((COMMAND *)self)->references);
}

static ULONG WINAPI command_release(IExplorerCommand *self)
{
    LONG count = InterlockedDecrement(&((COMMAND *)self)->references);
    if (!count) {
        free(self);
        InterlockedDecrement(&objects);
    }
    return (ULONG)count;
}

static HRESULT WINAPI command_title(IExplorerCommand *self, IShellItemArray *items, LPWSTR *out)
{
    UNREFERENCED_PARAMETER(items);
    return SHStrDupW(title(((COMMAND *)self)->remove), out);
}

static HRESULT WINAPI command_no_string(IExplorerCommand *self, IShellItemArray *items, LPWSTR *out)
{
    UNREFERENCED_PARAMETER(self);
    UNREFERENCED_PARAMETER(items);
    *out = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI command_name(IExplorerCommand *self, GUID *out)
{
    *out = ((COMMAND *)self)->remove ? delete_clsid : size_clsid;
    return S_OK;
}

static HRESULT WINAPI command_state(IExplorerCommand *self, IShellItemArray *items,
                                   BOOL slow, EXPCMDSTATE *out)
{
    DWORD count = 0;
    UNREFERENCED_PARAMETER(self);
    UNREFERENCED_PARAMETER(slow);
    *out = items && SUCCEEDED(IShellItemArray_GetCount(items, &count)) && count == 1 ?
           ECS_ENABLED : ECS_HIDDEN;
    return S_OK;
}

static HRESULT WINAPI command_invoke(IExplorerCommand *self, IShellItemArray *items, IBindCtx *binding)
{
    IShellItem *item;
    wchar_t *path = NULL, *executable, *line, *base;
    DWORD length, count;
    HRESULT hr;
    STARTUPINFOW startup = { sizeof(startup) };
    PROCESS_INFORMATION child;
    UNREFERENCED_PARAMETER(binding);

    if (!items || FAILED(IShellItemArray_GetCount(items, &count)) || count != 1)
        return E_INVALIDARG;
    hr = IShellItemArray_GetItemAt(items, 0, &item);
    if (FAILED(hr))
        return hr;
    hr = IShellItem_GetDisplayName(item, SIGDN_FILESYSPATH, &path);
    IShellItem_Release(item);
    if (FAILED(hr))
        return hr;
    executable = malloc(32768 * sizeof(*executable));
    line = malloc(32768 * sizeof(*line));
    if (!executable || !line) {
        hr = E_OUTOFMEMORY;
    } else {
        length = GetModuleFileNameW(module, executable, 32768);
        base = length && length < 32768 ? wcsrchr(executable, L'\\') : NULL;
        if (!base || (base - executable) > 32756 || wcschr(path, L'"') ||
            wcslen(path) < 4 || path[wcslen(path) - 1] == L'\\') {
            hr = E_INVALIDARG;
        } else {
            wcscpy_s(base + 1, 32768 - (size_t)(base + 1 - executable), L"stdio.exe");
            if (_snwprintf_s(line, 32768, _TRUNCATE, L"\"%ls\" %ls \"%ls\"", executable,
                             ((COMMAND *)self)->remove ? L"delete" : L"size", path) < 0) {
                hr = HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE);
            } else if (!CreateProcessW(executable, line, NULL, NULL, FALSE, 0, NULL, NULL,
                                       &startup, &child)) {
                hr = HRESULT_FROM_WIN32(GetLastError());
            } else {
                CloseHandle(child.hThread);
                CloseHandle(child.hProcess);
            }
        }
    }
    free(executable);
    free(line);
    CoTaskMemFree(path);
    return hr;
}

static HRESULT WINAPI command_flags(IExplorerCommand *self, EXPCMDFLAGS *out)
{
    UNREFERENCED_PARAMETER(self);
    *out = ECF_DEFAULT;
    return S_OK;
}

static HRESULT WINAPI command_enum(IExplorerCommand *self, IEnumExplorerCommand **out)
{
    UNREFERENCED_PARAMETER(self);
    *out = NULL;
    return E_NOTIMPL;
}

static IExplorerCommandVtbl command_vtable = {
    command_query, command_add, command_release, command_title, command_no_string,
    command_no_string, command_name, command_state, command_invoke, command_flags, command_enum
};

static HRESULT WINAPI factory_query(IClassFactory *self, REFIID iid, void **out)
{
    *out = NULL;
    if (!IsEqualIID(iid, &IID_IUnknown) && !IsEqualIID(iid, &IID_IClassFactory))
        return E_NOINTERFACE;
    *out = self;
    IClassFactory_AddRef(self);
    return S_OK;
}

static ULONG WINAPI factory_add(IClassFactory *self)
{
    return (ULONG)InterlockedIncrement(&((FACTORY *)self)->references);
}

static ULONG WINAPI factory_release(IClassFactory *self)
{
    LONG count = InterlockedDecrement(&((FACTORY *)self)->references);
    if (!count) {
        free(self);
        InterlockedDecrement(&objects);
    }
    return (ULONG)count;
}

static HRESULT WINAPI factory_create(IClassFactory *self, IUnknown *outer, REFIID iid, void **out)
{
    COMMAND *command;
    HRESULT hr;
    *out = NULL;
    if (outer)
        return CLASS_E_NOAGGREGATION;
    command = malloc(sizeof(*command));
    if (!command)
        return E_OUTOFMEMORY;
    *command = (COMMAND){ { &command_vtable }, 1, ((FACTORY *)self)->remove };
    InterlockedIncrement(&objects);
    hr = command_query(&command->iface, iid, out);
    command_release(&command->iface);
    return hr;
}

static HRESULT WINAPI factory_lock(IClassFactory *self, BOOL lock)
{
    UNREFERENCED_PARAMETER(self);
    if (lock)
        InterlockedIncrement(&objects);
    else
        InterlockedDecrement(&objects);
    return S_OK;
}

static IClassFactoryVtbl factory_vtable = {
    factory_query, factory_add, factory_release, factory_create, factory_lock
};

HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID iid, void **out)
{
    FACTORY *factory;
    HRESULT hr;
    BOOL remove = IsEqualCLSID(clsid, &delete_clsid);
    *out = NULL;
    if (!remove && !IsEqualCLSID(clsid, &size_clsid))
        return CLASS_E_CLASSNOTAVAILABLE;
    factory = malloc(sizeof(*factory));
    if (!factory)
        return E_OUTOFMEMORY;
    *factory = (FACTORY){ { &factory_vtable }, 1, remove };
    InterlockedIncrement(&objects);
    hr = factory_query(&factory->iface, iid, out);
    factory_release(&factory->iface);
    return hr;
}

HRESULT WINAPI DllCanUnloadNow(void)
{
    return InterlockedCompareExchange(&objects, 0, 0) ? S_FALSE : S_OK;
}

static LSTATUS set_string(const wchar_t *key, const wchar_t *name, const wchar_t *data)
{
    return RegSetKeyValueW(HKEY_CURRENT_USER, key, name, REG_SZ, data,
                           (DWORD)((wcslen(data) + 1) * sizeof(*data)));
}

static LSTATUS unregister_command(BOOL remove)
{
    wchar_t id[40], key[160];
    LSTATUS verb, com;
    StringFromGUID2(remove ? &delete_clsid : &size_clsid, id, ARRAYSIZE(id));
    swprintf_s(key, ARRAYSIZE(key), L"Software\\Classes\\Directory\\shell\\stdio.sys.%ls",
               remove ? L"delete" : L"size");
    verb = RegDeleteTreeW(HKEY_CURRENT_USER, key);
    swprintf_s(key, ARRAYSIZE(key), L"Software\\Classes\\CLSID\\%ls", id);
    com = RegDeleteTreeW(HKEY_CURRENT_USER, key);
    if (verb != ERROR_SUCCESS && verb != ERROR_FILE_NOT_FOUND && verb != ERROR_PATH_NOT_FOUND)
        return verb;
    return com == ERROR_FILE_NOT_FOUND || com == ERROR_PATH_NOT_FOUND ? ERROR_SUCCESS : com;
}

static LSTATUS register_command(BOOL remove, const wchar_t *path)
{
    wchar_t id[40], key[160];
    LSTATUS status;
    StringFromGUID2(remove ? &delete_clsid : &size_clsid, id, ARRAYSIZE(id));
    swprintf_s(key, ARRAYSIZE(key), L"Software\\Classes\\CLSID\\%ls\\InprocServer32", id);
    status = set_string(key, NULL, path);
    if (!status)
        status = set_string(key, L"ThreadingModel", L"Apartment");
    swprintf_s(key, ARRAYSIZE(key), L"Software\\Classes\\Directory\\shell\\stdio.sys.%ls",
               remove ? L"delete" : L"size");
    if (!status)
        status = set_string(key, L"MUIVerb", title(remove));
    if (!status)
        status = set_string(key, L"ExplorerCommandHandler", id);
    return status;
}

HRESULT WINAPI DllUnregisterServer(void)
{
    LSTATUS size = unregister_command(FALSE), remove = unregister_command(TRUE);
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL);
    return HRESULT_FROM_WIN32(size ? size : remove);
}

HRESULT WINAPI DllRegisterServer(void)
{
    wchar_t path[32768];
    DWORD length = GetModuleFileNameW(module, path, ARRAYSIZE(path));
    LSTATUS status;
    if (!length || length >= ARRAYSIZE(path))
        return HRESULT_FROM_WIN32(length ? ERROR_FILENAME_EXCED_RANGE : GetLastError());
    status = register_command(FALSE, path);
    if (!status)
        status = register_command(TRUE, path);
    if (status)
        DllUnregisterServer();
    else
        SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL);
    return HRESULT_FROM_WIN32(status);
}
