/*
 * Driver-backed L2CAP transport for the LibrePods KMDF Bluetooth profile
 * driver. The driver sources in windows/driver/kmdf are derived from
 * librepods-windows by ivLis-Studio (GPLv3).
 */
#include "driverl2capsocket.h"

#include <QThread>
#include <QDebug>
#include <QLoggingCategory>

#include <windows.h>
#include <setupapi.h>

#include "driver/shared/librepods_driver_protocol.h"

Q_LOGGING_CATEGORY(driverSocket, "librepods.driver")

namespace {

// {3E98D13A-B2A5-4C3D-AE1E-8DB4F3D8C1F0}
const GUID LIBREPODS_INTERFACE_GUID = {
    0x3E98D13A, 0xB2A5, 0x4C3D,
    {0xAE, 0x1E, 0x8D, 0xB4, 0xF3, 0xD8, 0xC1, 0xF0}};

QString winErrorString(DWORD code)
{
    wchar_t *buf = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                       FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                   reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
    QString out = buf ? QString::fromWCharArray(buf).trimmed() : QStringLiteral("error %1").arg(code);
    if (buf)
        LocalFree(buf);
    return out;
}

bool queryStatus(void *handle, librepods_driver_status &status)
{
    DWORD returned = 0;
    memset(&status, 0, sizeof(status));
    status.struct_size = sizeof(status);
    return DeviceIoControl(handle, LIBREPODS_IOCTL_GET_BRIDGE_STATUS,
                           nullptr, 0, &status, sizeof(status), &returned, nullptr) != FALSE;
}

} // namespace

class DriverL2CapSocket::Reader : public QThread
{
    Q_OBJECT
public:
    Reader(const QString &devicePath, const QBluetoothAddress &address, quint16 psm,
           std::shared_ptr<std::atomic<bool>> closing, QObject *parent = nullptr)
        : QThread(parent), m_devicePath(devicePath), m_address(address), m_psm(psm),
          m_closing(std::move(closing))
    {
    }

    // Abort an in-flight OPEN/READ ioctl from the GUI thread. Without this a
    // wedged channel-open keeps the socket in ConnectingState forever —
    // close() would have to detach a zombie thread on every retry.
    void cancelPending()
    {
        if (HANDLE h = m_openHandle.load())
            CancelIoEx(h, nullptr);
    }

signals:
    void opened(quint64 sessionId, quint32 mtu);
    void openFailed(const QString &message);
    void packet(const QByteArray &data);
    void channelLost(const QString &message);

protected:
    void run() override
    {
        // Dedicated handle: a synchronous file object serializes ioctls, so a
        // pending READ on the shared handle would block every SEND/CLOSE from
        // the GUI thread until a packet happens to arrive.
        HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(m_devicePath.utf16()),
                                    GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            emit openFailed(winErrorString(GetLastError()));
            return;
        }
        m_openHandle.store(handle);

        librepods_driver_open_session_request request;
        librepods_driver_open_session_response response;
        memset(&request, 0, sizeof(request));
        memset(&response, 0, sizeof(response));
        request.struct_size = sizeof(request);

        QByteArray addressText = m_address.toString().toLatin1();
        snprintf(request.bluetooth_address, sizeof(request.bluetooth_address),
                 "%s", addressText.constData());
        // Protocol v2: requested L2CAP PSM (0 => driver default AACP).
        request.psm = m_psm;

        DWORD returned = 0;
        if (!DeviceIoControl(handle, LIBREPODS_IOCTL_OPEN_AACP_CHANNEL,
                             &request, sizeof(request), &response, sizeof(response),
                             &returned, nullptr)) {
            emit openFailed(winErrorString(GetLastError()));
            m_openHandle.store(nullptr);
            CloseHandle(handle);
            return;
        }
        const quint64 sessionId = response.session_id;
        emit opened(sessionId, response.mtu);

        librepods_driver_read_packet_request readRequest;
        librepods_driver_read_packet_response readResponse;
        memset(&readRequest, 0, sizeof(readRequest));
        readRequest.struct_size = sizeof(readRequest);
        readRequest.packet_buffer_length = LIBREPODS_DRIVER_MAX_AACP_PACKET_LENGTH;
        readRequest.session_id = sessionId;

        while (!m_closing->load()) {
            memset(&readResponse, 0, sizeof(readResponse));
            returned = 0;
            const BOOL ok = DeviceIoControl(handle, LIBREPODS_IOCTL_READ_AACP_PACKET,
                                          &readRequest, sizeof(readRequest),
                                          &readResponse, sizeof(readResponse), &returned, nullptr);
            if (!ok) {
                if (!m_closing->load())
                    emit channelLost(winErrorString(GetLastError()));
                break;
            }
            if (readResponse.packet_length > 0 &&
                readResponse.packet_length <= LIBREPODS_DRIVER_MAX_AACP_PACKET_LENGTH) {
                emit packet(QByteArray(reinterpret_cast<const char *>(readResponse.packet),
                                       static_cast<int>(readResponse.packet_length)));
            }
        }
        m_openHandle.store(nullptr);
        CloseHandle(handle);
    }

private:
    QString m_devicePath;
    std::atomic<HANDLE> m_openHandle{nullptr};
    QBluetoothAddress m_address;
    quint16 m_psm;
    std::shared_ptr<std::atomic<bool>> m_closing;
};

DriverL2CapSocket::DriverL2CapSocket(QObject *parent)
    : BtSocketBase(parent)
{
}

DriverL2CapSocket::~DriverL2CapSocket()
{
    close();
}

QString DriverL2CapSocket::findDevicePath()
{
    const wchar_t *envPathRaw = _wgetenv(LIBREPODS_DRIVER_DEVICE_PATH_ENVIRONMENT);
    if (envPathRaw && envPathRaw[0] != L'\0')
        return QString::fromWCharArray(envPathRaw);

    HDEVINFO devInfo = SetupDiGetClassDevsW(&LIBREPODS_INTERFACE_GUID, nullptr, nullptr,
                                            DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devInfo == INVALID_HANDLE_VALUE)
        return QString();

    SP_DEVICE_INTERFACE_DATA interfaceData;
    memset(&interfaceData, 0, sizeof(interfaceData));
    interfaceData.cbSize = sizeof(interfaceData);

    QString path;
    if (SetupDiEnumDeviceInterfaces(devInfo, nullptr, &LIBREPODS_INTERFACE_GUID, 0, &interfaceData)) {
        DWORD required = 0;
        SetupDiGetDeviceInterfaceDetailW(devInfo, &interfaceData, nullptr, 0, &required, nullptr);
        if (required > 0) {
            QVector<BYTE> buffer(required, 0);
            auto *detail = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(buffer.data());
            detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
            if (SetupDiGetDeviceInterfaceDetailW(devInfo, &interfaceData, detail, required,
                                                 nullptr, nullptr)) {
                path = QString::fromWCharArray(detail->DevicePath);
            }
        }
    }
    SetupDiDestroyDeviceInfoList(devInfo);
    return path;
}

void *DriverL2CapSocket::openHandle() const
{
    const QString path = findDevicePath();
    if (path.isEmpty())
        return nullptr;
    HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()),
                                GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    return handle == INVALID_HANDLE_VALUE ? nullptr : handle;
}

int DriverL2CapSocket::driverProtocolVersion()
{
    const QString path = findDevicePath();
    if (path.isEmpty())
        return 0;
    HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()),
                                GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return 0;
    librepods_driver_status status;
    const bool ok = queryStatus(handle, status);
    CloseHandle(handle);
    return ok ? static_cast<int>(status.protocol_version) : 0;
}

bool DriverL2CapSocket::isDriverAvailable()
{
    const QString path = findDevicePath();
    if (path.isEmpty())
        return false;
    HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()),
                                GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return false;
    librepods_driver_status status;
    const bool ok = queryStatus(handle, status);
    CloseHandle(handle);
    // Any protocol version >= 1 serves the AACP channel; v2 adds caller PSMs.
    return ok &&
           status.protocol_version >= 1 &&
           (status.capability_flags & LIBREPODS_DRIVER_CAPABILITY_DRIVER_BACKED_L2CAP) != 0;
}

bool DriverL2CapSocket::isAttSupported()
{
    return isDriverAvailable() && driverProtocolVersion() >= 2;
}

QString DriverL2CapSocket::driverStatus()
{
    const QString path = findDevicePath();
    if (path.isEmpty())
        return QStringLiteral("no LibrePods driver device interface found");
    HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()),
                                GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return QStringLiteral("cannot open %1: %2").arg(path, winErrorString(GetLastError()));
    librepods_driver_status status;
    const bool ok = queryStatus(handle, status);
    CloseHandle(handle);
    if (!ok)
        return QStringLiteral("status query failed: %1").arg(winErrorString(GetLastError()));
    return QStringLiteral("version=%1 capabilities=0x%2 maxPacket=%3 lastError=%4")
        .arg(status.protocol_version)
        .arg(status.capability_flags, 0, 16)
        .arg(status.max_packet_size)
        .arg(status.last_error);
}

void DriverL2CapSocket::connectToService(const QBluetoothAddress &address, const QBluetoothUuid &uuid)
{
    Q_UNUSED(uuid); // the AACP channel is the driver's default PSM
    connectInternal(address, 0);
}

void DriverL2CapSocket::connectToService(const QBluetoothAddress &address, quint16 psm)
{
    connectInternal(address, psm);
}

void DriverL2CapSocket::connectInternal(const QBluetoothAddress &address, quint16 psm)
{
    if (m_state == QBluetoothSocket::SocketState::ConnectedState ||
        m_state == QBluetoothSocket::SocketState::ConnectingState) {
        return;
    }

    // A failed open or a dropped channel leaves a finished reader object,
    // the GUI-side device handle, and unread bytes behind. Nothing on the
    // fail()/channelLost paths tears those down, so a retry would leak one
    // handle per attempt and could deliver stale packets on the new channel.
    stopReader();
    if (m_handle) {
        CloseHandle(m_handle);
        m_handle = nullptr;
    }
    m_sessionId = 0;
    {
        QMutexLocker locker(&m_bufferMutex);
        m_readBuffer.clear();
    }

    m_devicePath = findDevicePath();
    if (m_devicePath.isEmpty()) {
        fail(QBluetoothSocket::SocketError::ServiceNotFoundError,
             QStringLiteral("LibrePods driver device not available"));
        return;
    }

    m_handle = openHandle();
    if (!m_handle) {
        fail(QBluetoothSocket::SocketError::ServiceNotFoundError,
             QStringLiteral("LibrePods driver device not available"));
        return;
    }

    m_peer = address;
    m_psm = psm;
    // Fresh flag per connection: a detached reader from a previous attempt
    // must keep seeing 'closing' so it exits when its ioctl returns.
    m_closing = std::make_shared<std::atomic<bool>>(false);
    setState(QBluetoothSocket::SocketState::ConnectingState);
    startReader();
}

void DriverL2CapSocket::startReader()
{
    m_reader = new Reader(m_devicePath, m_peer, m_psm, m_closing, this);
    connect(m_reader, &Reader::opened, this, [this](quint64 sessionId, quint32 mtu) {
        m_sessionId = sessionId;
        qCDebug(driverSocket) << "AACP channel open, session" << sessionId << "mtu" << mtu;
        setState(QBluetoothSocket::SocketState::ConnectedState);
        emit connected();
    });
    connect(m_reader, &Reader::openFailed, this, [this](const QString &message) {
        fail(QBluetoothSocket::SocketError::HostNotFoundError,
             QStringLiteral("AACP channel open failed: %1").arg(message));
    });
    connect(m_reader, &Reader::packet, this, [this](const QByteArray &data) {
        {
            QMutexLocker locker(&m_bufferMutex);
            m_readBuffer.append(data);
        }
        emit packetReceived(data);
        emit readyRead();
    });
    connect(m_reader, &Reader::channelLost, this, [this](const QString &message) {
        m_lastError = message;
        qCDebug(driverSocket) << "channel lost:" << message;
        if (m_state == QBluetoothSocket::SocketState::ConnectedState) {
            emit errorOccurred(QBluetoothSocket::SocketError::NetworkError);
            setState(QBluetoothSocket::SocketState::UnconnectedState);
            emit disconnected();
        }
    });
    m_reader->start();
}

void DriverL2CapSocket::stopReader()
{
    if (!m_reader)
        return;
    m_closing->store(true);
    if (m_sessionId != 0 && m_handle) {
        // Closing the channel makes the in-flight READ ioctl return immediately.
        QMutexLocker locker(&m_sendMutex);
        librepods_driver_close_session_request request;
        memset(&request, 0, sizeof(request));
        request.struct_size = sizeof(request);
        request.session_id = m_sessionId;
        DWORD returned = 0;
        DeviceIoControl(m_handle, LIBREPODS_IOCTL_CLOSE_AACP_CHANNEL,
                        &request, sizeof(request), nullptr, 0, &returned, nullptr);
        m_sessionId = 0;
    }
    // A reader blocked in a READ returns promptly once the channel close
    // flushes it; a reader stuck in the OPEN ioctl gets its synchronous
    // DeviceIoControl aborted by CancelIoEx on its dedicated handle.
    m_reader->cancelPending();
    if (!m_reader->wait(500)) {
        disconnect(m_reader, nullptr, this, nullptr);
        connect(m_reader, &QThread::finished, m_reader, &QObject::deleteLater);
        m_reader->setParent(nullptr);
    } else {
        m_reader->deleteLater();
    }
    m_reader = nullptr;
}

void DriverL2CapSocket::close()
{
    const bool wasConnected = m_state == QBluetoothSocket::SocketState::ConnectedState;
    setState(QBluetoothSocket::SocketState::UnconnectedState);
    stopReader();
    {
        QMutexLocker locker(&m_bufferMutex);
        m_readBuffer.clear();
    }
    if (m_handle) {
        CloseHandle(m_handle);
        m_handle = nullptr;
    }
    if (wasConnected)
        emit disconnected();
}

qint64 DriverL2CapSocket::write(const QByteArray &data)
{
    if (!isOpen() || !m_handle || m_sessionId == 0)
        return -1;
    if (data.size() > static_cast<int>(LIBREPODS_DRIVER_MAX_AACP_PACKET_LENGTH)) {
        m_lastError = QStringLiteral("packet exceeds driver MTU");
        return -1;
    }

    librepods_driver_send_packet_request request;
    memset(&request, 0, sizeof(request));
    request.struct_size = sizeof(request);
    request.packet_length = static_cast<librepods_u32>(data.size());
    request.session_id = m_sessionId;
    memcpy(request.packet, data.constData(), data.size());

    QMutexLocker locker(&m_sendMutex);
    DWORD returned = 0;
    if (!DeviceIoControl(m_handle, LIBREPODS_IOCTL_SEND_AACP_PACKET,
                         &request, sizeof(request), nullptr, 0, &returned, nullptr)) {
        m_lastError = winErrorString(GetLastError());
        return -1;
    }
    return data.size();
}

QByteArray DriverL2CapSocket::readAll()
{
    QMutexLocker locker(&m_bufferMutex);
    QByteArray data = m_readBuffer;
    m_readBuffer.clear();
    return data;
}

void DriverL2CapSocket::setState(QBluetoothSocket::SocketState state)
{
    m_state = state;
}

void DriverL2CapSocket::fail(QBluetoothSocket::SocketError error, const QString &context)
{
    m_lastError = context;
    qCDebug(driverSocket) << context;
    setState(QBluetoothSocket::SocketState::UnconnectedState);
    emit errorOccurred(error);
}

#include "driverl2capsocket.moc"
