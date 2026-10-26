#pragma once

#include <QObject>
#include <QVariantList>
#include <array>
#include <atomic>
#include <mutex>

class QThread;

// Loopback-capture spectrum for the Custom EQ card. When `enabled` is
// true, captures the system's default *render* endpoint (what the user
// hears), runs an FFT, and publishes ~24 log-spaced band magnitudes
// (0..1) via `levels`. Windows uses WASAPI loopback; other platforms
// currently report an empty spectrum.
//
// Capture is intentionally opt-in: the EQ card binds `enabled` to its
// own visibility so the FFT thread only runs while settings is open.
class AudioSpectrum : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY enabledChanged)
    Q_PROPERTY(QVariantList levels READ levels NOTIFY levelsChanged)

public:
    static constexpr int kBandCount = 24;

    explicit AudioSpectrum(QObject *parent = nullptr);
    ~AudioSpectrum() override;

    bool enabled() const { return m_running.load(); }
    void setEnabled(bool on);

    QVariantList levels() const;

signals:
    void enabledChanged();
    void levelsChanged();

private:
    void workerMain();
    void publish(const std::array<float, kBandCount> &bands);

    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stopRequested{false};
    QThread *m_thread = nullptr;

    mutable std::mutex m_mutex;
    std::array<float, kBandCount> m_bands{};
    QVariantList m_levelsCache;
};
