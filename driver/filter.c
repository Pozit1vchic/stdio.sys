#pragma warning(push)
#pragma warning(disable: 4324)
#include <fltKernel.h>
#pragma warning(pop)
#include "protocol.h"

static PFLT_FILTER filter;
static PFLT_PORT listener;

DRIVER_INITIALIZE DriverEntry;

static NTSTATUS unload(FLT_FILTER_UNLOAD_FLAGS flags)
{
    UNREFERENCED_PARAMETER(flags);
    if (listener)
        FltCloseCommunicationPort(listener);
    FltUnregisterFilter(filter);
    return STATUS_SUCCESS;
}

static NTSTATUS instance_setup(PCFLT_RELATED_OBJECTS objects, FLT_INSTANCE_SETUP_FLAGS flags,
                               DEVICE_TYPE device, FLT_FILESYSTEM_TYPE filesystem)
{
    UNREFERENCED_PARAMETER(objects);
    if (!(flags & FLTFL_INSTANCE_SETUP_MANUAL_ATTACHMENT) ||
        device != FILE_DEVICE_DISK_FILE_SYSTEM || filesystem != FLT_FSTYPE_NTFS)
        return STATUS_FLT_DO_NOT_ATTACH;
    return STATUS_SUCCESS;
}

static NTSTATUS detach(PCFLT_RELATED_OBJECTS objects, FLT_INSTANCE_QUERY_TEARDOWN_FLAGS flags)
{
    UNREFERENCED_PARAMETER(objects);
    UNREFERENCED_PARAMETER(flags);
    return STATUS_SUCCESS;
}

static NTSTATUS connect_client(PFLT_PORT client, PVOID server_cookie, PVOID input,
                               ULONG input_size, PVOID *client_cookie)
{
    PFLT_PORT *connection;
    UNREFERENCED_PARAMETER(server_cookie);
    UNREFERENCED_PARAMETER(input);
    if (input_size)
        return STATUS_INVALID_PARAMETER;
    connection = ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*connection), 'oIdS');
    if (!connection)
        return STATUS_INSUFFICIENT_RESOURCES;
    *connection = client;
    *client_cookie = connection;
    return STATUS_SUCCESS;
}

static VOID disconnect_client(PVOID client_cookie)
{
    FltCloseClientPort(filter, (PFLT_PORT *)client_cookie);
    ExFreePoolWithTag(client_cookie, 'oIdS');
}

static NTSTATUS status_message(PVOID client_cookie, PVOID input, ULONG input_size,
                               PVOID output, ULONG output_size, PULONG written)
{
    FILTER_STATUS_REQUEST request;
    FILTER_STATUS_REPLY reply = { STDIO_PROTOCOL_VERSION, sizeof(reply), 0 };
    UNREFERENCED_PARAMETER(client_cookie);
    *written = 0;
    if (!input || input_size != sizeof(request))
        return STATUS_INVALID_PARAMETER;
    if (!output || output_size < sizeof(reply))
        return STATUS_BUFFER_TOO_SMALL;
    __try {
        RtlCopyMemory(&request, input, sizeof(request));
        if (request.version != STDIO_PROTOCOL_VERSION)
            return STATUS_REVISION_MISMATCH;
        RtlCopyMemory(output, &reply, sizeof(reply));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return GetExceptionCode();
    }
    *written = sizeof(reply);
    return STATUS_SUCCESS;
}

static const FLT_REGISTRATION registration = {
    sizeof(registration), FLT_REGISTRATION_VERSION, 0,
    NULL, NULL, unload, instance_setup, detach, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL
};

NTSTATUS DriverEntry(PDRIVER_OBJECT driver, PUNICODE_STRING registry_path)
{
    NTSTATUS status;
    PSECURITY_DESCRIPTOR security;
    UNICODE_STRING name = RTL_CONSTANT_STRING(STDIO_PORT);
    OBJECT_ATTRIBUTES attributes;
    RTL_OSVERSIONINFOW version = { sizeof(version) };
    UNREFERENCED_PARAMETER(registry_path);

    status = RtlGetVersion(&version);
    if (!NT_SUCCESS(status))
        return status;
    if (version.dwMajorVersion < 10 || version.dwBuildNumber < 26100)
        return STATUS_NOT_SUPPORTED;
    status = FltRegisterFilter(driver, &registration, &filter);
    if (!NT_SUCCESS(status))
        return status;
    status = FltBuildDefaultSecurityDescriptor(&security, FLT_PORT_ALL_ACCESS);
    if (NT_SUCCESS(status)) {
        InitializeObjectAttributes(&attributes, &name, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE,
                                   NULL, security);
        status = FltCreateCommunicationPort(filter, &listener, &attributes, NULL, connect_client,
                                            disconnect_client, status_message, 8);
        FltFreeSecurityDescriptor(security);
        if (NT_SUCCESS(status))
            status = FltStartFiltering(filter);
    }
    if (!NT_SUCCESS(status))
        unload(0);
    return status;
}
