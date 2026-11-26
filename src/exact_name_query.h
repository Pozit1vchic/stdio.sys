#pragma once
#include <windows.h>
#include <wchar.h>

enum { EXACT_NAME_LIMIT = 255 };

/* Recognize only a complete canonical literal. The name is borrowed from query. */
static inline BOOL exact_name_query(PCWSTR query, PCWSTR *name, size_t *length)
{
    static const WCHAR prefix[] = L"System.FileName:=\"";
    const size_t prefix_length = ARRAYSIZE(prefix) - 1;
    PCWSTR component;
    size_t i;
    if (!query)
        return FALSE;
    for (i = 0; i < prefix_length; ++i) {
        if (!query[i])
            return FALSE;
    }
    if (CompareStringOrdinal(query, (int)prefix_length, prefix, (int)prefix_length, TRUE) != CSTR_EQUAL)
        return FALSE;
    component = query + prefix_length;
    for (i = 0; i <= EXACT_NAME_LIMIT; ++i) {
        WCHAR character = component[i];
        if (character == L'"')
            break;
        if (character < L' ' || wcschr(L"\\/:*?<>|", character))
            return FALSE;
    }
    if (!i || i > EXACT_NAME_LIMIT || component[i + 1] ||
        component[i - 1] == L'.' || component[i - 1] == L' ')
        return FALSE;
    if (name)
        *name = component;
    if (length)
        *length = i;
    return TRUE;
}
