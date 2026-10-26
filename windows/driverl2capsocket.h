#ifndef DRIVERL2CAPSOCKET_H
#define DRIVERL2CAPSOCKET_H

#include "btsocket.h"

#include <QMutex>
#include <atomic>
#include <memory>

// L2CAP transport backed by the LibrePods KMDF Bluetooth profile driver.
// The driver binds to the AirPods' vendor-specific BTHENUM service PDO and
// performs the outbound AACP L2CAP channel open in kernel mode, bypassing the
// Microsoft user-mode stack (which has no L2CAP support). Communication uses
// the shared IOCTL protocol in driver/shared/librepods_driver_protocol.h.
class DriverL2CapSocket : public BtSocketBase
{
    Q_OBJECT
public:
    explicit DriverL2CapSocket(QObject *parent = nullptr);
    ~DriverL2CapSocket() override;

    // True when the driver interface is present and reports driver-backed
    // L2CAP capability. The installed driver may speak protocol v1 (fixed
    // AACP PSM) — v2 adds caller-selectable PSM for the ATT channel.
    static bool isDriverAvailable();
    static bool isAttSupported();
    static QString driverStatus();

    void connectToService(const QBluetoothAddress &address, const QBluetoothUuid &uuid) override;
    void connectToService(const QBluetoothAddress &address, quint16 psm) override;
    bool isOpen() const override { return m_state == QBluetoothSocket::SocketState::ConnectedState; }
    QBluetoothSocket::SocketState state() const override { return m_state; }
    qint64 write(const QByteArray &data) override;
    QByteArray readAll() override;
    void close() override;
    QBluetoothAddress peerAddress() const override { return m_peer; }
    QString errorString() const override { return m_lastError; }

private:
    class Reader;
    friend class Reader;

    static QString findDevicePath();
    static int driverProtocolVersion();
    void *openHandle() const;
    void connectInternal(const QBluetoothAddress &address, quint16 psm);
    void startReader();
    void stopReader();
    void setState(QBluetoothSocket::SocketState state);
    void fail(QBluetoothSocket::SocketError error, const QString &context);

    void *m_handle = nullptr; // HANDLE — sends/closes only; reads run on a dedicated reader handle
    QString m_devicePath;
    quint64 m_sessionId = 0;
    QBluetoothSocket::SocketState m_state = QBluetoothSocket::SocketState::UnconnectedState;
    QBluetoothAddress m_peer;
    quint16 m_psm = 0; // 0 => driver default (AACP)
    QString m_lastError;
    QByteArray m_readBuffer;
    QMutex m_bufferMutex;
    QMutex m_sendMutex;
    Reader *m_reader = nullptr;
    // Shared with the reader thread: a detached reader may outlive this
    // object, so the flag must too.
    std::shared_ptr<std::atomic<bool>> m_closing = std::make_shared<std::atomic<bool>>(false);
};

#endif // DRIVERL2CAPSOCKET_H
