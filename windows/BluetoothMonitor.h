#ifndef BLUETOOTHMONITOR_H
#define BLUETOOTHMONITOR_H

#include <QObject>
#include <QString>

// Windows device connection monitor backed by WinRT DeviceInformation
// watchers, replacing the BlueZ/DBus-based Linux implementation. Emits the
// same deviceConnected/deviceDisconnected signals, filtered to devices whose
// SDP records advertise the AirPods AACP service UUID.
class BluetoothMonitor : public QObject
{
    Q_OBJECT
public:
    explicit BluetoothMonitor(QObject *parent = nullptr);
    ~BluetoothMonitor() override;

    // Bring up the DeviceWatcher. Can block on a wedged Bluetooth stack —
    // call off the GUI thread. Safe to call more than once.
    void startMonitoring();

    bool checkAlreadyConnectedDevices();
    // Emits devicePaired for every paired device whose cached SDP records
    // advertise the AACP service, even if it is not currently connected.
    void checkPairedDevices();

signals:
    void deviceConnected(const QString &macAddress, const QString &deviceName);
    void deviceDisconnected(const QString &macAddress, const QString &deviceName);
    void devicePaired(const QString &macAddress, const QString &deviceName);

private:
    // All WinRT interaction lives in the pimpl so this header stays moc-safe.
    struct Impl;
    Impl *impl = nullptr;

    void onDeviceArrived(const QString &deviceId);
    void onDeviceDeparted(const QString &deviceId);
    void probeDevice(const QString &deviceId,
                     std::function<void(const QString &, const QString &)> onMatch);
};

#endif // BLUETOOTHMONITOR_H
