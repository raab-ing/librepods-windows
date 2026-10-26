/*
 * GUI-side transport that forwards the BtSocketBase interface to
 * librepods-svc over a named local socket. The svc owns the real driver
 * channel — including surviving this process — so a GUI restart can adopt
 * the live session instead of dropping the pods' control channel.
 */
#include "svcchannelsocket.h"
#include "svcprotocol.h"

#include <QDateTime>
#include <QDebug>
#include <QLoggingCategory>

Q_LOGGING_CATEGORY(svcSocket, "librepods.svc")

namespace {
constexpr char kPipeName[] = "librepods-svc";
constexpr int kPingIntervalMs = 5000;
constexpr int kPongTimeoutMs = 15000;
constexpr quint16 kPsmAacp = 0x1001;
}

SvcChannelSocket::SvcChannelSocket(QObject *parent)
    : BtSocketBase(parent)
{
}

SvcChannelSocket::~SvcChannelSocket()
{
    close();
}

bool SvcChannelSocket::isServiceAvailable(int timeoutMs)
{
    if (qEnvironmentVariableIsSet("LIBREPODS_NO_SVC"))
        return false;
    QLocalSocket probe;
    probe.connectToServer(kPipeName);
    if (!probe.waitForConnected(timeoutMs))
        return false;
    return true;
}

void SvcChannelSocket::connectToService(const QBluetoothAddress &address, const QBluetoothUuid &uuid)
{
    Q_UNUSED(uuid);
    connectToService(address, kPsmAacp);
}

void SvcChannelSocket::connectToService(const QBluetoothAddress &address, quint16 psm)
{
    if (m_state == QBluetoothSocket::SocketState::ConnectedState ||
        m_state == QBluetoothSocket::SocketState::ConnectingState) {
        return;
    }
    close();

    m_pipe = new QLocalSocket(this);
    connect(m_pipe, &QLocalSocket::readyRead, this, &SvcChannelSocket::onReadyRead);
    connect(m_pipe, &QLocalSocket::disconnected, this, &SvcChannelSocket::onPipeDisconnected);
    connect(m_pipe, &QLocalSocket::errorOccurred, this, &SvcChannelSocket::onPipeError);
    connect(m_pipe, &QLocalSocket::connected, this, [this]() {
        m_pipe->write(SvcProto::encode(SvcProto::Hello, 0,
                                       QByteArray(1, static_cast<char>(SvcProto::kVersion))));
    });

    m_peer = address;
    m_psm = psm;
    m_lastPong = QDateTime::currentDateTime();
    setState(QBluetoothSocket::SocketState::ConnectingState);
    m_pipe->connectToServer(kPipeName);
}

qint64 SvcChannelSocket::write(const QByteArray &data)
{
    if (!isOpen() || !m_pipe || m_channelId == 0)
        return -1;
    m_pipe->write(SvcProto::encode(SvcProto::Write, m_channelId, data));
    return data.size();
}

QByteArray SvcChannelSocket::readAll()
{
    QByteArray data = m_readBuffer;
    m_readBuffer.clear();
    return data;
}

void SvcChannelSocket::close()
{
    const bool wasConnected = m_state == QBluetoothSocket::SocketState::ConnectedState;
    if (m_pingTimer) {
        m_pingTimer->stop();
        m_pingTimer->deleteLater();
        m_pingTimer = nullptr;
    }
    if (m_pipe) {
        if (m_channelId != 0 && m_pipe->state() == QLocalSocket::ConnectedState)
            m_pipe->write(SvcProto::encode(SvcProto::Close, m_channelId));
        m_channelId = 0;
        m_pipe->disconnect(this);
        m_pipe->disconnectFromServer();
        m_pipe->deleteLater();
        m_pipe = nullptr;
    }
    m_readBuffer.clear();
    setState(QBluetoothSocket::SocketState::UnconnectedState);
    if (wasConnected)
        emit disconnected();
}

void SvcChannelSocket::sendOpen(const QBluetoothAddress &address, quint16 psm)
{
    QByteArray payload;
    payload.append(static_cast<char>(psm & 0xFF));
    payload.append(static_cast<char>((psm >> 8) & 0xFF));
    payload.append(address.toString().toUtf8());
    m_pipe->write(SvcProto::encode(SvcProto::Open, 0, payload));
}

void SvcChannelSocket::onReadyRead()
{
    m_recv.append(m_pipe->readAll());
    QVector<SvcProto::Frame> frames;
    if (!SvcProto::decode(m_recv, frames)) {
        fail(QBluetoothSocket::SocketError::NetworkError,
             QStringLiteral("corrupt svc stream"));
        return;
    }
    for (const SvcProto::Frame &frame : frames) {
        switch (frame.type) {
        case SvcProto::HelloAck:
            sendOpen(m_peer, m_psm);
            break;

        case SvcProto::OpenOk:
            m_channelId = frame.channel;
            setState(QBluetoothSocket::SocketState::ConnectedState);
            m_pingTimer = new QTimer(this);
            connect(m_pingTimer, &QTimer::timeout, this, &SvcChannelSocket::onPingTick);
            m_pingTimer->start(kPingIntervalMs);
            m_pipe->write(SvcProto::encode(SvcProto::Ping, 0));
            emit connected();
            break;

        case SvcProto::OpenErr:
            fail(QBluetoothSocket::SocketError::HostNotFoundError,
                 QStringLiteral("svc open failed: %1")
                     .arg(QString::fromUtf8(frame.payload)));
            break;

        case SvcProto::Packet:
            if (frame.channel == m_channelId) {
                m_readBuffer.append(frame.payload);
                emit packetReceived(frame.payload);
                emit readyRead();
            }
            break;

        case SvcProto::State:
            if (frame.channel == m_channelId && frame.payload.size() >= 1) {
                const auto state = static_cast<SvcProto::ChanState>(frame.payload.at(0));
                const QString err = QString::fromUtf8(frame.payload.mid(1));
                if (state == SvcProto::StateLost || state == SvcProto::StateUnconnected) {
                    m_lastError = err;
                    if (m_state == QBluetoothSocket::SocketState::ConnectedState) {
                        emit errorOccurred(QBluetoothSocket::SocketError::NetworkError);
                        setState(QBluetoothSocket::SocketState::UnconnectedState);
                        emit disconnected();
                    }
                }
            }
            break;

        case SvcProto::Pong:
            m_lastPong = QDateTime::currentDateTime();
            break;

        default:
            break;
        }
    }
}

void SvcChannelSocket::onPingTick()
{
    if (!m_pipe)
        return;
    if (m_lastPong.msecsTo(QDateTime::currentDateTime()) > kPongTimeoutMs) {
        qCWarning(svcSocket) << "svc not answering pings — treating channel as lost";
        fail(QBluetoothSocket::SocketError::NetworkError,
             QStringLiteral("svc heartbeat lost"));
        return;
    }
    m_pipe->write(SvcProto::encode(SvcProto::Ping, 0));
}

void SvcChannelSocket::onPipeDisconnected()
{
    // The svc died or was restarted — the channel it owned is gone.
    if (m_state == QBluetoothSocket::SocketState::ConnectedState ||
        m_state == QBluetoothSocket::SocketState::ConnectingState) {
        const bool wasConnected = m_state == QBluetoothSocket::SocketState::ConnectedState;
        setState(QBluetoothSocket::SocketState::UnconnectedState);
        if (wasConnected) {
            emit errorOccurred(QBluetoothSocket::SocketError::NetworkError);
            emit disconnected();
        } else {
            emit errorOccurred(QBluetoothSocket::SocketError::ServiceNotFoundError);
        }
    }
}

void SvcChannelSocket::onPipeError(QLocalSocket::LocalSocketError error)
{
    Q_UNUSED(error);
    if (m_state == QBluetoothSocket::SocketState::ConnectingState) {
        fail(QBluetoothSocket::SocketError::ServiceNotFoundError,
             QStringLiteral("svc not reachable: %1").arg(m_pipe->errorString()));
    }
}

void SvcChannelSocket::setState(QBluetoothSocket::SocketState state)
{
    m_state = state;
}

void SvcChannelSocket::fail(QBluetoothSocket::SocketError error, const QString &context)
{
    m_lastError = context;
    qCDebug(svcSocket) << context;
    setState(QBluetoothSocket::SocketState::UnconnectedState);
    emit errorOccurred(error);
}
