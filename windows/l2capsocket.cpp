#include "l2capsocket.h"
#include "logger.h"

#include <QWinEventNotifier>
#include <QPointer>
#include <QMetaObject>
#include <QtConcurrent/QtConcurrent>

#include <ws2bth.h>
#include <bthsdpdef.h>
#include <bluetoothapis.h>
#include <rpc.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "bthprops.lib")
#pragma comment(lib, "rpcrt4.lib")

namespace
{

// Fallback PSM used by AirPods for the AACP control channel when SDP lookup
// does not return a port.
constexpr ULONG kAacpFallbackPsm = 0x1001;

void ensureWsaInit()
{
    static const bool initialized = [] {
        WSADATA data;
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    Q_UNUSED(initialized);
}

bool guidFromUuid(const QBluetoothUuid &uuid, GUID &out)
{
    QString text = uuid.toString();
    if (!text.startsWith('{'))
    {
        text.prepend('{').append('}');
    }
    return UuidFromStringW(reinterpret_cast<RPC_WSTR>(const_cast<ushort *>(text.utf16())), &out) == RPC_S_OK;
}

} // namespace

L2CapSocket::L2CapSocket(QObject *parent) : BtSocketBase(parent)
{
    ensureWsaInit();
}

L2CapSocket::~L2CapSocket()
{
    close();
}

void L2CapSocket::connectToService(const QBluetoothAddress &address, const QBluetoothUuid &uuid)
{
    if (m_state == QBluetoothSocket::SocketState::ConnectedState ||
        m_state == QBluetoothSocket::SocketState::ConnectingState)
    {
        return;
    }

    // SDP resolution involves an over-the-air query that can block for
    // seconds when the device is out of range; run it off the UI thread.
    m_peer = address;
    setState(QBluetoothSocket::SocketState::ConnectingState);
    QPointer<L2CapSocket> guard(this);
    QtConcurrent::run([guard, address, uuid]() {
        bool psmOk = false;
        unsigned short psm = resolvePsm(address, uuid, psmOk);
        QMetaObject::invokeMethod(guard, [guard, address, uuid, psm, psmOk]() {
            if (guard)
            {
                guard->startConnect(address, uuid, psm, psmOk);
            }
        }, Qt::QueuedConnection);
    });
}

void L2CapSocket::connectToService(const QBluetoothAddress &address, quint16 psm)
{
    if (m_state == QBluetoothSocket::SocketState::ConnectedState ||
        m_state == QBluetoothSocket::SocketState::ConnectingState)
    {
        return;
    }

    // Bare PSM connect: no SDP query needed.
    m_peer = address;
    setState(QBluetoothSocket::SocketState::ConnectingState);
    startConnect(address, QBluetoothUuid(), psm, true);
}

void L2CapSocket::startConnect(const QBluetoothAddress &address, const QBluetoothUuid &uuid,
                               unsigned short psm, bool psmOk)
{
    if (!psmOk)
    {
        LOG_WARN("SDP lookup for " << uuid.toString() << " failed; falling back to PSM 0x"
                 << QString::number(kAacpFallbackPsm, 16));
        psm = kAacpFallbackPsm;
    }

    m_socket = WSASocketW(AF_BTH, SOCK_STREAM, BTHPROTO_L2CAP, nullptr, 0, WSA_FLAG_OVERLAPPED);
    if (m_socket == INVALID_SOCKET)
    {
        fail(QBluetoothSocket::SocketError::NetworkError, QStringLiteral("WSASocket failed"));
        return;
    }

    m_event = WSACreateEvent();
    if (m_event == WSA_INVALID_EVENT)
    {
        fail(QBluetoothSocket::SocketError::NetworkError, QStringLiteral("WSACreateEvent failed"));
        return;
    }
    WSAEventSelect(m_socket, m_event, FD_CONNECT | FD_READ | FD_WRITE | FD_CLOSE);
    m_notifier = new QWinEventNotifier(m_event, this);
    connect(m_notifier, &QWinEventNotifier::activated, this, &L2CapSocket::onSocketEvent);

    SOCKADDR_BTH sa{};
    sa.addressFamily = AF_BTH;
    sa.btAddr = address.toUInt64();
    sa.port = psm;
    GUID serviceGuid{};
    if (guidFromUuid(uuid, serviceGuid))
    {
        sa.serviceClassId = serviceGuid;
    }

    int result = ::connect(m_socket, reinterpret_cast<SOCKADDR *>(&sa), sizeof(sa));
    if (result == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK)
    {
        fail(QBluetoothSocket::SocketError::RemoteHostClosedError,
             QStringLiteral("connect failed: %1").arg(WSAGetLastError()));
        close();
    }
}

void L2CapSocket::onSocketEvent()
{
    WSANETWORKEVENTS events{};
    if (WSAEnumNetworkEvents(m_socket, m_event, &events) != 0)
    {
        return;
    }

    if (events.lNetworkEvents & FD_CONNECT)
    {
        if (events.iErrorCode[FD_CONNECT_BIT] != 0)
        {
            fail(QBluetoothSocket::SocketError::HostNotFoundError,
                 QStringLiteral("L2CAP connect failed: %1").arg(events.iErrorCode[FD_CONNECT_BIT]));
            close();
            return;
        }
        setState(QBluetoothSocket::SocketState::ConnectedState);
        emit connected();
    }

    if (events.lNetworkEvents & FD_WRITE)
    {
        flushPending();
    }

    if (events.lNetworkEvents & FD_READ)
    {
        // Drain into per-recv chunks: each emission of packetReceived carries
        // exactly one socket read (one L2CAP SDU in practice), so ATT-style
        // packet protocols keep their boundaries even when several packets
        // arrive back-to-back.
        char buffer[4096];
        int received;
        do
        {
            received = ::recv(m_socket, buffer, sizeof(buffer), 0);
            if (received > 0)
            {
                const QByteArray chunk(buffer, received);
                m_rxBuffer.append(chunk);
                emit packetReceived(chunk);
            }
            else if (received == 0)
            {
                setState(QBluetoothSocket::SocketState::UnconnectedState);
                emit disconnected();
                return;
            }
            else if (WSAGetLastError() != WSAEWOULDBLOCK)
            {
                fail(QBluetoothSocket::SocketError::NetworkError,
                     QStringLiteral("recv failed: %1").arg(WSAGetLastError()));
                return;
            }
        } while (received > 0);
        emit readyRead();
    }

    if (events.lNetworkEvents & FD_CLOSE)
    {
        setState(QBluetoothSocket::SocketState::UnconnectedState);
        emit disconnected();
    }
}

qint64 L2CapSocket::write(const QByteArray &data)
{
    if (!isOpen() || data.isEmpty())
    {
        return -1;
    }

    if (!m_pendingWrite.isEmpty())
    {
        m_pendingWrite.append(data);
        return data.size();
    }

    int sent = ::send(m_socket, data.constData(), data.size(), 0);
    if (sent == SOCKET_ERROR)
    {
        if (WSAGetLastError() == WSAEWOULDBLOCK)
        {
            m_pendingWrite = data;
            return data.size();
        }
        fail(QBluetoothSocket::SocketError::NetworkError, QStringLiteral("send failed"));
        return -1;
    }
    return sent;
}

bool L2CapSocket::flushPending()
{
    while (!m_pendingWrite.isEmpty())
    {
        int sent = ::send(m_socket, m_pendingWrite.constData(), m_pendingWrite.size(), 0);
        if (sent == SOCKET_ERROR)
        {
            if (WSAGetLastError() == WSAEWOULDBLOCK)
            {
                return false;
            }
            fail(QBluetoothSocket::SocketError::NetworkError, QStringLiteral("send failed"));
            return false;
        }
        m_pendingWrite.remove(0, sent);
    }
    return true;
}

QByteArray L2CapSocket::readAll()
{
    QByteArray result = m_rxBuffer;
    m_rxBuffer.clear();
    return result;
}

void L2CapSocket::close()
{
    if (m_notifier)
    {
        m_notifier->deleteLater();
        m_notifier = nullptr;
    }
    if (m_event)
    {
        WSAEventSelect(m_socket, m_event, 0);
        WSACloseEvent(m_event);
        m_event = nullptr;
    }
    if (m_socket != INVALID_SOCKET)
    {
        ::closesocket(m_socket);
        m_socket = INVALID_SOCKET;
    }
    m_pendingWrite.clear();
    setState(QBluetoothSocket::SocketState::UnconnectedState);
}

void L2CapSocket::setState(QBluetoothSocket::SocketState state)
{
    m_state = state;
}

void L2CapSocket::fail(QBluetoothSocket::SocketError error, const QString &context)
{
    LOG_ERROR("L2CapSocket error: " << context);
    m_lastError = context;
    emit errorOccurred(error);
}

unsigned short L2CapSocket::resolvePsm(const QBluetoothAddress &address,
                                       const QBluetoothUuid &uuid, bool &ok)
{
    ok = false;

    GUID serviceGuid{};
    if (!guidFromUuid(uuid, serviceGuid))
    {
        return 0;
    }

    // Restrict the SDP query to the remote device and request only the
    // protocol descriptor list.
    BTHNS_RESTRICTIONBLOB restriction{};
    restriction.type = SDP_SERVICE_SEARCH_ATTRIBUTE_REQUEST;
    restriction.numRange = 1;
    restriction.pRange[0].minAttribute = SDP_ATTRIB_PROTOCOL_DESCRIPTOR_LIST;
    restriction.pRange[0].maxAttribute = SDP_ATTRIB_PROTOCOL_DESCRIPTOR_LIST;
    restriction.uuids[0].uuidType = SDP_ST_UUID128;
    restriction.uuids[0].u.uuid128 = serviceGuid;

    BLOB blob{};
    blob.cbSize = sizeof(restriction);
    blob.pBlobData = reinterpret_cast<BYTE *>(&restriction);

    SOCKADDR_BTH remoteAddr{};
    remoteAddr.addressFamily = AF_BTH;
    remoteAddr.btAddr = address.toUInt64();

    CSADDR_INFO csAddr{};
    csAddr.RemoteAddr.lpSockaddr = reinterpret_cast<LPSOCKADDR>(&remoteAddr);
    csAddr.RemoteAddr.iSockaddrLength = sizeof(remoteAddr);

    WSAQUERYSETW querySet{};
    querySet.dwSize = sizeof(querySet);
    querySet.dwNameSpace = NS_BTH;
    querySet.lpBlob = &blob;
    querySet.lpcsaBuffer = &csAddr;

    HANDLE lookup = nullptr;
    if (WSALookupServiceBeginW(&querySet, LUP_FLUSHCACHE | LUP_RETURN_ADDR | LUP_RES_SERVICE,
                               &lookup) != 0)
    {
        return 0;
    }

    unsigned short psm = 0;
    DWORD bufferSize = 4096;
    QByteArray buffer(static_cast<int>(bufferSize), Qt::Uninitialized);
    auto *result = reinterpret_cast<WSAQUERYSETW *>(buffer.data());

    while (WSALookupServiceNextW(lookup, LUP_RETURN_ADDR | LUP_RES_SERVICE, &bufferSize, result) == 0)
    {
        // On success the remote sockaddr port carries the protocol channel.
        if (result->lpcsaBuffer &&
            result->lpcsaBuffer->RemoteAddr.lpSockaddr &&
            result->lpcsaBuffer->RemoteAddr.iSockaddrLength >= static_cast<int>(sizeof(SOCKADDR_BTH)))
        {
            auto *sa = reinterpret_cast<SOCKADDR_BTH *>(result->lpcsaBuffer->RemoteAddr.lpSockaddr);
            if (sa->port != 0 && sa->port <= 0xFFFF)
            {
                psm = static_cast<unsigned short>(sa->port);
                ok = true;
                break;
            }
        }
    }
    WSALookupServiceEnd(lookup);
    return psm;
}
