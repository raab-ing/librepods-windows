#ifndef BLEMANAGER_H
#define BLEMANAGER_H

#include <QObject>
#include <functional>
#include <memory>
#include "bleinfo.h"

// Windows BLE manager backed by the WinRT BluetoothLEAdvertisementWatcher.
// Qt's Windows Bluetooth backend does not expose manufacturer-specific
// advertisement data, so proximity pairing messages (company 0x004C) must be
// captured natively. The parsing itself is shared with the Linux build via
// linux/ble/proximityparser.
class BleManager : public QObject
{
    Q_OBJECT
public:
    explicit BleManager(QObject *parent = nullptr);
    ~BleManager() override;

    void startScan();
    void stopScan();
    bool isScanning() const;

signals:
    void deviceFound(const BleInfo &device);

private:
    void postCommand(std::function<void()> cmd);
    void ensureWorker();

    // Pimpl hides WinRT types from moc. Shared with the worker thread: a
    // detached worker may outlive this object, so Impl must too.
    struct Impl;
    std::shared_ptr<Impl> impl;
};

#endif // BLEMANAGER_H
