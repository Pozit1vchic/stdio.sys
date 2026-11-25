#include "engine.h"
#include "bench.h"
#include <fltuser.h>
#include <fcntl.h>
#include <io.h>
#include <wchar.h>
#include "../driver/protocol.h"

static DWORD filter_status(void)
{
    HANDLE port;
    FILTER_STATUS_REQUEST request = { STDIO_PROTOCOL_VERSION };
    FILTER_STATUS_REPLY reply;
    DWORD returned;
    HRESULT hr = FilterConnectCommunicationPort(STDIO_PORT, 0, NULL, 0, NULL, &port);
    if (FAILED(hr))
        return (DWORD)hr;
    hr = FilterSendMessage(port, &request, sizeof(request), &reply, sizeof(reply), &returned);
    CloseHandle(port);
    if (FAILED(hr))
        return (DWORD)hr;
    if (returned != sizeof(reply) || reply.version != STDIO_PROTOCOL_VERSION || reply.size != sizeof(reply))
        return ERROR_REVISION_MISMATCH;
    wprintf(L"protocol=%lu file_io_callbacks=%lu\n", reply.version, reply.io_callbacks);
    return ERROR_SUCCESS;
}

static int help(void)
{
    fputws(L"stdio.sys\n"
           L"  size PATH | list PATH | find PATH NAME\n"
           L"  delete PATH --yes [auto|1..16]\n"
           L"  recycle PATH [PATH ...]\n"
           L"  shell-delete --yes PATH [PATH ...]\n"
           L"  copy DESTINATION SOURCE [SOURCE ...]\n"
           L"  copy-new NEW_DIRECTORY SOURCE_DIRECTORY [auto|1..16] [--unbuffered-large] [--report-items]\n"
           L"  move DESTINATION SOURCE [SOURCE ...]\n"
           L"  move-new NEW_PATH SOURCE (same volume)\n"
           L"  copy-file SOURCE NEW_FILE [--unbuffered|--no-preallocation]\n"
           L"  storage-info PATH\n"
           L"  index X: | index-dir PATH\n"
           L"  driver-status | driver-load | driver-unload\n"
           L"  prepare PARENT COUNT flat|deep|mixed|fanout\n"
           L"  prepare-large PARENT MIB\n"
           L"  bench-copy SOURCE NEW_FILE [--unbuffered|--no-preallocation] [--hash-after-copy]\n"
           L"  win32-size PATH | win32-find PATH NAME\n"
           L"  measure LABEL FILES cold|warm|as-created zero|robocopy EXE [ARGS ...]\n",
           stderr);
    return 2;
}

static void report_copy_item(void *context, const TREE_COPY_ITEM *item)
{
    static const wchar_t *const states[] = {
        L"complete", L"failed", L"skipped", L"cancelled", L"incomplete"
    };
    static const wchar_t *const effects[] = {
        L"untouched", L"directory-created", L"file-copied", L"file-outcome-unknown"
    };
    UNREFERENCED_PARAMETER(context);
    wprintf(L"%ls\t%ls\t%ls\t0x%08lX\t%ls\t%ls\n", item->directory ? L"directory" : L"file",
            states[item->state], effects[item->effect], item->error,
            item->source ? item->source : L"", item->destination ? item->destination : L"");
}

int wmain(int argc, wchar_t **argv)
{
    DWORD error = ERROR_INVALID_PARAMETER;
    ULONGLONG count;
    const wchar_t *verb;

    _setmode(_fileno(stdout), _isatty(_fileno(stdout)) ? _O_U16TEXT : _O_U8TEXT);
    _setmode(_fileno(stderr), _isatty(_fileno(stderr)) ? _O_U16TEXT : _O_U8TEXT);
    _setmode(_fileno(stdin), _isatty(_fileno(stdin)) ? _O_U16TEXT : _O_U8TEXT);
    if (argc < 2)
        return help();
    verb = argv[1];
    if ((argc == 3 && !wcscmp(verb, L"size")) ||
        ((argc == 4 || argc == 5) && !wcscmp(verb, L"delete") && !wcscmp(argv[3], L"--yes"))) {
        TREE_RESULT result;
        BOOL remove = !wcscmp(verb, L"delete");
        count = 0;
        if (argc == 5 && wcscmp(argv[4], L"auto") &&
            (!parse_unsigned(argv[4], &count) || !count || count > 16))
            return help();
        error = tree_run(argv[2], remove, (DWORD)count, &result);
        wprintf(L"files=%llu directories=%llu bytes=%llu errors=%llu workers=%lu\n",
                result.files, result.directories, result.bytes, result.errors, result.workers);
    } else if (argc == 3 && !wcscmp(verb, L"list")) {
        error = tree_list(argv[2]);
    } else if (argc == 4 && !wcscmp(verb, L"find") && *argv[3]) {
        error = tree_find(argv[2], argv[3], &count);
        wprintf(L"matches=%llu\n", count);
    } else if (argc == 3 && !wcscmp(verb, L"index")) {
        error = index_console(argv[2]);
        if (error == ERROR_ACCESS_DENIED || error == ERROR_JOURNAL_NOT_ACTIVE)
            fputws(L"Use index-dir PATH for a folder index under the current account.\n", stderr);
    } else if (argc == 3 && !wcscmp(verb, L"index-dir")) {
        error = directory_index_console(argv[2]);
    } else if (argc >= 3 && !wcscmp(verb, L"recycle")) {
        error = (DWORD)shell_operation(verb, argc - 2, argv + 2, NULL, GetConsoleWindow(), FALSE);
    } else if (argc >= 4 && (!wcscmp(verb, L"copy") || !wcscmp(verb, L"move"))) {
        error = (DWORD)shell_operation(verb, argc - 3, argv + 3, argv[2], GetConsoleWindow(), FALSE);
    } else if (argc >= 4 && argc <= 7 && !wcscmp(verb, L"copy-new")) {
        TREE_COPY_RESULT summary;
        const TREE_RESULT *result = &summary.totals;
        TREE_COPY_OPTIONS options = {0};
        BOOL unbuffered = FALSE;
        int option = 4;
        count = 0;
        if (option < argc && wcsncmp(argv[option], L"--", 2)) {
            if (wcscmp(argv[option], L"auto") &&
                (!parse_unsigned(argv[option], &count) || !count || count > 16))
                return help();
            ++option;
        }
        for (; option < argc; ++option) {
            if (!unbuffered && !wcscmp(argv[option], L"--unbuffered-large"))
                unbuffered = TRUE;
            else if (!options.report && !wcscmp(argv[option], L"--report-items"))
                options.report = report_copy_item;
            else
                return help();
        }
        error = copy_new_tree_report(argv[3], argv[2], (DWORD)count, unbuffered,
                                     options.report ? &options : NULL, &summary);
        wprintf(L"files=%llu directories=%llu bytes=%llu errors=%llu workers=%lu\n",
                result->files, result->directories, result->bytes, result->errors, result->workers);
        if (error || options.report)
            wprintf(L"directories_created=%llu files_outcome_unknown=%llu skipped=%llu cancelled=%llu\n",
                    summary.directories_created, summary.files_outcome_unknown,
                    summary.skipped, summary.cancelled);
    } else if (argc >= 4 && !wcscmp(verb, L"shell-delete") && !wcscmp(argv[2], L"--yes")) {
        error = (DWORD)shell_operation(L"delete", argc - 3, argv + 3, NULL, NULL, TRUE);
    } else if (argc >= 4 && argc <= 6 && (!wcscmp(verb, L"copy-file") || !wcscmp(verb, L"bench-copy"))) {
        BOOL benchmark = !wcscmp(verb, L"bench-copy");
        BOOL hash_after_copy = FALSE;
        STDIO_COPY_MODE mode = STDIO_COPY_BUFFERED;
        for (int i = 4; i < argc; ++i) {
            if (benchmark && !hash_after_copy && !wcscmp(argv[i], L"--hash-after-copy"))
                hash_after_copy = TRUE;
            else if (mode == STDIO_COPY_BUFFERED && !wcscmp(argv[i], L"--unbuffered"))
                mode = STDIO_COPY_UNBUFFERED;
            else if (mode == STDIO_COPY_BUFFERED && !wcscmp(argv[i], L"--no-preallocation"))
                mode = STDIO_COPY_NO_PREALLOCATION;
            else
                return help();
        }
        if (benchmark)
            error = benchmark_copy(argv[2], argv[3], mode, hash_after_copy);
        else
            error = copy_file_mode(argv[2], argv[3], mode);
    } else if (argc == 3 && !wcscmp(verb, L"storage-info")) {
        error = storage_info(argv[2]);
    } else if (argc == 4 && !wcscmp(verb, L"move-new")) {
        error = MoveFileExW(argv[3], argv[2], 0) ? ERROR_SUCCESS : GetLastError();
    } else if (argc == 2 && !wcscmp(verb, L"driver-status")) {
        error = filter_status();
    } else if (argc == 2 && !wcscmp(verb, L"driver-load")) {
        error = (DWORD)FilterLoad(L"stdio");
    } else if (argc == 2 && !wcscmp(verb, L"driver-unload")) {
        error = (DWORD)FilterUnload(L"stdio");
    } else if (argc == 5 && !wcscmp(verb, L"prepare") && parse_unsigned(argv[3], &count)) {
        error = benchmark_prepare(argv[2], count, argv[4]);
    } else if (argc == 4 && !wcscmp(verb, L"prepare-large") && parse_unsigned(argv[3], &count)) {
        error = benchmark_prepare_large(argv[2], count);
    } else if ((argc == 3 && !wcscmp(verb, L"win32-size")) ||
               (argc == 4 && !wcscmp(verb, L"win32-find") && *argv[3])) {
        error = benchmark_baseline(argv[2], argc == 4 ? argv[3] : NULL);
    } else if (argc >= 7 && !wcscmp(verb, L"measure") && parse_unsigned(argv[3], &count)) {
        error = benchmark_measure(argv[2], count, argv[4], argv[5], argc - 6, argv + 6);
    } else {
        return help();
    }
    if (error) {
        print_error(verb, error);
        return 1;
    }
    return 0;
}
