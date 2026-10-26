#ifndef MEDIACONTROLLER_H
#define MEDIACONTROLLER_H

#include <QObject>
#include <QString>
#include <QStringList>

class EarDetection;
class QTimer;

// Windows media controller replacing the MPRIS/PulseAudio Linux
// implementation. Media state and transport control go through the WinRT
// GlobalSystemMediaTransportControlsSessionManager; volume ducking for
// conversational awareness uses the classic IAudioEndpointVolume COM API.
// A2DP profile switching is managed by Windows itself, so the profile
// functions are intentionally no-ops (documented in windows/README.md).
class MediaController : public QObject
{
    Q_OBJECT
public:
    enum MediaState
    {
        Playing,
        Paused,
        Stopped
    };
    Q_ENUM(MediaState)
    enum EarDetectionBehavior
    {
        PauseWhenOneRemoved,
        PauseWhenBothRemoved,
        Disabled
    };
    Q_ENUM(EarDetectionBehavior)

    explicit MediaController(QObject *parent = nullptr);
    ~MediaController() override;

    void handleEarDetection(EarDetection *);
    void followMediaChanges();
    bool isActiveOutputDeviceAirPods();
    void handleConversationalAwareness(const QByteArray &data);
    void activateA2dpProfile();
    void removeAudioOutputDevice();
    void setConnectedDeviceMacAddress(const QString &macAddress);
    void setConnectedDeviceName(const QString &name);
    bool isA2dpProfileAvailable();
    QString getPreferredA2dpProfile();
    bool restartWirePlumber(); // Kept for API parity; always returns false on Windows

    void setEarDetectionBehavior(EarDetectionBehavior behavior);
    inline EarDetectionBehavior getEarDetectionBehavior() const { return earDetectionBehavior; }

    void play();
    void pause();
    // Transport actions for user-triggered controls (head gestures, stem
    // press). Unlike play()/pause(), these act on whatever session is
    // active rather than the ear-detection resume bookkeeping.
    void togglePlayPause();
    void nextTrack();
    void previousTrack();
    void volumeUp();
    void volumeDown();
    MediaState getCurrentMediaState() const;

Q_SIGNALS:
    void mediaStateChanged(MediaState state);

private:
    void refreshPlaybackState();
    float getDefaultEndpointVolume();
    bool setDefaultEndpointVolume(float volume);
    bool earPauseDue(EarDetection *) const;

    QStringList pausedByAppServices;
    float initialVolume = -1.0f;
    QString connectedDeviceMacAddress;
    QString connectedDeviceName;
    EarDetectionBehavior earDetectionBehavior = PauseWhenOneRemoved;
    MediaState m_lastState = Stopped;
    // Ear-status debounce: pods emit occasional NotInEar flickers while
    // worn; a pending pause is cancelled if the state reverts in time.
    QTimer *m_earPauseTimer = nullptr;
    EarDetection *m_earPauseSource = nullptr;

    // Pimpl hides WinRT/COM members from moc.
    struct Impl;
    Impl *impl = nullptr;
};

#endif // MEDIACONTROLLER_H
