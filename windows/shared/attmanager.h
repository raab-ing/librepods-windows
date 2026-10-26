#ifndef ATTMANAGER_H
#define ATTMANAGER_H

#include <QObject>
#include <QQueue>
#include <QByteArray>
#include <QTimer>

#include "btsocket.h"

// ATT session over a raw L2CAP channel (PSM 31). Port of the Android app's
// ATTManagerv2: read/write characteristic PDUs, CCCD notification enables,
// and a serialized request queue (ATT allows one outstanding request).
class AttManager : public QObject
{
    Q_OBJECT
public:
    // Characteristic handles exposed by the AirPods ATT server.
    enum Handle : quint8 {
        Transparency = 0x18,
        LoudSoundReduction = 0x1B, // present upstream but marked non-functional
        HearingAid = 0x2A
    };

    explicit AttManager(BtSocketBase *socket, QObject *parent = nullptr);

    bool isActive() const { return m_active; }

    // Mirror of the Android init: enable notifications, then read the
    // characteristics we care about.
    void start();
    void stop();

    QByteArray characteristicValue(quint8 handle) const { return m_values.value(handle); }

public slots:
    void readCharacteristic(quint8 handle);
    void writeCharacteristic(quint8 handle, const QByteArray &value);
    void enableNotifications(quint8 valueHandle);

signals:
    void characteristicRead(quint8 handle, const QByteArray &value);
    void characteristicWritten(quint8 handle);
    void notificationReceived(quint8 handle, const QByteArray &value);
    void requestFailed(quint8 handle, quint8 errorCode);
    void stopped();

private slots:
    void onPacket(const QByteArray &data);
    void onDisconnected();
    void onTimeout();

private:
    struct Op {
        quint8 opcode;
        quint8 handle;
        QByteArray payload;
        quint8 expectedResponse;
    };

    void enqueue(const Op &op);
    void dispatchNext();
    void completeCurrent();
    static QByteArray buildPdu(quint8 opcode, quint8 handle, const QByteArray &payload);

    BtSocketBase *m_socket;
    QQueue<Op> m_queue;
    bool m_inFlight = false;
    bool m_active = false;
    QTimer m_timeout;
    QHash<quint8, QByteArray> m_values;
};

#endif // ATTMANAGER_H
