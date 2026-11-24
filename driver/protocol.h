#pragma once

#define STDIO_PORT L"\\StdioSysStatus"
#define STDIO_PROTOCOL_VERSION 1UL

typedef struct {
    unsigned long version;
} FILTER_STATUS_REQUEST;

typedef struct {
    unsigned long version;
    unsigned long size;
    unsigned long io_callbacks;
} FILTER_STATUS_REPLY;
