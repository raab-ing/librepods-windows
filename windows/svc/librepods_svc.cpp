/*
 * librepods-svc: headless channel owner for the LibrePods driver.
 *
 * The GUI process talks to this service over a named local socket; the
 * service owns every DriverL2CapSocket (driver handle, reader thread, L2CAP
 * session). A crashing or restarting GUI leaves its channels orphaned but
 * alive — within the grace window a new client adopts them by address+PSM,
 * so the pods never see the control channel drop.
 */
#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLoggingCategory>
#include <QMutex>
#include <QTextStream>
#include <QTimer>

#include "../driverl2capsocket.h"
#include "../driver/shared/librepods_driver_protocol.h"
#include "../svcprotocol.h"

Q_LOGGING_CATEGORY(svc, "librepods.svc")

static QFile g_log;
static QMutex g_logMutex;

static void logHandler(QtMsgType type, const QMessageLogContext &, const QString &msg)
{
    const QString level =
        type == QtDebugMsg ? QStringLiteral("DEBUG") :
        type == QtInfoMsg ? QStringLiteral("INFO") :
        type == QtWarningMsg ? QStringLiteral("WARN") :
        type == QtCriticalMsg ? QStringLiteral("ERROR") :
                              QStringLiteral("FATAL");
    const QString line = QStringLiteral("%1 %2 %3\n")
        .arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz")),
             level, msg);
    QMutexLocker locker(&g_logMutex);
    if (g_log.isOpen()) {
        g_log.write(line.toUtf8());
        g_log.flush();
    }
    fprintf(stderr, "%s", qUtf8Printable(line));
}

namespace {

constexpr char kPipeName[] = "librepods-svc";
// The pods' L2CAP watchdog tears the whole ACL link down ~20s after an
// unanswered connect request — secondary-channel (ATT) opens get a shorter
// leash than the driver's 15s BRB timeout so more attempts fit the window.
constexpr int kSecondaryOpenTimeoutMs = 14000; // TEST: was 4000 — see if pods answer PSM31 slowly

struct Channel {
    quint16 id = 0;
    QString address;
    quint16 psm = 0;
    DriverL2CapSocket *socket = nullptr;
    QLocalSocket *owner = nullptr;
    QDateTime orphanedAt;
    // While an OPEN is in flight the channel belongs to the requester but
    // isn't adoptable yet.
    bool connecting = false;
};

} // namespace

class SvcServer : public QObject
{
    Q_OBJECT
public:
    explicit SvcServer(QObject *parent = nullptr) : QObject(parent)
    {
        // Proof-of-life before claiming the pipe: a running svc owns it even
        // if its socket file was unlinked, and removeServer() on a live
        // listener would strand its channels with no reachable owner.
        {
            QLocalSocket probe;
            probe.connectToServer(kPipeName);
            if (probe.waitForConnected(300)) {
                probe.write(SvcProto::encode(SvcProto::Ping, 0));
                if (probe.waitForReadyRead(500)) {
                    qCInfo(svc) << "another svc instance is alive — exiting";
                    return; // listening() reports false → main() exits 1
                }
            }
        }

        m_server = new QLocalServer(this);
        QLocalServer::removeServer(kPipeName); // stale socket file from a dead svc
        if (!m_server->listen(kPipeName)) {
            qCCritical(svc) << "cannot listen on" << kPipeName << ":" << m_server->errorString();
            QCoreApplication::exit(1);
            return;
        }
        connect(m_server, &QLocalServer::newConnection, this, &SvcServer::onNewConnection);

        m_reaper = new QTimer(this);
        connect(m_reaper, &QTimer::timeout, this, &SvcServer::reap);
        m_reaper->start(5000);
    }

    bool listening() const { return m_server && m_server->isListening(); }

private slots:
    void onNewConnection()
    {
        while (QLocalSocket *client = m_server->nextPendingConnection()) {
            connect(client, &QLocalSocket::readyRead, this, &SvcServer::onClientRead);
            connect(client, &QLocalSocket::disconnected, this, &SvcServer::onClientGone);
            m_clients.insert(client);
            qCInfo(svc) << "client connected" << reinterpret_cast<quintptr>(client);
        }
    }

    void onClientGone()
    {
        auto *client = qobject_cast<QLocalSocket *>(sender());
        if (!client)
            return;
        qCInfo(svc) << "client gone" << reinterpret_cast<quintptr>(client);
        m_clients.remove(client);
        m_recvBuffers.remove(client);
        // Orphan the client's channels instead of closing them — a restarting
        // GUI adopts them within the grace window.
        for (Channel *ch : m_channels) {
            if (ch->owner == client) {
                ch->owner = nullptr;
                ch->orphanedAt = QDateTime::currentDateTime();
            }
        }
        client->deleteLater();
    }

    void onClientRead()
    {
        auto *client = qobject_cast<QLocalSocket *>(sender());
        if (!client)
            return;
        QByteArray &buffer = m_recvBuffers[client];
        buffer.append(client->readAll());
        QVector<SvcProto::Frame> frames;
        if (!SvcProto::decode(buffer, frames)) {
            qCWarning(svc) << "corrupt stream from client" << reinterpret_cast<quintptr>(client);
            client->disconnectFromServer();
            return;
        }
        for (const SvcProto::Frame &frame : frames)
            handleFrame(client, frame);
    }

    void reap()
    {
        const QDateTime now = QDateTime::currentDateTime();
        QVector<quint16> dead;
        for (Channel *ch : m_channels) {
            if (ch->owner == nullptr &&
                ch->orphanedAt.msecsTo(now) > SvcProto::kOrphanGraceMs) {
                qCInfo(svc) << "reaping orphaned channel" << ch->id << ch->address;
                dead.append(ch->id);
            }
        }
        for (quint16 id : dead)
            destroyChannel(id);

        if (m_clients.isEmpty() && m_channels.isEmpty()) {
            if (!m_idleSince.isValid())
                m_idleSince = now;
            if (m_idleSince.msecsTo(now) > SvcProto::kIdleShutdownMs) {
                qCInfo(svc) << "idle shutdown";
                QCoreApplication::quit();
            }
        } else {
            m_idleSince = QDateTime();
        }
    }

private:
    void handleFrame(QLocalSocket *client, const SvcProto::Frame &frame)
    {
        switch (frame.type) {
        case SvcProto::Hello:
            client->write(SvcProto::encode(SvcProto::HelloAck, 0,
                                           QByteArray(1, static_cast<char>(SvcProto::kVersion))));
            break;

        case SvcProto::Ping:
            client->write(SvcProto::encode(SvcProto::Pong, 0));
            break;

        case SvcProto::Open:
            onOpen(client, frame);
            break;

        case SvcProto::Close:
            if (Channel *ch = findChannel(frame.channel); ch && ch->owner == client) {
                client->write(SvcProto::encode(SvcProto::CloseAck, ch->id));
                destroyChannel(ch->id);
            }
            break;

        case SvcProto::Write:
            if (Channel *ch = findChannel(frame.channel); ch && ch->owner == client)
                ch->socket->write(frame.payload);
            break;

        default:
            qCWarning(svc) << "unknown frame type" << static_cast<int>(frame.type);
            break;
        }
    }

    void onOpen(QLocalSocket *client, const SvcProto::Frame &frame)
    {
        if (frame.payload.size() < 2) {
            sendOpenErr(client, 0, QStringLiteral("malformed open request"));
            return;
        }
        const auto *p = reinterpret_cast<const uchar *>(frame.payload.constData());
        const quint16 psm = static_cast<quint16>(p[0] | (p[1] << 8));
        const QString address = QString::fromUtf8(frame.payload.mid(2));

        // Adoption: an orphaned channel for the same peer+PSM that is still
        // connected goes straight to the requester — the pods see no drop.
        for (Channel *ch : m_channels) {
            if (ch->owner == nullptr && ch->address == address && ch->psm == psm &&
                ch->socket->state() == QBluetoothSocket::SocketState::ConnectedState) {
                ch->owner = client;
                sendOpenOk(client, ch->id, 0, /*adopted=*/true);
                qCInfo(svc) << "channel" << ch->id << "adopted for" << address;
                return;
            }
        }

        auto *socket = new DriverL2CapSocket(this);
        auto *ch = new Channel;
        ch->id = nextChannelId();
        ch->address = address;
        ch->psm = psm;
        ch->socket = socket;
        ch->owner = client;
        ch->connecting = true;
        m_channels.insert(ch->id, ch);

        const quint16 id = ch->id;
        connect(socket, &BtSocketBase::connected, this, [this, id, client]() {
            Channel *ch = m_channels.value(id);
            if (!ch)
                return;
            ch->connecting = false;
            qCInfo(svc) << "channel" << id << "open" << ch->address << "psm" << ch->psm;
            if (ch->owner)
                sendOpenOk(ch->owner, id, 0, false);
            if (ch->owner != client) {
                // Requester vanished mid-open — orphan it, still adoptable.
                ch->owner = nullptr;
                ch->orphanedAt = QDateTime::currentDateTime();
            }
        });
        connect(socket, QOverload<QBluetoothSocket::SocketError>::of(&BtSocketBase::errorOccurred),
                this, [this, id](QBluetoothSocket::SocketError) {
            Channel *ch = m_channels.value(id);
            if (!ch)
                return;
            if (ch->connecting) {
                qCInfo(svc) << "channel" << id << "open failed:" << ch->socket->errorString()
                            << "psm" << ch->psm;
                if (ch->owner)
                    sendOpenErr(ch->owner, id, ch->socket->errorString());
                destroyChannel(id);
            } else {
                qCInfo(svc) << "channel" << id << "lost:" << ch->socket->errorString();
                sendState(ch, SvcProto::StateLost, ch->socket->errorString());
            }
        });
        connect(socket, &BtSocketBase::packetReceived, this, [this, id](const QByteArray &data) {
            Channel *ch = m_channels.value(id);
            if (ch && ch->owner)
                ch->owner->write(SvcProto::encode(SvcProto::Packet, id, data));
        });
        connect(socket, &BtSocketBase::disconnected, this, [this, id]() {
            Channel *ch = m_channels.value(id);
            if (!ch)
                return;
            sendState(ch, SvcProto::StateUnconnected, QStringLiteral("disconnected"));
            destroyChannel(id);
        });

        socket->connectToService(QBluetoothAddress(address), psm);

        if (psm != 0 && psm != LIBREPODS_PSM_AACP) {
            const quint16 id = ch->id;
            QTimer::singleShot(kSecondaryOpenTimeoutMs, this, [this, id]() {
                Channel *ch = m_channels.value(id);
                if (!ch || !ch->connecting)
                    return;
                qCInfo(svc) << "channel" << id << "open timed out after"
                            << kSecondaryOpenTimeoutMs << "ms psm" << ch->psm;
                if (ch->owner)
                    sendOpenErr(ch->owner, id, QStringLiteral("open timed out"));
                destroyChannel(id);
            });
        }
    }

    Channel *findChannel(quint16 id) { return m_channels.value(id); }

    quint16 nextChannelId()
    {
        do {
            m_nextId = static_cast<quint16>(m_nextId + 1);
            if (m_nextId == 0)
                m_nextId = 1;
        } while (m_channels.contains(m_nextId));
        return m_nextId;
    }

    void destroyChannel(quint16 id)
    {
        Channel *ch = m_channels.take(id);
        if (!ch)
            return;
        ch->socket->disconnect(this);
        ch->socket->close();
        ch->socket->deleteLater();
        delete ch;
    }

    void sendOpenOk(QLocalSocket *client, quint16 id, quint32 mtu, bool adopted)
    {
        QByteArray payload(9, 0);
        auto *p = reinterpret_cast<uchar *>(payload.data());
        p[0] = static_cast<uchar>(mtu & 0xFF);
        p[1] = static_cast<uchar>((mtu >> 8) & 0xFF);
        p[2] = static_cast<uchar>((mtu >> 16) & 0xFF);
        p[3] = static_cast<uchar>((mtu >> 24) & 0xFF);
        // capability flags — driver-backed L2CAP; keep simple for now.
        p[4] = 0; p[5] = 0; p[6] = 0; p[7] = 0;
        p[8] = adopted ? 1 : 0;
        client->write(SvcProto::encode(SvcProto::OpenOk, id, payload));
    }

    void sendOpenErr(QLocalSocket *client, quint16 id, const QString &err)
    {
        client->write(SvcProto::encode(SvcProto::OpenErr, id, err.toUtf8()));
    }

    void sendState(Channel *ch, SvcProto::ChanState state, const QString &err)
    {
        if (!ch->owner)
            return;
        QByteArray payload;
        payload.append(static_cast<char>(state));
        payload.append(err.toUtf8());
        ch->owner->write(SvcProto::encode(SvcProto::State, ch->id, payload));
    }

    QLocalServer *m_server = nullptr;
    QTimer *m_reaper = nullptr;
    QSet<QLocalSocket *> m_clients;
    QHash<QLocalSocket *, QByteArray> m_recvBuffers;
    QHash<quint16, Channel *> m_channels;
    quint16 m_nextId = 0;
    QDateTime m_idleSince;
};

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("librepods-svc"));

    g_log.setFileName(QCoreApplication::applicationDirPath() + QStringLiteral("/librepods-svc.log"));
    g_log.open(QIODevice::Append | QIODevice::WriteOnly);
    qInstallMessageHandler(logHandler);

    qCInfo(svc) << "starting; driver status:" << DriverL2CapSocket::driverStatus();

    SvcServer server;
    if (!server.listening())
        return 1;

    return app.exec();
}

#include "librepods_svc.moc"
