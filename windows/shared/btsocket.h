#ifndef BTSOCKET_H
#define BTSOCKET_H

#include <QObject>
#include <QBluetoothAddress>
#include <QBluetoothUuid>
#include <QBluetoothSocket>
#include <QByteArray>

// Common interface for the L2CAP transports. On Windows the driver-backed
// transport (DriverL2CapSocket) is preferred when the LibrePods KMDF profile
// driver is installed, with the Winsock AF_BTH socket (L2CapSocket) as a
// fallback; on Linux QtBtSocketAdapter wraps QBluetoothSocket's native L2CAP.
class BtSocketBase : public QObject
{
    Q_OBJECT
public:
    explicit BtSocketBase(QObject *parent = nullptr) : QObject(parent) {}

    virtual void connectToService(const QBluetoothAddress &address, const QBluetoothUuid &uuid) = 0;
    // Raw L2CAP PSM connect (e.g. 0x1F for the ATT channel). Default
    // implementation fails; transports that can't take a bare PSM override
    // this to report the error.
    virtual void connectToService(const QBluetoothAddress &address, quint16 psm)
    {
        Q_UNUSED(address);
        Q_UNUSED(psm);
        emit errorOccurred(QBluetoothSocket::SocketError::ServiceNotFoundError);
    }
    virtual bool isOpen() const = 0;
    virtual QBluetoothSocket::SocketState state() const = 0;
    virtual qint64 write(const QByteArray &data) = 0;
    virtual QByteArray readAll() = 0;
    virtual void close() = 0;
    virtual QBluetoothAddress peerAddress() const = 0;
    virtual QString errorString() const = 0;

signals:
    void connected();
    void disconnected();
    void readyRead();
    // One wire packet per emission. Protocols that need packet boundaries
    // (ATT PDUs) use this instead of the byte-stream readAll() — multiple
    // packets may coalesce into a single readAll() drain.
    void packetReceived(const QByteArray &data);
    void errorOccurred(QBluetoothSocket::SocketError error);
};

#endif // BTSOCKET_H
