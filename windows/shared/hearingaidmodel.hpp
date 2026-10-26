#ifndef HEARINGAIDMODEL_HPP
#define HEARINGAIDMODEL_HPP

#include <QObject>
#include <QByteArray>
#include <QTimer>
#include <cstring>

// QML-facing model for the AirPods HEARING_AID ATT characteristic (handle
// 0x2A). Codec ported from the Android app's data/HearingAid.kt:
//
//   bytes 0-3    header (02 02 60 00; byte 2 becomes 0x64 on write)
//   4..35        left EQ bands:  8 x float32 LE
//   36..51       left:  amplification, tone, conversationBoost(float>0.5),
//                ambientNoiseReduction
//   52..83       right EQ bands: 8 x float32 LE
//   84..99       right: amplification, tone, conversationBoost, ambientNR
//   100..103     ownVoiceAmplification
//
// The UI works on net values (like the Android adjustments screen): the
// amplification slider is the mean of both ears and the balance slider is
// the right-minus-left split; tone/boost/ambient apply to both ears.
class HearingAidModel : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool available READ available NOTIFY availableChanged)
    Q_PROPERTY(qreal amplification READ amplification WRITE setAmplification NOTIFY settingsChanged)
    Q_PROPERTY(qreal balance READ balance WRITE setBalance NOTIFY settingsChanged)
    Q_PROPERTY(qreal tone READ tone WRITE setTone NOTIFY settingsChanged)
    Q_PROPERTY(qreal ambientNoiseReduction READ ambientNoiseReduction WRITE setAmbientNoiseReduction NOTIFY settingsChanged)
    Q_PROPERTY(bool conversationBoost READ conversationBoost WRITE setConversationBoost NOTIFY settingsChanged)
    Q_PROPERTY(qreal ownVoiceAmplification READ ownVoiceAmplification WRITE setOwnVoiceAmplification NOTIFY settingsChanged)
public:
    explicit HearingAidModel(QObject *parent = nullptr) : QObject(parent)
    {
        m_debounce.setSingleShot(true);
        m_debounce.setInterval(100); // mirrors the Android 100 ms debounce
        connect(&m_debounce, &QTimer::timeout, this, [this]() {
            if (m_dirty)
            {
                m_dirty = false;
                emit writeRequested(m_data);
            }
        });
    }

    bool available() const { return m_available; }

    qreal amplification() const { return m_netAmplification; }
    qreal balance() const { return m_balance; }
    qreal tone() const { return m_tone; }
    qreal ambientNoiseReduction() const { return m_ambientNoiseReduction; }
    bool conversationBoost() const { return m_conversationBoost; }
    qreal ownVoiceAmplification() const { return m_ownVoiceAmplification; }

    void updateFromBytes(const QByteArray &data)
    {
        if (data.size() < 104)
            return;
        m_data = data;
        const uchar *p = reinterpret_cast<const uchar *>(m_data.constData());

        const float leftAmp = f32(p + 36);
        const float rightAmp = f32(p + 84);
        m_netAmplification = qBound(-1.0f, (leftAmp + rightAmp) / 2.0f, 1.0f);
        m_balance = qBound(-1.0f, rightAmp - leftAmp, 1.0f);
        m_tone = f32(p + 40);
        m_ambientNoiseReduction = f32(p + 48);
        m_conversationBoost = f32(p + 44) > 0.5f;
        m_ownVoiceAmplification = f32(p + 100);

        if (!m_available)
        {
            m_available = true;
            emit availableChanged();
        }
        emit settingsChanged();
    }

    void setUnavailable()
    {
        if (m_available)
        {
            m_available = false;
            emit availableChanged();
        }
        m_data.clear();
    }

    void setAmplification(qreal v)
    {
        if (m_data.size() < 104)
            return;
        m_netAmplification = v;
        writeEars();
    }
    void setBalance(qreal v)
    {
        if (m_data.size() < 104)
            return;
        m_balance = v;
        writeEars();
    }
    void setTone(qreal v)
    {
        if (m_data.size() < 104)
            return;
        m_tone = v;
        putF32(40, v); putF32(88, v);
        flush();
    }
    void setAmbientNoiseReduction(qreal v)
    {
        if (m_data.size() < 104)
            return;
        m_ambientNoiseReduction = v;
        putF32(48, v); putF32(96, v);
        flush();
    }
    void setConversationBoost(bool v)
    {
        if (m_data.size() < 104)
            return;
        m_conversationBoost = v;
        putF32(44, v ? 1.0f : 0.0f); putF32(92, v ? 1.0f : 0.0f);
        flush();
    }
    void setOwnVoiceAmplification(qreal v)
    {
        if (m_data.size() < 104)
            return;
        m_ownVoiceAmplification = v;
        putF32(100, v);
        flush();
    }

signals:
    void availableChanged();
    void settingsChanged();
    void writeRequested(const QByteArray &data);

private:
    static float f32(const uchar *p)
    {
        float f;
        std::memcpy(&f, p, 4); // little-endian on all supported hosts
        return f;
    }
    void putF32(int off, float v)
    {
        uchar *p = reinterpret_cast<uchar *>(m_data.data());
        std::memcpy(p + off, &v, 4);
    }
    // Split net amplification into per-ear values using the balance offset,
    // same arithmetic as the Android adjustments screen.
    void writeEars()
    {
        const float net = static_cast<float>(m_netAmplification);
        const float bal = static_cast<float>(m_balance);
        putF32(36, net + (bal < 0 ? -bal : 0.0f));
        putF32(84, net + (bal > 0 ? bal : 0.0f));
        flush();
    }
    void flush()
    {
        m_data[2] = 0x64; // write marker, per Android's sender
        m_dirty = true;
        m_debounce.start();
        emit settingsChanged();
    }

    QByteArray m_data;
    QTimer m_debounce;
    bool m_dirty = false;
    bool m_available = false;
    qreal m_netAmplification = 0;
    qreal m_balance = 0;
    qreal m_tone = 0;
    qreal m_ambientNoiseReduction = 0;
    qreal m_ownVoiceAmplification = 0;
    bool m_conversationBoost = false;
};

#endif // HEARINGAIDMODEL_HPP
