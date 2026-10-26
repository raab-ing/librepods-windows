/*
 * Derived from librepods-windows by ivLis-Studio
 * (https://github.com/ivLis-Studio/librepods-windows), GPLv3 — a Windows port of
 * LibrePods (https://github.com/kavishdevar/librepods).
 */
#pragma once

#include <ntddk.h>
#include <wdf.h>
#include <wdmsec.h>
#include <ntstrsafe.h>
#include <bthdef.h>
#include <bthddi.h>
#include <bthioctl.h>

#include "../shared/librepods_driver_protocol.h"

#define LIBREPODS_DRIVER_POOL_TAG 'pLbL'
#define LIBREPODS_SESSION_POOL_TAG 'sLbL'

typedef struct _LIBREPODS_SESSION_ENTRY {
    LIST_ENTRY ListEntry;
    UINT64 SessionId;
    WDFFILEOBJECT OwningFile;
    CHAR BluetoothAddress[LIBREPODS_DRIVER_MAX_BLUETOOTH_ADDRESS_LENGTH];
    BTH_ADDR RemoteAddress;
    L2CAP_CHANNEL_HANDLE ChannelHandle;
    ULONG CapabilityFlags;
    ULONG Mtu;
    // Refcounted lifetime: read work items can outlive list membership. The
    // list holds one reference; each in-flight operation holds another. When
    // the count reaches zero ZeroRefEvent is signaled and the closer frees.
    LONG RefCount;
    KEVENT ZeroRefEvent;
    // Set under SessionLock when teardown begins; an in-flight open checks it
    // before claiming the session as established.
    BOOLEAN Closing;
    // At most one pending read per session — concurrent ACL read BRBs on the
    // same channel would race the stream.
    BOOLEAN ReadPending;
    ULONG LastPacketLength;
    UCHAR LastPacket[LIBREPODS_DRIVER_MAX_AACP_PACKET_LENGTH];
} LIBREPODS_SESSION_ENTRY, *PLIBREPODS_SESSION_ENTRY;

// State shared between an ioctl handler, the request's cancel routine, and
// the work item that performs the blocking BRB sequence. The request is
// pended so the caller's thread stays interruptible: CancelIoEx and process
// teardown complete the request immediately while the kernel work unwinds.
typedef struct _LIBREPODS_PENDING_IO {
    WDFDEVICE Device;
    WDFREQUEST Request;
    PLIBREPODS_SESSION_ENTRY Session;
    LONG Cancelled;
    USHORT Psm;
    ULONG BufferLength;
} LIBREPODS_PENDING_IO, *PLIBREPODS_PENDING_IO;

typedef struct _LIBREPODS_REQUEST_CONTEXT {
    PLIBREPODS_PENDING_IO Pending;
} LIBREPODS_REQUEST_CONTEXT, *PLIBREPODS_REQUEST_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(LIBREPODS_REQUEST_CONTEXT, LibrepodsGetRequestContext)

typedef struct _LIBREPODS_WORKITEM_CONTEXT {
    PLIBREPODS_PENDING_IO Pending;
} LIBREPODS_WORKITEM_CONTEXT, *PLIBREPODS_WORKITEM_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(LIBREPODS_WORKITEM_CONTEXT, LibrepodsGetWorkItemContext)

typedef struct _LIBREPODS_DEVICE_CONTEXT {
    WDFWAITLOCK SessionLock;
    LIST_ENTRY Sessions;
    UINT64 NextSessionId;
    BOOLEAN HasProfileDriverInterface;
    BOOLEAN HasRemoteDeviceInfo;
    BTH_PROFILE_DRIVER_INTERFACE ProfileDriverInterface;
    BTH_DEVICE_INFO RemoteDeviceInfo;
    ULONG CapabilityFlags;
    ULONG LastError;
    ULONG MaxPacketSize;
    NTSTATUS LastNtStatus;
    // Remote I/O target on the local Bluetooth radio PDO
    // (GUID_BTHPORT_DEVICE_INTERFACE). Kernel-only SDP ioctls
    // (submit/remove record) are sent here because user-mode callers get
    // ERROR_NOT_SUPPORTED on the radio handle.
    WDFIOTARGET RadioTarget;
    WDFWAITLOCK RadioTargetLock;
} LIBREPODS_DEVICE_CONTEXT, *PLIBREPODS_DEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(LIBREPODS_DEVICE_CONTEXT, LibrepodsGetDeviceContext)

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD LibrepodsEvtDeviceAdd;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL LibrepodsEvtIoDeviceControl;
EVT_WDF_OBJECT_CONTEXT_CLEANUP LibrepodsEvtDeviceContextCleanup;
EVT_WDF_DEVICE_PREPARE_HARDWARE LibrepodsEvtDevicePrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE LibrepodsEvtDeviceReleaseHardware;
EVT_WDF_FILE_CLOSE LibrepodsEvtFileClose;

NTSTATUS
LibrepodsCreateDevice(
    _Inout_ PWDFDEVICE_INIT DeviceInit
    );

VOID
LibrepodsSetLastError(
    _In_ WDFDEVICE Device,
    _In_ ULONG LastError
    );

PLIBREPODS_SESSION_ENTRY
LibrepodsFindSessionLocked(
    _In_ PLIBREPODS_DEVICE_CONTEXT DeviceContext,
    _In_ UINT64 SessionId
    );

VOID
LibrepodsReleaseSession(
    _In_ PLIBREPODS_SESSION_ENTRY Session
    );

// Removes the session from the list, closes its channel, and waits for any
// in-flight work items to drop their references before freeing. Used by
// CLOSE_SESSION, EvtFileClose, and device teardown — one teardown path for
// every caller so the lifetime rules stay identical.
VOID
LibrepodsDestroySession(
    _In_ WDFDEVICE Device,
    _In_ PLIBREPODS_SESSION_ENTRY Session
    );
