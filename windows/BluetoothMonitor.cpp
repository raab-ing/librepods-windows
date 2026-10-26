#include "BluetoothMonitor.h"
#include "logger.h"

#include <QMutex>
#include <QPointer>
#include <QSet>
#include <QHash>
#include <QPair>
#include <QMetaObject>
#include <cstring>
#include <functional>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Storage.Streams.h>

using namespace winrt;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Devices::Bluetooth;
using namespace winrt::Windows::Devices::Enumeration;

namespace
{
// AirPods AACP service UUID: 74ec2172-0bad-4d01-8f77-997b2be0722a
constexpr quint8 AACP_UUID[] = {
    0x74, 0xec, 0x21, 0x72, 0x0b, 0xad, 0x4d, 0x01,
    0x8f, 0x77, 0x99, 0x7b, 0x2b, 0xe0, 0x72, 0x2a
};

QString formatBluetoothAddress(uint64_t address)
{
    QStringList octets;
    for (int shift = 40; shift >= 0; shift -= 8)
    {
        octets << QStringLiteral("%1").arg((address >> shift) & 0xFF, 2, 16, QLatin1Char('0'));
    }
    return octets.join(':').toUpper();
}

// Checks whether any SDP record of the device mentions the AirPods AACP UUID.
// SDP records are raw byte buffers; a contiguous scan for the 128-bit UUID is
// sufficient because service UUIDs are stored little-endian in record order.
bool advertisesAacpService(const BluetoothDevice &device)
{
    try
    {
        for (auto const &record : device.SdpRecords())
        {
            if (record.Length() < sizeof(AACP_UUID))
            {
                continue;
            }
            const quint8 *data = record.data();
            const quint8 *end = data + record.Length() - sizeof(AACP_UUID) + 1;
            for (const quint8 *p = data; p < end; ++p)
            {
                if (memcmp(p, AACP_UUID, sizeof(AACP_UUID)) == 0)
                {
                    return true;
                }
            }
        }
    }
    catch (const hresult_error &e)
    {
        LOG_WARN("Failed to read SDP records: " << QString::fromWCharArray(e.message().c_str()));
    }
    return false;
}
} // namespace

struct BluetoothMonitor::Impl
{
    DeviceWatcher watcher{nullptr};
    DeviceWatcher::Added_revoker addedRevoker;
    DeviceWatcher::Removed_revoker removedRevoker;
    DeviceWatcher::EnumerationCompleted_revoker enumerationCompletedRevoker;
    QMutex mutex;
    // deviceId -> (mac, name) for connected AirPods
    QHash<QString, QPair<QString, QString>> connectedAirPods;
};

BluetoothMonitor::BluetoothMonitor(QObject *parent) : QObject(parent), impl(new Impl)
{
}

// Watcher bring-up is split from the ctor: CreateWatcher() can block for a
// long time on a wedged Bluetooth stack, and running it inside
// AirPodsTrayApp's constructor froze startup before initializeBluetooth()
// could arm the remembered-MAC fallback. Callers must run this off the GUI
// thread (it is invoked from the app's deferred-init worker).
void BluetoothMonitor::startMonitoring()
{
    if (impl->watcher)
        return;

    // Watching devices filtered by connection status yields Added on connect
    // and Removed on disconnect.
    hstring selector = BluetoothDevice::GetDeviceSelectorFromConnectionStatus(
        BluetoothConnectionStatus::Connected);
    impl->watcher = DeviceInformation::CreateWatcher(selector);

    QPointer<BluetoothMonitor> guard(this);
    impl->addedRevoker = impl->watcher.Added(winrt::auto_revoke,
        [guard](DeviceWatcher const &, DeviceInformation const &info)
        {
            if (guard)
            {
                guard->onDeviceArrived(QString::fromWCharArray(info.Id().c_str()));
            }
        });
    impl->removedRevoker = impl->watcher.Removed(winrt::auto_revoke,
        [guard](DeviceWatcher const &, DeviceInformationUpdate const &info)
        {
            if (guard)
            {
                guard->onDeviceDeparted(QString::fromWCharArray(info.Id().c_str()));
            }
        });
    impl->watcher.Start();
}

BluetoothMonitor::~BluetoothMonitor()
{
    if (impl->watcher)
    {
        impl->watcher.Stop();
    }
    delete impl;
}

// Resolves a PnP device id to a BluetoothDevice and, if its (cached) SDP
// records advertise the AirPods AACP service, invokes onMatch on the Qt
// thread with the formatted address and name.
void BluetoothMonitor::probeDevice(const QString &deviceId,
                                   std::function<void(const QString &, const QString &)> onMatch)
{
    hstring winrtId = winrt::hstring(reinterpret_cast<const wchar_t *>(deviceId.utf16()));
    QPointer<BluetoothMonitor> guard(this);
    BluetoothDevice::FromIdAsync(winrtId).Completed(
        [guard, onMatch = std::move(onMatch)](IAsyncOperation<BluetoothDevice> const &op, AsyncStatus status) mutable
        {
            if (!guard || status != AsyncStatus::Completed)
            {
                return;
            }
            // Device can vanish mid-resolution during link flapping; every
            // projected accessor may throw hresult_error on this pool thread,
            // and an escape is an uncaught-exception terminate.
            try
            {
                BluetoothDevice device = op.GetResults();
                if (device == nullptr || !advertisesAacpService(device))
                {
                    return;
                }

                QString mac = formatBluetoothAddress(device.BluetoothAddress());
                QString name = QString::fromWCharArray(device.Name().c_str());

                QMetaObject::invokeMethod(guard,
                    [guard, mac, name, onMatch = std::move(onMatch)]() mutable {
                        onMatch(mac, name);
                    },
                    Qt::QueuedConnection);
            }
            catch (const hresult_error &e)
            {
                LOG_DEBUG("probeDevice dropped (device gone): "
                          << QString::fromWCharArray(e.message().c_str()));
            }
        });
}

void BluetoothMonitor::onDeviceArrived(const QString &deviceId)
{
    probeDevice(deviceId, [this, deviceId](const QString &mac, const QString &name) {
        {
            QMutexLocker lock(&impl->mutex);
            if (impl->connectedAirPods.contains(deviceId))
            {
                return;
            }
            impl->connectedAirPods.insert(deviceId, {mac, name});
        }
        LOG_DEBUG("AirPods connected: " << mac << " Name: " << name);
        emit deviceConnected(mac, name);
    });
}

void BluetoothMonitor::onDeviceDeparted(const QString &deviceId)
{
    QMetaObject::invokeMethod(this, [this, deviceId]() {
        QPair<QString, QString> device;
        {
            QMutexLocker lock(&impl->mutex);
            auto it = impl->connectedAirPods.find(deviceId);
            if (it == impl->connectedAirPods.end())
            {
                return;
            }
            device = it.value();
            impl->connectedAirPods.erase(it);
        }
        LOG_DEBUG("AirPods disconnected: " << device.first << " Name: " << device.second);
        emit deviceDisconnected(device.first, device.second);
    }, Qt::QueuedConnection);
}

void BluetoothMonitor::checkPairedDevices()
{
    hstring selector = BluetoothDevice::GetDeviceSelectorFromPairingState(true);
    DeviceInformation::FindAllAsync(selector).Completed(
        [this](IAsyncOperation<DeviceInformationCollection> const &op, AsyncStatus status)
        {
            if (status != AsyncStatus::Completed)
            {
                return;
            }
            try
            {
                for (auto const &info : op.GetResults())
                {
                    probeDevice(QString::fromWCharArray(info.Id().c_str()),
                                [this](const QString &mac, const QString &name) {
                                    LOG_DEBUG("Paired AirPods found: " << mac << " Name: " << name);
                                    emit devicePaired(mac, name);
                                });
                }
            }
            catch (const hresult_error &e)
            {
                LOG_DEBUG("checkPairedDevices dropped: "
                          << QString::fromWCharArray(e.message().c_str()));
            }
        });
}

bool BluetoothMonitor::checkAlreadyConnectedDevices()
{
    hstring selector = BluetoothDevice::GetDeviceSelectorFromConnectionStatus(
        BluetoothConnectionStatus::Connected);
    DeviceInformation::FindAllAsync(selector).Completed(
        [this](IAsyncOperation<DeviceInformationCollection> const &op, AsyncStatus status)
        {
            if (status != AsyncStatus::Completed)
            {
                return;
            }
            try
            {
                for (auto const &info : op.GetResults())
                {
                    onDeviceArrived(QString::fromWCharArray(info.Id().c_str()));
                }
            }
            catch (const hresult_error &e)
            {
                LOG_DEBUG("checkAlreadyConnectedDevices dropped: "
                          << QString::fromWCharArray(e.message().c_str()));
            }
        });
    return true;
}
