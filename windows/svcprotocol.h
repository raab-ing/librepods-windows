#ifndef LIBREPODS_SVCPROTOCOL_H
#define LIBREPODS_SVCPROTOCOL_H

/*
 * IPC protocol between librepods (GUI) and librepods-svc (channel owner).
 *
 * The svc owns every driver handle and channel; the GUI talks to it over a
 * named local socket. When the GUI crashes or restarts, svc-side channels
 * stay alive long enough to be adopted by the next client, so the pods
 * never see the control channel drop.
 *
 * Frame layout (little-endian):
 *   [0]    u8  magic 0x4C ('L')
 *   [1]    u8  message type
 *   [2..3] u16 channel id (0 for connection-level messages)
 *   [4..5] u16 payload length
 *   [6..]  payload
 */

#include <QtGlobal>
#include <QByteArray>
#include <QDataStream>

namespace SvcProto {

constexpr quint8 kMagic = 0x4C;
constexpr quint8 kVersion = 1;
constexpr int kHeaderSize = 6;
constexpr int kMaxPayload = 1024;
// Orphaned channels (owner GUI gone) stay adoptable this long before svc
// closes them for real.
constexpr int kOrphanGraceMs = 60000;
// An idle svc with zero clients and zero channels exits after this.
constexpr int kIdleShutdownMs = 10 * 60 * 1000;

enum MsgType : quint8 {
    // client -> svc
    Hello  = 0x01,   // payload: {version:u8}
    Open   = 0x02,   // payload: {psm:u16, addr:utf8}
    Close  = 0x03,   // channel: id
    Write  = 0x04,   // channel: id, payload: {data}
    Ping   = 0x05,

    // svc -> client
    HelloAck = 0x81, // payload: {version:u8}
    OpenOk   = 0x82, // channel: id, payload: {mtu:u32, caps:u32, adopted:u8}
    OpenErr  = 0x83, // channel: echoed request id, payload: {err:utf8}
    Packet   = 0x84, // channel: id, payload: {data}
    State    = 0x85, // channel: id, payload: {state:u8, err:utf8}
    Pong     = 0x86,
    CloseAck = 0x87, // channel: id
};

enum ChanState : quint8 {
    StateUnconnected = 0,
    StateConnecting  = 1,
    StateConnected   = 2,
    StateLost        = 3,
};

inline QByteArray encode(MsgType type, quint16 channel, const QByteArray &payload = {})
{
    QByteArray frame;
    frame.reserve(kHeaderSize + payload.size());
    frame.append(static_cast<char>(kMagic));
    frame.append(static_cast<char>(type));
    frame.append(static_cast<char>(channel & 0xFF));
    frame.append(static_cast<char>((channel >> 8) & 0xFF));
    frame.append(static_cast<char>(payload.size() & 0xFF));
    frame.append(static_cast<char>((payload.size() >> 8) & 0xFF));
    frame.append(payload);
    return frame;
}

struct Frame {
    MsgType type;
    quint16 channel;
    QByteArray payload;
};

// Consumes complete frames out of `buffer` (accumulated socket bytes),
// leaving any partial frame behind. Returns false on stream corruption
// (bad magic / oversized payload) so the caller can drop the connection.
inline bool decode(QByteArray &buffer, QVector<Frame> &out)
{
    out.clear();
    while (buffer.size() >= kHeaderSize) {
        const auto *p = reinterpret_cast<const uchar *>(buffer.constData());
        if (p[0] != kMagic)
            return false;
        const quint16 len = static_cast<quint16>(p[4] | (p[5] << 8));
        if (len > kMaxPayload)
            return false;
        if (buffer.size() < kHeaderSize + len)
            return true; // incomplete — wait for more
        Frame f;
        f.type = static_cast<MsgType>(p[1]);
        f.channel = static_cast<quint16>(p[2] | (p[3] << 8));
        f.payload = buffer.mid(kHeaderSize, len);
        buffer.remove(0, kHeaderSize + len);
        out.append(f);
    }
    return true;
}

} // namespace SvcProto

#endif // LIBREPODS_SVCPROTOCOL_H
