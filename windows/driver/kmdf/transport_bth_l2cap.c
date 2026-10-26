/*
 * Derived from librepods-windows by ivLis-Studio
 * (https://github.com/ivLis-Studio/librepods-windows), GPLv3 — a Windows port of
 * LibrePods (https://github.com/kavishdevar/librepods).
 */
#include "transport.h"

#include <bthioctl.h>

#define LIBREPODS_AACP_PSM ((USHORT)LIBREPODS_PSM_AACP)

static
BOOLEAN
LibrepodsTryParseHexNibble(
    _In_ CHAR Character,
    _Out_ UCHAR* Value
    )
{
    if (Character >= '0' && Character <= '9') {
        *Value = (UCHAR)(Character - '0');
        return TRUE;
    }

    if (Character >= 'a' && Character <= 'f') {
        *Value = (UCHAR)(10 + (Character - 'a'));
        return TRUE;
    }

    if (Character >= 'A' && Character <= 'F') {
        *Value = (UCHAR)(10 + (Character - 'A'));
        return TRUE;
    }

    return FALSE;
}

static
NTSTATUS
LibrepodsParseBluetoothAddress(
    _In_z_ const CHAR* BluetoothAddress,
    _Out_ BTH_ADDR* RemoteAddress
    )
{
    UCHAR octets[6];
    ULONG index;

    if (BluetoothAddress == NULL || RemoteAddress == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    if (RtlStringCbLengthA(
            BluetoothAddress,
            LIBREPODS_DRIVER_MAX_BLUETOOTH_ADDRESS_LENGTH,
            NULL) != STATUS_SUCCESS) {
        return STATUS_INVALID_PARAMETER;
    }

    for (index = 0; index < RTL_NUMBER_OF(octets); index++) {
        ULONG offset;
        UCHAR highNibble;
        UCHAR lowNibble;

        offset = index * 3;
        if (!LibrepodsTryParseHexNibble(BluetoothAddress[offset], &highNibble) ||
            !LibrepodsTryParseHexNibble(BluetoothAddress[offset + 1], &lowNibble)) {
            return STATUS_INVALID_PARAMETER;
        }

        if (index < (RTL_NUMBER_OF(octets) - 1) && BluetoothAddress[offset + 2] != ':') {
            return STATUS_INVALID_PARAMETER;
        }

        octets[index] = (UCHAR)((highNibble << 4) | lowNibble);
    }

    if (BluetoothAddress[17] != '\0') {
        return STATUS_INVALID_PARAMETER;
    }

    *RemoteAddress =
        ((BTH_ADDR)octets[0] << 40) |
        ((BTH_ADDR)octets[1] << 32) |
        ((BTH_ADDR)octets[2] << 24) |
        ((BTH_ADDR)octets[3] << 16) |
        ((BTH_ADDR)octets[4] << 8) |
        ((BTH_ADDR)octets[5]);
    return STATUS_SUCCESS;
}

static
ULONG
LibrepodsMapNtStatusToDriverError(
    _In_ NTSTATUS Status
    )
{
    switch (Status) {
        case STATUS_SUCCESS:
            return LIBREPODS_DRIVER_ERROR_NONE;

        case STATUS_NOT_IMPLEMENTED:
        case STATUS_NOT_SUPPORTED:
        case STATUS_INVALID_DEVICE_REQUEST:
            return LIBREPODS_DRIVER_ERROR_NOT_IMPLEMENTED;

        case STATUS_ACCESS_DENIED:
            return LIBREPODS_DRIVER_ERROR_ACCESS_DENIED;

        case STATUS_INVALID_PARAMETER:
        case STATUS_INVALID_BUFFER_SIZE:
        case STATUS_BUFFER_TOO_SMALL:
            return LIBREPODS_DRIVER_ERROR_INVALID_ARGUMENT;

        case STATUS_DEVICE_NOT_CONNECTED:
        case STATUS_DEVICE_NOT_READY:
        case STATUS_INVALID_DEVICE_STATE:
        case STATUS_PROTOCOL_UNREACHABLE:
        case STATUS_CONNECTION_DISCONNECTED:
        case STATUS_CONNECTION_RESET:
        case STATUS_CONNECTION_REFUSED:
        case STATUS_TIMEOUT:
        case STATUS_IO_TIMEOUT:
            return LIBREPODS_DRIVER_ERROR_TRANSPORT_BLOCKED;

        default:
            return LIBREPODS_DRIVER_ERROR_INTERNAL;
    }
}

static
NTSTATUS
LibrepodsSubmitBrbSynchronously(
    _In_ WDFDEVICE Device,
    _Inout_ PBRB Brb
    )
{
    WDFIOTARGET ioTarget;
    WDF_MEMORY_DESCRIPTOR otherArg1;
    WDF_REQUEST_SEND_OPTIONS sendOptions;
    NTSTATUS status;
    ULONG_PTR bytesReturned;

    ioTarget = WdfDeviceGetIoTarget(Device);
    if (ioTarget == WDF_NO_HANDLE) {
        return STATUS_INVALID_DEVICE_STATE;
    }

    // Bound every synchronous BRB: without a timeout an unresponsive peer
    // (e.g. pods asleep in the case) leaves the caller's ioctl — and the
    // userspace thread waiting on it — blocked in the kernel forever, which
    // wedges the owning process so hard it can't even be terminated.
    WDF_REQUEST_SEND_OPTIONS_INIT(&sendOptions, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&sendOptions, WDF_REL_TIMEOUT_IN_SEC(15));

    bytesReturned = 0;
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&otherArg1, Brb, Brb->BrbHeader.Length);

    status = WdfIoTargetSendInternalIoctlOthersSynchronously(
        ioTarget,
        WDF_NO_HANDLE,
        IOCTL_INTERNAL_BTH_SUBMIT_BRB,
        &otherArg1,
        NULL,
        NULL,
        &sendOptions,
        &bytesReturned);

    if (NT_SUCCESS(status) && !NT_SUCCESS(Brb->BrbHeader.Status)) {
        status = Brb->BrbHeader.Status;
    }

    if (NT_SUCCESS(status) && BTH_ERROR(Brb->BrbHeader.BtStatus)) {
        status = STATUS_UNSUCCESSFUL;
    }

    if (!NT_SUCCESS(status)) {
        LibrepodsGetDeviceContext(Device)->LastNtStatus = status;
    }

    return status;
}

static
VOID
LibrepodsPopulateDefaultOpenConfig(
    _Out_ struct _BRB_L2CA_OPEN_CHANNEL* OpenChannelBrb
    )
{
    // CF_LINK_NOTHING let our channels ride the ACL unauthenticated — the
    // pods answered AACP anyway but silently ignored the ATT (PSM 31)
    // listener's connect requests. Requiring an authenticated+encrypted
    // link matches what BlueZ/Apple hosts negotiate and arms the listener.
    OpenChannelBrb->ChannelFlags = CF_ROLE_EITHER | CF_LINK_AUTHENTICATED | CF_LINK_ENCRYPTED;

    OpenChannelBrb->ConfigOut.Flags = CFG_MTU;
    OpenChannelBrb->ConfigOut.Mtu.Min = L2CAP_MIN_MTU;
    OpenChannelBrb->ConfigOut.Mtu.Preferred = LIBREPODS_DRIVER_MAX_AACP_PACKET_LENGTH;
    OpenChannelBrb->ConfigOut.Mtu.Max = LIBREPODS_DRIVER_MAX_AACP_PACKET_LENGTH;

    OpenChannelBrb->ConfigIn.Flags = CFG_MTU;
    OpenChannelBrb->ConfigIn.Mtu.Min = L2CAP_MIN_MTU;
    OpenChannelBrb->ConfigIn.Mtu.Preferred = LIBREPODS_DRIVER_MAX_AACP_PACKET_LENGTH;
    OpenChannelBrb->ConfigIn.Mtu.Max = LIBREPODS_DRIVER_MAX_AACP_PACKET_LENGTH;
}

static
BOOLEAN
LibrepodsHasUsableProfileInterface(
    _In_ PLIBREPODS_DEVICE_CONTEXT DeviceContext
    )
{
    return DeviceContext->HasProfileDriverInterface &&
        DeviceContext->HasRemoteDeviceInfo &&
        DeviceContext->ProfileDriverInterface.BthInitializeBrb != NULL;
}

_Use_decl_annotations_
NTSTATUS
LibrepodsTransportQueryStatus(
    WDFDEVICE Device,
    ULONG* CapabilityFlags,
    ULONG* DriverError
    )
{
    PLIBREPODS_DEVICE_CONTEXT deviceContext;

    deviceContext = LibrepodsGetDeviceContext(Device);
    if (LibrepodsHasUsableProfileInterface(deviceContext)) {
        *CapabilityFlags =
            LIBREPODS_DRIVER_CAPABILITY_DRIVER_BACKED_L2CAP |
            LIBREPODS_DRIVER_CAPABILITY_AACP_CONTROL |
            LIBREPODS_DRIVER_CAPABILITY_BATTERY_NOTIFICATIONS |
            LIBREPODS_DRIVER_CAPABILITY_CANCELABLE_IO;
        *DriverError = LIBREPODS_DRIVER_ERROR_NONE;
        return STATUS_SUCCESS;
    }

    *CapabilityFlags = LIBREPODS_DRIVER_CAPABILITY_NONE;
    *DriverError = LIBREPODS_DRIVER_ERROR_TRANSPORT_BLOCKED;
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS
LibrepodsTransportOpenSession(
    WDFDEVICE Device,
    PLIBREPODS_SESSION_ENTRY Session,
    USHORT Psm,
    ULONG* CapabilityFlags,
    ULONG* Mtu,
    ULONG* DriverError
    )
{
    PLIBREPODS_DEVICE_CONTEXT deviceContext;
    BRB brb;
    NTSTATUS status;

    deviceContext = LibrepodsGetDeviceContext(Device);
    if (!LibrepodsHasUsableProfileInterface(deviceContext)) {
        *Mtu = 0;
        *CapabilityFlags = LIBREPODS_DRIVER_CAPABILITY_NONE;
        *DriverError = LIBREPODS_DRIVER_ERROR_TRANSPORT_BLOCKED;
        return STATUS_INVALID_DEVICE_STATE;
    }

    if (deviceContext->HasRemoteDeviceInfo) {
        Session->RemoteAddress = deviceContext->RemoteDeviceInfo.address;
    } else {
        status = LibrepodsParseBluetoothAddress(Session->BluetoothAddress, &Session->RemoteAddress);
        if (!NT_SUCCESS(status)) {
            *Mtu = 0;
            *CapabilityFlags = LIBREPODS_DRIVER_CAPABILITY_NONE;
            *DriverError = LIBREPODS_DRIVER_ERROR_INVALID_ARGUMENT;
            return status;
        }
    }

    RtlZeroMemory(&brb, sizeof(brb));
    deviceContext->ProfileDriverInterface.BthInitializeBrb(&brb, BRB_L2CA_OPEN_CHANNEL);

    brb.BrbL2caOpenChannel.ChannelHandle = NULL;
    brb.BrbL2caOpenChannel.Psm = Psm != 0 ? Psm : LIBREPODS_AACP_PSM;
    brb.BrbL2caOpenChannel.BtAddress = Session->RemoteAddress;
    LibrepodsPopulateDefaultOpenConfig(&brb.BrbL2caOpenChannel);

    status = LibrepodsSubmitBrbSynchronously(Device, &brb);
    if (!NT_SUCCESS(status)) {
        Session->ChannelHandle = NULL;
        *Mtu = 0;
        *CapabilityFlags = LIBREPODS_DRIVER_CAPABILITY_NONE;
        *DriverError = LibrepodsMapNtStatusToDriverError(status);
        return status;
    }

    if (brb.BrbL2caOpenChannel.ChannelHandle == NULL) {
        Session->ChannelHandle = NULL;
        *Mtu = 0;
        *CapabilityFlags = LIBREPODS_DRIVER_CAPABILITY_NONE;
        *DriverError = LIBREPODS_DRIVER_ERROR_INTERNAL;
        return STATUS_UNSUCCESSFUL;
    }

    Session->ChannelHandle = brb.BrbL2caOpenChannel.ChannelHandle;
    Session->LastPacketLength = 0;

    *Mtu = brb.BrbL2caOpenChannel.OutResults.Params.Mtu != 0
        ? brb.BrbL2caOpenChannel.OutResults.Params.Mtu
        : LIBREPODS_DRIVER_MAX_AACP_PACKET_LENGTH;
    *CapabilityFlags =
        LIBREPODS_DRIVER_CAPABILITY_DRIVER_BACKED_L2CAP |
        LIBREPODS_DRIVER_CAPABILITY_AACP_CONTROL |
        LIBREPODS_DRIVER_CAPABILITY_BATTERY_NOTIFICATIONS |
        LIBREPODS_DRIVER_CAPABILITY_CANCELABLE_IO;
    *DriverError = LIBREPODS_DRIVER_ERROR_NONE;
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS
LibrepodsTransportCloseSession(
    WDFDEVICE Device,
    PLIBREPODS_SESSION_ENTRY Session,
    ULONG* DriverError
    )
{
    PLIBREPODS_DEVICE_CONTEXT deviceContext;
    BRB brb;
    NTSTATUS status;

    if (Session->ChannelHandle == NULL) {
        *DriverError = LIBREPODS_DRIVER_ERROR_NONE;
        return STATUS_SUCCESS;
    }

    deviceContext = LibrepodsGetDeviceContext(Device);
    if (!LibrepodsHasUsableProfileInterface(deviceContext)) {
        Session->ChannelHandle = NULL;
        *DriverError = LIBREPODS_DRIVER_ERROR_NONE;
        return STATUS_SUCCESS;
    }

    RtlZeroMemory(&brb, sizeof(brb));
    deviceContext->ProfileDriverInterface.BthInitializeBrb(&brb, BRB_L2CA_CLOSE_CHANNEL);

    brb.BrbL2caCloseChannel.BtAddress = Session->RemoteAddress;
    brb.BrbL2caCloseChannel.ChannelHandle = Session->ChannelHandle;

    status = LibrepodsSubmitBrbSynchronously(Device, &brb);
    Session->ChannelHandle = NULL;

    if (!NT_SUCCESS(status)) {
        *DriverError = LibrepodsMapNtStatusToDriverError(status);
        return status;
    }

    *DriverError = LIBREPODS_DRIVER_ERROR_NONE;
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS
LibrepodsTransportSendPacket(
    WDFDEVICE Device,
    PLIBREPODS_SESSION_ENTRY Session,
    const UCHAR* Packet,
    ULONG PacketLength,
    ULONG* DriverError
    )
{
    PLIBREPODS_DEVICE_CONTEXT deviceContext;
    BRB brb;
    NTSTATUS status;

    if (Session->ChannelHandle == NULL || Packet == NULL || PacketLength == 0) {
        *DriverError = LIBREPODS_DRIVER_ERROR_INVALID_ARGUMENT;
        return STATUS_INVALID_PARAMETER;
    }

    deviceContext = LibrepodsGetDeviceContext(Device);
    if (!LibrepodsHasUsableProfileInterface(deviceContext)) {
        *DriverError = LIBREPODS_DRIVER_ERROR_TRANSPORT_BLOCKED;
        return STATUS_INVALID_DEVICE_STATE;
    }

    RtlZeroMemory(&brb, sizeof(brb));
    deviceContext->ProfileDriverInterface.BthInitializeBrb(&brb, BRB_L2CA_ACL_TRANSFER);

    brb.BrbL2caAclTransfer.BtAddress = Session->RemoteAddress;
    brb.BrbL2caAclTransfer.ChannelHandle = Session->ChannelHandle;
    brb.BrbL2caAclTransfer.TransferFlags = ACL_TRANSFER_DIRECTION_OUT;
    brb.BrbL2caAclTransfer.BufferSize = PacketLength;
    brb.BrbL2caAclTransfer.Buffer = (PVOID)Packet;
    brb.BrbL2caAclTransfer.BufferMDL = NULL;
    brb.BrbL2caAclTransfer.Timeout = 0;

    status = LibrepodsSubmitBrbSynchronously(Device, &brb);
    if (!NT_SUCCESS(status)) {
        *DriverError = LibrepodsMapNtStatusToDriverError(status);
        return status;
    }

    if (brb.BrbL2caAclTransfer.BufferSize != PacketLength) {
        *DriverError = LIBREPODS_DRIVER_ERROR_INTERNAL;
        return STATUS_UNSUCCESSFUL;
    }

    *DriverError = LIBREPODS_DRIVER_ERROR_NONE;
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS
LibrepodsTransportReadPacket(
    WDFDEVICE Device,
    PLIBREPODS_SESSION_ENTRY Session,
    UCHAR* PacketBuffer,
    ULONG PacketBufferLength,
    ULONG* PacketLength,
    ULONG* DriverError
    )
{
    PLIBREPODS_DEVICE_CONTEXT deviceContext;

    if (Session->ChannelHandle == NULL || PacketBuffer == NULL || PacketBufferLength == 0) {
        *PacketLength = 0;
        *DriverError = LIBREPODS_DRIVER_ERROR_INVALID_ARGUMENT;
        return STATUS_INVALID_PARAMETER;
    }

    deviceContext = LibrepodsGetDeviceContext(Device);
    if (!LibrepodsHasUsableProfileInterface(deviceContext)) {
        *PacketLength = 0;
        *DriverError = LIBREPODS_DRIVER_ERROR_TRANSPORT_BLOCKED;
        return STATUS_INVALID_DEVICE_STATE;
    }

    // Allocate the BRB through the profile interface (BthAllocateBrb) rather
    // than BthInitializeBrb on a stack BRB: only allocated BRBs carry
    // BRB_HEADER.Version=BLUETOOTH_V2, matching the Microsoft BthEcho sample
    // and BthPS3 for L2CAP transfers.
    {
        PBRB allocatedBrb;
        NTSTATUS allocStatus;

        allocatedBrb = deviceContext->ProfileDriverInterface.BthAllocateBrb(
            BRB_L2CA_ACL_TRANSFER,
            LIBREPODS_DRIVER_POOL_TAG);
        if (allocatedBrb == NULL) {
            *PacketLength = 0;
            *DriverError = LIBREPODS_DRIVER_ERROR_INTERNAL;
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        allocatedBrb->BrbL2caAclTransfer.BtAddress = Session->RemoteAddress;
        allocatedBrb->BrbL2caAclTransfer.ChannelHandle = Session->ChannelHandle;
        allocatedBrb->BrbL2caAclTransfer.TransferFlags =
            ACL_TRANSFER_DIRECTION_IN | ACL_SHORT_TRANSFER_OK;
        allocatedBrb->BrbL2caAclTransfer.BufferSize = PacketBufferLength;
        allocatedBrb->BrbL2caAclTransfer.Buffer = PacketBuffer;
        allocatedBrb->BrbL2caAclTransfer.BufferMDL = NULL;

        allocStatus = LibrepodsSubmitBrbSynchronously(Device, allocatedBrb);
        if (allocStatus == STATUS_TIMEOUT || allocStatus == STATUS_IO_TIMEOUT) {
            deviceContext->ProfileDriverInterface.BthFreeBrb(allocatedBrb);
            *PacketLength = 0;
            *DriverError = LIBREPODS_DRIVER_ERROR_NONE;
            return STATUS_SUCCESS;
        }

        if (!NT_SUCCESS(allocStatus)) {
            deviceContext->ProfileDriverInterface.BthFreeBrb(allocatedBrb);
            *PacketLength = 0;
            *DriverError = LibrepodsMapNtStatusToDriverError(allocStatus);
            return allocStatus;
        }

        if (allocatedBrb->BrbL2caAclTransfer.BufferSize > PacketBufferLength) {
            deviceContext->ProfileDriverInterface.BthFreeBrb(allocatedBrb);
            *PacketLength = 0;
            *DriverError = LIBREPODS_DRIVER_ERROR_INTERNAL;
            return STATUS_UNSUCCESSFUL;
        }

        *PacketLength = allocatedBrb->BrbL2caAclTransfer.BufferSize;
        deviceContext->ProfileDriverInterface.BthFreeBrb(allocatedBrb);
        *DriverError = LIBREPODS_DRIVER_ERROR_NONE;
        return STATUS_SUCCESS;
    }
}
