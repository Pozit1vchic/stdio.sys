#include "engine.h"
#include <wchar.h>

static DWORD check_entry(void *context, ULONGLONG parent, const FILE_ID_EXTD_DIR_INFO *entry)
{
    UNREFERENCED_PARAMETER(context);
    UNREFERENCED_PARAMETER(parent);
    /* The Shell's reparse and provider rules are independent of the engine walk. */
    if (entry->FileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_OFFLINE))
        return ERROR_NOT_SUPPORTED;
    return ERROR_SUCCESS;
}

/* Read-only comparison in the host process. The native property worker owns its result. */
__declspec(dllexport) DWORD WINAPI ShellFolderWalkCompare(PCWSTR path, ULONGLONG *files,
                                                          ULONGLONG *folders, ULONGLONG *bytes)
{
    ULONGLONG id;
    TREE_RESULT totals;
    DWORD error = tree_snapshot(path, check_entry, NULL, &id, &totals);
    if (error)
        return error;
    *files = totals.files;
    *folders = totals.directories - 1;
    *bytes = totals.bytes;
    return ERROR_SUCCESS;
}
