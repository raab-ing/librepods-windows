#include "blemanager.h"
#include "logger.h"
#include "proximityparser.h"

#include <QPointer>
#include <QMetaObject>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Devices.Bluetooth.Advertisement.h>

using namespace winrt;
using namespace winrt::Windows::Devices::Bluetooth::Advertisement;

static QString formatBluetoothAddress(uint64_t address)
{
    QStringList octets;
    for (int shift = 40; shift >= 0; shift -= 8)
    {
        octets << QStringLiteral("%1").arg((address >> shift) & 0xFF, 2, 16, QLatin1Char('0'));
    }
    return octets.join(':').toUpper();
}

// The watcher lives entirely on a dedicated worker thread: creating it,
// applying the filter, Start() and Stop() can all block for a long time on
// a wedged Bluetooth stack, and this object used to do that work in its
// constructor — inside AirPodsTrayApp's init list, before the first log
// line, freezing startup with zero evidence. Commands are posted to the
// worker; results come back as atomics.
struct BleManager::Impl
{
    std::thread worker;
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<std::function<void()>> commands;
    bool quit = false;
    BluetoothLEAdvertisementWatcher watcher{nullptr};
    std::atomic<bool> started{false};
};

BleManager::BleManager(QObject *parent) : QObject(parent), impl(std::make_shared<Impl>())
{
}

BleManager::~BleManager()
{
    {
        std::lock_guard lk(impl->mutex);
        impl->quit = true;
        impl->commands.clear();
    }
    impl->cv.notify_all();
    // The worker may be wedged inside a WinRT call — never join it. It holds
    // its own shared_ptr to Impl, so impl stays valid until the worker exits
    // even though this object is gone.
    if (impl->worker.joinable())
        impl->worker.detach();
}

void BleManager::postCommand(std::function<void()> cmd)
{
    ensureWorker();
    {
        std::lock_guard lk(impl->mutex);
        impl->commands.push_back(std::move(cmd));
    }
    impl->cv.notify_one();
}

void BleManager::ensureWorker()
{
    if (impl->worker.joinable())
        return;

    QPointer<BleManager> guard(this);
    std::shared_ptr<Impl> implRef = impl;
    impl->worker = std::thread([implRef, guard]() {
        std::shared_ptr<Impl> impl = implRef;
        // Revokers must outlive the command loop — auto_revoke unhooks the
        // handler when the token dies.
        BluetoothLEAdvertisementWatcher::Received_revoker receivedRevoker;
        BluetoothLEAdvertisementWatcher::Stopped_revoker stoppedRevoker;
        try
        {
            BluetoothLEAdvertisementFilter filter;
            BluetoothLEManufacturerData manufacturerFilter;
            manufacturerFilter.CompanyId(0x004C); // Apple
            filter.Advertisement().ManufacturerData().Append(manufacturerFilter);

            impl->watcher = BluetoothLEAdvertisementWatcher();
            impl->watcher.ScanningMode(BluetoothLEScanningMode::Active);
            impl->watcher.AdvertisementFilter(filter);

            receivedRevoker = impl->watcher.Received(winrt::auto_revoke,
                [guard](BluetoothLEAdvertisementWatcher const &, BluetoothLEAdvertisementReceivedEventArgs const &args)
                {
                    if (!guard)
                    {
                        return;
                    }

                    for (auto const &section : args.Advertisement().ManufacturerData())
                    {
                        if (section.CompanyId() != 0x004C)
                        {
                            continue;
                        }

                        auto buffer = section.Data();
                        QByteArray data(reinterpret_cast<const char *>(buffer.data()),
                                        static_cast<int>(buffer.Length()));
                        QString address = formatBluetoothAddress(args.BluetoothAddress());
                        QString name = QString::fromWCharArray(args.Advertisement().LocalName().c_str());

                        BleInfo deviceInfo;
                        if (ProximityParser::parseProximityAdvertisement(data, address, name, deviceInfo))
                        {
                            // Watcher callbacks arrive on a WinRT thread; hop to the Qt thread.
                            QMetaObject::invokeMethod(guard,
                                [guard, deviceInfo]() { emit guard->deviceFound(deviceInfo); },
                                Qt::QueuedConnection);
                        }
                    }
                });

            stoppedRevoker = impl->watcher.Stopped(winrt::auto_revoke,
                [guard](BluetoothLEAdvertisementWatcher const &,
                        BluetoothLEAdvertisementWatcherStoppedEventArgs const &args)
                {
                    if (!guard)
                    {
                        return;
                    }
                    QMetaObject::invokeMethod(guard, [guard, error = args.Error()]() {
                        guard->impl->started = false;
                        LOG_WARN("BLE advertisement watcher stopped, status error: " << static_cast<int>(error));
                    }, Qt::QueuedConnection);
                });
        }
        catch (const hresult_error &e)
        {
            impl->started = false;
            LOG_ERROR("BLE watcher setup failed (hr 0x"
                      << QString::number(e.code().value, 16) << "): "
                      << QString::fromWCharArray(e.message().c_str()));
        }

        for (;;)
        {
            std::function<void()> cmd;
            {
                std::unique_lock lk(impl->mutex);
                impl->cv.wait(lk, [impl]() { return impl->quit || !impl->commands.empty(); });
                if (impl->quit)
                {
                    break;
                }
                cmd = std::move(impl->commands.front());
                impl->commands.pop_front();
            }
            try
            {
                cmd();
            }
            catch (const hresult_error &e)
            {
                LOG_ERROR("BLE watcher command failed (hr 0x"
                          << QString::number(e.code().value, 16) << "): "
                          << QString::fromWCharArray(e.message().c_str()));
            }
        }

        try
        {
            impl->watcher.Stop();
        }
        catch (...)
        {
        }
        impl->started = false;
    });
}

void BleManager::startScan()
{
    postCommand([this]() {
        if (impl->watcher.Status() == BluetoothLEAdvertisementWatcherStatus::Started)
        {
            return;
        }
        LOG_DEBUG("Starting BLE advertisement watcher...");
        impl->watcher.Start();
    });
    impl->started = true; // optimistic; the Stopped revoker clears it
}

void BleManager::stopScan()
{
    impl->started = false;
    postCommand([this]() {
        if (impl->watcher.Status() == BluetoothLEAdvertisementWatcherStatus::Started)
        {
            LOG_DEBUG("Stopping BLE advertisement watcher...");
            impl->watcher.Stop();
        }
    });
}

bool BleManager::isScanning() const
{
    return impl->started.load();
}
