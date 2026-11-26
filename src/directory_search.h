#pragma once
#include <shobjidl.h>
#include <structuredquerycondition.h>

typedef struct DIRECTORY_SEARCH DIRECTORY_SEARCH;

/* One local NTFS scope. Serialize calls on a COM-initialized background thread
   using the process token. Release the scope when its search view closes. */
HRESULT directory_search_open(PCWSTR path, DIRECTORY_SEARCH **search);
void directory_search_close(DIRECTORY_SEARCH *search);

/* Accepts System.FileName:="name" for a recursive scope. The condition must
   come from the Shell parser for that same query. S_FALSE is an empty result.
   A failure leaves the output untouched for fallback. The caller owns the array. */
HRESULT directory_search_condition(DIRECTORY_SEARCH *search, PCWSTR query, ICondition *condition,
                                   IShellItemArray **items);
