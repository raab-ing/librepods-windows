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
        LIBREPODS_DRIVER_CAPABILITY_CANCELABLE_IO;
    *DriverError = LIBREPODS_DRIVER_ERROR_NOT_IMPLEMENTED;
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
    *Mtu = 0;
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

    *DriverError = LIBREPODS_DRIVER_ERROR_NOT_IMPLEMENTED;
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

    *DriverError = LIBREPODS_DRIVER_ERROR_NOT_IMPLEMENTED;
    return STATUS_NOT_SUPPORTED;
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
    UNREFERENCED_PARAMETER(Session);
    UNREFERENCED_PARAMETER(PacketBuffer);
    UNREFERENCED_PARAMETER(PacketBufferLength);

    *PacketLength = 0;
    *DriverError = LIBREPODS_DRIVER_ERROR_NOT_IMPLEMENTED;
    return STATUS_NOT_SUPPORTED;
}
