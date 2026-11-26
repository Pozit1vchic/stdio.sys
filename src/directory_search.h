#pragma once
#include <shobjidl.h>

typedef struct DIRECTORY_SEARCH DIRECTORY_SEARCH;

/* One local NTFS scope. Serialize calls on a COM-initialized background thread
   using the process token. Release the scope when its search view closes. */
HRESULT directory_search_open(PCWSTR path, DIRECTORY_SEARCH **search);
void directory_search_close(DIRECTORY_SEARCH *search);

/* Regular files with this exact name, recursively. S_FALSE is an empty result.
   On failure the caller must use the Shell query. Any returned array is owned
   by the caller and can outlive the search scope. */
HRESULT directory_search_exact(DIRECTORY_SEARCH *search, PCWSTR name, IShellItemArray **items);
