#ifndef SVCCHANNELSOCKET_H
#define SVCCHANNELSOCKET_H

#include "btsocket.h"

#include <QDateTime>
#include <QLocalSocket>
#include <QTimer>

// L2CAP transport via the librepods-svc companion process. The svc owns the
// driver handle and channel state machine; this socket is a thin framed-IPC
// client. Semantics match DriverL2CapSocket so callers can swap freely.
class SvcChannelSocket : public BtSocketBase
{
    Q_OBJECT
public:
    explicit SvcChannelSocket(QObject *parent = nullptr);
    ~SvcChannelSocket() override;

    // Cheap probe: is a svc answering on the pipe right now?
    static bool isServiceAvailable(int timeoutMs = 150);

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
    void onReadyRead();
    void onPipeDisconnected();
    void onPipeError(QLocalSocket::LocalSocketError error);
    void sendOpen(const QBluetoothAddress &address, quint16 psm);
    void setState(QBluetoothSocket::SocketState state);
    void fail(QBluetoothSocket::SocketError error, const QString &context);
    void onPingTick();

    QLocalSocket *m_pipe = nullptr;
    QByteArray m_recv;
    QBluetoothSocket::SocketState m_state = QBluetoothSocket::SocketState::UnconnectedState;
    QBluetoothAddress m_peer;
    quint16 m_psm = 0;
    quint16 m_channelId = 0;
    QString m_lastError;
    QByteArray m_readBuffer;
    QTimer *m_pingTimer = nullptr;
    QDateTime m_lastPong;
};

#endif // SVCCHANNELSOCKET_H
