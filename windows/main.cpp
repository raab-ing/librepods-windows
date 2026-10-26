#include "l2capsocket.h" // pulls winsock2.h; must precede windows.h
#include "driverl2capsocket.h"
#include "svcchannelsocket.h"
#include <bluetoothapis.h>
#include <shellapi.h>
#include "driver/shared/librepods_driver_protocol.h"

#include <QSettings>
#include <QLocalServer>
#include <QLocalSocket>
#include <QApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QBluetoothLocalDevice>
#include <QBluetoothDeviceInfo>
#include <QBluetoothSocket>
#include <QQuickWindow>
#include <QQuickStyle>
#include <QLoggingCategory>
#include <QThread>
#include <atomic>
#include <thread>
#include <QTimer>
#include <QEvent>
#include <QDateTime>
#include <QProcess>
#include <QProcessEnvironment>
#include <QFileInfo>
#include <QRegularExpression>
#include <QTranslator>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QPointer>
#include <QScreen>
#include <QElapsedTimer>

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
#include "gesturedetector.h"
#include "headorientation.h"
#include "crashdump.h"
#include "ble/blemanager.h"
#include "ble/bleutils.h"
#include "ble/proximityparser.h"
#include "QRCodeImageProvider.hpp"
#include "systemsleepmonitor.hpp"

#include <winrt/base.h>

using namespace AirpodsTrayApp::Enums;

Q_LOGGING_CATEGORY(librepods, "librepods")

// Socket type used for the AirPods control channel and the cross-device phone
// link. The driver-backed transport is preferred when the LibrePods KMDF
// profile driver is installed (Windows has no user-mode L2CAP); the Winsock
// AF_BTH socket is kept as a fallback.
using BtSocket = BtSocketBase;

// ATT channel (PSM 31) retry pacing. The pods ignore the L2CAP connect for
// long stretches and then accept it instantly when their state allows — a
// failed open already costs a ~15s kernel timeout, so short delays still
// leave the link mostly idle.
static constexpr int kAttFastRetryDelayMs = 1500;
static constexpr int kAttRetryDelayMs = 5000;
static constexpr int kAttMaxAttempts = 40;
// Pull-up cadence while the ACL link is down. The pods' page-scan window is
// narrow while another host holds them — 10s gives us several shots per
// minute at catching it instead of one every 30s.
static constexpr int kPullupRetryMs = 10000;

// AACP wedge self-healing: a run of consecutive channel-open failures (or
// open-but-silent channels) means the pod-side/stack-side L2CAP state is
// stuck — retrying the same open forever doesn't help. Past the threshold we
// reset the Bluetooth radio once, then let the normal retry loop land the
// link again. Cooldown keeps resets rare even if the wedge persists.
static constexpr int kWedgeThreshold = 8;
static constexpr int kChannelSilentMs = 15000;
static constexpr int kRadioResetCooldownMs = 10 * 60 * 1000;

class AirPodsTrayApp : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool airpodsConnected READ areAirpodsConnected NOTIFY airPodsStatusChanged)
    Q_PROPERTY(bool deviceLinkConnected READ deviceLinkConnected NOTIFY airPodsStatusChanged)
    Q_PROPERTY(int earDetectionBehavior READ earDetectionBehavior WRITE setEarDetectionBehavior NOTIFY earDetectionBehaviorChanged)
    Q_PROPERTY(bool crossDeviceEnabled READ crossDeviceEnabled WRITE setCrossDeviceEnabled NOTIFY crossDeviceEnabledChanged)
    Q_PROPERTY(AutoStartManager *autoStartManager READ autoStartManager CONSTANT)
    Q_PROPERTY(int retryAttempts READ retryAttempts WRITE setRetryAttempts NOTIFY retryAttemptsChanged)
    Q_PROPERTY(bool earPopupEnabled READ earPopupEnabled WRITE setEarPopupEnabled NOTIFY earPopupEnabledChanged)
    Q_PROPERTY(int earPopupSecs READ earPopupSecs WRITE setEarPopupSecs NOTIFY earPopupSecsChanged)
    Q_PROPERTY(bool hideOnStart READ hideOnStart CONSTANT)
    Q_PROPERTY(DeviceInfo *deviceInfo READ deviceInfo CONSTANT)
    Q_PROPERTY(HearingAidModel *hearingAidModel READ hearingAidModel CONSTANT)
    Q_PROPERTY(AudioSpectrum *spectrum READ spectrum CONSTANT)
    Q_PROPERTY(QString phoneMacStatus READ phoneMacStatus NOTIFY phoneMacStatusChanged)
    Q_PROPERTY(bool hearingAidEnabled READ hearingAidEnabled WRITE setHearingAidEnabled NOTIFY hearingAidEnabledChanged)
    Q_PROPERTY(bool driverAvailable READ driverAvailable CONSTANT)
    Q_PROPERTY(bool reduceMotion READ reduceMotion CONSTANT)
    Q_PROPERTY(QString statusHint READ statusHint NOTIFY airPodsStatusChanged)
    // ATT secondary channel state for the settings UI:
    // "off" | "opening" | "ready" | "failed" (+ attError details)
    Q_PROPERTY(QString attState READ attState NOTIFY attStateChanged)
    Q_PROPERTY(QString attError READ attError NOTIFY attStateChanged)
    // Configurable action maps (0 none, 1 play/pause, 2 next, 3 previous,
    // 4 volume up, 5 volume down, 6 cycle noise modes)
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
          m_hearingAid(new HearingAidModel(this)), m_spectrum(new AudioSpectrum(this)),
          m_driverAvailable(DriverL2CapSocket::isDriverAvailable())
        , m_systemSleepMonitor(new SystemSleepMonitor(this))
    {
        // Only DEBUG output is gated — INFO/WARN describe the connect
        // lifecycle and must reach the file log in release builds too.
        QLoggingCategory::setFilterRules(
            QString("librepods.debug=%1\nlibrepods.*.debug=%1").arg(debugMode ? "true" : "false"));
        LOG_INFO("Initializing LibrePods");

        // Honor Windows' "Show animations in Windows" accessibility toggle —
        // QML gates entrance/pulse/resize motion on this.
        {
            BOOL animOn = TRUE;
            if (SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animOn, 0))
                m_reduceMotion = !animOn;
        }

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
        // CA menu entry only exists on models that support it — modelChanged
        // is the shared capability signal, and fire once for the initial state.
        connect(m_deviceInfo, &DeviceInfo::modelChanged, trayManager, [this]() {
            trayManager->setConversationalAwarenessSupported(m_deviceInfo->supportsConversationalAwareness());
        });
        trayManager->setConversationalAwarenessSupported(m_deviceInfo->supportsConversationalAwareness());
        const auto updateTrayEarStatus = [this]() {
            trayManager->updateEarStatus(m_deviceInfo->isLeftPodInEar(), m_deviceInfo->isRightPodInEar(),
                                         m_deviceInfo->isLeftPodInCase(), m_deviceInfo->isRightPodInCase(),
                                         m_deviceInfo->podIcon(), m_deviceInfo->caseIcon(),
                                         m_deviceInfo->isHeadset());
        };
        connect(m_deviceInfo, &DeviceInfo::primaryChanged, trayManager, updateTrayEarStatus);
        connect(m_deviceInfo, &DeviceInfo::modelChanged, trayManager, updateTrayEarStatus);

        // iOS parity: opening the case lid surfaces the battery card. Lid
        // state arrives via BLE ads — no AACP channel needed, so this popup
        // still fires while the control channel is wedged.
        connect(m_deviceInfo, &DeviceInfo::proximityStateChanged, this, [this]() {
            const bool lidOpen = m_deviceInfo->caseLidOpen();
            if (lidOpen && !m_prevLidOpen)
                flashWindowAtTray(tr("Case opened"));
            m_prevLidOpen = lidOpen;
        });

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

        m_reconnectTimer = new QTimer(this);
        m_reconnectTimer->setSingleShot(true);
        connect(m_reconnectTimer, &QTimer::timeout, this, [this]() {
            if (!m_linkConnected && m_lastDevice.isValid())
                connectToDevice(m_lastDevice);
        });

        // Link-down events run through a grace window: micro-flaps during
        // case transitions recover in well under 2s and shouldn't tear down
        // the socket, blank the UI, or spam disconnect notifications.
        m_disconnectGraceTimer = new QTimer(this);
        m_disconnectGraceTimer->setSingleShot(true);
        m_disconnectGraceTimer->setInterval(2000);
        connect(m_disconnectGraceTimer, &QTimer::timeout, this, [this]() {
            if (m_linkConnected)
                return; // recovered during the grace window
            onDeviceDisconnected(m_pendingDisconnectAddress);
        });

        // The disconnect popup fires only on a *sustained* disconnect —
        // during first insertion the link naturally flaps while audio
        // profiles come up, and a popup firing in that window reads as a
        // false "disconnected" right as things are actually connecting.
        m_disconnectNotifyTimer = new QTimer(this);
        m_disconnectNotifyTimer->setSingleShot(true);
        m_disconnectNotifyTimer->setInterval(8000);
        connect(m_disconnectNotifyTimer, &QTimer::timeout, this, [this]() {
            if (!m_linkConnected && !areAirpodsConnected())
                flashWindowAtTray(tr("AirPods disconnected"));
        });

        // Deferred ATT open: the pods juggle the AACP handshake and audio
        // profiles at the same time — give the control session a moment to
        // settle before requesting the second channel.
        m_attOpenTimer = new QTimer(this);
        m_attOpenTimer->setSingleShot(true);
        connect(m_attOpenTimer, &QTimer::timeout, this, [this]() {
            if (areAirpodsConnected() && m_pendingAttAddress != QBluetoothAddress())
                openAttChannel(m_pendingAttAddress);
        });

        // ATT retry: the pods accept the PSM-31 channel only when they're in
        // a receptive state, so failed opens retry on a cadence instead of
        // giving up after two tries.
        m_attRetryTimer = new QTimer(this);
        m_attRetryTimer->setSingleShot(true);
        connect(m_attRetryTimer, &QTimer::timeout, this, [this]() {
            if (areAirpodsConnected() && !m_attSocket && !m_attTearingDown &&
                m_pendingAttAddress != QBluetoothAddress())
                openAttChannel(m_pendingAttAddress);
        });

        // Ear-detection popup: flash the window next to the tray when a pod
        // changes state (out of ear / into case / back in), then auto-hide.
        m_popupHideTimer = new QTimer(this);
        m_popupHideTimer->setSingleShot(true);
        connect(m_popupHideTimer, &QTimer::timeout, this, [this]() {
            m_popupShown = false;
            m_lastPopupHideMs = QDateTime::currentMSecsSinceEpoch();
            // Route through QML so the exit animation runs — raw hide()
            // pops the window instantly.
            QObject *root = this->parent ? this->parent->rootObjects().constFirst() : nullptr;
            if (root && QMetaObject::invokeMethod(root, "dismissFlyout"))
                return;
            if (QWindow *w = mainFlyoutWindow())
                w->hide();
        });

        // Wedge watchdog: a channel that opens but never delivers a single
        // packet is dead weight — count it as a failure so the radio-reset
        // heuristic sees the same signal as plain open failures, then recycle
        // so the retry loop can try a fresh channel.
        m_channelDataWatchdog = new QTimer(this);
        m_channelDataWatchdog->setSingleShot(true);
        connect(m_channelDataWatchdog, &QTimer::timeout, this, [this]() {
            if (!socket || !socket->isOpen())
                return;
            m_channelFailStreak++;
            LOG_WARN("AACP channel silent " << kChannelSilentMs / 1000
                     << "s after open (streak " << m_channelFailStreak << ") — recycling");
            socket->close();
            maybeRadioReset();
        });

        // Head-tracking starvation watchdog: enabled + worn + connected but
        // no 0x17 sample in 15s means the pods dropped the stream (warm
        // reconnect without FEATURES_ACK, or the phone re-asserted
        // ownership). Re-arming START is exactly what a manual toggle did.
        m_headStreamWatchdog = new QTimer(this);
        m_headStreamWatchdog->setInterval(15000);
        connect(m_headStreamWatchdog, &QTimer::timeout, this, [this]() {
            if (!m_deviceInfo->headTrackingEnabled() || !socket || !socket->isOpen())
                return;
            if (!m_deviceInfo->isLeftPodInEar() && !m_deviceInfo->isRightPodInEar())
                return;
            if (QDateTime::currentMSecsSinceEpoch() - m_lastHeadSampleMs < 15000)
                return;
            LOG_INFO("Head tracking stream stalled — re-arming START");
            writePacketToSocket(AirPodsPackets::HeadTracking::START,
                                "Head tracking re-arm packet written: ");
            updateGestureDetection();
        });
        m_headStreamWatchdog->start();

        // Head gestures run on the AACP 0x17 acceleration stream. Android
        // only arms the detector during an incoming call; here it maps to
        // media actions, so any confident false positive touches playback.
        // Two guards: the detector only runs while an action is configured
        // (updateGestureDetection), and a cooldown stops rhythmic garbage
        // from machine-gunning toggles.
        m_gestureDetector = new GestureDetector(this);
        auto dispatchGesture = [this](int action) {
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            if (now - m_lastGestureActionMs < kGestureActionCooldownMs)
            {
                LOG_INFO("Gesture action suppressed (cooldown)");
                return;
            }
            m_lastGestureActionMs = now;
            runAction(action);
        };
        connect(m_gestureDetector, &GestureDetector::gestureAccepted, this, [this, dispatchGesture]() {
            LOG_INFO("Head gesture detected: nod (accept)");
            m_lastGesture = tr("Nod (accept)");
            emit lastGestureChanged();
            dispatchGesture(m_gestureNodAction);
        });
        connect(m_gestureDetector, &GestureDetector::gestureRejected, this, [this, dispatchGesture]() {
            LOG_INFO("Head gesture detected: shake (reject)");
            m_lastGesture = tr("Shake (reject)");
            emit lastGestureChanged();
            dispatchGesture(m_gestureShakeAction);
        });

        monitor = new BluetoothMonitor(this);
        connect(monitor, &BluetoothMonitor::deviceConnected, this, &AirPodsTrayApp::nativeDeviceConnected);
        connect(monitor, &BluetoothMonitor::deviceDisconnected, this, &AirPodsTrayApp::nativeDeviceDisconnected);
        connect(monitor, &BluetoothMonitor::devicePaired, this, &AirPodsTrayApp::nativeDevicePaired);

        connect(m_bleManager, &BleManager::deviceFound, this, &AirPodsTrayApp::bleDeviceFound);
        connect(m_deviceInfo->getBattery(), &Battery::primaryChanged, this, &AirPodsTrayApp::primaryChanged);
        connect(m_deviceInfo, &DeviceInfo::bluetoothAddressChanged, this, [this]() { emit airPodsStatusChanged(); });
        connect(m_systemSleepMonitor, &SystemSleepMonitor::systemGoingToSleep, this, &AirPodsTrayApp::onSystemGoingToSleep);
        connect(m_systemSleepMonitor, &SystemSleepMonitor::systemWakingUp, this, &AirPodsTrayApp::onSystemWakingUp);

        // Load settings
        CrossDevice.isEnabled = loadCrossDeviceEnabled();
        setEarDetectionBehavior(loadEarDetectionSettings());
        setRetryAttempts(loadRetryAttempts());
        m_earPopupEnabled = m_settings->value("ui/earPopupEnabled", true).toBool();
        m_earPopupSecs = m_settings->value("ui/earPopupSecs", 4).toInt();
        m_deviceInfo->setHeadTrackingEnabled(m_settings->value("headTracking/enabled", false).toBool());
        // Opt-in: Android's detector does nothing outside calls, so fresh
        // installs default to no media action — an always-on play/pause
        // mapping makes ordinary head movement a phantom button.
        m_gestureNodAction = m_settings->value("gestures/nodAction", 0).toInt();
        m_gestureShakeAction = m_settings->value("gestures/shakeAction", 0).toInt();
        m_stemSingleAction = m_settings->value("stem/singleAction", 0).toInt();
        m_stemDoubleAction = m_settings->value("stem/doubleAction", 0).toInt();
        m_stemTripleAction = m_settings->value("stem/tripleAction", 0).toInt();
        m_stemLongAction = m_settings->value("stem/longAction", 0).toInt();

        LOG_INFO("AirPodsTrayApp initialized");

        // Dev harness: LIBREPODS_FAKE_MODEL=<model number> simulates a
        // connected link at that model so per-model UI gating can be
        // exercised without hardware (e.g. =A2096 for AirPods Max).
        const QString fakeModel = qEnvironmentVariable("LIBREPODS_FAKE_MODEL");
        if (!fakeModel.isEmpty()) {
            m_fakeConnected = true;
            m_deviceInfo->setDeviceName(QStringLiteral("Simulated AirPods"));
            m_deviceInfo->setBluetoothAddress(QStringLiteral("AA:BB:CC:DD:EE:FF"));
            m_deviceInfo->setManufacturer(QStringLiteral("Apple Inc."));
            m_deviceInfo->setModelNumber(fakeModel);
            m_deviceInfo->setModel(parseModelNumber(fakeModel));
            m_deviceInfo->setDeviceIdentity(QStringLiteral("SIM123456"),
                                            QStringLiteral("SIML00001"),
                                            QStringLiteral("SIMR00001"),
                                            QStringLiteral("000000"),
                                            QStringLiteral("1.0"));
            // Battery via the real parse path: L 87 / R 82 / case 64,
            // or headset 90 on Max.
            if (m_deviceInfo->isHeadset()) {
                m_deviceInfo->getBattery()->parsePacket(
                    QByteArray::fromHex("0400040004000101015a0201"));
                m_deviceInfo->getEarDetection()->overrideEarDetectionStatus(true, false);
            } else {
                m_deviceInfo->getBattery()->parsePacket(
                    QByteArray::fromHex("04000400040003040157020102015202010801400201"));
                // LIBREPODS_FAKE_DOCKED=left|right|both puts pod(s) in the
                // case (bytes 6/7 = 0x02 InCase / 0x00 InEar).
                const QString docked = qEnvironmentVariable("LIBREPODS_FAKE_DOCKED");
                if (docked == QLatin1String("left") || docked == QLatin1String("both")) {
                    m_deviceInfo->getEarDetection()->parseData(
                        QByteArray::fromHex(docked == QLatin1String("both")
                            ? "0000000000000202" : "0000000000000200"));
                    m_deviceInfo->updateProximityState(true, docked == QLatin1String("both"), false);
                } else if (docked == QLatin1String("right")) {
                    m_deviceInfo->getEarDetection()->parseData(
                        QByteArray::fromHex("0000000000000002"));
                    m_deviceInfo->updateProximityState(true, false, false);
                } else {
                    m_deviceInfo->getEarDetection()->overrideEarDetectionStatus(true, true);
                }
            }
            m_deviceInfo->setNoiseControlModeInt(2);
            emit airPodsStatusChanged();
            LOG_INFO("FAKE MODEL harness active:" << fakeModel);
            return;
        }

        initializeBluetooth();

        // Deferred init OFF the GUI thread: on a wedged Bluetooth stack the
        // monitor's CreateWatcher, the WinRT enumerations and Qt's
        // connectedDevices() can all block indefinitely — any of them in the
        // constructor froze startup before initializeBluetooth() (and its
        // remembered-MAC fallback) ever ran.
        QPointer<AirPodsTrayApp> guard(this);
        QPointer<BluetoothMonitor> monGuard(monitor);
        std::thread([guard, monGuard]() {
            // Warm the svc transport here — its spawn+probe can take ~1s,
            // and running it inside connectToDevice() on the GUI thread
            // would freeze the flyout during first connect. Only bother
            // when paired pods are remembered (no pair → no connect).
            if (guard && !guard->m_deviceInfo->bluetoothAddress().isEmpty())
                guard->ensureService();
            if (monGuard)
            {
                monGuard->startMonitoring();
                monGuard->checkAlreadyConnectedDevices();
                monGuard->checkPairedDevices();
            }
            QBluetoothLocalDevice localDevice;
            const QList<QBluetoothAddress> addrs = localDevice.connectedDevices();
            if (!guard)
                return;
            QMetaObject::invokeMethod(guard, [guard, addrs]() {
                for (const QBluetoothAddress &address : addrs) {
                    QBluetoothDeviceInfo device(address, "", 0);
                    if (guard->isAirPodsDevice(device)) {
                        guard->connectToDevice(device);
                        return;
                    }
                }
            }, Qt::QueuedConnection);
        }).detach();
    }

    ~AirPodsTrayApp() {
        saveCrossDeviceEnabled();
        saveEarDetectionSettings();

        delete socket;
        delete phoneSocket;
    }

    bool areAirpodsConnected() const {
        if (m_fakeConnected)
            return true;
        return socket && socket->isOpen() && socket->state() == QBluetoothSocket::SocketState::ConnectedState;
    }
    // The OS-level Bluetooth link (what Windows Settings shows as
    // Connected/Disconnected), tracked by BluetoothMonitor's
    // ConnectionStatus watcher. Independent of whether our AACP control
    // socket could be opened.
    bool deviceLinkConnected() const {
        // Presented state lags m_linkConnected by the disconnect grace
        // window so micro-flaps don't strobe the UI.
        return m_presentedLinkUp || areAirpodsConnected();
    }
    void setLinkConnected(bool connected) {
        if (m_linkConnected != connected)
        {
            m_linkConnected = connected;
            emit airPodsStatusChanged();
        }
    }
    int earDetectionBehavior() const { return mediaController->getEarDetectionBehavior(); }
    bool crossDeviceEnabled() const { return CrossDevice.isEnabled; }
    AutoStartManager *autoStartManager() const { return m_autoStartManager; }
    int retryAttempts() const { return m_retryAttempts; }
    bool earPopupEnabled() const { return m_earPopupEnabled; }
    void setEarPopupEnabled(bool enabled)
    {
        if (m_earPopupEnabled == enabled)
            return;
        m_earPopupEnabled = enabled;
        m_settings->setValue("ui/earPopupEnabled", enabled);
        if (!enabled)
            m_popupHideTimer->stop();
        emit earPopupEnabledChanged(enabled);
    }
    int earPopupSecs() const { return m_earPopupSecs; }
    void setEarPopupSecs(int secs)
    {
        secs = qBound(2, secs, 60);
        if (m_earPopupSecs == secs)
            return;
        m_earPopupSecs = secs;
        m_settings->setValue("ui/earPopupSecs", secs);
        emit earPopupSecsChanged(secs);
    }
    bool hideOnStart() const { return m_hideOnStart; }
    DeviceInfo *deviceInfo() const { return m_deviceInfo; }
    HearingAidModel *hearingAidModel() const { return m_hearingAid; }
    AudioSpectrum *spectrum() const { return m_spectrum; }
    QString phoneMacStatus() const { return m_phoneMacStatus; }
    bool hearingAidEnabled() const { return m_deviceInfo->hearingAidEnabled(); }
    bool driverAvailable() const { return m_driverAvailable; }
    bool reduceMotion() const { return m_reduceMotion; }
    // Plain-language one-liner shown in the UI when the channel is down.
    QString statusHint() const {
        if (areAirpodsConnected())
            return {};
        if (!m_driverAvailable)
            return QStringLiteral("Limited mode — controls unavailable");
        if (m_deviceInfo->bluetoothAddress().isEmpty())
            return QStringLiteral("No paired AirPods found");
        if (!deviceLinkConnected())
            return QStringLiteral("Not connected");
        // Link is up but the control channel hasn't opened yet.
        return QStringLiteral("Connecting…");
    }

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

    // Detector only runs while it can actually dispatch something —
    // head tracking streaming and at least one mapped action. With no
    // actions configured there is nothing for a detection to do except
    // be a false-positive surface.
    void updateGestureDetection()
    {
        const bool wanted = m_deviceInfo->headTrackingEnabled()
                            && (m_gestureNodAction != 0 || m_gestureShakeAction != 0);
        if (wanted)
            m_gestureDetector->start();
        else
            m_gestureDetector->stop();
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
        {
            // Qt's Windows Bluetooth backend is a dummy — QBluetoothLocalDevice
            // returns a bogus address, which made every AUDIO_SOURCE report for
            // our own adapter look like "another device" and released ownership
            // in a loop. Read the real radio address via Win32 instead.
            HANDLE hRadio = NULL;
            BLUETOOTH_FIND_RADIO_PARAMS params = { sizeof(params) };
            HBLUETOOTH_RADIO_FIND hFind = BluetoothFindFirstRadio(&params, &hRadio);
            if (hFind)
            {
                BLUETOOTH_RADIO_INFO info = { sizeof(info) };
                if (BluetoothGetRadioInfo(hRadio, &info) == ERROR_SUCCESS)
                    m_localAdapterMac = QBluetoothAddress(info.address.ullLong).toString().toUpper();
                BluetoothFindRadioClose(hFind);
                CloseHandle(hRadio);
            }
            if (m_localAdapterMac.isEmpty())
                m_localAdapterMac = QBluetoothLocalDevice().address().toString().toUpper();
        }
        return m_localAdapterMac;
    }

public:
    QString devPoseLine() const
    {
        const bool rp = m_deviceInfo->getBattery()->getPrimaryPod() == Battery::Component::Right;
        return QStringLiteral("yaw=%1 pitch=%2 o=%3,%4,%5,%6 accel=%7,%8 pod=%9 age=%10ms axes=%11,%12 nb=%13,%14 pkt=%15")
            .arg(m_deviceInfo->headYaw()).arg(m_deviceInfo->headPitch())
            .arg(m_rawO1).arg(m_rawO2).arg(m_rawO3).arg(m_rawO4)
            .arg(m_rawAccH).arg(m_rawAccV)
            .arg(rp ? QStringLiteral("R") : QStringLiteral("L"))
            .arg(QDateTime::currentMSecsSinceEpoch() - m_lastHeadSampleMs)
            .arg(m_headOrientation.haveYaw() ? 1 : 0)
            .arg(m_headOrientation.havePitch() ? 1 : 0)
            .arg(m_headOrientation.yawBursts())
            .arg(m_headOrientation.pitchBursts())
            .arg(QString::fromLatin1(m_lastHeadPacket.toHex()));
    }

public slots:
    void setGestureNodAction(int action)   { if (m_gestureNodAction != action)   { m_gestureNodAction = action;   m_settings->setValue("gestures/nodAction", action);   updateGestureDetection(); emit actionConfigChanged(); } }
    void setGestureShakeAction(int action) { if (m_gestureShakeAction != action) { m_gestureShakeAction = action; m_settings->setValue("gestures/shakeAction", action); updateGestureDetection(); emit actionConfigChanged(); } }
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
        LOG_INFO("Writing custom EQ: enabled=" << enabled << " bands=" << low << "/" << mid << "/" << high);
        if (writePacketToSocket(AirPodsPackets::CustomEq::packet(enabled, low, mid, high),
                                "Custom EQ packet written: "))
            m_deviceInfo->setCustomEq(enabled, low, mid, high);
    }

    // TiPi takeover — claim ownership of the pods' multidevice session and
    // nudge the current owner off it (best effort; the actual A2DP link
    // still has to come up at the OS level).
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
                                    local, target, QStringLiteral("Windows"), false),
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

    bool isAirPodsDevice(const QBluetoothDeviceInfo &device)
    {
        return device.serviceUuids().contains(QBluetoothUuid("74ec2172-0bad-4d01-8f77-997b2be0722a"));
    }

    // Coarse model inference from the Windows device name ("AirPods Pro - Find
    // My" etc.) — enough to pick the right icons before AACP metadata arrives.
    static AirPodsModel inferModelFromName(const QString &name)
    {
        const QString n = name.toLower();
        if (n.contains("max")) return AirPodsModel::AirPodsMaxUSBC;
        if (n.contains("pro")) return AirPodsModel::AirPodsPro2USBC;
        if (n.contains("airpods 4")) return AirPodsModel::AirPods4;
        if (n.contains("airpods 3")) return AirPodsModel::AirPods3;
        if (n.contains("airpods")) return AirPodsModel::AirPods2;
        return AirPodsModel::Unknown;
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
        // Optimistic update — the mode echo can lag ~1s, during which the UI
        // shows a stale mode (adaptive slider stuck visible after switching
        // away). The echo still corrects us if the pods reject it.
        m_deviceInfo->setNoiseControlMode(mode);
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
            LOG_ERROR("Socket not open, cannot start magic pairing");
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
    // Bool toggles share one helper; byte-valued commands get their own.
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
        updateGestureDetection();
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
    // Link-down pull-up with backoff: consecutive open failures mean the
    // pod-side channel table is still flushing our half-open attempts, and
    // hammering keeps it wedged (the link historically only landed once the
    // app stopped trying). Each failure lengthens the wait toward a 60s cap;
    // the streak resets on the first inbound packet.
    void scheduleChannelPullup()
    {
        if (m_reconnectTimer->isActive())
            return;
        const int delay = qMin(kPullupRetryMs * qMax(m_channelFailStreak, 1), 60000);
        LOG_INFO("Link is down; retrying channel open in " << delay / 1000
                 << "s (fail streak " << m_channelFailStreak << ")");
        m_reconnectTimer->start(delay);
    }

    // Our pods' ads arrive every ~500ms while the link is down — fresh proof
    // they're awake, so don't keep waiting out the pull-up backoff. Strictly
    // shorten-only: the remainingTime gate means the ad rotation can pull a
    // pending attempt earlier but never postpone it.
    void kickChannelPullup()
    {
        if (m_linkConnected || areAirpodsConnected())
            return;
        if (m_reconnectTimer->isActive())
        {
            if (m_reconnectTimer->remainingTime() > 1500)
            {
                LOG_INFO("Pods sighted via BLE — pulling channel retry in to ~1s");
                m_reconnectTimer->start(1200);
            }
            return;
        }
        if (m_lastDevice.isValid())
        {
            LOG_INFO("Pods sighted via BLE — connecting now");
            m_reconnectTimer->start(1200);
        }
        else if (!m_deviceInfo->bluetoothAddress().isEmpty()
                 && (!socket || !socket->isOpen()))
        {
            LOG_INFO("Pods sighted via BLE — connecting now");
            connectToDevice(m_deviceInfo->bluetoothAddress());
        }
    }

    void onSystemWakingUp()
    {
        LOG_INFO("System is waking up, starting ble scan");
        m_bleManager->startScan();

        // Check whether AirPods reconnected while we were asleep
        monitor->checkAlreadyConnectedDevices();
        monitor->checkPairedDevices();
    }

    // Anchor the window bottom-right like a Windows 11 tray flyout before
    // showing it, so it surfaces next to the notification-area icons.
    static void positionWindowNearTray(QWindow *window)
    {
        if (!window)
            return;
        QScreen *screen = window->screen() ? window->screen() : QGuiApplication::primaryScreen();
        if (!screen)
            return;
        const QRect avail = screen->availableGeometry();
        int bottom = avail.bottom();
#ifdef Q_OS_WIN
        // availableGeometry doesn't reserve space for an auto-hide taskbar,
        // so the flyout could land on top of it. Ask the shell for the real
        // taskbar rect and clamp above it when it's bottom-docked.
        APPBARDATA abd = {};
        abd.cbSize = sizeof(abd);
        if (SHAppBarMessage(ABM_GETTASKBARPOS, &abd) && abd.uEdge == ABE_BOTTOM)
            bottom = qMin(bottom, static_cast<int>(abd.rc.top));
#endif
        window->setX(avail.right() - window->width() - 12);
        window->setY(bottom - window->height() - 12);
    }

    // The flyout's QML root — prefer the engine's root object (guaranteed to
    // be the ApplicationWindow, works before its native handle exists) with
    // an objectName fallback; never trust topLevelWindows() order since the
    // tray QMenu is a top-level window too.
    QWindow *mainFlyoutWindow()
    {
        if (parent && !parent->rootObjects().isEmpty())
            if (QWindow *w = qobject_cast<QWindow *>(parent->rootObjects().constFirst()))
                return w;
        for (QWindow *w : QGuiApplication::topLevelWindows())
            if (w->objectName() == QStringLiteral("mainWindowObject"))
                return w;
        return nullptr;
    }

    // Flash the window next to the tray for ear-detection events: toast-style
    // surface, auto-hidden by m_popupHideTimer. No-op while the window is
    // already open — never hide a window the user summoned, and don't steal
    // focus for a transient notification.
    Q_INVOKABLE void flashWindowAtTray(const QString &eventText = QString())
    {
        if (!m_earPopupEnabled)
        {
            LOG_INFO("Ear-event popup: skipped (disabled)");
            return;
        }
        QWindow *window = mainFlyoutWindow();
        if (!window)
        {
            LOG_WARN("Ear-event popup: main window not found");
            return;
        }
        // A visible window we didn't pop up was summoned by the user — don't
        // move it or arm an auto-hide. Note Qt's isActive() is unreliable on
        // this frameless topmost window (it can report active while Windows'
        // foreground is elsewhere), so "in use" is decided purely by whether
        // the window is up without being ours. The input-event filter is what
        // promotes a transient window to user-engaged.
        if (window->isVisible() && !m_popupShown)
        {
            LOG_INFO("Ear-event popup: in-place pulse (window already open): " << eventText);
            if (QObject *root = parent->rootObjects().constFirst())
                QMetaObject::invokeMethod(root, "earEventPulse");
            return;
        }
        if (window->isVisible())
        {
            // Our popup is still up — pulse it and re-arm the auto-hide so a
            // burst of events can't hold it open indefinitely.
            LOG_INFO("Ear-event popup: refresh pulse + re-arm hide: " << eventText);
            if (QObject *root = parent->rootObjects().constFirst())
                QMetaObject::invokeMethod(root, "earEventPulse");
        }
        else
        {
            // Dedupe: the same event edge can repeat while pod state settles
            // (ear flag and lid bit flap on independent clocks). An identical
            // popup re-showing within ~2.5s of an auto-hide is the same
            // notification — swallow it.
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            if (!eventText.isEmpty() && eventText == m_lastPopupText
                && now - m_lastPopupHideMs < 2500)
            {
                LOG_INFO("Ear-event popup: suppressed duplicate re-show: " << eventText);
                return;
            }
            LOG_INFO("Ear-event popup: showing flyout: " << eventText);
            m_lastPopupText = eventText;
            positionWindowNearTray(window);
            // Route through QML so the entrance animation runs — raw show()
            // can leave card.opacity stranded mid-animation.
            if (QObject *root = parent->rootObjects().constFirst())
                QMetaObject::invokeMethod(root, "popupShow");
            else
            {
                window->show();
                window->raise();
            }
            m_popupShown = true;
        }
        m_popupHideTimer->start(m_earPopupSecs * 1000);
    }

    // Real input on a transient (popup-shown) flyout means the user is
    // engaging with it — promote it out of auto-hide. Qt's active state is
    // unreliable on a frameless StaysOnTop window, so this uses actual input
    // delivery rather than focus bookkeeping.
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (m_popupShown && watched == mainFlyoutWindow())
        {
            switch (event->type())
            {
            case QEvent::MouseButtonPress:
            case QEvent::MouseButtonRelease:
            case QEvent::KeyPress:
            case QEvent::Wheel:
            case QEvent::TouchBegin:
            case QEvent::TabletPress:
                m_popupShown = false;
                m_popupHideTimer->stop();
                break;
            default:
                break;
            }
        }
        return QObject::eventFilter(watched, event);
    }

private slots:
    void onTrayIconActivated()
    {
        // User summoned — a pending ear-popup hide timer must not win.
        m_popupHideTimer->stop();
        m_popupShown = false;
        QQuickWindow *window = qobject_cast<QQuickWindow *>(mainFlyoutWindow());
        if (window)
        {
            positionWindowNearTray(window);
            window->show();
            window->raise();
            window->requestActivate();
        }
    }

    void onOpenApp()
    {
        m_popupHideTimer->stop();
        m_popupShown = false;
        QObject *rootObject = parent->rootObjects().first();
        if (rootObject) {
            QMetaObject::invokeMethod(rootObject, "reopen", Q_ARG(QVariant, "app"));
            positionWindowNearTray(qobject_cast<QWindow *>(rootObject));
        }
        else
        {
            loadMainModule();
        }
    }

    void onOpenSettings()
    {
        m_popupHideTimer->stop();
        m_popupShown = false;
        QObject *rootObject = parent->rootObjects().first();
        if (rootObject) {
            QMetaObject::invokeMethod(rootObject, "reopen", Q_ARG(QVariant, "settings"));
            positionWindowNearTray(qobject_cast<QWindow *>(rootObject));
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

    void nativeDeviceConnected(const QString &address, const QString &name)
    {
        const bool wasFlapping = m_disconnectGraceTimer->isActive();
        m_disconnectGraceTimer->stop();
        m_disconnectNotifyTimer->stop();
        setLinkConnected(true);
        m_presentedLinkUp = true;
        // A fresh link-up is the best possible moment to open the channel —
        // reset pacing so it happens immediately.
        m_retryCount = 0;
        m_reconnectTimer->stop();
        if (wasFlapping && socket)
        {
            // The channel most likely died with the ACL link — a stale
            // socket must not shadow a fresh open on the live link.
            socket->close();
            socket->deleteLater();
            socket = nullptr;
        }
        // Let the pod stack finish its own post-connect setup before we fire
        // the L2CAP request — an open issued in the same millisecond as the
        // ACL event can land while the peer's L2CAP layer is still settling,
        // and a dropped connect request leaves us waiting on a full timeout.
        QTimer::singleShot(1200, this, [this, address, name]() {
            if (!m_linkConnected)
                return;
            QBluetoothDeviceInfo device(QBluetoothAddress(address), name, 0);
            connectToDevice(device);
        });
    }

    // Paired but not link-connected AirPods. Opening the L2CAP channel pulls
    // the baseband link up, so this doubles as app-initiated auto-connect.
    void nativeDevicePaired(const QString &address, const QString &name)
    {
        if (socket && socket->isOpen())
        {
            return;
        }
        LOG_INFO("Paired AirPods found, connecting: " << address << " " << name);
        m_deviceInfo->setBluetoothAddress(address);
        QBluetoothDeviceInfo device(QBluetoothAddress(address), name, 0);
        connectToDevice(device);
    }

    // Secondary L2CAP channel (PSM 31) carrying raw ATT PDUs — the hearing
    // aid and transparency characteristics live there, per the Android app.
    // Optional: failures must never disturb the AACP channel.
    void scheduleAttOpen(const QBluetoothAddress &address)
    {
        if (qEnvironmentVariableIsSet("LIBREPODS_NO_ATT"))
            return; // diagnostic kill-switch: AACP-only sessions
        m_pendingAttAddress = address;
        m_attAttempts = 0;
        m_attSessionElapsed.start();
        m_attRetryTimer->stop();
        setAttState(QStringLiteral("opening"));
        m_attOpenTimer->start(100);
    }

    void openAttChannel(const QBluetoothAddress &address)
    {
        if (m_attSocket || m_attTearingDown)
            return; // already open/opening or mid-teardown

        // Re-claim session ownership if the pods haven't confirmed it yet —
        // their ATT listener only arms for the TiPi owner.
        if (!m_ownsConnection && socket && socket->isOpen())
            writePacketToSocket(AirPodsPackets::Ownership::ownsPacket(), "Ownership claim (pre-ATT) written: ");

        BtSocketBase *att = nullptr;
        if (SvcChannelSocket::isServiceAvailable())
        {
            // The svc owns driver channels — its open fails gracefully if the
            // installed driver lacks caller-PSM support.
            att = new SvcChannelSocket();
        }
        else if (DriverL2CapSocket::isAttSupported())
        {
            att = new DriverL2CapSocket();
        }
        else if (!m_driverAvailable)
        {
            // No driver at all: the Winsock L2CAP fallback may still reach
            // PSM 31 on stacks where it works.
            att = new L2CapSocket();
        }
        else
        {
            LOG_INFO("ATT channel skipped: installed driver speaks protocol v1 "
                     "(needs the v2 build for caller-selected PSM)");
            setAttState(QStringLiteral("failed"),
                        tr("driver protocol v1 — needs v2 for ATT"));
            return;
        }

        m_attSocket = att;
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
        // The kernel open can take seconds — the AACP link may drop and
        // tear this channel down before a queued signal lands. Every
        // lambda therefore validates that 'att' is still the live socket
        // and that the manager still exists before touching anything.
        connect(att, &BtSocketBase::connected, this, [this, att]() {
            if (m_attSocket != att || !m_attManager)
                return;
            LOG_INFO("ATT channel open (PSM 31)");
            m_attAttempts = 0;
            setAttState(QStringLiteral("ready"));
            m_attManager->start();
        });
        connect(att, QOverload<QBluetoothSocket::SocketError>::of(&BtSocketBase::errorOccurred),
                this, [this, att](QBluetoothSocket::SocketError) {
                    if (m_attSocket != att)
                        return;
                    const QString err = att->errorString();
                    const int attempt = ++m_attAttempts;
                    LOG_WARN("ATT channel open failed (attempt " << attempt << ", t+"
                             << m_attSessionElapsed.elapsed() / 1000 << "s): " << err);
                    teardownAttChannel();
                    if (m_linkConnected)
                        scheduleAttRetry(err);
                    else
                        setAttState(QStringLiteral("failed"), err);
                });
        connect(att, &BtSocketBase::disconnected, this, [this, att]() {
            if (m_attSocket != att)
                return;
            teardownAttChannel();
            // A live ATT channel dropped while AACP stayed up — rebuild it
            // on the same cadence as a failed open.
            if (m_linkConnected)
                scheduleAttRetry();
        });

        att->connectToService(address, static_cast<quint16>(LIBREPODS_PSM_ATT));
    }

    // The pods take the PSM-31 channel only when they feel like it — keep
    // knocking on a cadence while the link is up. A failed open costs a
    // ~15s kernel timeout anyway, so a short retry delay still leaves the
    // link mostly idle.
    void scheduleAttRetry(const QString &lastError = {})
    {
        if (m_attAttempts >= kAttMaxAttempts || !m_linkConnected ||
            m_pendingAttAddress == QBluetoothAddress())
        {
            if (m_attAttempts >= kAttMaxAttempts)
                setAttState(QStringLiteral("failed"),
                            lastError.isEmpty() ? tr("open attempts exhausted") : lastError);
            return;
        }
        setAttState(QStringLiteral("opening"));
        m_attRetryTimer->start(m_attAttempts < 3 ? kAttFastRetryDelayMs
                                                 : kAttRetryDelayMs);
    }

    void teardownAttChannel()
    {
        if (m_attTearingDown)
            return;
        m_attTearingDown = true;
        m_attOpenTimer->stop();
        m_attRetryTimer->stop();
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
        if (m_attState == QStringLiteral("ready"))
            setAttState(QStringLiteral("off"));
        m_attTearingDown = false;
    }

    void onDeviceDisconnected(const QBluetoothAddress &address)
    {
        LOG_INFO("Device disconnected: " << address.toString()
                 << " (attAttempts=" << m_attAttempts << " attState=" << m_attState
                 << " attElapsed="
                 << (m_attSessionElapsed.isValid() ? m_attSessionElapsed.elapsed() / 1000 : -1)
                 << "s)");
        setLinkConnected(false);
        m_presentedLinkUp = false;
        teardownAttChannel();
        m_gestureDetector->stop();
        m_headOrientation.reset();
        if (socket)
        {
            LOG_WARN("Socket is still open, closing it");
            socket->close();
            socket->deleteLater();
            socket = nullptr;
        }
        if (phoneSocket && phoneSocket->isOpen())
        {
            phoneSocket->write(AirPodsPackets::Connection::AIRPODS_DISCONNECTED);
            LOG_DEBUG("AIRPODS_DISCONNECTED packet written: " << AirPodsPackets::Connection::AIRPODS_DISCONNECTED.toHex());
        }

        // Clear link-scoped state but keep physical telemetry — battery, ear
        // position and dock status describe the pods, not the link, and BLE
        // ads refresh them once the scan restarts.
        m_deviceInfo->reset(true);
        m_bleManager->startScan();
        emit airPodsStatusChanged();

        // The toast waits for a *sustained* disconnect — during first
        // insertion the link flaps while audio profiles come up, and firing
        // inside that window reads as a false "disconnected" right as the
        // pods are actually coming online.
        m_disconnectNotifyTimer->start();
        trayManager->resetTrayIcon();

        // Nothing else re-arms the pull-up when the socket dies cleanly —
        // the error-handler's 30s cadence only runs off errorOccurred, so a
        // link that drops with no error callback would leave the app idle
        // until some external event woke it.
        if (m_lastDevice.isValid())
            scheduleChannelPullup();
    }

    void nativeDeviceDisconnected(const QString &address, const QString &name)
    {
        if (address != m_deviceInfo->bluetoothAddress())
        {
            LOG_WARN("Disconnected device does not match connected device: " << address << " != " << m_deviceInfo->bluetoothAddress());
            return;
        }
        // Report link-down to connect/retry logic immediately, but defer the
        // teardown — case-transition micro-flaps recover inside the window.
        setLinkConnected(false);
        m_pendingDisconnectAddress = QBluetoothAddress(address);
        m_disconnectGraceTimer->start();
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
        if (socket && socket->peerAddress() == device.address() &&
            (socket->isOpen() || socket->state() == QBluetoothSocket::SocketState::ConnectingState))
        {
            LOG_INFO("Already connected/connecting to the device: " << device.name());
            return;
        }

        m_lastDevice = device;
        LOG_INFO("Connecting to device: " << device.name());

        // Clean up any existing socket
        if (socket)
        {
            socket->close();
            socket->deleteLater();
            socket = nullptr;
        }

        // Populate what we can from Windows' device record even when the AACP
        // control channel can't be opened (no driver): name and pod icons.
        if (!device.name().isEmpty())
        {
            QString cleanName = device.name();
            cleanName.remove(QRegularExpression("\\s*-\\s*Find My$"));
            if (m_deviceInfo->deviceName().isEmpty())
                m_deviceInfo->setDeviceName(cleanName);
            if (m_deviceInfo->model() == AirPodsModel::Unknown)
                m_deviceInfo->setModel(inferModelFromName(device.name()));
            m_deviceInfo->saveToSettings(*m_settings);
        }

        BtSocket *localSocket = newSocket();
        socket = localSocket;

        // Connection handler
        auto handleConnection = [this, localSocket, device]()
        {
            setLinkConnected(true);
            m_presentedLinkUp = true;
            m_disconnectGraceTimer->stop();
            m_disconnectNotifyTimer->stop();
            m_retryCount = 0;
            m_reconnectTimer->stop();
            // Socket state changed even when the OS link was already up —
            // airpodsConnected depends on it, so always refresh the UI.
            emit airPodsStatusChanged();
            connect(localSocket, &BtSocket::readyRead, this, [this, localSocket]()
                    {
            QByteArray data = localSocket->readAll();
            QMetaObject::invokeMethod(this, "parseData", Qt::QueuedConnection, Q_ARG(QByteArray, data));
            QMetaObject::invokeMethod(this, "relayPacketToPhone", Qt::QueuedConnection, Q_ARG(QByteArray, data)); });
            sendHandshake();
            m_channelDataWatchdog->start(kChannelSilentMs);
            // FEATURES_ACK is not guaranteed — on warm links the pods
            // sometimes skip the ack entirely, which used to strand the
            // whole post-ack sequence (notifications, SSL/EQ enable, stem
            // config). Fire it 1.5s after connect if the ack never came.
            m_featuresAcked = false;
            // Give the fresh channel 15s to start streaming before the
            // starvation watchdog considers it stalled.
            m_lastHeadSampleMs = QDateTime::currentMSecsSinceEpoch();
            QTimer::singleShot(1500, this, [this, localSocket]() {
                if (socket != localSocket || !localSocket->isOpen() || m_featuresAcked)
                    return;
                writePacketToSocket(AirPodsPackets::Connection::REQUEST_NOTIFICATIONS,
                                    "Request notifications packet written (no FEATURES_ACK): ");
                writePacketToSocket(AirPodsPackets::Connection::EQ_FEATURE_ENABLE,
                                    "EQ feature enable packet written (no FEATURES_ACK): ");
                writePacketToSocket(AirPodsPackets::Ownership::ownsPacket(),
                                    "Ownership claim written (no FEATURES_ACK): ");
                sendStemConfig();
                // Same omission stranded head tracking on every warm
                // reconnect — the toggle stayed on but the pods never got
                // START, so the stream stayed dead until a manual retoggle.
                if (m_deviceInfo->headTrackingEnabled())
                {
                    writePacketToSocket(AirPodsPackets::HeadTracking::START,
                                        "Start head tracking packet written (no FEATURES_ACK): ");
                    updateGestureDetection();
                    LOG_INFO("Head tracking stream armed (no FEATURES_ACK)");
                }
            });
            // Secondary ATT channel — deferred so the AACP handshake
            // settles first; failures log without disturbing AACP.
            scheduleAttOpen(device.address());
        };

        // Error handler with retry. Retries are paced by link state: a failed
        // open while the baseband link is down costs ~10s of paging and can
        // collide with the pods' own inbound page-in — churning opens kept
        // the link from ever landing (it only connected once the app was
        // closed). Link-down failures therefore back off to a slow pull-up
        // cadence and let deviceConnected win the race instead.
        auto handleError = [this, device, localSocket](QBluetoothSocket::SocketError error)
        {
            LOG_ERROR("Socket error: " << error << ", " << localSocket->errorString());

            if (!m_linkConnected)
            {
                // Open-failure territory — the channel never came up. Only
                // these count toward the wedge streak; mid-session errors on
                // an established channel are churn, not wedge evidence.
                m_channelFailStreak++;
                maybeRadioReset();
                scheduleChannelPullup();
                return;
            }

            if (m_retryCount < m_retryAttempts)
            {
                m_retryCount++;
                LOG_INFO("Retrying connection (attempt " << m_retryCount << ")");
                QTimer::singleShot(1500, this, [this, device]()
                                   {
                                       if (!m_linkConnected)
                                       {
                                           // Link dropped since the retry was armed — a fresh
                                           // open now would page a device that just left. Hand
                                           // pull-up to the slow timer; without this hand-off
                                           // nothing owns the next attempt and the app stalls.
                                           scheduleChannelPullup();
                                           return;
                                       }
                                       connectToDevice(device); });
            }
            else
            {
                LOG_ERROR("Failed to connect after " << m_retryAttempts << " attempts");
                m_retryCount = 0;
            }
        };

        connect(localSocket, &BtSocket::connected, this, handleConnection);
        connect(localSocket, &BtSocket::disconnected, this, [this]() { emit airPodsStatusChanged(); });
        connect(localSocket, QOverload<QBluetoothSocket::SocketError>::of(&BtSocket::errorOccurred),
                this, handleError);

        localSocket->connectToService(device.address(), QBluetoothUuid("74ec2172-0bad-4d01-8f77-997b2be0722a"));

        // Connect timeout: the driver's OPEN ioctl can block far past any
        // reasonable page-in window, and while the socket reads
        // ConnectingState every retry short-circuits as "already
        // connecting" — one wedged open used to stall the app for good.
        QTimer::singleShot(45000, this, [this, localSocket, handleError]() {
            if (socket == localSocket &&
                localSocket->state() == QBluetoothSocket::SocketState::ConnectingState)
            {
                LOG_WARN("Channel open timed out after 45s; abandoning attempt");
                localSocket->close();
                // close() emits nothing for a Connecting socket, so without this
                // the attempt dies silently and no retry is ever scheduled.
                handleError(QBluetoothSocket::SocketError::NetworkError);
            }
        });

        m_deviceInfo->setBluetoothAddress(device.address().toString());
        m_deviceInfo->saveToSettings(*m_settings);
        mediaController->setConnectedDeviceMacAddress(device.address().toString());
        notifyAndroidDevice();
    }

    void parseData(const QByteArray &data)
    {
        LOG_DEBUG("Received: " << data.toHex());
        // Any inbound packet proves the channel is alive — disarm the silence
        // watchdog and clear the wedge streak.
        m_channelDataWatchdog->stop();
        m_channelFailStreak = 0;

        if (data.startsWith(AirPodsPackets::Parse::HANDSHAKE_ACK))
        {
            writePacketToSocket(AirPodsPackets::Connection::SET_SPECIFIC_FEATURES, "Set specific features packet written: ");
        }
        else if (data.startsWith(AirPodsPackets::Parse::FEATURES_ACK))
        {
            m_featuresAcked = true;
            writePacketToSocket(AirPodsPackets::Connection::REQUEST_NOTIFICATIONS, "Request notifications packet written: ");
            writePacketToSocket(AirPodsPackets::Connection::EQ_FEATURE_ENABLE, "EQ feature enable packet written: ");
            // Claim TiPi session ownership every connect, not just on media
            // takeover — the Android app requires OWNS_CONNECTION==1 before
            // the pods arm the ATT (PSM 31) listener.
            writePacketToSocket(AirPodsPackets::Ownership::ownsPacket(), "Ownership claim written: ");
            sendStemConfig();

            if (m_deviceInfo->headTrackingEnabled())
            {
                writePacketToSocket(AirPodsPackets::HeadTracking::START, "Start head tracking packet written: ");
                updateGestureDetection();
                LOG_INFO("Head tracking stream armed (FEATURES_ACK)");
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
            // AACP ear detection is authoritative for docking, and while the
            // channel is up the BLE scan is stopped — without this, podsInCase
            // never updates and the case UI can't appear until link drop + ads.
            using ES = EarDetection::EarDetectionStatus;
            auto *ed = m_deviceInfo->getEarDetection();
            const auto ps = ed->getprimaryStatus(), ss = ed->getsecondaryStatus();
            if (ps != ES::Disconnected || ss != ES::Disconnected)
            {
                const int docked = (ps == ES::InCase ? 1 : 0) + (ss == ES::InCase ? 1 : 0);
                m_deviceInfo->updateProximityState(docked >= 1, docked >= 2,
                                                   m_deviceInfo->caseLidOpen());
            }
            // Popup only on real pod moves (ear/hand/case), evaluated per pod:
            // a Disconnected on either side of a pod's transition is link
            // churn, but one pod lagging Disconnected must not mask the
            // other's real transition.
            const bool pMoved = ps != m_prevPrimaryEar && ps != ES::Disconnected
                && m_prevPrimaryEar != ES::Disconnected;
            const bool sMoved = ss != m_prevSecondaryEar && ss != ES::Disconnected
                && m_prevSecondaryEar != ES::Disconnected;
            if (pMoved || sMoved)
            {
                m_lastEarTransitionMs = QDateTime::currentMSecsSinceEpoch();
                LOG_INFO("Ear-event transition: " << static_cast<int>(ps) << "/" << static_cast<int>(ss)
                         << " <- " << static_cast<int>(m_prevPrimaryEar) << "/" << static_cast<int>(m_prevSecondaryEar));
                // The orientation decode is pod-placement-relative: its
                // neutral vector is only meaningful when calibrated with
                // the pod seated in ear. Recalibrate on any in-ear
                // transition so case/desk poses can't poison the baseline.
                if (ps == ES::InEar || ss == ES::InEar)
                    m_headOrientation.reset();
                const int inEar = (ps == ES::InEar ? 1 : 0) + (ss == ES::InEar ? 1 : 0);
                const int inCase = (ps == ES::InCase ? 1 : 0) + (ss == ES::InCase ? 1 : 0);
                QString text;
                if (inEar == 2) text = tr("Both AirPods in ear");
                else if (inEar == 1) text = tr("AirPod in ear");
                else if (inCase >= 1) text = tr("AirPod in case");
                else text = tr("AirPods out of ear");
                flashWindowAtTray(text);
            }
            // The pods' ATT listener only accepts while they're receptive —
            // in-ear. If the retry budget exhausted while they were cased,
            // an ear-in transition is our cue to re-arm rather than leaving
            // hearing aid dead until the next full reconnect. Gated to
            // "failed" so it can't fight an in-flight or open channel.
            if ((ps == ES::InEar || ss == ES::InEar)
                && m_attState == QStringLiteral("failed")
                && m_pendingAttAddress != QBluetoothAddress())
            {
                LOG_INFO("Pod in ear with ATT channel failed — re-arming open attempts");
                scheduleAttOpen(m_pendingAttAddress);
            }
            m_prevPrimaryEar = ps;
            m_prevSecondaryEar = ss;
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
            mediaController->setConnectedDeviceName(m_deviceInfo->deviceName());
            initiateMagicPairing();
            mediaController->setConnectedDeviceMacAddress(m_deviceInfo->bluetoothAddress().replace(":", "_"));
            if (m_deviceInfo->getEarDetection()->oneOrMorePodsInEar()) // AirPods get added as output device only after this
            {
                mediaController->activateA2dpProfile();
            }
            // Keep the watcher running while connected: the AACP channel has
            // no lid-state field — lid open/close only exists in the ads.
            // bleDeviceFound gates everything else out while connected, so
            // the rotation can't fight channel-owned state.
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
                m_lastHeadSampleMs = QDateTime::currentMSecsSinceEpoch();
                m_lastHeadPacket = data;
                m_rawO1 = sample.o1; m_rawO2 = sample.o2; m_rawO3 = sample.o3; m_rawO4 = sample.o4;
                m_rawAccH = sample.accelHorizontal; m_rawAccV = sample.accelVertical;
                // Feed unconditionally — matching Android — except during
                // the ~350ms settle window after an ear transition, when
                // the stream emits garbage bursts that shake the display.
                if (QDateTime::currentMSecsSinceEpoch() - m_lastEarTransitionMs > 350)
                {
                    const int podSign =
                        m_deviceInfo->getBattery()->getPrimaryPod() == Battery::Component::Right
                            ? -1 : +1;
                    m_headOrientation.feed(sample.o1, sample.o2, sample.o3,
                                           sample.accelHorizontal, sample.accelVertical,
                                           podSign);
                    if (m_headOrientation.valid())
                    {
                        m_deviceInfo->setHeadOrientation(m_headOrientation.yaw(),
                                                         m_headOrientation.pitch());
                    }
                    m_gestureDetector->feed(sample.accelHorizontal, sample.accelVertical);
                }
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
        // AUDIO_SOURCE — the pods report which host currently feeds audio.
        // If it isn't us, release ownership like Android instead of
        // fighting the peer device.
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
            {
                LOG_INFO("Custom EQ echoed by pods: enabled=" << enabled
                         << " bands=" << low << "/" << mid << "/" << high);
                m_deviceInfo->setCustomEq(enabled, low, mid, high);
            }
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
        // The cross-device phone link uses a custom L2CAP service that only the
        // Winsock transport can attempt; the driver only speaks AACP.
        phoneSocket = new L2CapSocket();
        connect(phoneSocket, &BtSocket::connected, this, [this]() {
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

        connect(phoneSocket, QOverload<QBluetoothSocket::SocketError>::of(&BtSocket::errorOccurred), this, [this](QBluetoothSocket::SocketError error) {
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
                // Windows has no bluetoothctl equivalent; closing the L2CAP
                // channel releases control without dropping the audio link.
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

    // Each pod and the case broadcast independently on a ~500ms rotation and
    // briefly disagree about the dock bits while pods are moving. A count
    // change is applied only when two consecutive advertisements agree —
    // except a docked advertiser asserting both-docked, which is near-certain
    // on its own. Lid state is decoded on every ad and debounced separately
    // in applyLidState — undocked pod ads can carry a stale bit.
    void applyProximityState(const BleInfo &device)
    {
        const int newCount = device.areBothPodsInCase ? 2 : (device.isOnePodInCase ? 1 : 0);
        const int current = m_deviceInfo->podsInCase();
        applyLidState(device.lidState);
        const bool lidOpen = m_deviceInfo->caseLidOpen();
        if (newCount == current)
        {
            m_lastProximityClaim = -1;
            m_deviceInfo->updateProximityState(newCount >= 1, newCount >= 2, lidOpen);
            return;
        }
        const bool certain = newCount == 2 && device.isThisPodInTheCase;
        if (!certain && newCount != m_lastProximityClaim)
        {
            m_lastProximityClaim = newCount;
            return; // first ad claiming this count — wait for agreement
        }
        m_lastProximityClaim = -1;
        m_deviceInfo->updateProximityState(newCount >= 1, newCount >= 2, lidOpen);
    }

    // Lid flips commit only after the same value is asserted continuously
    // for a full advertiser rotation (~1.2s): during real transitions each
    // advertiser settles on its own schedule, and pod ads in flight carry
    // stale lid bits — a short claim window edges into phantom "Case
    // opened" flaps seconds after the real event. Any disagreeing ad
    // restarts the clock, so a truth-asserting case in the rotation can
    // never be outvoted.
    void applyLidState(BleInfo::LidState state)
    {
        if (state == BleInfo::LidState::UNKNOWN)
            return;
        const bool open = state == BleInfo::LidState::OPEN;
        if (open == m_deviceInfo->caseLidOpen())
        {
            m_lastLidClaim = -1;
            return;
        }
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (m_lastLidClaim != open)
        {
            m_lastLidClaim = open;
            m_lidClaimSinceMs = now;
            return; // first ad claiming this state — wait out the rotation
        }
        if (now - m_lidClaimSinceMs < 1200)
            return; // claim is holding but hasn't spanned a rotation yet
        m_lastLidClaim = -1;
        m_deviceInfo->updateProximityState(m_deviceInfo->podsInCase() >= 1,
                                           m_deviceInfo->podsInCase() >= 2, open);
    }

    void bleDeviceFound(const BleInfo &device)
    {
        // While the AACP channel is open it owns all live pod state —
        // battery, ear position, dock status. Rotating ads each carry a
        // single advertiser's view; letting them write state concurrently
        // with AACP packets makes the UI fight itself every ~500ms.
        if (areAirpodsConnected())
        {
            // Exception: AACP carries no lid bit — lid state exists only in
            // the ads, so let just that field through. Strict IRK
            // attribution: a neighbor's same-model AirPods must not flip
            // our lid state.
            if (BLEUtils::isValidIrkRpa(m_deviceInfo->magicAccIRK(), device.address))
                applyLidState(device.lidState);
            return;
        }
        if (BLEUtils::isValidIrkRpa(m_deviceInfo->magicAccIRK(), device.address)) {
            m_deviceInfo->setModel(device.modelName);
            auto decryptet = BLEUtils::decryptLastBytes(device.encryptedPayload, m_deviceInfo->magicAccEncKey());
            m_deviceInfo->getBattery()->parseEncryptedPacket(decryptet, device.primaryLeft, device.isThisPodInTheCase, isModelHeadset(m_deviceInfo->model()));
            applyProximityState(device);
            // Both-in-case is authoritative: a lagging ad must not leave a
            // pod flagged in-ear once the debounced state says both docked.
            const bool bothInCase = m_deviceInfo->podsInCase() >= 2;
            m_deviceInfo->getEarDetection()->overrideEarDetectionStatus(
                device.isPrimaryInEar && !bothInCase, device.isSecondaryInEar && !bothInCase);
            kickChannelPullup();
            return;
        }

        // No magic keys (driver never connected) or a foreign device's ad.
        // The plaintext proximity fields still carry useful state; gate on
        // the model so a neighbor's AirPods can't poison ours.
        if (device.modelName == AirPodsModel::Unknown)
            return;
        const AirPodsModel known = m_deviceInfo->model();
        if (known != AirPodsModel::Unknown && device.modelName != known)
            return;
        if (known == AirPodsModel::Unknown) {
            // Only attribute the ad once we actually have a paired device
            // whose name gave us no usable model hint.
            if (m_deviceInfo->bluetoothAddress().isEmpty())
                return;
            m_deviceInfo->setModel(device.modelName);
            m_deviceInfo->saveToSettings(*m_settings);
        }

        m_deviceInfo->getBattery()->parseProximityData(
            device.leftPodBattery, device.rightPodBattery, device.caseBattery,
            device.leftCharging, device.rightCharging, device.caseCharging,
            device.primaryLeft, isModelHeadset(m_deviceInfo->model()));
        applyProximityState(device);
        const bool bothInCase = m_deviceInfo->podsInCase() >= 2;
        m_deviceInfo->getEarDetection()->overrideEarDetectionStatus(
            device.isPrimaryInEar && !bothInCase, device.isSecondaryInEar && !bothInCase);
        // The pill reports the AACP link; the ads additionally tell us what
        // the pods are actually doing. Only surface the active states — an
        // ad-side "Disconnected"/"Unknown" while the link is up would be
        // misleading.
        using CS = BleInfo::ConnectionState;
        if (device.connectionState == CS::MUSIC || device.connectionState == CS::CALL ||
            device.connectionState == CS::RINGING || device.connectionState == CS::HANGING_UP ||
            device.connectionState == CS::IDLE)
        {
            m_deviceInfo->setConnectionState(ProximityParser::getConnectionStateName(device.connectionState));
        }
        else
        {
            m_deviceInfo->setConnectionState(QString());
        }
        m_deviceInfo->updateBatteryStatus();
        kickChannelPullup();
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

        if (force && !m_deviceInfo->bluetoothAddress().isEmpty()) {
            // Opening the L2CAP channel on Windows initiates the Bluetooth
            // link implicitly; there is no bluetoothctl-style connect step.
            LOG_INFO("Forcing connection to AirPods");
            connectToDevice(m_deviceInfo->bluetoothAddress());
            return;
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
        LOG_INFO("initializeBluetooth: starting");
        connectToPhone();

        m_deviceInfo->loadFromSettings(*m_settings);
        LOG_INFO("initializeBluetooth: remembered address:"
                 << m_deviceInfo->bluetoothAddress()
                 << "connected:" << areAirpodsConnected());

        // Fallback: with a remembered address we don't need discovery —
        // opening the L2CAP channel pulls the baseband link up. Covers the
        // case where WinRT/Qt Bluetooth enumeration is wedged but the
        // driver transport still works. Armed BEFORE startScan: a wedged
        // BLE watcher init can block inside it and the fallback — which
        // exists for exactly this broken-stack case — would never run.
        if (!areAirpodsConnected() && !m_deviceInfo->bluetoothAddress().isEmpty()) {
            QTimer::singleShot(1500, this, [this]() {
                if (!areAirpodsConnected())
                    connectToDevice(m_deviceInfo->bluetoothAddress());
            });
        }

        if (!areAirpodsConnected()) {
            m_bleManager->startScan();
        }
    }

    // When open attempts keep failing (or land silent), the pod-side/stack
    // L2CAP state is wedged in a way no retry can fix — reset the radio once
    // per cooldown window and let the retry loop land the link cleanly.
    void maybeRadioReset()
    {
        if (m_channelFailStreak < kWedgeThreshold)
            return;
        if (m_lastRadioReset.isValid()
            && m_lastRadioReset.msecsTo(QDateTime::currentDateTime()) < kRadioResetCooldownMs)
            return;
        m_channelFailStreak = 0;
        m_lastRadioReset = QDateTime::currentDateTime();
        runRadioReset();
    }

    static void runRadioReset()
    {
        LOG_WARN("AACP channel wedged — resetting Bluetooth radio (audio will blip ~10s)");
        // Same WinRT Radio API as radio_toggle.ps1 — embedded so the recovery
        // path has no external file dependency.
        static const char *kScript = R"PS(
Add-Type -AssemblyName System.Runtime.WindowsRuntime
$null = [Windows.Devices.Radios.Radio, Windows.System.User.DeviceAssociation, ContentType=WindowsRuntime]
$null = [Windows.Devices.Radios.RadioAccessStatus, Windows.System.User.DeviceAssociation, ContentType=WindowsRuntime]
$asTaskGeneric = ([System.WindowsRuntimeSystemExtensions].GetMethods() |
  Where-Object { $_.Name -eq 'AsTask' -and $_.IsGenericMethod -and
                 $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1' })[0]
function Await-Op($op, $resultType) {
    $t = $asTaskGeneric.MakeGenericMethod($resultType).Invoke($null, @($op))
    $t.Wait(15000) | Out-Null
    $t.Result
}
$listType = [System.Collections.Generic.IReadOnlyList[Windows.Devices.Radios.Radio]]
$radios = Await-Op ([Windows.Devices.Radios.Radio]::GetRadiosAsync()) $listType
$bt = $radios | Where-Object { $_.Kind -eq [Windows.Devices.Radios.RadioKind]::Bluetooth } | Select-Object -First 1
if (-not $bt) { exit 1 }
$r = Await-Op ($bt.SetStateAsync([Windows.Devices.Radios.RadioState]::Off)) ([Windows.Devices.Radios.RadioAccessStatus])
Start-Sleep -Seconds 4
$r = Await-Op ($bt.SetStateAsync([Windows.Devices.Radios.RadioState]::On)) ([Windows.Devices.Radios.RadioAccessStatus])
)PS";
        QProcess::startDetached(QStringLiteral("powershell"),
                                {QStringLiteral("-NoProfile"),
                                 QStringLiteral("-ExecutionPolicy"), QStringLiteral("Bypass"),
                                 QStringLiteral("-Command"), QString::fromLatin1(kScript)});
    }

    void loadMainModule() {
        parent->load(QUrl(QStringLiteral("qrc:/librepods/Main.qml")));
        QWindow *root = qobject_cast<QWindow *>(parent->rootObjects().first());
        positionWindowNearTray(root);
        if (root)
            root->installEventFilter(this);
        // The launch-visible flyout is a glance, not a summon: arm the same
        // auto-hide as event popups so it can't sit open forever when it was
        // never focused. Real input on it cancels the hide via eventFilter.
        if (!m_hideOnStart && m_earPopupEnabled && root && root->isVisible())
        {
            m_popupShown = true;
            m_popupHideTimer->start(m_earPopupSecs * 1000);
        }
        // The flyout hides and re-shows constantly (tray toggle, ear popup,
        // focus-loss dismiss). Without persistent resources Qt can drop the
        // scene graph on hide and re-show as a transparent, never-repainted
        // window — visible to the OS, invisible to the user.
        if (QQuickWindow *quickWindow = qobject_cast<QQuickWindow *>(root))
        {
            quickWindow->setPersistentSceneGraph(true);
            quickWindow->setPersistentGraphics(true);
        }
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
    void earPopupEnabledChanged(bool enabled);
    void earPopupSecsChanged(int secs);
    void retryAttemptsChanged(int attempts);
    void oneBudANCModeChanged(bool enabled);
    void phoneMacStatusChanged();
    void hearingAidEnabledChanged(bool enabled);
    void attStateChanged();
    void actionConfigChanged();
    void lastGestureChanged();

private:
    BtSocket *socket = nullptr;
    BtSocket *phoneSocket = nullptr;
    QByteArray lastBatteryStatus;
    QByteArray lastEarDetectionStatus;
    MediaController* mediaController;
    TrayIconManager *trayManager;
    BluetoothMonitor *monitor;
    QSettings *m_settings;
    AutoStartManager *m_autoStartManager;
    int m_retryAttempts = 3;
    bool m_earPopupEnabled = true;
    int m_earPopupSecs = 4;
    QTimer *m_popupHideTimer = nullptr;
    // Set while the flyout is up for a transient reason (event popup or the
    // launch glance) rather than a user summon. Transient windows always
    // auto-hide; real input on them promotes to user-engaged (eventFilter).
    bool m_popupShown = false;
    // Same-text re-show dedupe window for settling-state flap.
    QString m_lastPopupText;
    qint64 m_lastPopupHideMs = 0;
    EarDetection::EarDetectionStatus m_prevPrimaryEar = EarDetection::EarDetectionStatus::Disconnected;
    EarDetection::EarDetectionStatus m_prevSecondaryEar = EarDetection::EarDetectionStatus::Disconnected;
    qint64 m_lastEarTransitionMs = 0;
    bool m_hideOnStart = false;
    DeviceInfo *m_deviceInfo;
    BleManager *m_bleManager;
    HearingAidModel *m_hearingAid = nullptr;
    AudioSpectrum *m_spectrum = nullptr;
    BtSocketBase *m_attSocket = nullptr;
    AttManager *m_attManager = nullptr;
    SystemSleepMonitor *m_systemSleepMonitor = nullptr;
    QString m_phoneMacStatus;
    bool m_driverAvailable = false;
    bool m_reduceMotion = false; // Windows "Show animations" toggle — QML snaps instead of animating
    int m_lastProximityClaim = -1; // dock count claimed by the previous ad (-1 = none pending)
    int m_lastLidClaim = -1;       // lid state claimed by the previous ad (0 closed / 1 open / -1 none)
    qint64 m_lidClaimSinceMs = 0;  // when the pending lid claim was first asserted
    bool m_presentedLinkUp = false; // debounced link state for UI surfaces
    QTimer *m_disconnectGraceTimer = nullptr;
    QBluetoothAddress m_pendingDisconnectAddress;
    bool m_linkConnected = false;
    bool m_fakeConnected = false; // LIBREPODS_FAKE_MODEL dev harness
    int m_retryCount = 0;
    QTimer *m_reconnectTimer = nullptr;
    QBluetoothDeviceInfo m_lastDevice;
    QTimer *m_disconnectNotifyTimer = nullptr;
    QTimer *m_attOpenTimer = nullptr;
    QTimer *m_attRetryTimer = nullptr;
    QBluetoothAddress m_pendingAttAddress;
    int m_attAttempts = 0;
    bool m_prevLidOpen = false;
    int m_channelFailStreak = 0;
    QTimer *m_channelDataWatchdog = nullptr;
    QTimer *m_headStreamWatchdog = nullptr;
    qint64 m_lastHeadSampleMs = 0;
    QDateTime m_lastRadioReset;
    QElapsedTimer m_attSessionElapsed;
    bool m_attTearingDown = false;
    QString m_attState = QStringLiteral("off");
    QString m_attError;
    GestureDetector *m_gestureDetector = nullptr;
    HeadOrientation m_headOrientation;
    int m_gestureNodAction = 0;
    int m_gestureShakeAction = 0;
    qint64 m_lastGestureActionMs = 0;
    static constexpr qint64 kGestureActionCooldownMs = 2500;
    int m_rawO1 = 0, m_rawO2 = 0, m_rawO3 = 0, m_rawO4 = 0;
    int m_rawAccH = 0, m_rawAccV = 0;
    QByteArray m_lastHeadPacket;
    int m_stemSingleAction = 0;
    int m_stemDoubleAction = 0;
    int m_stemTripleAction = 0;
    int m_stemLongAction = 0;
    QString m_lastGesture;
    bool m_ownsConnection = false;
    QString m_localAdapterMac;
    bool m_featuresAcked = false;

    // The librepods-svc companion process owns the driver channel — including
    // surviving this process — when it is present. Spawning happens once: if
    // the exe sits beside the app we start it detached and probe until the
    // pipe answers. Without the svc we fall back to the in-process driver
    // socket, then the Winsock L2CAP socket.
    static bool ensureService()
    {
        // Probed from both the deferred-init worker (warm-up) and the GUI
        // thread (first connect) — the one-shot flag must be atomic.
        static std::atomic<bool> attempted{false};
        if (SvcChannelSocket::isServiceAvailable())
            return true;
        if (attempted.exchange(true))
            return false;
        const QString exe = QCoreApplication::applicationDirPath()
                            + QStringLiteral("/librepods-svc.exe");
        if (!QFileInfo::exists(exe))
            return false;
        QProcess::startDetached(exe, QStringList());
        // The worker warms this path well before first connect, so the pipe
        // is normally already listening — a short poll suffices here.
        for (int i = 0; i < 10; ++i) {
            if (SvcChannelSocket::isServiceAvailable(60))
                return true;
            QThread::msleep(40);
        }
        return false;
    }

    static BtSocket *newSocket()
    {
        if (ensureService())
            return new SvcChannelSocket();
        if (DriverL2CapSocket::isDriverAvailable())
            return new DriverL2CapSocket();
        return new L2CapSocket();
    }
};

int main(int argc, char *argv[]) {
    try {
        winrt::init_apartment(winrt::apartment_type::single_threaded);
    } catch (...) {
        // COM may already be initialized for this thread.
    }

    QApplication app(argc, argv);
    // The Windows native style ignores palette colors and custom control
    // delegates — Basic is the cross-platform style built for both.
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    app.setWindowIcon(QIcon(QStringLiteral(":/icons/assets/airpods.png")));
    // Persistent file log + minidump handler — detached launches lose
    // stdout, which is why past crashes left no evidence.
    CrashDump::install();

    qCInfo(librepods) << "LibrePods driver transport:" << DriverL2CapSocket::driverStatus();

    // Load translations
    QTranslator *translator = new QTranslator(&app);
    QString locale = QLocale::system().name();

    QStringList translationPaths = {
        QCoreApplication::applicationDirPath() + "/translations",
        QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/translations"
    };

    for (const QString &path : translationPaths) {
        if (translator->load("librepods_" + locale, path)) {
            app.installTranslator(translator);
            break;
        }
    }

    const QString lockName = QStringLiteral("librepods_server");

    QLocalServer::removeServer(lockName);

    QFile stale(QStandardPaths::writableLocation(QStandardPaths::TempLocation) + "/" + lockName);
    if (stale.exists())
        stale.remove();

    QLocalSocket socket_check;
    socket_check.connectToServer(lockName);

    if (socket_check.waitForConnected(300)) {
        // A wedged process can leave its named-pipe listener accepting
        // connections while its event loop is dead — waitForConnected alone
        // can't tell zombie from live. Proof of life: a live instance reads
        // the message and disconnects promptly; a zombie leaves it open.
        socket_check.write("reopen");
        socket_check.flush();
        socket_check.waitForBytesWritten(200);
        if (socket_check.waitForDisconnected(800)) {
            LOG_INFO("Another instance already running! Reopening window...");
            return 0;
        }
        LOG_WARN("Existing instance is unresponsive; taking over the lock");
        socket_check.disconnectFromServer();
        QLocalServer::removeServer(lockName);
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
    QLocalServer::removeServer(lockName);

    if (!server.listen(lockName))
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
                QMetaObject::invokeMethod(trayApp, "onOpenApp", Qt::QueuedConnection);
            }
            else if (msg == "settings") {
                LOG_INFO("Opening settings page");
                QMetaObject::invokeMethod(trayApp, "onOpenSettings", Qt::QueuedConnection);
            }
            else if (msg.startsWith("scroll:")) {
                QObject *rootObject = engine.rootObjects().first();
                if (rootObject) {
                    QMetaObject::invokeMethod(rootObject, "scrollSettings",
                                              Q_ARG(QVariant, msg.mid(7).toDouble()));
                }
            }
            else if (msg == "popup") {
                QMetaObject::invokeMethod(trayApp, "flashWindowAtTray", Qt::QueuedConnection,
                                          Q_ARG(QString, QStringLiteral("Test popup")));
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
            else if (msg.startsWith("set:")) {
                // set:<name>:<v>[,<v2>,<v3>] — invoke a setter or write a
                // Q_PROPERTY through the same WRITE path QML uses.
                // Scope prefixes: di:→deviceInfo, ha:→hearingAidModel, as:→autoStartManager.
                QString body = msg.mid(4);
                QObject *target = trayApp;
                if (body.startsWith("di:"))      { target = trayApp->deviceInfo();       body = body.mid(3); }
                else if (body.startsWith("ha:")) { target = trayApp->hearingAidModel();  body = body.mid(3); }
                else if (body.startsWith("as:")) { target = trayApp->autoStartManager(); body = body.mid(3); }
                const int firstColon = body.indexOf(':');
                if (firstColon <= 0) {
                    LOG_ERROR("dev set: malformed '" << body << "'");
                } else {
                    const QByteArray name = body.left(firstColon).toUtf8();
                    const QStringList vals = body.mid(firstColon + 1).split(',');
                    const QMetaObject *mo = target->metaObject();
                    const int pi = mo->indexOfProperty(name.constData());
                    bool ok = false;
                    if (pi >= 0 && vals.size() == 1) {
                        const QString v = vals[0];
                        const QVariant var = (v == "true" || v == "false") ? QVariant(v == "true")
                                           : v.contains('.') ? QVariant(v.toDouble()) : QVariant(v.toInt());
                        ok = mo->property(pi).write(target, var);
                    }
                    if (!ok && vals.size() == 3)
                        ok = QMetaObject::invokeMethod(target, name.constData(),
                                                       Q_ARG(int, vals[0].toInt()), Q_ARG(int, vals[1].toInt()),
                                                       Q_ARG(int, vals[2].toInt()));
                    if (!ok && vals.size() == 1) {
                        const QString v = vals[0];
                        const bool isBool = (v == "true" || v == "false");
                        if (!isBool && !v.contains('.'))
                            ok = QMetaObject::invokeMethod(target, name.constData(), Q_ARG(int, v.toInt()));
                        if (!ok && v.contains('.'))
                            ok = QMetaObject::invokeMethod(target, name.constData(), Q_ARG(double, v.toDouble()));
                        if (!ok)
                            ok = QMetaObject::invokeMethod(target, name.constData(), Q_ARG(bool, isBool ? v == "true" : v.toInt() != 0));
                    }
                    LOG_INFO("dev set " << name << "(" << vals.join(",") << ") -> " << (ok ? "ok" : "FAILED"));
                }
            }
            else if (msg == "pose") {
                // One atomic line: mapped angles + raw sensor fields + which
                // pod sourced the stream. Built for the calibration harness.
                LOG_INFO("dev pose " << trayApp->devPoseLine());
            }
            else if (msg.startsWith("get:")) {
                // get:[scope:]name — read a Q_PROPERTY and log its value.
                QString body = msg.mid(4);
                QObject *target = trayApp;
                if (body.startsWith("di:"))      { target = trayApp->deviceInfo();       body = body.mid(3); }
                else if (body.startsWith("ha:")) { target = trayApp->hearingAidModel();  body = body.mid(3); }
                else if (body.startsWith("as:")) { target = trayApp->autoStartManager(); body = body.mid(3); }
                const QMetaObject *mo = target->metaObject();
                const int pi = mo->indexOfProperty(body.toUtf8().constData());
                LOG_INFO("dev get " << body << " -> "
                         << (pi >= 0 ? mo->property(pi).read(target).toString()
                                     : QStringLiteral("FAILED (no such property)")));
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
    });

    QObject::connect(&app, &QCoreApplication::aboutToQuit, [&]() {
        LOG_INFO("Application quitting. Cleaning up local server...");

        if (server.isListening()) {
            server.close();
        }

        QLocalServer::removeServer(lockName);
    });
    return app.exec();
}

#include "main.moc"
