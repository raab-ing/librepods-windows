#include <QSettings>
#include <QLocalServer>
#include <QLocalSocket>
#include <QApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QBluetoothLocalDevice>
#include <QBluetoothSocket>
#include <QQuickWindow>
#include <QQuickStyle>
#include <QLoggingCategory>
#include <QThread>
#include <QTimer>
#include <QProcess>
#include <QRegularExpression>
#include <QTranslator>
#include <QLibraryInfo>
#include <QDir>
#include <QStandardPaths>

#include "airpods_packets.h"
#include "logger.h"
#include "media/mediacontroller.h"
#include "trayiconmanager.h"
#include "enums.h"
#include "battery.hpp"
#include "BluetoothMonitor.h"
#include "autostartmanager.hpp"
#include "audiospectrum.h"
#include "deviceinfo.hpp"
#include "attmanager.h"
#include "hearingaidmodel.hpp"
#include "qtbtsocketadapter.h"
#include "gesturedetector.h"
#include "headorientation.h"
#include "ble/blemanager.h"
#include "ble/bleutils.h"
#include "QRCodeImageProvider.hpp"
#include "systemsleepmonitor.hpp"

using namespace AirpodsTrayApp::Enums;

Q_LOGGING_CATEGORY(librepods, "librepods")

class AirPodsTrayApp : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool airpodsConnected READ areAirpodsConnected NOTIFY airPodsStatusChanged)
    Q_PROPERTY(int earDetectionBehavior READ earDetectionBehavior WRITE setEarDetectionBehavior NOTIFY earDetectionBehaviorChanged)
    Q_PROPERTY(bool crossDeviceEnabled READ crossDeviceEnabled WRITE setCrossDeviceEnabled NOTIFY crossDeviceEnabledChanged)
    Q_PROPERTY(AutoStartManager *autoStartManager READ autoStartManager CONSTANT)
    Q_PROPERTY(int retryAttempts READ retryAttempts WRITE setRetryAttempts NOTIFY retryAttemptsChanged)
    Q_PROPERTY(bool hideOnStart READ hideOnStart CONSTANT)
    Q_PROPERTY(DeviceInfo *deviceInfo READ deviceInfo CONSTANT)
    Q_PROPERTY(HearingAidModel *hearingAidModel READ hearingAidModel CONSTANT)
    Q_PROPERTY(AudioSpectrum *spectrum READ spectrum CONSTANT)
    Q_PROPERTY(QString phoneMacStatus READ phoneMacStatus NOTIFY phoneMacStatusChanged)
    Q_PROPERTY(bool hearingAidEnabled READ hearingAidEnabled WRITE setHearingAidEnabled NOTIFY hearingAidEnabledChanged)
    // Linux uses BlueZ directly — no companion driver concept. Stubs keep the
    // shared QML happy.
    Q_PROPERTY(bool driverAvailable READ driverAvailable CONSTANT)
    Q_PROPERTY(QString statusHint READ statusHint NOTIFY airPodsStatusChanged)
    Q_PROPERTY(bool deviceLinkConnected READ deviceLinkConnected NOTIFY airPodsStatusChanged)
    Q_PROPERTY(QString attState READ attState NOTIFY attStateChanged)
    Q_PROPERTY(QString attError READ attError NOTIFY attStateChanged)
    Q_PROPERTY(int gestureNodAction READ gestureNodAction WRITE setGestureNodAction NOTIFY actionConfigChanged)
    Q_PROPERTY(int gestureShakeAction READ gestureShakeAction WRITE setGestureShakeAction NOTIFY actionConfigChanged)
    Q_PROPERTY(int stemSingleAction READ stemSingleAction WRITE setStemSingleAction NOTIFY actionConfigChanged)
    Q_PROPERTY(int stemDoubleAction READ stemDoubleAction WRITE setStemDoubleAction NOTIFY actionConfigChanged)
    Q_PROPERTY(int stemTripleAction READ stemTripleAction WRITE setStemTripleAction NOTIFY actionConfigChanged)
    Q_PROPERTY(int stemLongAction READ stemLongAction WRITE setStemLongAction NOTIFY actionConfigChanged)
    Q_PROPERTY(QString lastGesture READ lastGesture NOTIFY lastGestureChanged)

public:
    AirPodsTrayApp(bool debugMode, bool hideOnStart, QQmlApplicationEngine *parent = nullptr)
        : QObject(parent), debugMode(debugMode), m_settings(new QSettings("AirPodsTrayApp", "AirPodsTrayApp"))
        , m_autoStartManager(new AutoStartManager(this)), m_hideOnStart(hideOnStart), parent(parent)
        , m_deviceInfo(new DeviceInfo(this)), m_bleManager(new BleManager(this)),
          m_hearingAid(new HearingAidModel(this)), m_spectrum(new AudioSpectrum(this))
        , m_systemSleepMonitor(new SystemSleepMonitor(this))
    {
        QLoggingCategory::setFilterRules(QString("librepods.debug=%1").arg(debugMode ? "true" : "false"));
        LOG_INFO("Initializing LibrePods");

        // Initialize tray icon and connect signals
        trayManager = new TrayIconManager(this);
        connect(trayManager, &TrayIconManager::trayClicked, this, &AirPodsTrayApp::onTrayIconActivated);
        connect(trayManager, &TrayIconManager::openApp, this, &AirPodsTrayApp::onOpenApp);
        connect(trayManager, &TrayIconManager::openSettings, this, &AirPodsTrayApp::onOpenSettings);
        connect(trayManager, &TrayIconManager::noiseControlChanged, this, &AirPodsTrayApp::setNoiseControlMode);
        connect(trayManager, &TrayIconManager::conversationalAwarenessToggled, this, &AirPodsTrayApp::setConversationalAwareness);
        connect(m_deviceInfo, &DeviceInfo::batteryStatusChanged, trayManager, &TrayIconManager::updateBatteryStatus);
        connect(m_deviceInfo, &DeviceInfo::noiseControlModeChanged, trayManager, &TrayIconManager::updateNoiseControlState);
        connect(m_deviceInfo, &DeviceInfo::conversationalAwarenessChanged, trayManager, &TrayIconManager::updateConversationalAwareness);
        connect(m_deviceInfo, &DeviceInfo::modelChanged, trayManager, [this]() {
            trayManager->setConversationalAwarenessSupported(m_deviceInfo->supportsConversationalAwareness());
        });
        trayManager->setConversationalAwarenessSupported(m_deviceInfo->supportsConversationalAwareness());

        // Initialize MediaController and connect signals
        mediaController = new MediaController(this);
        connect(mediaController, &MediaController::mediaStateChanged, this, &AirPodsTrayApp::handleMediaStateChange);
        mediaController->followMediaChanges();

        // Hearing-aid settings ride the ATT channel; the manager is swapped
        // in/out as that channel opens and closes.
        connect(m_hearingAid, &HearingAidModel::writeRequested, this,
                [this](const QByteArray &data) {
                    if (m_attManager)
                        m_attManager->writeCharacteristic(AttManager::HearingAid, data);
                });

        monitor = new BluetoothMonitor(this);
        connect(monitor, &BluetoothMonitor::deviceConnected, this, &AirPodsTrayApp::bluezDeviceConnected);
        connect(monitor, &BluetoothMonitor::deviceDisconnected, this, &AirPodsTrayApp::bluezDeviceDisconnected);

        // Deferred ATT open: let the AACP handshake settle before asking
        // for the second channel.
        m_attOpenTimer = new QTimer(this);
        m_attOpenTimer->setSingleShot(true);
        connect(m_attOpenTimer, &QTimer::timeout, this, [this]() {
            if (socket && socket->isOpen() && !m_pendingAttAddress.isNull())
                openAttChannel(m_pendingAttAddress);
        });

        // Head gestures run on the AACP 0x17 acceleration stream.
        m_gestureDetector = new GestureDetector(this);
        connect(m_gestureDetector, &GestureDetector::gestureAccepted, this, [this]() {
            LOG_INFO("Head gesture detected: nod (accept)");
            m_lastGesture = tr("Nod (accept)");
            emit lastGestureChanged();
            runAction(m_gestureNodAction);
        });
        connect(m_gestureDetector, &GestureDetector::gestureRejected, this, [this]() {
            LOG_INFO("Head gesture detected: shake (reject)");
            m_lastGesture = tr("Shake (reject)");
            emit lastGestureChanged();
            runAction(m_gestureShakeAction);
        });

        connect(m_bleManager, &BleManager::deviceFound, this, &AirPodsTrayApp::bleDeviceFound);
        connect(m_deviceInfo->getBattery(), &Battery::primaryChanged, this, &AirPodsTrayApp::primaryChanged);
        connect(m_systemSleepMonitor, &SystemSleepMonitor::systemGoingToSleep, this, &AirPodsTrayApp::onSystemGoingToSleep);
        connect(m_systemSleepMonitor, &SystemSleepMonitor::systemWakingUp, this, &AirPodsTrayApp::onSystemWakingUp);

        // Load settings
        CrossDevice.isEnabled = loadCrossDeviceEnabled();
        setEarDetectionBehavior(loadEarDetectionSettings());
        setRetryAttempts(loadRetryAttempts());
        m_deviceInfo->setHeadTrackingEnabled(m_settings->value("headTracking/enabled", false).toBool());
        m_gestureNodAction = m_settings->value("gestures/nodAction", 1).toInt();
        m_gestureShakeAction = m_settings->value("gestures/shakeAction", 2).toInt();
        m_stemSingleAction = m_settings->value("stem/singleAction", 0).toInt();
        m_stemDoubleAction = m_settings->value("stem/doubleAction", 0).toInt();
        m_stemTripleAction = m_settings->value("stem/tripleAction", 0).toInt();
        m_stemLongAction = m_settings->value("stem/longAction", 0).toInt();

        monitor->checkAlreadyConnectedDevices();
        LOG_INFO("AirPodsTrayApp initialized");

        QBluetoothLocalDevice localDevice;

        const QList<QBluetoothAddress> connectedDevices = localDevice.connectedDevices();
        for (const QBluetoothAddress &address : connectedDevices) {
            QBluetoothDeviceInfo device(address, "", 0);
            if (isAirPodsDevice(device)) {
                connectToDevice(device);

                // On startup after reboot, activate A2DP profile for already connected AirPods
                QTimer::singleShot(2000, this, [this, address]()
                {
                    QString formattedAddress = address.toString().replace(":", "_");
                    mediaController->setConnectedDeviceMacAddress(formattedAddress);
                    mediaController->activateA2dpProfile();
                    LOG_INFO("A2DP profile activation attempted for AirPods found on startup");
                });
                return;
            }
        }

        initializeDBus();
        initializeBluetooth();
    }

    ~AirPodsTrayApp() {
        saveCrossDeviceEnabled();
        saveEarDetectionSettings();

        delete socket;
        delete phoneSocket;
    }

    bool areAirpodsConnected() const { return socket && socket->isOpen() && socket->state() == QBluetoothSocket::SocketState::ConnectedState; }
    int earDetectionBehavior() const { return mediaController->getEarDetectionBehavior(); }
    bool crossDeviceEnabled() const { return CrossDevice.isEnabled; }
    AutoStartManager *autoStartManager() const { return m_autoStartManager; }
    int retryAttempts() const { return m_retryAttempts; }
    bool hideOnStart() const { return m_hideOnStart; }
    DeviceInfo *deviceInfo() const { return m_deviceInfo; }
    HearingAidModel *hearingAidModel() const { return m_hearingAid; }
    AudioSpectrum *spectrum() const { return m_spectrum; }
    QString phoneMacStatus() const { return m_phoneMacStatus; }
    bool hearingAidEnabled() const { return m_deviceInfo->hearingAidEnabled(); }
    bool driverAvailable() const { return true; }
    QString statusHint() const { return {}; }
    bool deviceLinkConnected() const { return areAirpodsConnected(); }
    QString attState() const { return m_attState; }
    QString attError() const { return m_attError; }
    int gestureNodAction() const { return m_gestureNodAction; }
    int gestureShakeAction() const { return m_gestureShakeAction; }
    int stemSingleAction() const { return m_stemSingleAction; }
    int stemDoubleAction() const { return m_stemDoubleAction; }
    int stemTripleAction() const { return m_stemTripleAction; }
    int stemLongAction() const { return m_stemLongAction; }
    QString lastGesture() const { return m_lastGesture; }

private:
    void setAttState(const QString &state, const QString &error = {})
    {
        if (m_attState != state || m_attError != error)
        {
            m_attState = state;
            m_attError = error;
            emit attStateChanged();
        }
    }

    // Shared dispatch for head-gesture and stem-press action maps.
    void runAction(int action)
    {
        switch (action)
        {
        case 1: mediaController->togglePlayPause(); break;
        case 2: mediaController->nextTrack(); break;
        case 3: mediaController->previousTrack(); break;
        case 4: mediaController->volumeUp(); break;
        case 5: mediaController->volumeDown(); break;
        case 6: cycleNoiseMode(); break;
        default: break;
        }
    }

    void cycleNoiseMode()
    {
        using M = NoiseControlMode;
        static const M order[] = {M::Off, M::NoiseCancellation, M::Transparency, M::Adaptive};
        const int idx = static_cast<int>(m_deviceInfo->noiseControlMode());
        setNoiseControlMode(order[(idx + 1) % 4]);
    }

    void handleStemPress(AirPodsPackets::StemPress::Type type, AirPodsPackets::StemPress::Bud bud)
    {
        LOG_INFO("Stem press: type " << int(type) << " bud " << int(bud));
        switch (type)
        {
        case AirPodsPackets::StemPress::Type::Single: runAction(m_stemSingleAction); break;
        case AirPodsPackets::StemPress::Type::Double: runAction(m_stemDoubleAction); break;
        case AirPodsPackets::StemPress::Type::Triple: runAction(m_stemTripleAction); break;
        case AirPodsPackets::StemPress::Type::Long:   runAction(m_stemLongAction); break;
        }
    }

    // Tell the pods which press types we handle ourselves (STEM_CONFIG,
    // control id 0x39) so their native action doesn't double-fire with ours.
    void sendStemConfig()
    {
        const quint8 mask = (m_stemSingleAction ? 0x01 : 0) |
                            (m_stemDoubleAction ? 0x02 : 0) |
                            (m_stemTripleAction ? 0x04 : 0) |
                            (m_stemLongAction   ? 0x08 : 0);
        writePacketToSocket(ControlCommand::createCommand(0x39, mask),
                            "Stem config packet written: ");
    }

    QString localAdapterMac()
    {
        if (m_localAdapterMac.isEmpty())
            m_localAdapterMac = QBluetoothLocalDevice().address().toString().toUpper();
        return m_localAdapterMac;
    }

public slots:
    void setGestureNodAction(int action)   { if (m_gestureNodAction != action)   { m_gestureNodAction = action;   m_settings->setValue("gestures/nodAction", action);   emit actionConfigChanged(); } }
    void setGestureShakeAction(int action) { if (m_gestureShakeAction != action) { m_gestureShakeAction = action; m_settings->setValue("gestures/shakeAction", action); emit actionConfigChanged(); } }
    void setStemSingleAction(int action)   { if (m_stemSingleAction != action)   { m_stemSingleAction = action;   m_settings->setValue("stem/singleAction", action);    sendStemConfig(); emit actionConfigChanged(); } }
    void setStemDoubleAction(int action)   { if (m_stemDoubleAction != action)   { m_stemDoubleAction = action;   m_settings->setValue("stem/doubleAction", action);    sendStemConfig(); emit actionConfigChanged(); } }
    void setStemTripleAction(int action)   { if (m_stemTripleAction != action)   { m_stemTripleAction = action;   m_settings->setValue("stem/tripleAction", action);    sendStemConfig(); emit actionConfigChanged(); } }
    void setStemLongAction(int action)     { if (m_stemLongAction != action)     { m_stemLongAction = action;     m_settings->setValue("stem/longAction", action);      sendStemConfig(); emit actionConfigChanged(); } }

    void setCustomEqEnabled(bool enabled)
    {
        sendCustomEq(enabled, m_deviceInfo->customEqLow(), m_deviceInfo->customEqMid(),
                     m_deviceInfo->customEqHigh());
    }
    void setCustomEqBand(int low, int mid, int high)
    {
        sendCustomEq(m_deviceInfo->customEqEnabled(), low, mid, high);
    }
    void sendCustomEq(bool enabled, int low, int mid, int high)
    {
        if (writePacketToSocket(AirPodsPackets::CustomEq::packet(enabled, low, mid, high),
                                "Custom EQ packet written: "))
            m_deviceInfo->setCustomEq(enabled, low, mid, high);
    }

    // TiPi takeover — claim ownership of the pods' multidevice session.
    void takeOverAudio()
    {
        if (!socket || !socket->isOpen())
        {
            LOG_WARN("Cannot take over audio: control channel is down");
            return;
        }
        const QString local = localAdapterMac();
        writePacketToSocket(AirPodsPackets::Ownership::ownsPacket(), "Ownership claim written: ");
        QString target;
        for (const QString &d : m_deviceInfo->connectedDevices())
            if (d.compare(local, Qt::CaseInsensitive) != 0) { target = d; break; }
        if (!target.isEmpty())
        {
            writePacketToSocket(AirPodsPackets::SmartRouting::mediaInformation(
                                    local, target, QStringLiteral("Linux"), false),
                                "Media information written: ");
            writePacketToSocket(AirPodsPackets::SmartRouting::hijackRequest(target),
                                "Hijack request written: ");
        }
        else
        {
            LOG_INFO("Ownership claimed; no other TiPi peer to displace");
        }
    }

private:
    bool debugMode;
    bool isConnectedLocally = false;

    QQmlApplicationEngine *parent = nullptr;

    struct {
        bool isAvailable = true;
        bool isEnabled = true; // Ability to disable the feature
    } CrossDevice;

    void initializeDBus() { }

    bool isAirPodsDevice(const QBluetoothDeviceInfo &device)
    {
        return device.serviceUuids().contains(QBluetoothUuid("74ec2172-0bad-4d01-8f77-997b2be0722a"));
    }

    void notifyAndroidDevice()
    {
        if (!CrossDevice.isEnabled) {
            return;
        }

        if (phoneSocket && phoneSocket->isOpen())
        {
            phoneSocket->write(AirPodsPackets::Phone::NOTIFICATION);
            LOG_DEBUG("Sent notification packet to Android: " << AirPodsPackets::Phone::NOTIFICATION.toHex());
        }
        else
        {
            LOG_WARN("Phone socket is not open, cannot send notification packet");
        }
    }

    void disconnectDevice(const QString &devicePath) {
        LOG_INFO("Disconnecting device at " << devicePath);
    }

public slots:
    void connectToDevice(const QString &address) {
        LOG_INFO("Connecting to device with address: " << address);
        QBluetoothAddress btAddress(address);
        QBluetoothDeviceInfo device(btAddress, "", 0);
        connectToDevice(device);
    }

    void setNoiseControlMode(NoiseControlMode mode)
    {
        if (m_deviceInfo->noiseControlMode() == mode)
        {
            LOG_INFO("Noise control mode is already set to: " << static_cast<int>(mode));
            return;
        }
        LOG_INFO("Setting noise control mode to: " << mode);
        QByteArray packet = AirPodsPackets::NoiseControl::getPacketForMode(mode);
        writePacketToSocket(packet, "Noise control mode packet written: ");
    }
    void setNoiseControlModeInt(int mode)
    {
        if (mode < 0 || mode > static_cast<int>(NoiseControlMode::Adaptive))
        {
            LOG_ERROR("Invalid noise control mode: " << mode);
            return;
        }
        setNoiseControlMode(static_cast<NoiseControlMode>(mode));
    }

    void setConversationalAwareness(bool enabled)
    {
        LOG_INFO("Setting conversational awareness to: " << (enabled ? "enabled" : "disabled"));
        QByteArray packet = enabled ? AirPodsPackets::ConversationalAwareness::ENABLED
                                    : AirPodsPackets::ConversationalAwareness::DISABLED;

        writePacketToSocket(packet, "Conversational awareness packet written: ");
        m_deviceInfo->setConversationalAwareness(enabled);
    }

    void setOneBudANCMode(bool enabled)
    {
        if (m_deviceInfo->oneBudANCMode() == enabled)
        {
            LOG_INFO("One Bud ANC mode is already " << (enabled ? "enabled" : "disabled"));
            return;
        }

        LOG_INFO("Setting One Bud ANC mode to: " << (enabled ? "enabled" : "disabled"));
        QByteArray packet = enabled ? AirPodsPackets::OneBudANCMode::ENABLED
                                    : AirPodsPackets::OneBudANCMode::DISABLED;

        if (writePacketToSocket(packet, "One Bud ANC mode packet written: "))
        {
            m_deviceInfo->setOneBudANCMode(enabled);
        }
        else
        {
            LOG_ERROR("Failed to send One Bud ANC mode command: socket not open");
        }
    }

    void setRetryAttempts(int attempts)
    {
        if (m_retryAttempts != attempts)
        {
            LOG_DEBUG("Setting retry attempts to: " << attempts);
            m_retryAttempts = attempts;
            emit retryAttemptsChanged(attempts);
            saveRetryAttempts(attempts);
        }
    }

    void initiateMagicPairing()
    {
        if (!socket || !socket->isOpen())
        {
            LOG_ERROR("Socket nicht offen, Magic Pairing kann nicht gestartet werden");
            return;
        }

        writePacketToSocket(AirPodsPackets::MagicPairing::REQUEST_MAGIC_CLOUD_KEYS, "Magic Pairing packet written: ");
    }

    void setAdaptiveNoiseLevel(int level)
    {
        level = qBound(0, level, 100);
        if (m_deviceInfo->adaptiveNoiseLevel() != level && m_deviceInfo->adaptiveModeActive())
        {
            QByteArray packet = AirPodsPackets::AdaptiveNoise::getPacket(level);
            writePacketToSocket(packet, "Adaptive noise level packet written: ");
            m_deviceInfo->setAdaptiveNoiseLevel(level);
        }
    }

    // ---- pod-side config setters (control-command ids) ----
    void sendBoolConfig(const QByteArray &on, const QByteArray &off, bool enabled,
                        const char *what)
    {
        if (writePacketToSocket(enabled ? on : off, QByteArray(what) + " written: "))
            LOG_INFO("Set " << what << " to " << enabled);
    }

    void setMicMode(int mode)
    {
        if (writePacketToSocket(AirPodsPackets::MicMode::packet(static_cast<quint8>(mode)),
                                "Mic mode written: "))
            m_deviceInfo->setMicMode(mode);
    }
    void setPodEarDetection(bool enabled)
    {
        sendBoolConfig(AirPodsPackets::EarDetectionConfig::ENABLED,
                       AirPodsPackets::EarDetectionConfig::DISABLED, enabled, "pod ear detection");
        m_deviceInfo->setPodEarDetection(enabled);
    }
    void setPressSpeed(int v)
    {
        if (writePacketToSocket(AirPodsPackets::PressSpeed::packet(static_cast<quint8>(v)),
                                "Press speed written: "))
            m_deviceInfo->setPressSpeed(v);
    }
    void setHoldDuration(int v)
    {
        if (writePacketToSocket(AirPodsPackets::HoldDuration::packet(static_cast<quint8>(v)),
                                "Hold duration written: "))
            m_deviceInfo->setHoldDuration(v);
    }
    void setListeningModeCycle(int mask)
    {
        if (writePacketToSocket(AirPodsPackets::ListeningModeCycle::packet(static_cast<quint8>(mask)),
                                "Listening-mode cycle written: "))
            m_deviceInfo->setListeningModeCycle(mask);
    }
    void setChimeVolume(int vol)
    {
        if (writePacketToSocket(AirPodsPackets::ChimeVolume::packet(static_cast<quint8>(qBound(0, vol, 100))),
                                "Chime volume written: "))
            m_deviceInfo->setChimeVolume(vol);
    }
    void setAutomaticConnection(bool enabled)
    {
        sendBoolConfig(AirPodsPackets::AutoConnection::ENABLED,
                       AirPodsPackets::AutoConnection::DISABLED, enabled, "automatic connection");
        m_deviceInfo->setAutomaticConnection(enabled);
    }
    void setMuteOnSinglePress(bool enabled)
    {
        if (writePacketToSocket(AirPodsPackets::CallManagement::packet(enabled),
                                "Call management written: "))
            m_deviceInfo->setMuteOnSinglePress(enabled);
    }
    void setInCaseTone(bool enabled)
    {
        sendBoolConfig(AirPodsPackets::InCaseTone::ENABLED,
                       AirPodsPackets::InCaseTone::DISABLED, enabled, "in-case tone");
        m_deviceInfo->setInCaseTone(enabled);
    }
    void setSiriMultitone(bool enabled)
    {
        sendBoolConfig(AirPodsPackets::SiriMultitone::ENABLED,
                       AirPodsPackets::SiriMultitone::DISABLED, enabled, "Siri chime");
        m_deviceInfo->setSiriMultitone(enabled);
    }
    void setSleepDetection(bool enabled)
    {
        sendBoolConfig(AirPodsPackets::SleepDetection::ENABLED,
                       AirPodsPackets::SleepDetection::DISABLED, enabled, "sleep detection");
        m_deviceInfo->setSleepDetection(enabled);
    }
    void setLoudSoundReduction(bool enabled)
    {
        sendBoolConfig(AirPodsPackets::LoudSoundReduction::ENABLED,
                       AirPodsPackets::LoudSoundReduction::DISABLED, enabled, "loud sound reduction");
        m_deviceInfo->setLoudSoundReduction(enabled);
    }
    void setPpeCapLevel(int level)
    {
        if (writePacketToSocket(AirPodsPackets::PpeCapLevel::packet(static_cast<quint8>(qBound(0, level, 100))),
                                "PPE cap level written: "))
            m_deviceInfo->setPpeCapLevel(level);
    }
    void setOptimizedCharging(bool enabled)
    {
        sendBoolConfig(AirPodsPackets::OptimizedCharging::ENABLED,
                       AirPodsPackets::OptimizedCharging::DISABLED, enabled, "optimized charging");
        m_deviceInfo->setOptimizedCharging(enabled);
    }
    void setHearingGainSwipe(bool enabled)
    {
        sendBoolConfig(AirPodsPackets::HearingGainSwipe::ENABLED,
                       AirPodsPackets::HearingGainSwipe::DISABLED, enabled, "hearing gain swipe");
        m_deviceInfo->setHearingGainSwipe(enabled);
    }
    void setAllowOffOption(bool enabled)
    {
        sendBoolConfig(AirPodsPackets::AllowOffOption::ENABLED,
                       AirPodsPackets::AllowOffOption::DISABLED, enabled, "off listening option");
        m_deviceInfo->setAllowOffOption(enabled);
    }
    void setVolumeSwipe(bool enabled)
    {
        sendBoolConfig(AirPodsPackets::VolumeSwipe::ENABLED,
                       AirPodsPackets::VolumeSwipe::DISABLED, enabled, "volume swipe");
        m_deviceInfo->setVolumeSwipeEnabled(enabled);
    }
    void setVolumeSwipeInterval(int v)
    {
        if (writePacketToSocket(AirPodsPackets::VolumeSwipe::getIntervalPacket(static_cast<quint8>(v)),
                                "Volume swipe interval written: "))
            m_deviceInfo->setVolumeSwipeInterval(v);
    }
    void setAdaptiveVolume(bool enabled)
    {
        sendBoolConfig(AirPodsPackets::AdaptiveVolume::ENABLED,
                       AirPodsPackets::AdaptiveVolume::DISABLED, enabled, "adaptive volume");
        m_deviceInfo->setAdaptiveVolume(enabled);
    }
    void setCrownDirection(int v)
    {
        if (writePacketToSocket(AirPodsPackets::CrownDirection::packet(static_cast<quint8>(v)),
                                "Crown direction written: "))
            m_deviceInfo->setCrownDirection(v);
    }

    void renameAirPods(const QString &newName)
    {
        if (newName.isEmpty())
        {
            LOG_WARN("Cannot set empty name");
            return;
        }
        if (newName.size() > 32)
        {
            LOG_WARN("Name is too long, must be 32 characters or less");
            return;
        }
        if (newName == m_deviceInfo->deviceName())
        {
            LOG_INFO("Name is already set to: " << newName);
            return;
        }

        QByteArray packet = AirPodsPackets::Rename::getPacket(newName);
        if (writePacketToSocket(packet, "Rename packet written: "))
        {
            LOG_INFO("Sent rename command for new name: " << newName);
            m_deviceInfo->setDeviceName(newName);
        }
        else
        {
            LOG_ERROR("Failed to send rename command: socket not open");
        }
    }

    void setEarDetectionBehavior(int behavior)
    {
        if (behavior == earDetectionBehavior())
        {
            LOG_INFO("Ear detection behavior is already set to: " << behavior);
            return;
        }

        mediaController->setEarDetectionBehavior(static_cast<MediaController::EarDetectionBehavior>(behavior));
        saveEarDetectionSettings();
        emit earDetectionBehaviorChanged(behavior);
    }

    void setCrossDeviceEnabled(bool enabled)
    {
        if (CrossDevice.isEnabled == enabled)
        {
            LOG_INFO("Cross-device feature is already " << (enabled ? "enabled" : "disabled"));
            return;
        }

        CrossDevice.isEnabled = enabled;
        saveCrossDeviceEnabled();
        connectToPhone();
        emit crossDeviceEnabledChanged(enabled);
    }

    void setPhoneMac(const QString &mac)
    {
        if (mac.isEmpty()) {
            LOG_WARN("Empty MAC provided, ignoring");
            m_phoneMacStatus = QStringLiteral("No MAC provided (ignoring)");
            emit phoneMacStatusChanged();
            return;
        }

        // Basic MAC address validation (accepts formats like AA:BB:CC:DD:EE:FF, AABBCCDDEEFF, AA-BB-CC-DD-EE-FF)
        QRegularExpression re("^([0-9A-Fa-f]{2}([-:]?)){5}[0-9A-Fa-f]{2}$");
        if (!re.match(mac).hasMatch()) {
            LOG_ERROR("Invalid MAC address format: " << mac);
            m_phoneMacStatus = QStringLiteral("Invalid MAC: ") + mac;
            emit phoneMacStatusChanged();
            return;
        }

        // Set environment variable for the running process
        qputenv("PHONE_MAC_ADDRESS", mac.toUtf8());
        LOG_INFO("PHONE_MAC_ADDRESS environment variable set to: " << mac);

        m_phoneMacStatus = QStringLiteral("Updated MAC: ") + mac;
        emit phoneMacStatusChanged();

        // Update QML context property so UI placeholders reflect the new value
        if (parent) {
            parent->rootContext()->setContextProperty("PHONE_MAC_ADDRESS", mac);
        }

        // If a phone socket exists, restart connection using the new MAC
        if (phoneSocket && phoneSocket->isOpen()) {
            phoneSocket->close();
            phoneSocket->deleteLater();
            phoneSocket = nullptr;
        }
        connectToPhone();
    }

    void updatePhoneMacStatus(const QString &status)
    {
        m_phoneMacStatus = status;
        emit phoneMacStatusChanged();
    }

    void setHearingAidEnabled(bool enabled)
    {
        LOG_INFO("Setting hearing aid to: " << (enabled ? "enabled" : "disabled"));
        QByteArray packet = enabled ? AirPodsPackets::HearingAid::ENABLED
                                    : AirPodsPackets::HearingAid::DISABLED;

        writePacketToSocket(packet, "Hearing aid packet written: ");
        m_deviceInfo->setHearingAidEnabled(enabled);

        // The pods refuse ATT reads of 0x2A/0x18 while the feature is off —
        // re-read once it flips on so the settings card populates without
        // waiting for a reconnect.
        if (enabled && m_attManager && m_attManager->isActive())
        {
            m_attManager->readCharacteristic(AttManager::HearingAid);
            m_attManager->readCharacteristic(AttManager::Transparency);
        }
    }

    void setHeadTrackingEnabled(bool enabled)
    {
        LOG_INFO("Setting head tracking to: " << (enabled ? "enabled" : "disabled"));
        m_deviceInfo->setHeadTrackingEnabled(enabled);
        m_settings->setValue("headTracking/enabled", enabled);
        // The packet only exists while the channel is up; if currently
        // connected send it now, otherwise FEATURES_ACK sends START on the
        // next connect.
        if (socket && socket->isOpen())
        {
            writePacketToSocket(enabled ? AirPodsPackets::HeadTracking::START
                                        : AirPodsPackets::HeadTracking::STOP,
                                "Head tracking packet written: ");
        }
        if (enabled && socket && socket->isOpen())
            m_gestureDetector->start();
        else
            m_gestureDetector->stop();
        if (!enabled)
        {
            m_headOrientation.reset();
            m_deviceInfo->setHeadOrientation(0, 0);
        }
    }

    bool writePacketToSocket(const QByteArray &packet, const QString &logMessage)
    {
        if (socket && socket->isOpen())
        {
            socket->write(packet);
            LOG_DEBUG(logMessage << packet.toHex());
            return true;
        }
        else
        {
            LOG_ERROR("Socket is not open, cannot write packet");
            return false;
        }
    }

    bool loadCrossDeviceEnabled() { return m_settings->value("crossdevice/enabled", false).toBool(); }
    void saveCrossDeviceEnabled() { m_settings->setValue("crossdevice/enabled", CrossDevice.isEnabled); }

    int loadEarDetectionSettings() { return m_settings->value("earDetection/setting", MediaController::EarDetectionBehavior::PauseWhenOneRemoved).toInt(); }
    void saveEarDetectionSettings() { m_settings->setValue("earDetection/setting", mediaController->getEarDetectionBehavior()); }

    int loadRetryAttempts() const { return m_settings->value("bluetooth/retryAttempts", 3).toInt(); }
    void saveRetryAttempts(int attempts) { m_settings->setValue("bluetooth/retryAttempts", attempts); }

    void onSystemGoingToSleep()
    {
        if (m_bleManager->isScanning())
        {
            LOG_INFO("Stopping BLE scan before going to sleep");
            m_bleManager->stopScan();
        }
    }
    void onSystemWakingUp()
    {
        LOG_INFO("System is waking up, starting ble scan");
        m_bleManager->startScan();

        // Check if AirPods are already connected and activate A2DP profile
        if (areAirpodsConnected() && m_deviceInfo && !m_deviceInfo->bluetoothAddress().isEmpty())
        {
            LOG_INFO("AirPods already connected after wake-up, re-activating A2DP profile");
            mediaController->setConnectedDeviceMacAddress(m_deviceInfo->bluetoothAddress().replace(":", "_"));

            // Always activate A2DP profile after system wake since the profile might have been lost
            QTimer::singleShot(1000, this, [this]()
            {
                mediaController->activateA2dpProfile();
                LOG_INFO("A2DP profile activation attempted after system wake-up");
            });
        }

        // Also check for already connected devices via BlueZ
        monitor->checkAlreadyConnectedDevices();
    }

private slots:
    void onTrayIconActivated()
    {
        QQuickWindow *window = qobject_cast<QQuickWindow *>(
            QGuiApplication::topLevelWindows().constFirst());
        if (window)
        {
            window->show();
            window->raise();
            window->requestActivate();
        }
    }

    void onOpenApp()
    {
        QObject *rootObject = parent->rootObjects().first();
        if (rootObject) {
            QMetaObject::invokeMethod(rootObject, "reopen", Q_ARG(QVariant, "app"));
        }
        else
        {
            loadMainModule();
        }
    }

    void onOpenSettings()
    {
        QObject *rootObject = parent->rootObjects().first();
        if (rootObject) {
            QMetaObject::invokeMethod(rootObject, "reopen", Q_ARG(QVariant, "settings"));
        }
        else
        {
            loadMainModule();
        }
    }

    void sendHandshake() {
        LOG_INFO("Connected to device, sending initial packets");
        writePacketToSocket(AirPodsPackets::Connection::HANDSHAKE, "Handshake packet written: ");
    }

    void bluezDeviceConnected(const QString &address, const QString &name)
    {
        // Let the pod stack finish its own post-connect setup before we fire
        // the L2CAP request — an open issued in the same millisecond as the
        // ACL event can land while the peer's L2CAP layer is still settling.
        QTimer::singleShot(1200, this, [this, address, name]() {
            QBluetoothDeviceInfo device(QBluetoothAddress(address), name, 0);
            connectToDevice(device);
        });

        // After system reboot, AirPods might be connected but A2DP profile not active
        // Attempt to activate A2DP profile after a delay to ensure connection is established
        QTimer::singleShot(2000, this, [this, address]()
        {
            if (!address.isEmpty())
            {
                QString formattedAddress = address;
                formattedAddress = formattedAddress.replace(":", "_");
                mediaController->setConnectedDeviceMacAddress(formattedAddress);
                mediaController->activateA2dpProfile();
                LOG_INFO("A2DP profile activation attempted for newly connected device");
            }
        });
    }

    // Secondary L2CAP channel (PSM 31) carrying raw ATT PDUs — the hearing
    // aid and transparency characteristics live there, per the Android app.
    // Optional: failures must never disturb the AACP channel.
    void scheduleAttOpen(const QBluetoothAddress &address)
    {
        m_pendingAttAddress = address;
        m_attAttempts = 0;
        setAttState(QStringLiteral("opening"));
        m_attOpenTimer->start(400);
    }

    void openAttChannel(const QBluetoothAddress &address)
    {
        if (m_attSocket || m_attTearingDown)
            return; // already open/opening or mid-teardown

        QBluetoothSocket *raw = new QBluetoothSocket(QBluetoothServiceInfo::L2capProtocol);
        QtBtSocketAdapter *att = new QtBtSocketAdapter(raw, this);
        m_attSocket = att;
        m_attQtSocket = raw;
        setAttState(QStringLiteral("opening"));

        m_attManager = new AttManager(att, this);
        connect(m_attManager, &AttManager::characteristicRead, this,
                [this](quint8 handle, const QByteArray &value) {
                    if (handle == AttManager::HearingAid)
                    {
                        m_hearingAid->updateFromBytes(value);
                    }
                    else
                    {
                        LOG_DEBUG("ATT characteristic 0x" << QString::number(handle, 16)
                                  << " = " << value.toHex());
                    }
                });
        connect(m_attManager, &AttManager::notificationReceived, this,
                [this](quint8 handle, const QByteArray &value) {
                    LOG_DEBUG("ATT notify 0x" << QString::number(handle, 16) << " = " << value.toHex());
                });
        // The open can take seconds — the AACP link may drop and tear this
        // channel down before a queued signal lands. Every lambda validates
        // that 'att' is still the live socket and the manager still exists.
        connect(att, &BtSocketBase::connected, this, [this, att]() {
            if (m_attSocket != att || !m_attManager)
                return;
            LOG_INFO("ATT channel open (PSM 31)");
            m_attAttempts = 0;
            setAttState(QStringLiteral("ready"));
            m_attManager->start();
        });
        connect(att, QOverload<QBluetoothSocket::SocketError>::of(&BtSocketBase::errorOccurred),
                this, [this, att, address](QBluetoothSocket::SocketError) {
                    if (m_attSocket != att)
                        return;
                    const QString err = att->errorString();
                    LOG_WARN("ATT channel open failed: " << err);
                    const int attempt = ++m_attAttempts;
                    teardownAttChannel();
                    if (attempt < 2 && socket && socket->isOpen())
                    {
                        setAttState(QStringLiteral("opening"));
                        QTimer::singleShot(1500, this, [this, address]() {
                            if (socket && socket->isOpen())
                                openAttChannel(address);
                        });
                    }
                    else
                    {
                        setAttState(QStringLiteral("failed"), err);
                    }
                });
        connect(att, &BtSocketBase::disconnected, this, [this, att]() {
            if (m_attSocket != att)
                return;
            teardownAttChannel();
        });

        att->connectToService(address, 0x001F);
    }

    void teardownAttChannel()
    {
        if (m_attTearingDown)
            return;
        m_attTearingDown = true;
        m_hearingAid->setUnavailable();
        if (m_attManager)
        {
            m_attManager->stop();
            m_attManager->deleteLater();
            m_attManager = nullptr;
        }
        if (m_attSocket)
        {
            BtSocketBase *att = m_attSocket;
            m_attSocket = nullptr;
            // Sever deliveries first: queued signals from a torn-down
            // socket must not reach our lambdas after the members move on.
            disconnect(att, nullptr, this, nullptr);
            att->close();
            att->deleteLater();
        }
        if (m_attQtSocket)
        {
            QBluetoothSocket *raw = m_attQtSocket;
            m_attQtSocket = nullptr;
            disconnect(raw, nullptr, this, nullptr);
            raw->deleteLater();
        }
        if (m_attState == QStringLiteral("ready"))
            setAttState(QStringLiteral("off"));
        m_attTearingDown = false;
    }

    void onDeviceDisconnected(const QBluetoothAddress &address)
    {
        LOG_INFO("Device disconnected: " << address.toString());
        teardownAttChannel();
        m_gestureDetector->stop();
        m_headOrientation.reset();
        if (socket)
        {
            LOG_WARN("Socket is still open, closing it");
            socket->close();
            socket = nullptr;
        }
        if (phoneSocket && phoneSocket->isOpen())
        {
            phoneSocket->write(AirPodsPackets::Connection::AIRPODS_DISCONNECTED);
            LOG_DEBUG("AIRPODS_DISCONNECTED packet written: " << AirPodsPackets::Connection::AIRPODS_DISCONNECTED.toHex());
        }

        // Clear the device name and model
        m_deviceInfo->reset();
        m_bleManager->startScan();
        emit airPodsStatusChanged();

        trayManager->resetTrayIcon();
    }

    void bluezDeviceDisconnected(const QString &address, const QString &name)
    {
        if (address == m_deviceInfo->bluetoothAddress())
        {
            onDeviceDisconnected(QBluetoothAddress(address));
        } else {
            LOG_WARN("Disconnected device does not match connected device: " << address << " != " << m_deviceInfo->bluetoothAddress());
        }
    }

    void parseMetadata(const QByteArray &data)
    {
        // Verify the data starts with the METADATA header
        if (!data.startsWith(AirPodsPackets::Parse::METADATA))
        {
            LOG_ERROR("Invalid metadata packet: Incorrect header");
            return;
        }

        int pos = AirPodsPackets::Parse::METADATA.size(); // Start after the header

        // Check if there is enough data to skip the initial bytes (based on example structure)
        if (data.size() < pos + 6)
        {
            LOG_ERROR("Metadata packet too short to parse initial bytes");
            return;
        }
        pos += 6; // Skip 6 bytes after the header as per example structure

        auto extractString = [&data, &pos]() -> QString
        {
            if (pos >= data.size())
            {
                return QString();
            }
            int start = pos;
            while (pos < data.size() && data.at(pos) != '\0')
            {
                ++pos;
            }
            QString str = QString::fromUtf8(data.mid(start, pos - start));
            if (pos < data.size())
            {
                ++pos; // Move past the null terminator
            }
            return str;
        };

        // The information packet is a run of null-terminated strings:
        // name, model, manufacturer, serial, version1, version2,
        // hardwareRevision, updaterIdentifier, leftSerial, rightSerial,
        // version3 (the firmware version shown by the Android app).
        QStringList fields;
        for (int i = 0; i < 11 && pos < data.size(); ++i)
            fields.append(extractString());
        auto field = [&fields](int i) { return i < fields.size() ? fields.at(i) : QString(); };

        m_deviceInfo->setDeviceName(field(0));
        m_deviceInfo->setModelNumber(field(1));
        m_deviceInfo->setManufacturer(field(2));
        m_deviceInfo->setDeviceIdentity(field(3), field(8), field(9), field(10), field(6));

        m_deviceInfo->setModel(parseModelNumber(m_deviceInfo->modelNumber()));
        emit modelChanged();

        // Log extracted metadata
        LOG_INFO("Parsed AirPods metadata:");
        LOG_INFO("Device Name: " << m_deviceInfo->deviceName());
        LOG_INFO("Model Number: " << m_deviceInfo->modelNumber());
        LOG_INFO("Manufacturer: " << m_deviceInfo->manufacturer());
        LOG_INFO("Serial: " << m_deviceInfo->serialNumber() << " L: " << m_deviceInfo->leftSerialNumber()
                 << " R: " << m_deviceInfo->rightSerialNumber());
        LOG_INFO("Firmware: " << m_deviceInfo->firmwareVersion() << " HW: " << m_deviceInfo->hardwareRevision());
    }

    QString getEarStatus(char value)
    {
        return (value == 0x00) ? "In Ear" : (value == 0x01) ? "Out of Ear"
                                                            : "In case";
    }

    void connectToDevice(const QBluetoothDeviceInfo &device)
    {
        if (socket && socket->isOpen() && socket->peerAddress() == device.address())
        {
            LOG_INFO("Already connected to the device: " << device.name());
            return;
        }

        LOG_INFO("Connecting to device: " << device.name());

        // Clean up any existing socket
        if (socket)
        {
            socket->close();
            socket->deleteLater();
            socket = nullptr;
        }

        QBluetoothSocket *localSocket = new QBluetoothSocket(QBluetoothServiceInfo::L2capProtocol);
        socket = localSocket;

        // Connection handler
        auto handleConnection = [this, localSocket]()
        {
            connect(localSocket, &QBluetoothSocket::readyRead, this, [this, localSocket]()
                    {
            QByteArray data = localSocket->readAll();
            QMetaObject::invokeMethod(this, "parseData", Qt::QueuedConnection, Q_ARG(QByteArray, data));
            QMetaObject::invokeMethod(this, "relayPacketToPhone", Qt::QueuedConnection, Q_ARG(QByteArray, data)); });
            sendHandshake();
            // Secondary ATT channel — deferred so the AACP handshake
            // settles first; failures log without disturbing AACP.
            scheduleAttOpen(device.address());
        };

        // Error handler with retry
        auto handleError = [this, device, localSocket](QBluetoothSocket::SocketError error)
        {
            LOG_ERROR("Socket error: " << error << ", " << localSocket->errorString());

            static int retryCount = 0;
            if (retryCount < m_retryAttempts)
            {
                retryCount++;
                LOG_INFO("Retrying connection (attempt " << retryCount << ")");
                QTimer::singleShot(1500, this, [this, device]()
                                   { connectToDevice(device); });
            }
            else
            {
                LOG_ERROR("Failed to connect after 3 attempts");
                retryCount = 0;
            }
        };

        connect(localSocket, &QBluetoothSocket::connected, this, handleConnection);
        connect(localSocket, QOverload<QBluetoothSocket::SocketError>::of(&QBluetoothSocket::errorOccurred),
                this, handleError);

        localSocket->connectToService(device.address(), QBluetoothUuid("74ec2172-0bad-4d01-8f77-997b2be0722a"));
        m_deviceInfo->setBluetoothAddress(device.address().toString());
        notifyAndroidDevice();
    }

    void parseData(const QByteArray &data)
    {
        LOG_DEBUG("Received: " << data.toHex());

        if (data.startsWith(AirPodsPackets::Parse::HANDSHAKE_ACK))
        {
            writePacketToSocket(AirPodsPackets::Connection::SET_SPECIFIC_FEATURES, "Set specific features packet written: ");
        }
        else if (data.startsWith(AirPodsPackets::Parse::FEATURES_ACK))
        {
            writePacketToSocket(AirPodsPackets::Connection::REQUEST_NOTIFICATIONS, "Request notifications packet written: ");
            writePacketToSocket(AirPodsPackets::Connection::EQ_FEATURE_ENABLE, "EQ feature enable packet written: ");
            sendStemConfig();

            if (m_deviceInfo->headTrackingEnabled())
            {
                writePacketToSocket(AirPodsPackets::HeadTracking::START, "Start head tracking packet written: ");
                m_gestureDetector->start();
            }

            QTimer::singleShot(2000, this, [this]() {
                if (m_deviceInfo->batteryStatus().isEmpty()) {
                    writePacketToSocket(AirPodsPackets::Connection::REQUEST_NOTIFICATIONS, "Request notifications packet written: ");
                }
            });
        }
        // Magic Cloud Keys Response
        else if (data.startsWith(AirPodsPackets::MagicPairing::MAGIC_CLOUD_KEYS_HEADER))
        {
            auto keys = AirPodsPackets::MagicPairing::parseMagicCloudKeysPacket(data);
            LOG_INFO("Received Magic Cloud Keys:");
            LOG_INFO("MagicAccIRK: " << keys.magicAccIRK.toHex());
            LOG_INFO("MagicAccEncKey: " << keys.magicAccEncKey.toHex());

            // Store the keys
            m_deviceInfo->setMagicAccIRK(keys.magicAccIRK);
            m_deviceInfo->setMagicAccEncKey(keys.magicAccEncKey);
            m_deviceInfo->saveToSettings(*m_settings);
        }
        // Get CA state
        else if (data.startsWith(AirPodsPackets::ConversationalAwareness::HEADER)) {
            if (auto result = AirPodsPackets::ConversationalAwareness::parseState(data))
            {
                m_deviceInfo->setConversationalAwareness(result.value());
                LOG_INFO("Conversational awareness state received: " << m_deviceInfo->conversationalAwareness());
            }
        }
        // Hearing Aid state
        else if (data.startsWith(AirPodsPackets::HearingAid::HEADER)) {
            if (auto result = AirPodsPackets::HearingAid::parseState(data))
            {
                m_deviceInfo->setHearingAidEnabled(result.value());
                LOG_INFO("Hearing aid state received: " << m_deviceInfo->hearingAidEnabled());
            }
        }
        // Noise Control Mode
        else if (data.size() == 11 && data.startsWith(AirPodsPackets::NoiseControl::HEADER))
        {
            if (auto value = AirPodsPackets::NoiseControl::parseMode(data))
            {
                m_deviceInfo->setNoiseControlMode(value.value());
                LOG_INFO("Noise control mode received: " << m_deviceInfo->noiseControlMode());
            }
        }
        // Ear Detection
        else if (data.size() == 8 && data.startsWith(AirPodsPackets::Parse::EAR_DETECTION))
        {
            m_deviceInfo->getEarDetection()->parseData(data);
            mediaController->handleEarDetection(m_deviceInfo->getEarDetection());
        }
        // Battery Status
        else if ((data.size() == 22 || data.size() == 12) && data.startsWith(AirPodsPackets::Parse::BATTERY_STATUS))
        {
            m_deviceInfo->getBattery()->parsePacket(data);
            m_deviceInfo->updateBatteryStatus();
            LOG_INFO("Battery status: " << m_deviceInfo->batteryStatus());
        }
        // Conversational Awareness Data
        else if (data.size() == 10 && data.startsWith(AirPodsPackets::ConversationalAwareness::DATA_HEADER))
        {
            LOG_INFO("Received conversational awareness data");
            mediaController->handleConversationalAwareness(data);
        }
        else if (data.startsWith(AirPodsPackets::Parse::METADATA))
        {
            parseMetadata(data);
            initiateMagicPairing();
            mediaController->setConnectedDeviceMacAddress(m_deviceInfo->bluetoothAddress().replace(":", "_"));
            if (m_deviceInfo->getEarDetection()->oneOrMorePodsInEar()) // AirPods get added as output device only after this
            {
                mediaController->activateA2dpProfile();
            }
            m_bleManager->stopScan();
            emit airPodsStatusChanged();
        }
        else if (data.startsWith(AirPodsPackets::OneBudANCMode::HEADER)) {
            if (auto value = AirPodsPackets::OneBudANCMode::parseState(data))
            {
                m_deviceInfo->setOneBudANCMode(value.value());
                LOG_INFO("One Bud ANC mode received: " << m_deviceInfo->oneBudANCMode());
            }
        }
        // Head tracking stream — offsets 51-56 carry the calibrated
        // orientation, and the same axis pair feeds the nod/shake gesture
        // detector (Android's interpretation of the axes).
        else if (data.size() >= 70 && data.startsWith(AirPodsPackets::HeadTracking::HEADER))
        {
            AirPodsPackets::HeadTracking::Sample sample;
            if (AirPodsPackets::HeadTracking::parseSample(data, sample))
            {
                m_headOrientation.feed(sample.o1, sample.o2, sample.o3);
                if (m_headOrientation.valid())
                    m_deviceInfo->setHeadOrientation(m_headOrientation.yaw(),
                                                     m_headOrientation.pitch());
                m_gestureDetector->feed(sample.accelHorizontal, sample.accelVertical);
            }
        }
        // Stem press events — dispatch through the configurable action map.
        else if (data.startsWith(AirPodsPackets::StemPress::header()))
        {
            AirPodsPackets::StemPress::Type type;
            AirPodsPackets::StemPress::Bud bud;
            if (AirPodsPackets::StemPress::parse(data, type, bud))
                handleStemPress(type, bud);
        }
        // STEM_CONFIG status (control-command id 0x39) — pods report back
        // which press types are host-intercepted. bit0 single, bit1 double,
        // bit2 triple, bit3 long.
        else if (data.startsWith(AirPodsPackets::StemConfig::header()))
        {
            const quint8 mask = AirPodsPackets::StemConfig::mask(data);
            LOG_DEBUG("Stem config status: " << QString::number(mask, 16));
        }
        // OWNS_CONNECTION status (control-command id 0x06)
        else if (data.startsWith(AirPodsPackets::Ownership::header()) && data.size() >= 8)
        {
            if (auto owns = AirPodsPackets::Ownership::parse(data))
            {
                LOG_INFO("Ownership state: " << (owns.value() ? "ours" : "not ours"));
                m_ownsConnection = owns.value();
            }
        }
        // AUDIO_SOURCE — release ownership when a peer host feeds audio.
        else if (data.startsWith(AirPodsPackets::AudioSource::header()))
        {
            QString mac;
            AirPodsPackets::AudioSource::Type type;
            if (AirPodsPackets::AudioSource::parse(data, mac, type))
            {
                LOG_INFO("Audio source: " << mac << " type " << static_cast<int>(type));
                const QString local = localAdapterMac();
                if (type != AirPodsPackets::AudioSource::Type::None &&
                    !local.isEmpty() && mac.compare(local, Qt::CaseInsensitive) != 0)
                {
                    LOG_INFO("Another device owns the audio session — releasing ownership");
                    writePacketToSocket(AirPodsPackets::Ownership::releasePacket(),
                                        "Ownership release written: ");
                }
            }
        }
        // TiPi connected-device list (multidevice session members)
        else if (data.startsWith(AirPodsPackets::ConnectedDevices::header()))
        {
            m_deviceInfo->setConnectedDevices(AirPodsPackets::ConnectedDevices::parse(data));
            LOG_INFO("TiPi connected devices: " << m_deviceInfo->connectedDevices().join(", "));
        }
        // Custom EQ state packet
        else if (data.startsWith(AirPodsPackets::CustomEq::header()) && data.size() >= 13)
        {
            bool enabled;
            int low, mid, high;
            if (AirPodsPackets::CustomEq::parse(data, enabled, low, mid, high))
                m_deviceInfo->setCustomEq(enabled, low, mid, high);
        }
        // Remaining control-command statuses — the pods push the whole
        // config set after REQUEST_NOTIFICATIONS; dispatch by identifier.
        else if (data.startsWith(ControlCommand::header()) && data.size() >= 8)
        {
            dispatchControlStatus(data);
        }
        else
        {
            LOG_DEBUG("Unrecognized packet format: " << data.toHex());
        }
    }

    // Single-byte control-command statuses, keyed by the identifier at
    // offset 6. Commands already claimed by dedicated branches above never
    // reach here; unknown IDs get logged with their payload so unhandled
    // configs are visible in the debug log instead of silently dropped.
    void dispatchControlStatus(const QByteArray &data)
    {
        namespace AP = AirPodsPackets;
        const quint8 id = static_cast<quint8>(data.at(6));
        const quint8 v0 = data.size() > 7 ? static_cast<quint8>(data.at(7)) : 0;
        auto *di = m_deviceInfo;
        switch (id)
        {
        case 0x01: di->setMicMode(v0); LOG_DEBUG("Mic mode: " << v0); break;
        case 0x0A: di->setPodEarDetection(v0 == 0x01); LOG_DEBUG("Pod ear detection: " << v0); break;
        case 0x17: di->setPressSpeed(v0); LOG_DEBUG("Press speed: " << v0); break;
        case 0x18: di->setHoldDuration(v0); LOG_DEBUG("Hold duration: " << v0); break;
        case 0x1A: di->setListeningModeCycle(v0); LOG_DEBUG("Listening-mode cycle mask: " << v0); break;
        case 0x1C: di->setCrownDirection(v0); LOG_DEBUG("Crown direction: " << v0); break;
        case 0x1E: LOG_DEBUG("Auto-answer mode: " << v0); break;
        case 0x1F: di->setChimeVolume(v0); LOG_DEBUG("Chime volume: " << v0); break;
        case 0x20: di->setAutomaticConnection(v0 == 0x01); LOG_DEBUG("Automatic connection: " << v0); break;
        case 0x23: di->setVolumeSwipeInterval(v0); LOG_DEBUG("Volume swipe interval: " << v0); break;
        case 0x24:
            if (auto m = AP::CallManagement::muteOnSingle(data))
                di->setMuteOnSinglePress(m.value());
            break;
        case 0x25:
            if (auto s = AP::VolumeSwipe::Type::parseState(data))
                di->setVolumeSwipeEnabled(s.value());
            break;
        case 0x26:
            if (auto s = AP::AdaptiveVolume::Type::parseState(data))
                di->setAdaptiveVolume(s.value());
            break;
        case 0x27: LOG_DEBUG("Software mute config: " << v0); break;
        case 0x2F:
            if (auto s = AP::HearingGainSwipe::Type::parseState(data))
                di->setHearingGainSwipe(s.value());
            break;
        case 0x31:
            if (auto s = AP::InCaseTone::Type::parseState(data))
                di->setInCaseTone(s.value());
            break;
        case 0x32:
            if (auto s = AP::SiriMultitone::Type::parseState(data))
                di->setSiriMultitone(s.value());
            break;
        case 0x33: di->setHearingAssistMode(v0); LOG_DEBUG("Hearing assist config: " << v0); break;
        case 0x34:
            if (auto s = AP::AllowOffOption::Type::parseState(data))
                di->setAllowOffOption(s.value());
            break;
        case 0x35:
            if (auto s = AP::SleepDetection::Type::parseState(data))
                di->setSleepDetection(s.value());
            break;
        case 0x36:
            if (auto s = AP::AllowAutoConnect::Type::parseState(data))
                LOG_INFO("Allow auto-connect: " << s.value());
            break;
        case 0x37:
            if (auto s = AP::LoudSoundReduction::Type::parseState(data))
                di->setLoudSoundReduction(s.value());
            break;
        case 0x38: di->setPpeCapLevel(v0); LOG_DEBUG("PPE cap level: " << v0); break;
        case 0x3B:
            if (auto s = AP::OptimizedCharging::Type::parseState(data))
                di->setOptimizedCharging(s.value());
            break;
        // Pod-native press action assignments (0x14/0x15/0x16) — what a
        // press/hold does at the firmware level. Logged rather than
        // surfaced: the value semantics aren't documented anywhere.
        case 0x14: LOG_INFO("Single-press mode (pod-native): " << v0); break;
        case 0x15: LOG_INFO("Double-press mode (pod-native): " << v0); break;
        case 0x16: LOG_INFO("Press-and-hold mode (pod-native): " << v0); break;
        default:
            LOG_DEBUG("Control status id " << QString::number(id, 16) << ": " << data.mid(7).toHex());
            break;
        }
    }

    void connectToPhone() {
        if (!CrossDevice.isEnabled) {
            return;
        }

        if (phoneSocket && phoneSocket->isOpen()) {
            LOG_INFO("Already connected to the phone");
            return;
        }
        QBluetoothAddress phoneAddress("00:00:00:00:00:00"); // Default address, will be overwritten if PHONE_MAC_ADDRESS is set
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();

        if (!env.value("PHONE_MAC_ADDRESS").isEmpty())
        {
            phoneAddress = QBluetoothAddress(env.value("PHONE_MAC_ADDRESS"));
        }
        phoneSocket = new QBluetoothSocket(QBluetoothServiceInfo::L2capProtocol);
        connect(phoneSocket, &QBluetoothSocket::connected, this, [this]() {
            LOG_INFO("Connected to phone");
            if (!lastBatteryStatus.isEmpty()) {
                phoneSocket->write(lastBatteryStatus);
                LOG_DEBUG("Sent last battery status to phone: " << lastBatteryStatus.toHex());
            }
            if (!lastEarDetectionStatus.isEmpty()) {
                phoneSocket->write(lastEarDetectionStatus);
                LOG_DEBUG("Sent last ear detection status to phone: " << lastEarDetectionStatus.toHex());
            }
        });

        connect(phoneSocket, QOverload<QBluetoothSocket::SocketError>::of(&QBluetoothSocket::errorOccurred), this, [this](QBluetoothSocket::SocketError error) {
            LOG_ERROR("Phone socket error: " << error << ", " << phoneSocket->errorString());
        });

        phoneSocket->connectToService(phoneAddress, QBluetoothUuid("1abbb9a4-10e4-4000-a75c-8953c5471342"));
    }

    void relayPacketToPhone(const QByteArray &packet)
    {
        if (!CrossDevice.isEnabled) {
            return;
        }
        if (phoneSocket && phoneSocket->isOpen())
        {
            phoneSocket->write(AirPodsPackets::Phone::NOTIFICATION + packet);
        }
        else
        {
            connectToPhone();
            LOG_WARN("Phone socket is not open, cannot relay packet");
        }
    }

    void handlePhonePacket(const QByteArray &packet) {
        if (packet.startsWith(AirPodsPackets::Phone::NOTIFICATION))
        {
            QByteArray airpodsPacket = packet.mid(4);
            if (socket && socket->isOpen()) {
                socket->write(airpodsPacket);
                LOG_DEBUG("Relayed packet to AirPods: " << airpodsPacket.toHex());
            } else {
                LOG_ERROR("Socket is not open, cannot relay packet to AirPods");
            }
        }
        else if (packet.startsWith(AirPodsPackets::Phone::CONNECTED))
        {
            LOG_INFO("AirPods connected");
            isConnectedLocally = true;
            CrossDevice.isAvailable = false;
        }
        else if (packet.startsWith(AirPodsPackets::Phone::DISCONNECTED))
        {
            LOG_INFO("AirPods disconnected");
            isConnectedLocally = false;
            CrossDevice.isAvailable = true;
        }
        else if (packet.startsWith(AirPodsPackets::Phone::STATUS_REQUEST))
        {
            LOG_INFO("Connection status request received");
            QByteArray response = (socket && socket->isOpen()) ? AirPodsPackets::Phone::CONNECTED
                                                               : AirPodsPackets::Phone::DISCONNECTED;
            phoneSocket->write(response);
            LOG_DEBUG("Sent connection status response: " << response.toHex());
        }
        else if (packet.startsWith(AirPodsPackets::Phone::DISCONNECT_REQUEST))
        {
            LOG_INFO("Disconnect request received");
            if (socket && socket->isOpen()) {
                socket->close();
                LOG_INFO("Disconnected from AirPods");
                QProcess process;
                process.start("bluetoothctl", QStringList() << "disconnect" << m_deviceInfo->bluetoothAddress());
                process.waitForFinished();
                QString output = process.readAllStandardOutput().trimmed();
                LOG_INFO("Bluetoothctl output: " << output);
                isConnectedLocally = false;
                CrossDevice.isAvailable = true;
            }
        }
        else
        {
            if (socket && socket->isOpen()) {
                socket->write(packet);
                LOG_DEBUG("Relayed packet to AirPods: " << packet.toHex());
            } else {
                LOG_ERROR("Socket is not open, cannot relay packet to AirPods");
            }
        }
    }

    void onPhoneDataReceived() {
        QByteArray data = phoneSocket->readAll();
        LOG_DEBUG("Data received from phone: " << data.toHex());
        QMetaObject::invokeMethod(this, "handlePhonePacket", Qt::QueuedConnection, Q_ARG(QByteArray, data));
    }

    void bleDeviceFound(const BleInfo &device)
    {
        if (BLEUtils::isValidIrkRpa(m_deviceInfo->magicAccIRK(), device.address)) {
            m_deviceInfo->setModel(device.modelName);
            auto decryptet = BLEUtils::decryptLastBytes(device.encryptedPayload, m_deviceInfo->magicAccEncKey());
            m_deviceInfo->getBattery()->parseEncryptedPacket(decryptet, device.primaryLeft, device.isThisPodInTheCase, isModelHeadset(m_deviceInfo->model()));
            m_deviceInfo->getEarDetection()->overrideEarDetectionStatus(device.isPrimaryInEar, device.isSecondaryInEar);
        }
    }

public:
    void handleMediaStateChange(MediaController::MediaState state) {
        if (state == MediaController::MediaState::Playing) {
            LOG_INFO("Media started playing, sending disconnect request to Android and taking over audio");
            sendDisconnectRequestToAndroid();
            connectToAirPods(true);
        }
    }

    void sendDisconnectRequestToAndroid()
    {
        if (!CrossDevice.isEnabled) return;

        if (phoneSocket && phoneSocket->isOpen())
        {
            phoneSocket->write(AirPodsPackets::Phone::DISCONNECT_REQUEST);
            LOG_DEBUG("Sent disconnect request to Android: " << AirPodsPackets::Phone::DISCONNECT_REQUEST.toHex());
        }
        else
        {
            LOG_WARN("Phone socket is not open, cannot send disconnect request");
        }
    }

    bool isPhoneConnected() {
        return phoneSocket && phoneSocket->isOpen();
    }

    void connectToAirPods(bool force) {
        if (socket && socket->isOpen()) {
            LOG_INFO("Already connected to AirPods");
            return;
        }

        if (force) {
            LOG_INFO("Forcing connection to AirPods");
            QProcess process;
            process.start("bluetoothctl", QStringList() << "connect" << m_deviceInfo->bluetoothAddress());
            process.waitForFinished();
            QString output = process.readAllStandardOutput().trimmed();
            LOG_INFO("Bluetoothctl output: " << output);
        }
        QBluetoothLocalDevice localDevice;
        const QList<QBluetoothAddress> connectedDevices = localDevice.connectedDevices();
        for (const QBluetoothAddress &address : connectedDevices) {
            QBluetoothDeviceInfo device(address, "", 0);
            LOG_DEBUG("Connected device: " << device.name() << " (" << device.address().toString() << ")");
            if (isAirPodsDevice(device)) {
                connectToDevice(device);
                return;
            }
        }
        LOG_WARN("AirPods not found among connected devices");
    }

    void initializeBluetooth() {
        connectToPhone();

        m_deviceInfo->loadFromSettings(*m_settings);
        if (!areAirpodsConnected()) {
            m_bleManager->startScan();
        }
    }

    void loadMainModule() {
        parent->load(QUrl(QStringLiteral("qrc:/linux/Main.qml")));
    }

signals:
    void noiseControlModeChanged(NoiseControlMode mode);
    void earDetectionStatusChanged(const QString &status);
    void batteryStatusChanged(const QString &status);
    void conversationalAwarenessChanged(bool enabled);
    void adaptiveNoiseLevelChanged(int level);
    void deviceNameChanged(const QString &name);
    void modelChanged();
    void primaryChanged();
    void airPodsStatusChanged();
    void earDetectionBehaviorChanged(int behavior);
    void crossDeviceEnabledChanged(bool enabled);
    void retryAttemptsChanged(int attempts);
    void oneBudANCModeChanged(bool enabled);
    void phoneMacStatusChanged();
    void hearingAidEnabledChanged(bool enabled);
    void attStateChanged();
    void actionConfigChanged();
    void lastGestureChanged();

private:
    QBluetoothSocket *socket = nullptr;
    QBluetoothSocket *phoneSocket = nullptr;
    QByteArray lastBatteryStatus;
    QByteArray lastEarDetectionStatus;
    MediaController* mediaController;
    TrayIconManager *trayManager;
    BluetoothMonitor *monitor;
    QSettings *m_settings;
    AutoStartManager *m_autoStartManager;
    int m_retryAttempts = 3;
    bool m_hideOnStart = false;
    DeviceInfo *m_deviceInfo;
    BleManager *m_bleManager;
    HearingAidModel *m_hearingAid = nullptr;
    AudioSpectrum *m_spectrum = nullptr;
    BtSocketBase *m_attSocket = nullptr;
    AttManager *m_attManager = nullptr;
    QBluetoothSocket *m_attQtSocket = nullptr;
    SystemSleepMonitor *m_systemSleepMonitor = nullptr;
    QString m_phoneMacStatus;
    QTimer *m_attOpenTimer = nullptr;
    QBluetoothAddress m_pendingAttAddress;
    int m_attAttempts = 0;
    bool m_attTearingDown = false;
    QString m_attState = QStringLiteral("off");
    QString m_attError;
    GestureDetector *m_gestureDetector = nullptr;
    HeadOrientation m_headOrientation;
    int m_gestureNodAction = 1;
    int m_gestureShakeAction = 2;
    int m_stemSingleAction = 0;
    int m_stemDoubleAction = 0;
    int m_stemTripleAction = 0;
    int m_stemLongAction = 0;
    QString m_lastGesture;
    bool m_ownsConnection = false;
    QString m_localAdapterMac;
};

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    // Basic style is the cross-platform style that honors palette colors
    // and custom control delegates — the controls are themed to match.
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    // Load translations
    QTranslator *translator = new QTranslator(&app);
    QString locale = QLocale::system().name();

    // Try to load translation from various locations
    QStringList translationPaths = {
        QCoreApplication::applicationDirPath() + "/translations",
        QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/librepods/translations",
        "/usr/share/librepods/translations",
        "/usr/local/share/librepods/translations"
    };

    for (const QString &path : translationPaths) {
        if (translator->load("librepods_" + locale, path)) {
            app.installTranslator(translator);
            break;
        }
    }

    QLocalServer::removeServer("app_server");

    QFile stale("/tmp/app_server");
    if (stale.exists())
        stale.remove();

    QLocalSocket socket_check;
    socket_check.connectToServer("app_server");

    if (socket_check.waitForConnected(300)) {
        LOG_INFO("Another instance already running! Reopening window...");

        socket_check.write("reopen");
        socket_check.flush();
        socket_check.waitForBytesWritten(200);
        socket_check.disconnectFromServer();

        return 0;
    }
    app.setDesktopFileName("me.kavishdevar.librepods");
    app.setQuitOnLastWindowClosed(false);

    bool debugMode = false;
    bool hideOnStart = false;
    for (int i = 1; i < argc; ++i) {
        if (QString(argv[i]) == "--debug")
            debugMode = true;

        if (QString(argv[i]) == "--hide")
            hideOnStart = true;
    }

    QQmlApplicationEngine engine;
    qmlRegisterType<Battery>("me.kavishdevar.Battery", 1, 0, "Battery");
    qmlRegisterType<DeviceInfo>("me.kavishdevar.DeviceInfo", 1, 0, "DeviceInfo");
    AirPodsTrayApp *trayApp = new AirPodsTrayApp(debugMode, hideOnStart, &engine);
    engine.rootContext()->setContextProperty("airPodsTrayApp", trayApp);

    // Expose PHONE_MAC_ADDRESS environment variable to QML for placeholder in settings
    {
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        QString phoneMacEnv = env.value("PHONE_MAC_ADDRESS", "");
        engine.rootContext()->setContextProperty("PHONE_MAC_ADDRESS", phoneMacEnv);
        // Initialize the visible status in the GUI
        trayApp->updatePhoneMacStatus(phoneMacEnv.isEmpty() ? QStringLiteral("No phone MAC set") : phoneMacEnv);
    }

    engine.addImageProvider("qrcode", new QRCodeImageProvider());
    trayApp->loadMainModule();

    QLocalServer server;
    QLocalServer::removeServer("app_server");

    if (!server.listen("app_server"))
    {
        LOG_ERROR("Unable to start the listening server");
        LOG_DEBUG("Server error: " << server.errorString());
    }
    else
    {
        LOG_DEBUG("Server started, waiting for connections...");
    }
    QObject::connect(&server, &QLocalServer::newConnection, [&]() {
        QLocalSocket* socket = server.nextPendingConnection();
        // Handles Proper Connection
        QObject::connect(socket, &QLocalSocket::readyRead, [socket, &engine, &trayApp]() {
            QString msg = socket->readAll();
            // Check if the message is "reopen", if so, trigger onOpenApp function
            if (msg == "reopen") {
                LOG_INFO("Reopening app window");
                QObject *rootObject = engine.rootObjects().first();
                if (rootObject) {
                    QMetaObject::invokeMethod(rootObject, "reopen", Q_ARG(QVariant, "app"));
                }
                else
                {
                    trayApp->loadMainModule();
                }
            }
            else if (msg == "noise:off") {
                trayApp->setNoiseControlModeInt(0);
            }
            else if (msg == "noise:anc") {
                trayApp->setNoiseControlModeInt(1);
            }
            else if (msg == "noise:transparency") {
                trayApp->setNoiseControlModeInt(2);
            }
            else if (msg == "noise:adaptive") {
                trayApp->setNoiseControlModeInt(3);
            }
            else if (msg == "settings") {
                QObject *rootObject = engine.rootObjects().first();
                if (rootObject) {
                    QMetaObject::invokeMethod(rootObject, "reopen", Q_ARG(QVariant, "settings"));
                }
                else
                {
                    trayApp->loadMainModule();
                }
            }
            else
            {
                LOG_ERROR("Unknown message received: " << msg);
            }
            socket->disconnectFromServer();
        });
        // Handles connection errors
        QObject::connect(socket, &QLocalSocket::errorOccurred, [socket]() {
            LOG_ERROR("Failed to connect to the duplicate app instance");
            LOG_DEBUG("Connection error: " << socket->errorString());
        });

        // Handle server-level errors
        QObject::connect(&server, &QLocalServer::serverError, [&]() {
            LOG_ERROR("Server failed to accept a new connection");
            LOG_DEBUG("Server error: " << server.errorString());
        });
    });

    QObject::connect(&app, &QCoreApplication::aboutToQuit, [&]() {
        LOG_DEBUG("Application quitting. Cleaning up local server...");

        if (server.isListening()) {
            server.close();
        }

        QLocalServer::removeServer("app_server");
        QFile stale("/tmp/app_server");
        if (stale.exists())
            stale.remove();
    });
    return app.exec();
}

#include "main.moc"
