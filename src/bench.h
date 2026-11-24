#pragma once
#include "engine.h"

BOOL parse_unsigned(const wchar_t *text, ULONGLONG *value);
DWORD benchmark_prepare(const wchar_t *parent, ULONGLONG count, const wchar_t *layout);
DWORD benchmark_prepare_large(const wchar_t *parent, ULONGLONG mib);
DWORD benchmark_measure(const wchar_t *label, ULONGLONG files, const wchar_t *cache,
                         const wchar_t *exit_policy, int argc, wchar_t **argv);
DWORD benchmark_baseline(const wchar_t *path, const wchar_t *needle);
