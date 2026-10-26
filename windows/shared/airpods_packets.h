// airpods_packets.h
#ifndef AIRPODS_PACKETS_H
#define AIRPODS_PACKETS_H

#include <QByteArray>
#include <optional>
#include <climits>

#include "enums.h"
#include "BasicControlCommand.hpp"

namespace AirPodsPackets
{
    // Noise Control Mode Packets
    namespace NoiseControl
    {
        using NoiseControlMode = AirpodsTrayApp::Enums::NoiseControlMode;
        static const QByteArray HEADER = ControlCommand::header() + 0x0D;
        static const QByteArray OFF = ControlCommand::createCommand(0x0D, 0x01);
        static const QByteArray NOISE_CANCELLATION = ControlCommand::createCommand(0x0D, 0x02);
        static const QByteArray TRANSPARENCY = ControlCommand::createCommand(0x0D, 0x03);
        static const QByteArray ADAPTIVE = ControlCommand::createCommand(0x0D, 0x04);

        static const QByteArray getPacketForMode(AirpodsTrayApp::Enums::NoiseControlMode mode)
        {
            switch (mode)
            {
            case NoiseControlMode::Off:
                return OFF;
            case NoiseControlMode::NoiseCancellation:
                return NOISE_CANCELLATION;
            case NoiseControlMode::Transparency:
                return TRANSPARENCY;
            case NoiseControlMode::Adaptive:
                return ADAPTIVE;
            default:
                return QByteArray();
            }
        }

        inline std::optional<NoiseControlMode> parseMode(const QByteArray &data)
        {
            char mode = ControlCommand::parseActive(data).value_or(CHAR_MAX) - 1;
            if (mode < static_cast<quint8>(NoiseControlMode::MinValue) ||
                mode > static_cast<quint8>(NoiseControlMode::MaxValue))
            {
                return std::nullopt;
            }
            return static_cast<NoiseControlMode>(mode);
        }
    }

    // One Bud ANC Mode
    namespace OneBudANCMode
    {
        using Type = BasicControlCommand<0x1B>;
        static const QByteArray ENABLED = Type::enabled();
        static const QByteArray DISABLED = Type::disabled();
        static const QByteArray HEADER = Type::header();
        inline std::optional<bool> parseState(const QByteArray &data) { return Type::parseState(data); }
    }

    // Volume Swipe (partial - still needs custom interval function)
    namespace VolumeSwipe
    {
        using Type = BasicControlCommand<0x25>;
        static const QByteArray ENABLED = Type::enabled();
        static const QByteArray DISABLED = Type::disabled();
        static const QByteArray HEADER = Type::header();
        inline std::optional<bool> parseState(const QByteArray &data) { return Type::parseState(data); }

        // Keep custom interval function
        static QByteArray getIntervalPacket(quint8 interval)
        {
            return ControlCommand::createCommand(0x23, interval);
        }
    }

    // Adaptive Volume Config
    namespace AdaptiveVolume
    {
        using Type = BasicControlCommand<0x26>;
        static const QByteArray ENABLED = Type::enabled();
        static const QByteArray DISABLED = Type::disabled();
        static const QByteArray HEADER = Type::header();
        inline std::optional<bool> parseState(const QByteArray &data) { return Type::parseState(data); }
    }

    // Conversational Awareness
    namespace ConversationalAwareness
    {
        using Type = BasicControlCommand<0x28>;
        static const QByteArray ENABLED = Type::enabled();
        static const QByteArray DISABLED = Type::disabled();
        static const QByteArray HEADER = Type::header();
        static const QByteArray DATA_HEADER = QByteArray::fromHex("040004004B00020001");
        inline std::optional<bool> parseState(const QByteArray &data) { return Type::parseState(data); }
    }

    // Hearing Assist
    namespace HearingAssist
    {
        using Type = BasicControlCommand<0x33>;
        static const QByteArray ENABLED = Type::enabled();
        static const QByteArray DISABLED = Type::disabled();
        static const QByteArray HEADER = Type::header();
        inline std::optional<bool> parseState(const QByteArray &data) { return Type::parseState(data); }
    }

    // Hearing Aid
    namespace HearingAid
    {
        static const QByteArray HEADER = ControlCommand::header() + static_cast<char>(0x2C);
        static const QByteArray ENABLED = ControlCommand::createCommand(0x2C, 0x01, 0x01);
        static const QByteArray DISABLED = ControlCommand::createCommand(0x2C, 0x02, 0x02);

        inline std::optional<bool> parseState(const QByteArray &data)
        {
            if (!data.startsWith(HEADER) || data.size() < HEADER.size() + 2)
                return std::nullopt;

            QByteArray value = data.mid(HEADER.size(), 2);
            if (value.size() != 2)
                return std::nullopt;

            char b1 = value.at(0);
            char b2 = value.at(1);

            if (b1 == 0x01 && b2 == 0x01)
                return true;
            if (b1 == 0x02 || b2 == 0x02)
                return false;

            return std::nullopt;
        }
    }

    // Allow Off Option
    namespace AllowOffOption
    {
        using Type = BasicControlCommand<0x34>;
        static const QByteArray ENABLED = Type::enabled();
        static const QByteArray DISABLED = Type::disabled();
        static const QByteArray HEADER = Type::header();
        inline std::optional<bool> parseState(const QByteArray &data) { return Type::parseState(data); }
    }

    // Microphone mode — control id 0x01: which pod's mic is live.
    // 0x00 automatic, 0x01 always right, 0x02 always left.
    namespace MicMode
    {
        inline const QByteArray &header()
        {
            static const QByteArray h = ControlCommand::header() + static_cast<char>(0x01);
            return h;
        }
        inline QByteArray packet(quint8 mode) { return ControlCommand::createCommand(0x01, mode); }
    }

    // Pod-side ear detection — control id 0x0A. Toggles the pods' own
    // in-ear sensors at the firmware level, independent of the app's
    // pause-on-removal behavior.
    namespace EarDetectionConfig
    {
        using Type = BasicControlCommand<0x0A>;
        static const QByteArray ENABLED = Type::enabled();
        static const QByteArray DISABLED = Type::disabled();
        static const QByteArray HEADER = Type::header();
        inline std::optional<bool> parseState(const QByteArray &data) { return Type::parseState(data); }
    }

    // Double-press speed — control id 0x17: 0 default, 1 slower, 2 slowest.
    namespace PressSpeed
    {
        inline const QByteArray &header()
        {
            static const QByteArray h = ControlCommand::header() + static_cast<char>(0x17);
            return h;
        }
        inline QByteArray packet(quint8 speed) { return ControlCommand::createCommand(0x17, speed); }
    }

    // Press-and-hold duration — control id 0x18: 0 default, 1 longer, 2 longest.
    namespace HoldDuration
    {
        inline const QByteArray &header()
        {
            static const QByteArray h = ControlCommand::header() + static_cast<char>(0x18);
            return h;
        }
        inline QByteArray packet(quint8 dur) { return ControlCommand::createCommand(0x18, dur); }
    }

    // Listening-mode cycle — control id 0x1A: bitmask of which modes stem
    // long-press cycles through. 0x01 Off, 0x02 ANC, 0x04 Transparency,
    // 0x08 Adaptive.
    namespace ListeningModeCycle
    {
        inline const QByteArray &header()
        {
            static const QByteArray h = ControlCommand::header() + static_cast<char>(0x1A);
            return h;
        }
        inline QByteArray packet(quint8 mask) { return ControlCommand::createCommand(0x1A, mask); }
    }

    // Digital Crown rotation direction — control id 0x1C (AirPods Max).
    namespace CrownDirection
    {
        inline const QByteArray &header()
        {
            static const QByteArray h = ControlCommand::header() + static_cast<char>(0x1C);
            return h;
        }
        inline QByteArray packet(quint8 dir) { return ControlCommand::createCommand(0x1C, dir); }
    }

    // Auto-answer mode — control id 0x1E (semantics unverified; parse+log only).
    namespace AutoAnswer
    {
        inline const QByteArray &header()
        {
            static const QByteArray h = ControlCommand::header() + static_cast<char>(0x1E);
            return h;
        }
    }

    // Siri chime volume — control id 0x1F: [volume 0-100, 0x50].
    namespace ChimeVolume
    {
        inline const QByteArray &header()
        {
            static const QByteArray h = ControlCommand::header() + static_cast<char>(0x1F);
            return h;
        }
        inline QByteArray packet(quint8 vol) { return ControlCommand::createCommand(0x1F, vol, 0x50); }
    }

    // Automatic connection — control id 0x20: pods auto-connect to this
    // host when it comes in range.
    namespace AutoConnection
    {
        using Type = BasicControlCommand<0x20>;
        static const QByteArray ENABLED = Type::enabled();
        static const QByteArray DISABLED = Type::disabled();
        static const QByteArray HEADER = Type::header();
        inline std::optional<bool> parseState(const QByteArray &data) { return Type::parseState(data); }
    }

    // Call management — control id 0x24: [0x00, mapping]. 0x03 = default
    // (mute on double press), 0x02 = swapped (mute on single press).
    namespace CallManagement
    {
        inline const QByteArray &header()
        {
            static const QByteArray h = ControlCommand::header() + static_cast<char>(0x24);
            return h;
        }
        inline QByteArray packet(bool muteOnSingle)
        {
            return ControlCommand::createCommand(0x24, 0x00, muteOnSingle ? 0x02 : 0x03);
        }
        // Inbound value at byte 8 (two-byte payload).
        inline std::optional<bool> muteOnSingle(const QByteArray &data)
        {
            if (!data.startsWith(header()) || data.size() < 9)
                return std::nullopt;
            return static_cast<quint8>(data.at(8)) == 0x02;
        }
    }

    // Software mute — control id 0x27 (semantics unverified; parse+log only).
    namespace SoftwareMute
    {
        inline const QByteArray &header()
        {
            static const QByteArray h = ControlCommand::header() + static_cast<char>(0x27);
            return h;
        }
    }

    // HPS gain swipe — control id 0x2F: volume-swipe gesture while hearing
    // aid is active adjusts gain instead of media volume.
    namespace HearingGainSwipe
    {
        using Type = BasicControlCommand<0x2F>;
        static const QByteArray ENABLED = Type::enabled();
        static const QByteArray DISABLED = Type::disabled();
        static const QByteArray HEADER = Type::header();
        inline std::optional<bool> parseState(const QByteArray &data) { return Type::parseState(data); }
    }

    // In-case tone — control id 0x31: speaker sounds for Find My etc.
    namespace InCaseTone
    {
        using Type = BasicControlCommand<0x31>;
        static const QByteArray ENABLED = Type::enabled();
        static const QByteArray DISABLED = Type::disabled();
        static const QByteArray HEADER = Type::header();
        inline std::optional<bool> parseState(const QByteArray &data) { return Type::parseState(data); }
    }

    // Siri multitone — control id 0x32: chime sounds for Siri interactions.
    namespace SiriMultitone
    {
        using Type = BasicControlCommand<0x32>;
        static const QByteArray ENABLED = Type::enabled();
        static const QByteArray DISABLED = Type::disabled();
        static const QByteArray HEADER = Type::header();
        inline std::optional<bool> parseState(const QByteArray &data) { return Type::parseState(data); }
    }

    // Sleep detection — control id 0x35: pods pause media when they
    // detect you've fallen asleep.
    namespace SleepDetection
    {
        using Type = BasicControlCommand<0x35>;
        static const QByteArray ENABLED = Type::enabled();
        static const QByteArray DISABLED = Type::disabled();
        static const QByteArray HEADER = Type::header();
        inline std::optional<bool> parseState(const QByteArray &data) { return Type::parseState(data); }
    }

    // Allow auto-connect — control id 0x36 (observed by Android; no UI).
    namespace AllowAutoConnect
    {
        using Type = BasicControlCommand<0x36>;
        static const QByteArray ENABLED = Type::enabled();
        static const QByteArray DISABLED = Type::disabled();
        static const QByteArray HEADER = Type::header();
        inline std::optional<bool> parseState(const QByteArray &data) { return Type::parseState(data); }
    }

    // Loud sound reduction (PPE) — control id 0x37: cap transients over
    // the configured level.
    namespace LoudSoundReduction
    {
        using Type = BasicControlCommand<0x37>;
        static const QByteArray ENABLED = Type::enabled();
        static const QByteArray DISABLED = Type::disabled();
        static const QByteArray HEADER = Type::header();
        inline std::optional<bool> parseState(const QByteArray &data) { return Type::parseState(data); }
    }

    // PPE cap level — control id 0x38: 0-100 reduction ceiling.
    namespace PpeCapLevel
    {
        inline const QByteArray &header()
        {
            static const QByteArray h = ControlCommand::header() + static_cast<char>(0x38);
            return h;
        }
        inline QByteArray packet(quint8 level) { return ControlCommand::createCommand(0x38, level); }
    }

    // Optimized charging — control id 0x3B: dynamic end-of-charge so the
    // case tops off at ~80% until needed.
    namespace OptimizedCharging
    {
        using Type = BasicControlCommand<0x3B>;
        static const QByteArray ENABLED = Type::enabled();
        static const QByteArray DISABLED = Type::disabled();
        static const QByteArray HEADER = Type::header();
        inline std::optional<bool> parseState(const QByteArray &data) { return Type::parseState(data); }
    }

    // Connection Packets
    namespace Connection
    {
        static const QByteArray HANDSHAKE = QByteArray::fromHex("00000400010002000000000000000000");
        static const QByteArray SET_SPECIFIC_FEATURES = QByteArray::fromHex("040004004d00d700000000000000");
        static const QByteArray REQUEST_NOTIFICATIONS = QByteArray::fromHex("040004000f00ffffffffff");
        // Opcode 0x29 (SSL) raw packet, sent after the notification request
        // on connect — "enables setting EQ": without it band writes ACK but
        // don't apply. Wire format must match Android's sendDataPacket
        // exactly: opcode 0x29 + payload 00 00 ff*6. Wrapping it as a
        // control-command (09 00 29 ...) produces an oversized value field
        // the pods treat as a protocol violation — they answer the packet
        // but drop the whole ACL ~22s later.
        static const QByteArray EQ_FEATURE_ENABLE = QByteArray::fromHex("04000400290000ffffffffffff");
        static const QByteArray AIRPODS_DISCONNECTED = QByteArray::fromHex("00010000");
    }

    // Phone Communication Packets
    namespace Phone
    {
        static const QByteArray NOTIFICATION = QByteArray::fromHex("00040001");
        static const QByteArray CONNECTED = QByteArray::fromHex("00010001");
        static const QByteArray DISCONNECTED = QByteArray::fromHex("00010000");
        static const QByteArray STATUS_REQUEST = QByteArray::fromHex("00020003");
        static const QByteArray DISCONNECT_REQUEST = QByteArray::fromHex("00020000");
    }

    // Adaptive Noise Packets
    namespace AdaptiveNoise
    {
        const QByteArray HEADER = QByteArray::fromHex("0400040009002E");

        inline QByteArray getPacket(int level)
        {
            return HEADER + static_cast<char>(level) + QByteArray::fromHex("000000");
        }
    }

    // Head Tracking (AACP opcode 0x17). The "alternate" variants are what
    // the Android app defaults to; the classic variant differs only in a
    // few bytes and exists upstream if ever needed.
    namespace HeadTracking
    {
        static const QByteArray HEADER = QByteArray::fromHex("0400040017");
        static const QByteArray START = QByteArray::fromHex(
            "040004001700000010000f000873420b081010021a0501409c000000");
        static const QByteArray STOP = QByteArray::fromHex(
            "040004001700000010000f000875420b081010021a050100000000");

        // Streaming packets are >= 70 bytes. Verified against live captures:
        //   offsets 43-49  orientation components o1..o4; HeadOrientation
        //                  derives yaw/pitch from the first three per the
        //                  Android reference (a quaternion decode was tried
        //                  and reverted — nods split across Euler axes)
        //   offsets 51/53  horizontal/vertical acceleration (head-gesture
        //                  input), little-endian int16
        struct Sample
        {
            int o1 = 0, o2 = 0, o3 = 0, o4 = 0;
            int accelHorizontal = 0, accelVertical = 0;
        };

        inline bool parseSample(const QByteArray &data, Sample &out)
        {
            if (data.size() < 70 || !data.startsWith(HEADER))
                return false;
            auto i16 = [&data](int at) -> int {
                return static_cast<qint16>(static_cast<quint8>(data.at(at)) |
                                           (static_cast<quint8>(data.at(at + 1)) << 8));
            };
            out.o1 = i16(43);
            out.o2 = i16(45);
            out.o3 = i16(47);
            out.o4 = i16(49);
            out.accelHorizontal = i16(51);
            out.accelVertical = i16(53);
            return true;
        }
    }

    // Stem press events (AACP opcode 0x19): 8-byte packet, press type at
    // offset 6, bud at offset 7 — per Android's parseStemPressResponse.
    namespace StemPress
    {
        inline const QByteArray &header()
        {
            static const QByteArray h = QByteArray::fromHex("0400040019");
            return h;
        }

        enum class Type : quint8 { Single = 0x05, Double = 0x06, Triple = 0x07, Long = 0x08 };
        enum class Bud : quint8 { Left = 0x01, Right = 0x02 };

        inline bool parse(const QByteArray &data, Type &type, Bud &bud)
        {
            if (data.size() != 8 || !data.startsWith(header()))
                return false;
            const quint8 t = static_cast<quint8>(data.at(6));
            const quint8 b = static_cast<quint8>(data.at(7));
            switch (t)
            {
            case 0x05: type = Type::Single; break;
            case 0x06: type = Type::Double; break;
            case 0x07: type = Type::Triple; break;
            case 0x08: type = Type::Long; break;
            default: return false;
            }
            switch (b)
            {
            case 0x01: bud = Bud::Left; break;
            case 0x02: bud = Bud::Right; break;
            default: return false;
            }
            return true;
        }
    }

    // STEM_CONFIG (control id 0x39) — press-interception bitmask. Outbound:
    // tells the pods which press types we handle so their native action
    // doesn't double-fire. Inbound reports use the same mask —
    // bit0 single, bit1 double, bit2 triple, bit3 long.
    namespace StemConfig
    {
        inline const QByteArray &header()
        {
            static const QByteArray h = ControlCommand::header() + static_cast<char>(0x39);
            return h;
        }
        inline quint8 mask(const QByteArray &data)
        {
            return data.size() > 7 ? static_cast<quint8>(data.at(7)) : 0;
        }
    }

    // AACP control-command identifier 0x06 — OWNS_CONNECTION: reports and
    // requests which host owns the pods' audio session (TiPi).
    namespace Ownership
    {
        inline const QByteArray &header()
        {
            static const QByteArray h = ControlCommand::header() + static_cast<char>(0x06);
            return h;
        }
        inline QByteArray ownsPacket() { return ControlCommand::createCommand(0x06, 0x01); }
        inline QByteArray releasePacket() { return ControlCommand::createCommand(0x06, 0x00); }

        // Inbound: control command carrying id 0x06; value byte 7 is 0x01
        // when the pod reports our host as owner.
        inline std::optional<bool> parse(const QByteArray &data)
        {
            if (data.size() < 8 || !data.startsWith(header()))
                return std::nullopt;
            return static_cast<quint8>(data.at(7)) == 0x01;
        }
    }

    // AUDIO_SOURCE (AACP opcode 0x0E): which device currently feeds audio to
    // the pods. MAC bytes at 6-11 are reversed; source type at 12 —
    // 0=none, 1=call, 2=media (Android's parseAudioSourceResponse).
    namespace AudioSource
    {
        inline const QByteArray &header()
        {
            static const QByteArray h = QByteArray::fromHex("040004000E");
            return h;
        }

        enum class Type : quint8 { None = 0x00, Call = 0x01, Media = 0x02 };

        inline bool parse(const QByteArray &data, QString &mac, Type &type)
        {
            if (data.size() < 13 || !data.startsWith(header()))
                return false;
            QStringList parts;
            for (int i = 11; i >= 6; --i)
                parts << QString::number(static_cast<quint8>(data.at(i)), 16)
                             .rightJustified(2, '0').toUpper();
            mac = parts.join(':');
            const quint8 t = static_cast<quint8>(data.at(12));
            type = t == 0x01 ? Type::Call : t == 0x02 ? Type::Media : Type::None;
            return true;
        }
    }

    // CONNECTED_DEVICES (AACP opcode 0x2E, "TiPi"): the list of hosts
    // attached to the pods' Apple multidevice session. Count at offset 8,
    // then 8-byte entries: 6-byte MAC + 2 info bytes.
    namespace ConnectedDevices
    {
        inline const QByteArray &header()
        {
            static const QByteArray h = QByteArray::fromHex("040004002E");
            return h;
        }

        inline QStringList parse(const QByteArray &data)
        {
            QStringList devices;
            if (data.size() < 9 || !data.startsWith(header()))
                return devices;
            const int count = static_cast<quint8>(data.at(8));
            int offset = 9;
            for (int i = 0; i < count; ++i)
            {
                if (offset + 6 > data.size())
                    break;
                QStringList parts;
                for (int b = 0; b < 6; ++b)
                    parts << QString::number(static_cast<quint8>(data.at(offset + b)), 16)
                                 .rightJustified(2, '0').toUpper();
                devices << parts.join(':');
                offset += 8;
            }
            return devices;
        }
    }

    // CUSTOM_EQ (AACP opcode 0x63): 3-band custom equalizer — packet is
    // `63 00 | 05 00 (LE len) | 01 | state | low | mid | high`, state 2 =
    // enabled, band values 0-100 (Android's data/CustomEq.kt).
    namespace CustomEq
    {
        inline const QByteArray &header()
        {
            static const QByteArray h = QByteArray::fromHex("0400040063");
            return h;
        }

        inline QByteArray packet(bool enabled, int low, int mid, int high)
        {
            QByteArray p = header();
            p.append(char(0x00));
            p.append(char(0x05)); p.append(char(0x00)); // payload length LE = 5
            p.append(char(0x01));
            p.append(char(enabled ? 0x02 : 0x01));
            p.append(char(qBound(0, low, 100)));
            p.append(char(qBound(0, mid, 100)));
            p.append(char(qBound(0, high, 100)));
            return p;
        }

        inline bool parse(const QByteArray &data, bool &enabled, int &low, int &mid, int &high)
        {
            // After the 6-byte header: len(2), 0x01, state, low, mid, high
            if (data.size() < 13 || !data.startsWith(header()))
                return false;
            enabled = static_cast<quint8>(data.at(9)) == 0x02;
            low = static_cast<quint8>(data.at(10));
            mid = static_cast<quint8>(data.at(11));
            high = static_cast<quint8>(data.at(12));
            return true;
        }
    }

    // SMART_ROUTING (AACP opcode 0x10) — TiPi packets for the multidevice
    // session takeover. Layouts ported from Android's
    // createMediaInformationPacket / createHijackRequestPacket.
    namespace SmartRouting
    {
        inline QByteArray macBytes(const QString &mac)
        {
            const QStringList parts = mac.split(':');
            QByteArray out;
            for (int i = parts.size() - 1; i >= 0; --i)
                out.append(static_cast<char>(parts.at(i).toInt(nullptr, 16)));
            return out;
        }

        // 138-byte media-information announcement addressed to a peer host.
        // The 2-byte field after the MAC is the tail size; Android's buffer
        // is zero-padded to the fixed size, so the trailing zeros matter.
        inline QByteArray mediaInformation(const QString &selfMac, const QString &targetMac,
                                           const QString &hostName, bool streaming = false)
        {
            QByteArray tail;
            tail.append(QByteArray::fromHex("01E54A"));
            tail.append("PlayingApp").append(char(0x56));
            tail.append("com.librepods.app").append(char(0x52));
            tail.append("HostStreamingState").append(char(0x42));
            tail.append(streaming ? "YES" : "NO").append(char(0x49));
            tail.append("btAddress").append(char(0x51));
            tail.append(selfMac.toUtf8());
            tail.append("btName").append(char(0x47));
            tail.append(hostName.toUtf8());
            tail.append(char(0x58));
            tail.append("otherDevice").append("AudioCategory").append(QByteArray::fromHex("312D01"));
            tail.append(138 - 8 - tail.size() > 0 ? 138 - 8 - tail.size() : 0, char(0));

            QByteArray buf = QByteArray::fromHex("040004001000");
            buf.append(macBytes(targetMac));
            buf.append(char(tail.size())).append(char(0x00));
            buf.append(tail);
            return buf;
        }

        // 106-byte hijack request per Android's createHijackRequestPacket.
        inline QByteArray hijackRequest(const QString &targetMac)
        {
            QByteArray tail;
            tail.append(QByteArray::fromHex("01E5"));
            tail.append(char(0x4A));
            tail.append("localscore").append(QByteArray::fromHex("3064"));
            tail.append(char(0x46));
            tail.append("reason").append(char(0x48));
            tail.append("Hijackv2").append(char(0x51));
            tail.append("audioRoutingScore").append(QByteArray::fromHex("312D015F"));
            tail.append("audioRoutingSetOwnershipToFalse").append(char(0x01));
            tail.append(char(0x4B));
            tail.append("remotescore").append(char(0xA5));
            tail.append(106 - 8 - tail.size() > 0 ? 106 - 8 - tail.size() : 0, char(0));

            QByteArray buf = QByteArray::fromHex("040004001000");
            buf.append(macBytes(targetMac));
            buf.append(char(tail.size())).append(char(0x00));
            buf.append(tail);
            return buf;
        }
    }

    namespace Rename
    {
        static QByteArray getPacket(const QString &newName)
        {
            QByteArray nameBytes = newName.toUtf8();                   // Convert name to UTF-8
            quint8 size = static_cast<char>(nameBytes.size());         // Name length (1 byte)
            QByteArray packet = QByteArray::fromHex("040004001A0001"); // Header
            packet.append(size);                                       // Append size byte
            packet.append('\0');                                       // Append null byte
            packet.append(nameBytes);                                  // Append name bytes
            return packet;
        }
    }

    namespace MagicPairing {
        static const QByteArray REQUEST_MAGIC_CLOUD_KEYS = QByteArray::fromHex("0400040030000500");
        static const QByteArray MAGIC_CLOUD_KEYS_HEADER = QByteArray::fromHex("04000400310002");

        struct MagicCloudKeys {
            QByteArray magicAccIRK;      // 16 bytes
            QByteArray magicAccEncKey;    // 16 bytes
        };

        inline MagicCloudKeys parseMagicCloudKeysPacket(const QByteArray &data)
        {
            MagicCloudKeys keys;

            if (data.size() < 47 || !data.startsWith(MAGIC_CLOUD_KEYS_HEADER))
            {
                return keys;
            }

            int index = MAGIC_CLOUD_KEYS_HEADER.size();

            // First TLV block (MagicAccIRK)
            if (static_cast<quint8>(data.at(index)) != 0x01)
                return keys;
            index += 1;

            quint16 len1 = (static_cast<quint8>(data.at(index)) << 8) | static_cast<quint8>(data.at(index + 1));
            if (len1 != 16)
                return keys;
            index += 3; // Skip length (2 bytes) and reserved byte (1 byte)

            keys.magicAccIRK = data.mid(index, 16);
            index += 16;

            // Second TLV block (MagicAccEncKey)
            if (static_cast<quint8>(data.at(index)) != 0x04)
                return keys;
            index += 1;

            quint16 len2 = (static_cast<quint8>(data.at(index)) << 8) | static_cast<quint8>(data.at(index + 1));
            if (len2 != 16)
                return keys;
            index += 3; // Skip length (2 bytes) and reserved byte (1 byte)

            keys.magicAccEncKey = data.mid(index, 16);

            return keys;
        }
    }

    // Parsing Headers
    namespace Parse
    {
        static const QByteArray EAR_DETECTION = QByteArray::fromHex("040004000600");
        static const QByteArray BATTERY_STATUS = QByteArray::fromHex("040004000400");
        static const QByteArray METADATA = QByteArray::fromHex("040004001d");
        static const QByteArray HANDSHAKE_ACK = QByteArray::fromHex("01000400");
        static const QByteArray FEATURES_ACK = QByteArray::fromHex("040004002b00"); // Note: Only tested with airpods pro 2
    }
}

#endif // AIRPODS_PACKETS_H
