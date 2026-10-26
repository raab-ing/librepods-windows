/*
 * Derived from librepods-windows by ivLis-Studio
 * (https://github.com/ivLis-Studio/librepods-windows), GPLv3 — a Windows port of
 * LibrePods (https://github.com/kavishdevar/librepods).
 */
#include "transport.h"

_Use_decl_annotations_
NTSTATUS
LibrepodsTransportQueryStatus(
    WDFDEVICE Device,
    ULONG* CapabilityFlags,
    ULONG* DriverError
    )
{
    UNREFERENCED_PARAMETER(Device);

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
LibrepodsTransportOpenSession(
    WDFDEVICE Device,
    PLIBREPODS_SESSION_ENTRY Session,
    USHORT Psm,
    ULONG* CapabilityFlags,
    ULONG* Mtu,
    ULONG* DriverError
    )
{
    UNREFERENCED_PARAMETER(Device);
    UNREFERENCED_PARAMETER(Session);
    UNREFERENCED_PARAMETER(Psm);
    *Mtu = LIBREPODS_DRIVER_MAX_AACP_PACKET_LENGTH;
    return LibrepodsTransportQueryStatus(Device, CapabilityFlags, DriverError);
}

_Use_decl_annotations_
NTSTATUS
LibrepodsTransportCloseSession(
    WDFDEVICE Device,
    PLIBREPODS_SESSION_ENTRY Session,
    ULONG* DriverError
    )
{
    UNREFERENCED_PARAMETER(Device);
    UNREFERENCED_PARAMETER(Session);

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
    UNREFERENCED_PARAMETER(Device);

    Session->LastPacketLength = PacketLength;
    RtlCopyMemory(Session->LastPacket, Packet, PacketLength);
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
    UNREFERENCED_PARAMETER(Device);

    if (Session->LastPacketLength == 0) {
        *PacketLength = 0;
        *DriverError = LIBREPODS_DRIVER_ERROR_NONE;
        return STATUS_SUCCESS;
    }

    if (Session->LastPacketLength > PacketBufferLength) {
        *DriverError = LIBREPODS_DRIVER_ERROR_INVALID_ARGUMENT;
        return STATUS_BUFFER_TOO_SMALL;
    }

    RtlCopyMemory(PacketBuffer, Session->LastPacket, Session->LastPacketLength);
    *PacketLength = Session->LastPacketLength;
    *DriverError = LIBREPODS_DRIVER_ERROR_NONE;
    return STATUS_SUCCESS;
}
