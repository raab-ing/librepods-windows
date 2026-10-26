/*
 * Derived from librepods-windows by ivLis-Studio
 * (https://github.com/ivLis-Studio/librepods-windows), GPLv3 — a Windows port of
 * LibrePods (https://github.com/kavishdevar/librepods).
 */
#include "driver.h"
#include "transport.h"

#define LIBREPODS_PENDING_POOL_TAG 'iLbL'

// {0850302A-B344-4FDA-9BE9-90576B8D46F0} — local radio PDO device interface.
// Defined locally: bthdef.h only declares it extern and the GUID lib is not
// always on the link line.
static const GUID LIBREPODS_GUID_BTHPORT_DEVICE_INTERFACE = {
    0x0850302a, 0xb344, 0x4fda, {0x9b, 0xe9, 0x90, 0x57, 0x6b, 0x8d, 0x46, 0xf0}
};

static
VOID
LibrepodsCompleteRequest(
    _In_ WDFREQUEST Request,
    _In_ NTSTATUS Status,
    _In_ size_t Information
    )
{
    WdfRequestCompleteWithInformation(Request, Status, Information);
}

/*
 * Session lifetime: RefCount counts live holders — the Sessions list counts
 * as one, each in-flight work item holds one more. List membership is the
 * arbiter of ownership: whoever removes the session from the list runs
 * LibrepodsDestroySession, which marks it closing (so an in-flight open
 * knows not to adopt it), closes the channel (which flushes a blocked ACL
 * read BRB back immediately instead of waiting out its timeout), drops the
 * list/creator reference, waits for stragglers, then frees.
 */

_Use_decl_annotations_
VOID
LibrepodsReleaseSession(
    PLIBREPODS_SESSION_ENTRY Session
    )
{
    if (InterlockedDecrement(&Session->RefCount) == 0) {
        KeSetEvent(&Session->ZeroRefEvent, IO_NO_INCREMENT, FALSE);
    }
}

_Use_decl_annotations_
VOID
LibrepodsDestroySession(
    WDFDEVICE Device,
    PLIBREPODS_SESSION_ENTRY Session
    )
{
    ULONG driverError = LIBREPODS_DRIVER_ERROR_NONE;
    PLIBREPODS_DEVICE_CONTEXT deviceContext = LibrepodsGetDeviceContext(Device);

    WdfWaitLockAcquire(deviceContext->SessionLock, NULL);
    Session->Closing = TRUE;
    WdfWaitLockRelease(deviceContext->SessionLock);

    // Closing the channel flushes pending ACL transfer BRBs, so a read work
    // item blocked in the Bluetooth stack unwinds promptly.
    LibrepodsTransportCloseSession(Device, Session, &driverError);

    LibrepodsReleaseSession(Session);
    KeWaitForSingleObject(
        &Session->ZeroRefEvent,
        Executive,
        KernelMode,
        FALSE,
        NULL);
    ExFreePoolWithTag(Session, LIBREPODS_SESSION_POOL_TAG);
}

/*
 * Request cancel routine (DISPATCH_LEVEL): completing the request here is
 * what releases the caller's thread — that is the whole point of pending
 * the ioctl. The work item owns all unwinding; the request handle stays
 * valid for it because the handler took a WdfObjectReference before marking
 * the request cancelable.
 */
static
VOID
LibrepodsEvtRequestCancel(
    _In_ WDFREQUEST Request
    )
{
    PLIBREPODS_REQUEST_CONTEXT requestContext = LibrepodsGetRequestContext(Request);
    InterlockedExchange(&requestContext->Pending->Cancelled, 1);
    WdfRequestComplete(Request, STATUS_CANCELLED);
}

/*
 * Pend a request onto a work item: attach the pending block as the request's
 * context (so the cancel routine can find it), pin the request, arm
 * cancellation, and queue the work.
 *
 * *RequestPending = TRUE on any outcome where the caller must not complete
 * the request: either it is queued on a work item (work item owns everything)
 * or the cancel routine already completed it (caller frees its resources and
 * returns STATUS_PENDING — touching the request again would double-complete).
 * *RequestPending = FALSE means the request never left caller ownership and
 * the dispatcher should complete it with the returned status.
 */
static
NTSTATUS
LibrepodsPendRequest(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ PLIBREPODS_PENDING_IO Pending,
    _In_ PFN_WDF_WORKITEM WorkItemFunc,
    _Out_ BOOLEAN* RequestPending
    )
{
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_WORKITEM_CONFIG workItemConfig;
    WDFWORKITEM workItem;
    PLIBREPODS_REQUEST_CONTEXT requestContext;
    NTSTATUS status;

    *RequestPending = FALSE;

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, LIBREPODS_REQUEST_CONTEXT);
    status = WdfObjectAllocateContext((WDFOBJECT)Request, &attributes, (PVOID*)&requestContext);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    requestContext->Pending = Pending;

    WdfObjectReference((WDFOBJECT)Request);

    status = WdfRequestMarkCancelableEx(Request, LibrepodsEvtRequestCancel);
    if (!NT_SUCCESS(status)) {
        WdfObjectDereference((WDFOBJECT)Request);
        return status;
    }

    Pending->Request = Request;
    Pending->Device = Device;

    WDF_WORKITEM_CONFIG_INIT(&workItemConfig, WorkItemFunc);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, LIBREPODS_WORKITEM_CONTEXT);
    attributes.ParentObject = Device;

    status = WdfWorkItemCreate(&workItemConfig, &attributes, &workItem);
    if (!NT_SUCCESS(status)) {
        NTSTATUS unmarkStatus = WdfRequestUnmarkCancelable(Request);
        WdfObjectDereference((WDFOBJECT)Request);
        if (unmarkStatus == STATUS_CANCELLED) {
            // The cancel routine already completed the request — do not let
            // the caller complete it a second time.
            *RequestPending = TRUE;
            return STATUS_CANCELLED;
        }
        return status;
    }
    LibrepodsGetWorkItemContext(workItem)->Pending = Pending;

    WdfWorkItemEnqueue(workItem);
    *RequestPending = TRUE;
    return STATUS_PENDING;
}

static
NTSTATUS
LibrepodsHandleGetBridgeStatus(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request
    )
{
    PLIBREPODS_DEVICE_CONTEXT deviceContext;
    librepods_driver_status* output;
    NTSTATUS status;

    status = WdfRequestRetrieveOutputBuffer(
        Request,
        sizeof(librepods_driver_status),
        (PVOID*)&output,
        NULL);

    if (!NT_SUCCESS(status)) {
        return status;
    }

    deviceContext = LibrepodsGetDeviceContext(Device);
    RtlZeroMemory(output, sizeof(*output));
    output->struct_size = sizeof(*output);
    output->protocol_version = LIBREPODS_DRIVER_PROTOCOL_VERSION;
    output->capability_flags = deviceContext->CapabilityFlags;
    output->max_packet_size = deviceContext->MaxPacketSize;
    output->last_error = deviceContext->LastError;
    output->reserved0 = (librepods_u32)(ULONG)deviceContext->LastNtStatus;
    output->reserved1 = 0x4C425032;

    LibrepodsCompleteRequest(Request, STATUS_SUCCESS, sizeof(*output));
    return STATUS_SUCCESS;
}

/*
 * OPEN: the session enters the Sessions list before the L2CAP connect runs
 * so EvtFileClose covers it — a client dying mid-open can never orphan a
 * channel on the pods. Cancel paths converge here: whoever removes the
 * session from the list owns its teardown.
 */
static
VOID
LibrepodsEvtOpenWorkItem(
    _In_ WDFWORKITEM WorkItem
    )
{
    WDFDEVICE device = (WDFDEVICE)WdfWorkItemGetParentObject(WorkItem);
    PLIBREPODS_DEVICE_CONTEXT deviceContext = LibrepodsGetDeviceContext(device);
    PLIBREPODS_PENDING_IO pending = LibrepodsGetWorkItemContext(WorkItem)->Pending;
    WDFREQUEST request = pending->Request;
    PLIBREPODS_SESSION_ENTRY session = pending->Session;
    librepods_driver_open_session_response* output;
    BOOLEAN inserted = FALSE;
    BOOLEAN ownsRequest;
    NTSTATUS status = STATUS_UNSUCCESSFUL;

    if (!pending->Cancelled) {
        WdfWaitLockAcquire(deviceContext->SessionLock, NULL);
        session->SessionId = deviceContext->NextSessionId++;
        InterlockedIncrement(&session->RefCount); // list reference
        InsertTailList(&deviceContext->Sessions, &session->ListEntry);
        WdfWaitLockRelease(deviceContext->SessionLock);
        inserted = TRUE;

        status = LibrepodsTransportOpenSession(
            device,
            session,
            pending->Psm,
            &session->CapabilityFlags,
            &session->Mtu,
            &deviceContext->LastError);
    }

    // Cancel routine already ran -> it completed the request; just unwind.
    ownsRequest = (WdfRequestUnmarkCancelable(request) != STATUS_CANCELLED) &&
                  !pending->Cancelled;

    if (ownsRequest) {
        BOOLEAN adopted;

        WdfWaitLockAcquire(deviceContext->SessionLock, NULL);
        adopted = !session->Closing;
        WdfWaitLockRelease(deviceContext->SessionLock);

        if (NT_SUCCESS(status) && adopted) {
            status = WdfRequestRetrieveOutputBuffer(
                request,
                sizeof(librepods_driver_open_session_response),
                (PVOID*)&output,
                NULL);
            if (NT_SUCCESS(status)) {
                RtlZeroMemory(output, sizeof(*output));
                output->struct_size = sizeof(*output);
                output->capability_flags = session->CapabilityFlags;
                output->mtu = session->Mtu;
                output->session_id = session->SessionId;
            }
        }

        if (NT_SUCCESS(status) && adopted) {
            // Open adopted: the session lives on its list reference; drop the
            // creator ref and complete — no teardown.
            LibrepodsCompleteRequest(request, STATUS_SUCCESS, sizeof(*output));
            LibrepodsReleaseSession(session);
            goto done;
        }

        LibrepodsCompleteRequest(
            request,
            NT_SUCCESS(status) ? STATUS_CANCELLED : status,
            0);
    }

    // Unwind whatever state exists. List membership decides who tears down:
    // if a concurrent DestroySession already claimed it, our only remaining
    // job is the channel-close (if the open succeeded) plus our creator ref;
    // otherwise removing it ourselves makes us the destroyer.
    if (inserted) {
        BOOLEAN closing;

        WdfWaitLockAcquire(deviceContext->SessionLock, NULL);
        closing = session->Closing;
        if (!closing) {
            RemoveEntryList(&session->ListEntry);
        }
        WdfWaitLockRelease(deviceContext->SessionLock);

        // A successful open left a live channel — close it before letting go.
        if (NT_SUCCESS(status)) {
            ULONG ignored = LIBREPODS_DRIVER_ERROR_NONE;
            LibrepodsTransportCloseSession(device, session, &ignored);
        }

        if (closing) {
            // DestroySession dropped the list ref and waits on ZeroRefEvent;
            // release only our creator reference.
            LibrepodsReleaseSession(session);
        } else {
            LibrepodsReleaseSession(session); // list ref
            LibrepodsReleaseSession(session); // creator ref -> zero
            ExFreePoolWithTag(session, LIBREPODS_SESSION_POOL_TAG);
        }
    } else {
        ExFreePoolWithTag(session, LIBREPODS_SESSION_POOL_TAG);
    }

done:
    WdfObjectDereference((WDFOBJECT)request);
    ExFreePoolWithTag(pending, LIBREPODS_PENDING_POOL_TAG);
}

static
NTSTATUS
LibrepodsHandleOpenSession(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request
    )
{
    librepods_driver_open_session_request* input;
    PLIBREPODS_PENDING_IO pending;
    PLIBREPODS_SESSION_ENTRY session;
    size_t inputLength;
    USHORT psm;
    NTSTATUS status;

    // Minimum is the v1 layout (everything before 'psm'): v1 clients send a
    // struct whose size lands exactly there and get the default AACP PSM.
    status = WdfRequestRetrieveInputBuffer(
        Request,
        FIELD_OFFSET(librepods_driver_open_session_request, psm),
        (PVOID*)&input,
        &inputLength);

    if (!NT_SUCCESS(status)) {
        return status;
    }

    if (input->struct_size < (librepods_u32)FIELD_OFFSET(librepods_driver_open_session_request, psm) ||
        input->bluetooth_address[0] == '\0') {
        LibrepodsSetLastError(Device, LIBREPODS_DRIVER_ERROR_INVALID_ARGUMENT);
        return STATUS_INVALID_PARAMETER;
    }

    psm = 0;
    if (input->struct_size >= sizeof(*input) &&
        inputLength >= sizeof(*input)) {
        if (input->psm > 0xFFFF) {
            LibrepodsSetLastError(Device, LIBREPODS_DRIVER_ERROR_INVALID_ARGUMENT);
            return STATUS_INVALID_PARAMETER;
        }
        psm = (USHORT)input->psm;
    }

    pending = (PLIBREPODS_PENDING_IO)ExAllocatePoolZero(
        NonPagedPoolNx,
        sizeof(LIBREPODS_PENDING_IO),
        LIBREPODS_PENDING_POOL_TAG);
    if (pending == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    session = (PLIBREPODS_SESSION_ENTRY)ExAllocatePoolZero(
        NonPagedPoolNx,
        sizeof(LIBREPODS_SESSION_ENTRY),
        LIBREPODS_SESSION_POOL_TAG);
    if (session == NULL) {
        ExFreePoolWithTag(pending, LIBREPODS_PENDING_POOL_TAG);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    session->RefCount = 1; // creator reference — dropped by the work item
    KeInitializeEvent(&session->ZeroRefEvent, NotificationEvent, FALSE);
    session->OwningFile = WdfRequestGetFileObject(Request);
    RtlStringCbCopyA(
        session->BluetoothAddress,
        sizeof(session->BluetoothAddress),
        input->bluetooth_address);

    pending->Psm = psm;
    pending->Session = session;

    {
        BOOLEAN requestPending = FALSE;

        status = LibrepodsPendRequest(
            Device,
            Request,
            pending,
            LibrepodsEvtOpenWorkItem,
            &requestPending);
        if (requestPending) {
            // Queued (work item owns resources) or already completed by the
            // cancel routine — in the latter case the work item never ran,
            // so release our allocations here.
            if (status != STATUS_PENDING) {
                ExFreePoolWithTag(session, LIBREPODS_SESSION_POOL_TAG);
                ExFreePoolWithTag(pending, LIBREPODS_PENDING_POOL_TAG);
            }
            return STATUS_PENDING;
        }
    }

    ExFreePoolWithTag(session, LIBREPODS_SESSION_POOL_TAG);
    ExFreePoolWithTag(pending, LIBREPODS_PENDING_POOL_TAG);
    return status;
}

static
NTSTATUS
LibrepodsHandleCloseSession(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request
    )
{
    PLIBREPODS_DEVICE_CONTEXT deviceContext;
    librepods_driver_close_session_request* input;
    PLIBREPODS_SESSION_ENTRY session;
    NTSTATUS status;

    status = WdfRequestRetrieveInputBuffer(
        Request,
        sizeof(librepods_driver_close_session_request),
        (PVOID*)&input,
        NULL);

    if (!NT_SUCCESS(status)) {
        return status;
    }

    if (input->struct_size < sizeof(*input) || input->session_id == 0) {
        LibrepodsSetLastError(Device, LIBREPODS_DRIVER_ERROR_INVALID_ARGUMENT);
        return STATUS_INVALID_PARAMETER;
    }

    deviceContext = LibrepodsGetDeviceContext(Device);

    WdfWaitLockAcquire(deviceContext->SessionLock, NULL);
    session = LibrepodsFindSessionLocked(deviceContext, input->session_id);
    if (session != NULL && !session->Closing) {
        RemoveEntryList(&session->ListEntry);
    } else {
        session = NULL;
    }
    WdfWaitLockRelease(deviceContext->SessionLock);

    if (session == NULL) {
        LibrepodsSetLastError(Device, LIBREPODS_DRIVER_ERROR_INVALID_ARGUMENT);
        return STATUS_NOT_FOUND;
    }

    LibrepodsDestroySession(Device, session);
    LibrepodsCompleteRequest(Request, STATUS_SUCCESS, 0);
    return STATUS_SUCCESS;
}

static
NTSTATUS
LibrepodsHandleSendPacket(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request
    )
{
    PLIBREPODS_DEVICE_CONTEXT deviceContext;
    librepods_driver_send_packet_request* input;
    PLIBREPODS_SESSION_ENTRY session;
    size_t inputLength;
    size_t requiredLength;
    NTSTATUS status;

    status = WdfRequestRetrieveInputBuffer(
        Request,
        FIELD_OFFSET(librepods_driver_send_packet_request, packet),
        (PVOID*)&input,
        &inputLength);

    if (!NT_SUCCESS(status)) {
        return status;
    }

    if (input->struct_size < sizeof(*input) ||
        input->session_id == 0 ||
        input->packet_length == 0 ||
        input->packet_length > LIBREPODS_DRIVER_MAX_AACP_PACKET_LENGTH) {
        LibrepodsSetLastError(Device, LIBREPODS_DRIVER_ERROR_INVALID_ARGUMENT);
        return STATUS_INVALID_PARAMETER;
    }

    requiredLength = FIELD_OFFSET(librepods_driver_send_packet_request, packet) + input->packet_length;
    if (inputLength < requiredLength) {
        LibrepodsSetLastError(Device, LIBREPODS_DRIVER_ERROR_INVALID_ARGUMENT);
        return STATUS_BUFFER_TOO_SMALL;
    }

    deviceContext = LibrepodsGetDeviceContext(Device);

    WdfWaitLockAcquire(deviceContext->SessionLock, NULL);
    session = LibrepodsFindSessionLocked(deviceContext, input->session_id);
    if (session != NULL && !session->Closing) {
        InterlockedIncrement(&session->RefCount);
    } else {
        session = NULL;
    }
    WdfWaitLockRelease(deviceContext->SessionLock);

    if (session == NULL) {
        LibrepodsSetLastError(Device, LIBREPODS_DRIVER_ERROR_INVALID_ARGUMENT);
        return STATUS_NOT_FOUND;
    }

    status = LibrepodsTransportSendPacket(
        Device,
        session,
        input->packet,
        (ULONG)input->packet_length,
        &deviceContext->LastError);

    LibrepodsReleaseSession(session);

    if (NT_SUCCESS(status)) {
        LibrepodsCompleteRequest(Request, STATUS_SUCCESS, 0);
    }

    return status;
}

/*
 * READ: pend the request and let a work item run the blocking ACL receive,
 * so the caller's thread stays interruptible. At most one pending read per
 * session — concurrent reads on one channel would race the byte stream.
 */
static
VOID
LibrepodsEvtReadWorkItem(
    _In_ WDFWORKITEM WorkItem
    )
{
    WDFDEVICE device = (WDFDEVICE)WdfWorkItemGetParentObject(WorkItem);
    PLIBREPODS_DEVICE_CONTEXT deviceContext = LibrepodsGetDeviceContext(device);
    PLIBREPODS_PENDING_IO pending = LibrepodsGetWorkItemContext(WorkItem)->Pending;
    WDFREQUEST request = pending->Request;
    PLIBREPODS_SESSION_ENTRY session = pending->Session;
    librepods_driver_read_packet_response* output = NULL;
    ULONG packetLength = 0;
    NTSTATUS status = STATUS_UNSUCCESSFUL;

    if (!pending->Cancelled) {
        status = WdfRequestRetrieveOutputBuffer(
            request,
            sizeof(librepods_driver_read_packet_response),
            (PVOID*)&output,
            NULL);
        if (NT_SUCCESS(status)) {
            RtlZeroMemory(output, sizeof(*output));
            output->struct_size = sizeof(*output);
            status = LibrepodsTransportReadPacket(
                device,
                session,
                output->packet,
                pending->BufferLength,
                &packetLength,
                &deviceContext->LastError);
        }
    }

    if (WdfRequestUnmarkCancelable(request) != STATUS_CANCELLED && !pending->Cancelled) {
        if (NT_SUCCESS(status)) {
            output->packet_length = packetLength;
            LibrepodsCompleteRequest(
                request,
                STATUS_SUCCESS,
                FIELD_OFFSET(librepods_driver_read_packet_response, packet) + packetLength);
        } else {
            LibrepodsCompleteRequest(request, status, 0);
        }
    }
    // else: the cancel routine already completed the request.

    WdfWaitLockAcquire(deviceContext->SessionLock, NULL);
    session->ReadPending = FALSE;
    WdfWaitLockRelease(deviceContext->SessionLock);

    LibrepodsReleaseSession(session);
    WdfObjectDereference((WDFOBJECT)request);
    ExFreePoolWithTag(pending, LIBREPODS_PENDING_POOL_TAG);
}

static
NTSTATUS
LibrepodsHandleReadPacket(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request
    )
{
    PLIBREPODS_DEVICE_CONTEXT deviceContext;
    librepods_driver_read_packet_request* input;
    PLIBREPODS_PENDING_IO pending;
    PLIBREPODS_SESSION_ENTRY session;
    NTSTATUS status;

    status = WdfRequestRetrieveInputBuffer(
        Request,
        sizeof(librepods_driver_read_packet_request),
        (PVOID*)&input,
        NULL);

    if (!NT_SUCCESS(status)) {
        return status;
    }

    if (input->struct_size < sizeof(*input) ||
        input->session_id == 0 ||
        input->packet_buffer_length == 0 ||
        input->packet_buffer_length > LIBREPODS_DRIVER_MAX_AACP_PACKET_LENGTH) {
        LibrepodsSetLastError(Device, LIBREPODS_DRIVER_ERROR_INVALID_ARGUMENT);
        return STATUS_INVALID_PARAMETER;
    }

    deviceContext = LibrepodsGetDeviceContext(Device);

    WdfWaitLockAcquire(deviceContext->SessionLock, NULL);
    session = LibrepodsFindSessionLocked(deviceContext, input->session_id);
    if (session != NULL && !session->Closing && !session->ReadPending) {
        session->ReadPending = TRUE;
        InterlockedIncrement(&session->RefCount);
    } else {
        session = NULL;
    }
    WdfWaitLockRelease(deviceContext->SessionLock);

    if (session == NULL) {
        LibrepodsSetLastError(Device, LIBREPODS_DRIVER_ERROR_INVALID_ARGUMENT);
        return STATUS_NOT_FOUND;
    }

    pending = (PLIBREPODS_PENDING_IO)ExAllocatePoolZero(
        NonPagedPoolNx,
        sizeof(LIBREPODS_PENDING_IO),
        LIBREPODS_PENDING_POOL_TAG);
    if (pending == NULL) {
        WdfWaitLockAcquire(deviceContext->SessionLock, NULL);
        session->ReadPending = FALSE;
        WdfWaitLockRelease(deviceContext->SessionLock);
        LibrepodsReleaseSession(session);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    pending->Session = session;
    pending->BufferLength = (ULONG)input->packet_buffer_length;

    {
        BOOLEAN requestPending = FALSE;

        status = LibrepodsPendRequest(
            Device,
            Request,
            pending,
            LibrepodsEvtReadWorkItem,
            &requestPending);
        if (requestPending) {
            if (status != STATUS_PENDING) {
                // Cancel routine completed the request before the work item
                // existed — unwind the session reference ourselves.
                WdfWaitLockAcquire(deviceContext->SessionLock, NULL);
                session->ReadPending = FALSE;
                WdfWaitLockRelease(deviceContext->SessionLock);
                LibrepodsReleaseSession(session);
                ExFreePoolWithTag(pending, LIBREPODS_PENDING_POOL_TAG);
            }
            return STATUS_PENDING;
        }
    }

    WdfWaitLockAcquire(deviceContext->SessionLock, NULL);
    session->ReadPending = FALSE;
    WdfWaitLockRelease(deviceContext->SessionLock);
    LibrepodsReleaseSession(session);
    ExFreePoolWithTag(pending, LIBREPODS_PENDING_POOL_TAG);
    return status;
}

/*
 * Lazily opens a remote I/O target on the local Bluetooth radio PDO
 * (GUID_BTHPORT_DEVICE_INTERFACE). The bthport SDP server honors
 * IOCTL_BTH_SDP_SUBMIT/REMOVE only from kernel-mode callers; from user
 * mode the same ioctls fail with ERROR_NOT_SUPPORTED. Opening the radio
 * device by its interface symbolic link gives us a target those requests
 * can be forwarded to.
 */
static
NTSTATUS
LibrepodsEnsureRadioTarget(
    _In_ WDFDEVICE Device
    )
{
    PLIBREPODS_DEVICE_CONTEXT deviceContext = LibrepodsGetDeviceContext(Device);
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_IO_TARGET_OPEN_PARAMS openParams;
    PZZWSTR interfaceList = NULL;
    UNICODE_STRING interfaceName;
    WDFIOTARGET target = NULL;
    NTSTATUS status;

    WdfWaitLockAcquire(deviceContext->RadioTargetLock, NULL);
    if (deviceContext->RadioTarget != NULL) {
        WdfWaitLockRelease(deviceContext->RadioTargetLock);
        return STATUS_SUCCESS;
    }

    status = IoGetDeviceInterfaces(
        &LIBREPODS_GUID_BTHPORT_DEVICE_INTERFACE,
        NULL,
        0,
        &interfaceList);
    if (NT_SUCCESS(status) && (interfaceList == NULL || interfaceList[0] == L'\0')) {
        status = STATUS_NOT_FOUND;
    }

    if (NT_SUCCESS(status)) {
        RtlInitUnicodeString(&interfaceName, interfaceList);
        WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
        attributes.ParentObject = Device;
        status = WdfIoTargetCreate(Device, &attributes, &target);
    }
    if (NT_SUCCESS(status)) {
        WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_NAME(
            &openParams,
            &interfaceName,
            STANDARD_RIGHTS_ALL);
        status = WdfIoTargetOpen(target, &openParams);
        if (NT_SUCCESS(status)) {
            deviceContext->RadioTarget = target;
        } else {
            WdfObjectDelete(target);
        }
    }
    if (interfaceList != NULL) {
        ExFreePool(interfaceList);
    }

    WdfWaitLockRelease(deviceContext->RadioTargetLock);
    return status;
}

/*
 * Publishes a raw SDP attribute stream into the local SDP server as an
 * Apple-vendor Device ID record (or whatever the caller supplies). The
 * returned HANDLE_SDP can be passed to REMOVE_SDP_RECORD to unpublish.
 */
static
NTSTATUS
LibrepodsHandleSubmitSdpRecord(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request
    )
{
    PLIBREPODS_DEVICE_CONTEXT deviceContext = LibrepodsGetDeviceContext(Device);
    librepods_driver_sdp_submit_request *submitRequest;
    librepods_driver_sdp_submit_response submitResponse;
    WDF_MEMORY_DESCRIPTOR inputDescriptor;
    WDF_MEMORY_DESCRIPTOR outputDescriptor;
    PBTH_SDP_RECORD recordInfo = NULL;
    HANDLE_SDP sdpHandle = 0;
    size_t inputLength;
    size_t recordInfoLength;
    ULONG driverError = LIBREPODS_DRIVER_ERROR_NONE;
    NTSTATUS status;

    status = WdfRequestRetrieveInputBuffer(
        Request,
        FIELD_OFFSET(librepods_driver_sdp_submit_request, record),
        (PVOID*)&submitRequest,
        &inputLength);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    if (submitRequest->struct_size < (librepods_u32)FIELD_OFFSET(librepods_driver_sdp_submit_request, record) ||
        submitRequest->record_length == 0 ||
        submitRequest->record_length > LIBREPODS_DRIVER_MAX_SDP_RECORD_LENGTH ||
        (size_t)submitRequest->record_length > inputLength - (size_t)FIELD_OFFSET(librepods_driver_sdp_submit_request, record)) {
        driverError = LIBREPODS_DRIVER_ERROR_INVALID_ARGUMENT;
        status = STATUS_INVALID_PARAMETER;
        goto Done;
    }

    recordInfoLength = FIELD_OFFSET(BTH_SDP_RECORD, record) + submitRequest->record_length;
    recordInfo = ExAllocatePool2(POOL_FLAG_NON_PAGED, recordInfoLength, LIBREPODS_PENDING_POOL_TAG);
    if (recordInfo == NULL) {
        driverError = LIBREPODS_DRIVER_ERROR_INTERNAL;
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto Done;
    }
    recordInfo->fSecurity = SERVICE_SECURITY_USE_DEFAULTS;
    recordInfo->fOptions = 0;
    recordInfo->fCodService = 0;
    recordInfo->recordLength = submitRequest->record_length;
    RtlCopyMemory(recordInfo->record, submitRequest->record, submitRequest->record_length);

    status = LibrepodsEnsureRadioTarget(Device);
    if (!NT_SUCCESS(status)) {
        driverError = LIBREPODS_DRIVER_ERROR_DRIVER_UNAVAILABLE;
        goto Done;
    }

    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&inputDescriptor, recordInfo, (ULONG)recordInfoLength);
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&outputDescriptor, &sdpHandle, sizeof(sdpHandle));
    status = WdfIoTargetSendIoctlSynchronously(
        deviceContext->RadioTarget,
        WDF_NO_HANDLE,
        IOCTL_BTH_SDP_SUBMIT_RECORD_WITH_INFO,
        &inputDescriptor,
        &outputDescriptor,
        NULL,
        NULL);
    if (!NT_SUCCESS(status)) {
        // WITH_INFO may be retired on newer stacks; the legacy form takes
        // the raw attribute stream with no info header.
        NTSTATUS legacyStatus;

        sdpHandle = 0;
        WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(
            &inputDescriptor,
            (PVOID)submitRequest->record,
            submitRequest->record_length);
        legacyStatus = WdfIoTargetSendIoctlSynchronously(
            deviceContext->RadioTarget,
            WDF_NO_HANDLE,
            IOCTL_BTH_SDP_SUBMIT_RECORD,
            &inputDescriptor,
            &outputDescriptor,
            NULL,
            NULL);
        if (!NT_SUCCESS(legacyStatus)) {
            // Keep both statuses visible: last_error=INTERNAL marks the
            // submit path, LastNtStatus holds the radio's real verdict.
            deviceContext->LastNtStatus = legacyStatus;
            driverError = LIBREPODS_DRIVER_ERROR_INTERNAL;
            status = legacyStatus;
            goto Done;
        }
        status = STATUS_SUCCESS;
    }

    {
        PVOID outputBuffer;

        status = WdfRequestRetrieveOutputBuffer(
            Request,
            sizeof(submitResponse),
            &outputBuffer,
            NULL);
        if (!NT_SUCCESS(status)) {
            driverError = LIBREPODS_DRIVER_ERROR_INVALID_ARGUMENT;
            goto Done;
        }
        RtlZeroMemory(&submitResponse, sizeof(submitResponse));
        submitResponse.struct_size = sizeof(submitResponse);
        submitResponse.sdp_handle = (librepods_u64)sdpHandle;
        RtlCopyMemory(outputBuffer, &submitResponse, sizeof(submitResponse));
        ExFreePoolWithTag(recordInfo, LIBREPODS_PENDING_POOL_TAG);
        LibrepodsSetLastError(Device, LIBREPODS_DRIVER_ERROR_NONE);
        LibrepodsCompleteRequest(Request, STATUS_SUCCESS, sizeof(submitResponse));
        return STATUS_SUCCESS;
    }

Done:
    if (recordInfo != NULL) {
        ExFreePoolWithTag(recordInfo, LIBREPODS_PENDING_POOL_TAG);
    }
    deviceContext->LastNtStatus = status;
    LibrepodsSetLastError(Device, driverError);
    return status;
}

static
NTSTATUS
LibrepodsHandleRemoveSdpRecord(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request
    )
{
    PLIBREPODS_DEVICE_CONTEXT deviceContext = LibrepodsGetDeviceContext(Device);
    librepods_driver_sdp_remove_request *removeRequest;
    WDF_MEMORY_DESCRIPTOR inputDescriptor;
    HANDLE_SDP sdpHandle;
    NTSTATUS status;

    status = WdfRequestRetrieveInputBuffer(
        Request,
        sizeof(librepods_driver_sdp_remove_request),
        (PVOID*)&removeRequest,
        NULL);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    if (removeRequest->struct_size < sizeof(librepods_driver_sdp_remove_request)) {
        LibrepodsSetLastError(Device, LIBREPODS_DRIVER_ERROR_INVALID_ARGUMENT);
        return STATUS_INVALID_PARAMETER;
    }

    status = LibrepodsEnsureRadioTarget(Device);
    if (!NT_SUCCESS(status)) {
        LibrepodsSetLastError(Device, LIBREPODS_DRIVER_ERROR_DRIVER_UNAVAILABLE);
        return status;
    }

    sdpHandle = (HANDLE_SDP)removeRequest->sdp_handle;
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&inputDescriptor, &sdpHandle, sizeof(sdpHandle));
    status = WdfIoTargetSendIoctlSynchronously(
        deviceContext->RadioTarget,
        WDF_NO_HANDLE,
        IOCTL_BTH_SDP_REMOVE_RECORD,
        &inputDescriptor,
        NULL,
        NULL,
        NULL);
    LibrepodsSetLastError(
        Device,
        NT_SUCCESS(status) ? LIBREPODS_DRIVER_ERROR_NONE : LIBREPODS_DRIVER_ERROR_INTERNAL);
    return status;
}

_Use_decl_annotations_
VOID
LibrepodsEvtIoDeviceControl(
    WDFQUEUE Queue,
    WDFREQUEST Request,
    size_t OutputBufferLength,
    size_t InputBufferLength,
    ULONG IoControlCode
    )
{
    WDFDEVICE device;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    device = WdfIoQueueGetDevice(Queue);
    status = STATUS_INVALID_DEVICE_REQUEST;

    switch (IoControlCode) {
        case LIBREPODS_IOCTL_GET_BRIDGE_STATUS:
            status = LibrepodsHandleGetBridgeStatus(device, Request);
            break;

        case LIBREPODS_IOCTL_OPEN_AACP_CHANNEL:
            status = LibrepodsHandleOpenSession(device, Request);
            break;

        case LIBREPODS_IOCTL_CLOSE_AACP_CHANNEL:
            status = LibrepodsHandleCloseSession(device, Request);
            break;

        case LIBREPODS_IOCTL_SEND_AACP_PACKET:
            status = LibrepodsHandleSendPacket(device, Request);
            break;

        case LIBREPODS_IOCTL_READ_AACP_PACKET:
            status = LibrepodsHandleReadPacket(device, Request);
            break;

        case LIBREPODS_IOCTL_SUBMIT_SDP_RECORD:
            status = LibrepodsHandleSubmitSdpRecord(device, Request);
            break;

        case LIBREPODS_IOCTL_REMOVE_SDP_RECORD:
            status = LibrepodsHandleRemoveSdpRecord(device, Request);
            break;

        default:
            LibrepodsSetLastError(device, LIBREPODS_DRIVER_ERROR_INVALID_ARGUMENT);
            status = STATUS_INVALID_DEVICE_REQUEST;
            break;
    }

    if (status == STATUS_PENDING) {
        return;
    }

    if (status != STATUS_SUCCESS) {
        LibrepodsCompleteRequest(Request, status, 0);
    }
}
