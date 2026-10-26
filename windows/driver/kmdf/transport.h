/*
 * Derived from librepods-windows by ivLis-Studio
 * (https://github.com/ivLis-Studio/librepods-windows), GPLv3 — a Windows port of
 * LibrePods (https://github.com/kavishdevar/librepods).
 */
#pragma once

#include "driver.h"

NTSTATUS
LibrepodsTransportQueryStatus(
    _In_ WDFDEVICE Device,
    _Out_ ULONG* CapabilityFlags,
    _Out_ ULONG* DriverError
    );

NTSTATUS
LibrepodsTransportOpenSession(
    _In_ WDFDEVICE Device,
    _Inout_ PLIBREPODS_SESSION_ENTRY Session,
    _In_ USHORT Psm,
    _Out_ ULONG* CapabilityFlags,
    _Out_ ULONG* Mtu,
    _Out_ ULONG* DriverError
    );

NTSTATUS
LibrepodsTransportCloseSession(
    _In_ WDFDEVICE Device,
    _Inout_ PLIBREPODS_SESSION_ENTRY Session,
    _Out_ ULONG* DriverError
    );

NTSTATUS
LibrepodsTransportSendPacket(
    _In_ WDFDEVICE Device,
    _Inout_ PLIBREPODS_SESSION_ENTRY Session,
    _In_reads_bytes_(PacketLength) const UCHAR* Packet,
    _In_ ULONG PacketLength,
    _Out_ ULONG* DriverError
    );

NTSTATUS
LibrepodsTransportReadPacket(
    _In_ WDFDEVICE Device,
    _Inout_ PLIBREPODS_SESSION_ENTRY Session,
    _Out_writes_bytes_to_(PacketBufferLength, *PacketLength) UCHAR* PacketBuffer,
    _In_ ULONG PacketBufferLength,
    _Out_ ULONG* PacketLength,
    _Out_ ULONG* DriverError
    );
