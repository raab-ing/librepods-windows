#include <QByteArray>

// Control Command Header
namespace ControlCommand
{
    // Function-local statics: namespace-scope QByteArray constants elsewhere
    // depend on this during their own dynamic init, and initialization order
    // across scopes is not guaranteed — an empty HEADER makes
    // startsWith(HEADER) match every packet and silently swallow them.
    inline const QByteArray &header()
    {
        static const QByteArray h = QByteArray::fromHex("040004000900");
        return h;
    }

    // Helper function to create control command packets
    inline QByteArray createCommand(quint8 identifier, quint8 data1 = 0x00, quint8 data2 = 0x00,
                                    quint8 data3 = 0x00, quint8 data4 = 0x00)
    {
        QByteArray packet = header();
        packet.append(static_cast<char>(identifier));
        packet.append(static_cast<char>(data1));
        packet.append(static_cast<char>(data2));
        packet.append(static_cast<char>(data3));
        packet.append(static_cast<char>(data4));
        return packet;
    }

    inline std::optional<char> parseActive(const QByteArray &data)
    {
        if (!data.startsWith(header()))
            return std::nullopt;

        return static_cast<quint8>(data.at(7));
    }
}

template <quint8 CommandId>
struct BasicControlCommand
{
    static constexpr quint8 ID = CommandId;

    static const QByteArray &header()
    {
        static const QByteArray h = ControlCommand::header() + static_cast<char>(CommandId);
        return h;
    }

    static const QByteArray &enabled()
    {
        static const QByteArray e = ControlCommand::createCommand(CommandId, 0x01);
        return e;
    }

    static const QByteArray &disabled()
    {
        static const QByteArray d = ControlCommand::createCommand(CommandId, 0x02);
        return d;
    }

    static QByteArray create(quint8 data1 = 0x00, quint8 data2 = 0x00,
                             quint8 data3 = 0x00, quint8 data4 = 0x00)
    {
        return ControlCommand::createCommand(ID, data1, data2, data3, data4);
    }

    // Basically returns the byte at the index 7
    static std::optional<bool> parseState(const QByteArray &data)
    {
        switch (ControlCommand::parseActive(data).value_or(0x00))
        {
        case 0x01: // Enabled
            return true;
        case 0x02: // Disabled
            return false;
        default:
            return std::nullopt;
        }
    }

    static std::optional<char> getValue(const QByteArray &data)
    {
        return ControlCommand::parseActive(data);
    }
};
