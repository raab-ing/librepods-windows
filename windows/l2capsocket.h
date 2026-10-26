#ifndef L2CAPSOCKET_H
#define L2CAPSOCKET_H

#include "btsocket.h"

#include <winsock2.h>
#include <ws2bth.h>

class QWinEventNotifier;

// Drop-in replacement for the subset of QBluetoothSocket that LibrePods uses,
// implemented directly over Winsock's AF_BTH L2CAP sockets. Fallback transport
// when the LibrePods KMDF profile driver is not installed.
class L2CapSocket : public BtSocketBase
{
    Q_OBJECT
public:
    explicit L2CapSocket(QObject *parent = nullptr);
    ~L2CapSocket() override;

    void connectToService(const QBluetoothAddress &address, const QBluetoothUuid &uuid) override;
    void connectToService(const QBluetoothAddress &address, quint16 psm) override;
    bool isOpen() const override { return m_socket != INVALID_SOCKET && m_state == QBluetoothSocket::SocketState::ConnectedState; }
    QBluetoothSocket::SocketState state() const override { return m_state; }
    qint64 write(const QByteArray &data) override;
    QByteArray readAll() override;
    void close() override;
    QBluetoothAddress peerAddress() const override { return m_peer; }
    QString errorString() const override { return m_lastError; }

private:
    void onSocketEvent();
    void startConnect(const QBluetoothAddress &address, const QBluetoothUuid &uuid,
                      unsigned short psm, bool psmOk);
    void setState(QBluetoothSocket::SocketState state);
    void fail(QBluetoothSocket::SocketError error, const QString &context);
    bool flushPending();
    static unsigned short resolvePsm(const QBluetoothAddress &address,
                                     const QBluetoothUuid &uuid, bool &ok);

    SOCKET m_socket = INVALID_SOCKET;
    WSAEVENT m_event = nullptr;
    QWinEventNotifier *m_notifier = nullptr;
    QBluetoothSocket::SocketState m_state = QBluetoothSocket::SocketState::UnconnectedState;
    QBluetoothAddress m_peer;
    QByteArray m_pendingWrite;
    QByteArray m_rxBuffer;
    QString m_lastError;
};

#endif // L2CAPSOCKET_H
