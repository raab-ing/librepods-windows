// ATT session manager for the AirPods secondary L2CAP channel (PSM 31).
// Wire format mirrors the Android ATTManagerv2:
//   read request    0x0A | handle u16 LE          -> response 0x0B | value
//   write request   0x12 | handle u16 LE | value -> response 0x13
//   notification in 0x1B | handle u16 LE | value
//   error response  0x01 | req opcode | handle u16 | error code
#include "attmanager.h"

#include "logger.h"

#include <QHash>

namespace
{
constexpr quint8 kOpError = 0x01;
constexpr quint8 kOpReadRequest = 0x0A;
constexpr quint8 kOpReadResponse = 0x0B;
constexpr quint8 kOpWriteRequest = 0x12;
constexpr quint8 kOpWriteResponse = 0x13;
constexpr quint8 kOpNotification = 0x1B;
constexpr int kRequestTimeoutMs = 2000;
} // namespace

AttManager::AttManager(BtSocketBase *socket, QObject *parent)
    : QObject(parent), m_socket(socket)
{
    m_timeout.setSingleShot(true);
    connect(&m_timeout, &QTimer::timeout, this, &AttManager::onTimeout);
    if (m_socket)
    {
        connect(m_socket, &BtSocketBase::packetReceived, this, &AttManager::onPacket);
        connect(m_socket, &BtSocketBase::disconnected, this, &AttManager::onDisconnected);
    }
}

void AttManager::start()
{
    if (!m_socket || !m_socket->isOpen() || m_active)
        return;
    m_active = true;

    // Android enables CCCD notifications for the two live characteristics;
    // the CCCD sits at value handle + 1.
    enableNotifications(Handle::Transparency);
    enableNotifications(Handle::HearingAid);

    // Same read order as Android: LSR first (legacy probe), then the two
    // characteristics the UI actually consumes.
    readCharacteristic(Handle::LoudSoundReduction);
    readCharacteristic(Handle::Transparency);
    readCharacteristic(Handle::HearingAid);
}

void AttManager::stop()
{
    m_active = false;
    m_queue.clear();
    m_inFlight = false;
    m_timeout.stop();
}

void AttManager::readCharacteristic(quint8 handle)
{
    enqueue({kOpReadRequest, handle, QByteArray(), kOpReadResponse});
}

void AttManager::writeCharacteristic(quint8 handle, const QByteArray &value)
{
    m_values[handle] = value;
    enqueue({kOpWriteRequest, handle, value, kOpWriteResponse});
}

void AttManager::enableNotifications(quint8 valueHandle)
{
    // CCCD at valueHandle + 1, single-byte enable payload (Android format).
    enqueue({kOpWriteRequest, static_cast<quint8>(valueHandle + 1),
             QByteArray(1, 0x01), kOpWriteResponse});
}

void AttManager::enqueue(const Op &op)
{
    if (!m_socket || !m_socket->isOpen())
        return;
    m_queue.enqueue(op);
    dispatchNext();
}

void AttManager::dispatchNext()
{
    if (m_inFlight || m_queue.isEmpty() || !m_socket || !m_socket->isOpen())
        return;

    const Op op = m_queue.head();
    const QByteArray pdu = buildPdu(op.opcode, op.handle, op.payload);
    if (m_socket->write(pdu) < 0)
    {
        LOG_WARN("ATT write failed: " << m_socket->errorString());
        m_queue.dequeue();
        dispatchNext();
        return;
    }
    m_inFlight = true;
    m_timeout.start(kRequestTimeoutMs);
}

QByteArray AttManager::buildPdu(quint8 opcode, quint8 handle, const QByteArray &payload)
{
    QByteArray pdu;
    pdu.reserve(3 + payload.size());
    pdu.append(static_cast<char>(opcode));
    pdu.append(static_cast<char>(handle));
    pdu.append(char(0x00)); // handle high byte — all handles fit in u8
    pdu.append(payload);
    return pdu;
}

void AttManager::onPacket(const QByteArray &data)
{
    if (!m_socket || data.isEmpty())
        return;

    // Drain the byte-stream buffer as well — transports append every packet
    // there for readAll() consumers; this manager consumes packet-level PDUs
    // only, so the accumulation is dead weight otherwise.
    m_socket->readAll();

    const quint8 opcode = static_cast<quint8>(data.at(0));

    if (opcode == kOpNotification)
    {
        if (data.size() >= 3)
        {
            const quint8 handle = static_cast<quint8>(data.at(1));
            const QByteArray value = data.mid(3);
            m_values[handle] = value;
            emit notificationReceived(handle, value);
            emit characteristicRead(handle, value);
        }
        return;
    }

    if (!m_inFlight || m_queue.isEmpty())
    {
        LOG_DEBUG("ATT: stray PDU opcode 0x" << QString::number(opcode, 16));
        return;
    }

    const Op op = m_queue.head();

    if (opcode == kOpError)
    {
        const quint8 errCode = data.size() > 4 ? static_cast<quint8>(data.at(4)) : 0xFF;
        LOG_WARN("ATT error response for handle 0x"
                 << QString::number(op.handle, 16) << " code 0x" << QString::number(errCode, 16));
        emit requestFailed(op.handle, errCode);
        completeCurrent();
        return;
    }

    if (opcode != op.expectedResponse)
    {
        LOG_DEBUG("ATT: unexpected opcode 0x" << QString::number(opcode, 16)
                  << " (wanted 0x" << QString::number(op.expectedResponse, 16) << ")");
        return;
    }

    if (op.opcode == kOpReadRequest)
    {
        const QByteArray value = data.mid(1);
        m_values[op.handle] = value;
        emit characteristicRead(op.handle, value);
    }
    else
    {
        emit characteristicWritten(op.handle);
    }
    completeCurrent();
}

void AttManager::completeCurrent()
{
    m_inFlight = false;
    m_timeout.stop();
    if (!m_queue.isEmpty())
        m_queue.dequeue();
    dispatchNext();
}

void AttManager::onTimeout()
{
    if (!m_queue.isEmpty())
    {
        const Op op = m_queue.head();
        LOG_WARN("ATT request timeout for handle 0x" << QString::number(op.handle, 16));
        emit requestFailed(op.handle, 0xFF);
    }
    completeCurrent();
}

void AttManager::onDisconnected()
{
    stop();
    m_values.clear();
    emit stopped();
}
