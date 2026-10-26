#pragma once

#include <QObject>
#include <QByteArray>
#include <QStringList>
#include <QSettings>
#include "battery.hpp"
#include "enums.h"
#include "eardetection.hpp"

using namespace AirpodsTrayApp::Enums;

class DeviceInfo : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString batteryStatus READ batteryStatus WRITE setBatteryStatus NOTIFY batteryStatusChanged)
    Q_PROPERTY(int noiseControlMode READ noiseControlModeInt WRITE setNoiseControlModeInt NOTIFY noiseControlModeChangedInt)
    Q_PROPERTY(bool conversationalAwareness READ conversationalAwareness WRITE setConversationalAwareness NOTIFY conversationalAwarenessChanged)
    Q_PROPERTY(bool hearingAidEnabled READ hearingAidEnabled WRITE setHearingAidEnabled NOTIFY hearingAidEnabledChanged)
    Q_PROPERTY(int adaptiveNoiseLevel READ adaptiveNoiseLevel WRITE setAdaptiveNoiseLevel NOTIFY adaptiveNoiseLevelChanged)
    Q_PROPERTY(QString deviceName READ deviceName WRITE setDeviceName NOTIFY deviceNameChanged)
    Q_PROPERTY(Battery *battery READ getBattery CONSTANT)
    Q_PROPERTY(bool oneBudANCMode READ oneBudANCMode WRITE setOneBudANCMode NOTIFY oneBudANCModeChanged)
    Q_PROPERTY(AirPodsModel model READ model WRITE setModel NOTIFY modelChanged)
    Q_PROPERTY(bool adaptiveModeActive READ adaptiveModeActive NOTIFY noiseControlModeChangedInt)
    Q_PROPERTY(QString podIcon READ podIcon NOTIFY modelChanged)
    Q_PROPERTY(QString caseIcon READ caseIcon NOTIFY modelChanged)
    Q_PROPERTY(bool isHeadset READ isHeadset NOTIFY modelChanged)
    Q_PROPERTY(bool supportsANC READ supportsANC NOTIFY modelChanged)
    Q_PROPERTY(bool supportsAdaptive READ supportsAdaptive NOTIFY modelChanged)
    Q_PROPERTY(bool supportsStemPress READ supportsStemPress NOTIFY modelChanged)
    Q_PROPERTY(bool supportsHeadTracking READ supportsHeadTracking NOTIFY modelChanged)
    Q_PROPERTY(bool supportsHearingAid READ supportsHearingAid NOTIFY modelChanged)
    Q_PROPERTY(bool supportsConversationalAwareness READ supportsConversationalAwareness NOTIFY modelChanged)
    Q_PROPERTY(bool supportsVolumeSwipe READ supportsVolumeSwipe NOTIFY modelChanged)
    Q_PROPERTY(bool supportsCaseBattery READ supportsCaseBattery NOTIFY modelChanged)
    Q_PROPERTY(bool leftPodInEar READ isLeftPodInEar NOTIFY primaryChanged)
    Q_PROPERTY(bool rightPodInEar READ isRightPodInEar NOTIFY primaryChanged)
    Q_PROPERTY(bool leftPodInCase READ isLeftPodInCase NOTIFY primaryChanged)
    Q_PROPERTY(bool rightPodInCase READ isRightPodInCase NOTIFY primaryChanged)
    Q_PROPERTY(QString bluetoothAddress READ bluetoothAddress WRITE setBluetoothAddress NOTIFY bluetoothAddressChanged)
    Q_PROPERTY(QString magicAccIRK READ magicAccIRKHex CONSTANT)
    Q_PROPERTY(QString magicAccEncKey READ magicAccEncKeyHex CONSTANT)
    // Plaintext proximity-advertisement state (available without AACP keys)
    Q_PROPERTY(int podsInCase READ podsInCase NOTIFY proximityStateChanged)
    Q_PROPERTY(bool caseLidOpen READ caseLidOpen NOTIFY proximityStateChanged)
    Q_PROPERTY(QString connectionState READ connectionState NOTIFY proximityStateChanged)

    // Device identity (from the AACP information/metadata packet)
    Q_PROPERTY(QString modelNumber READ modelNumber NOTIFY deviceIdentityChanged)
    Q_PROPERTY(QString manufacturer READ manufacturer NOTIFY deviceIdentityChanged)
    Q_PROPERTY(QString serialNumber READ serialNumber NOTIFY deviceIdentityChanged)
    Q_PROPERTY(QString leftSerialNumber READ leftSerialNumber NOTIFY deviceIdentityChanged)
    Q_PROPERTY(QString rightSerialNumber READ rightSerialNumber NOTIFY deviceIdentityChanged)
    Q_PROPERTY(QString firmwareVersion READ firmwareVersion NOTIFY deviceIdentityChanged)
    Q_PROPERTY(QString hardwareRevision READ hardwareRevision NOTIFY deviceIdentityChanged)

    // Head tracking (AACP 0x17): calibrated orientation in degrees
    Q_PROPERTY(bool headTrackingEnabled READ headTrackingEnabled WRITE setHeadTrackingEnabled NOTIFY headTrackingChanged)
    Q_PROPERTY(double headYaw READ headYaw NOTIFY headTrackingChanged)
    Q_PROPERTY(double headPitch READ headPitch NOTIFY headTrackingChanged)

    // TiPi connected-device list (AACP 0x2E) + custom EQ state (0x63)
    Q_PROPERTY(QStringList connectedDevices READ connectedDevices NOTIFY connectedDevicesChanged)
    Q_PROPERTY(bool customEqEnabled READ customEqEnabled NOTIFY customEqChanged)
    Q_PROPERTY(int customEqLow READ customEqLow NOTIFY customEqChanged)
    Q_PROPERTY(int customEqMid READ customEqMid NOTIFY customEqChanged)
    Q_PROPERTY(int customEqHigh READ customEqHigh NOTIFY customEqChanged)

    // Pod-side control-command state (pushed by the pods after the
    // REQUEST_NOTIFICATIONS handshake). One shared notify — these all
    // change together rarely and every consumer re-reads the group.
    Q_PROPERTY(int micMode READ micMode NOTIFY podConfigChanged)
    Q_PROPERTY(bool podEarDetection READ podEarDetection NOTIFY podConfigChanged)
    Q_PROPERTY(int pressSpeed READ pressSpeed NOTIFY podConfigChanged)
    Q_PROPERTY(int holdDuration READ holdDuration NOTIFY podConfigChanged)
    Q_PROPERTY(int listeningModeCycle READ listeningModeCycle NOTIFY podConfigChanged)
    Q_PROPERTY(int chimeVolume READ chimeVolume NOTIFY podConfigChanged)
    Q_PROPERTY(bool automaticConnection READ automaticConnection NOTIFY podConfigChanged)
    Q_PROPERTY(bool muteOnSinglePress READ muteOnSinglePress NOTIFY podConfigChanged)
    Q_PROPERTY(bool inCaseTone READ inCaseTone NOTIFY podConfigChanged)
    Q_PROPERTY(bool siriMultitone READ siriMultitone NOTIFY podConfigChanged)
    Q_PROPERTY(bool sleepDetection READ sleepDetection NOTIFY podConfigChanged)
    Q_PROPERTY(bool loudSoundReduction READ loudSoundReduction NOTIFY podConfigChanged)
    Q_PROPERTY(int ppeCapLevel READ ppeCapLevel NOTIFY podConfigChanged)
    Q_PROPERTY(bool optimizedCharging READ optimizedCharging NOTIFY podConfigChanged)
    Q_PROPERTY(bool hearingGainSwipe READ hearingGainSwipe NOTIFY podConfigChanged)
    Q_PROPERTY(bool allowOffOption READ allowOffOption NOTIFY podConfigChanged)
    Q_PROPERTY(int crownDirection READ crownDirection NOTIFY podConfigChanged)
    Q_PROPERTY(bool volumeSwipeEnabled READ volumeSwipeEnabled NOTIFY podConfigChanged)
    Q_PROPERTY(int volumeSwipeInterval READ volumeSwipeInterval NOTIFY podConfigChanged)
    Q_PROPERTY(bool adaptiveVolume READ adaptiveVolume NOTIFY podConfigChanged)
    Q_PROPERTY(int hearingAssistMode READ hearingAssistMode NOTIFY podConfigChanged)
    Q_PROPERTY(bool supportsSleepDetection READ supportsSleepDetection NOTIFY modelChanged)
    Q_PROPERTY(bool supportsLoudSoundReduction READ supportsLoudSoundReduction NOTIFY modelChanged)
    Q_PROPERTY(bool supportsCaseSpeaker READ supportsCaseSpeaker NOTIFY modelChanged)

public:
    explicit DeviceInfo(QObject *parent = nullptr) : QObject(parent), m_battery(new Battery(this)), m_earDetection(new EarDetection(this)) {
        connect(getEarDetection(), &EarDetection::statusChanged, this, &DeviceInfo::primaryChanged);
    }

    QString batteryStatus() const { return m_batteryStatus; }
    void setBatteryStatus(const QString &status)
    {
        if (m_batteryStatus != status)
        {
            m_batteryStatus = status;
            emit batteryStatusChanged(status);
        }
    }

    NoiseControlMode noiseControlMode() const { return m_noiseControlMode; }
    void setNoiseControlMode(NoiseControlMode mode)
    {
        if (m_noiseControlMode != mode)
        {
            m_noiseControlMode = mode;
            emit noiseControlModeChanged(mode);
            emit noiseControlModeChangedInt(static_cast<int>(mode));
        }
    }
    int noiseControlModeInt() const { return static_cast<int>(noiseControlMode()); }
    void setNoiseControlModeInt(int mode) { setNoiseControlMode(static_cast<NoiseControlMode>(mode)); }

    bool conversationalAwareness() const { return m_conversationalAwareness; }
    void setConversationalAwareness(bool enabled)
    {
        if (m_conversationalAwareness != enabled)
        {
            m_conversationalAwareness = enabled;
            emit conversationalAwarenessChanged(enabled);
        }
    }

    bool hearingAidEnabled() const { return m_hearingAidEnabled; }
    void setHearingAidEnabled(bool enabled)
    {
        if (m_hearingAidEnabled != enabled)
        {
            m_hearingAidEnabled = enabled;
            emit hearingAidEnabledChanged(enabled);
        }
    }

    int adaptiveNoiseLevel() const { return m_adaptiveNoiseLevel; }
    void setAdaptiveNoiseLevel(int level)
    {
        if (m_adaptiveNoiseLevel != level)
        {
            m_adaptiveNoiseLevel = level;
            emit adaptiveNoiseLevelChanged(level);
        }
    }

    QString deviceName() const { return m_deviceName; }
    void setDeviceName(const QString &name)
    {
        if (m_deviceName != name)
        {
            m_deviceName = name;
            emit deviceNameChanged(name);
        }
    }

    Battery *getBattery() const { return m_battery; }

    bool oneBudANCMode() const { return m_oneBudANCMode; }
    void setOneBudANCMode(bool enabled)
    {
        if (m_oneBudANCMode != enabled)
        {
            m_oneBudANCMode = enabled;
            emit oneBudANCModeChanged(enabled);
        }
    }

    AirPodsModel model() const { return m_model; }
    void setModel(AirPodsModel model)
    {
        if (m_model != model)
        {
            m_model = model;
            emit modelChanged();
        }
    }

    QByteArray magicAccIRK() const { return m_magicAccIRK; }
    void setMagicAccIRK(const QByteArray &irk) { m_magicAccIRK = irk; }
    QString magicAccIRKHex() const { return QString::fromUtf8(m_magicAccIRK.toHex()); }

    QByteArray magicAccEncKey() const { return m_magicAccEncKey; }
    void setMagicAccEncKey(const QByteArray &key) { m_magicAccEncKey = key; }
    QString magicAccEncKeyHex() const { return QString::fromUtf8(m_magicAccEncKey.toHex()); }

    QString modelNumber() const { return m_modelNumber; }
    void setModelNumber(const QString &modelNumber) { m_modelNumber = modelNumber; }

    QString manufacturer() const { return m_manufacturer; }
    void setManufacturer(const QString &manufacturer) { m_manufacturer = manufacturer; }

    QString bluetoothAddress() const { return m_bluetoothAddress; }
    void setBluetoothAddress(const QString &address)
    {
        if (m_bluetoothAddress != address)
        {
            m_bluetoothAddress = address;
            emit bluetoothAddressChanged(address);
        }
    }

    QString podIcon() const { return getModelIcon(model()).first; }
    QString caseIcon() const { return getModelIcon(model()).second; }
    bool isHeadset() const { return isModelHeadset(model()); }

    // Per-model capabilities. Unknown model (pre-metadata) is permissive so
    // nothing wrongly hides while detection is still settling.
    bool supportsANC() const
    {
        switch (m_model) {
        case AirPodsModel::AirPods1:
        case AirPodsModel::AirPods2:
        case AirPodsModel::AirPods3:
        case AirPodsModel::AirPods4:
            return false;
        default:
            return true; // Pro 1/2, Max, AirPods 4 ANC, Unknown
        }
    }
    bool supportsAdaptive() const // adaptive audio (ANC + transparency blend)
    {
        switch (m_model) {
        case AirPodsModel::AirPodsPro2Lightning:
        case AirPodsModel::AirPodsPro2USBC:
        case AirPodsModel::AirPods4ANC:
            return true;
        default:
            return m_model == AirPodsModel::Unknown;
        }
    }
    bool supportsStemPress() const // force-sensor stem, not tap or crown
    {
        switch (m_model) {
        case AirPodsModel::AirPods1:
        case AirPodsModel::AirPods2:
        case AirPodsModel::AirPodsMaxLightning:
        case AirPodsModel::AirPodsMaxUSBC:
            return false;
        default:
            return true; // Pro 1/2, AirPods 3/4, Unknown
        }
    }
    bool supportsHeadTracking() const
    {
        switch (m_model) {
        case AirPodsModel::AirPods1:
        case AirPodsModel::AirPods2:
            return false;
        default:
            return true; // 3, 4, Pro, Pro 2, Max, Unknown
        }
    }
    bool supportsHearingAid() const
    {
        switch (m_model) {
        case AirPodsModel::AirPodsPro2Lightning:
        case AirPodsModel::AirPodsPro2USBC:
            return true;
        default:
            return m_model == AirPodsModel::Unknown;
        }
    }
    bool supportsConversationalAwareness() const
    {
        switch (m_model) {
        case AirPodsModel::AirPodsPro2Lightning:
        case AirPodsModel::AirPodsPro2USBC:
        case AirPodsModel::AirPods4ANC:
            return true;
        default:
            return m_model == AirPodsModel::Unknown;
        }
    }
    bool supportsVolumeSwipe() const // stem swipe for volume
    {
        switch (m_model) {
        case AirPodsModel::AirPodsPro2Lightning:
        case AirPodsModel::AirPodsPro2USBC:
            return true;
        default:
            return m_model == AirPodsModel::Unknown;
        }
    }
    bool supportsCaseBattery() const { return !isHeadset(); }
    bool isLeftPodInEar() const
    {
        if (getBattery()->getPrimaryPod() == Battery::Component::Left) return getEarDetection()->isPrimaryInEar();
        else return getEarDetection()->isSecondaryInEar();
    }
    bool isRightPodInEar() const
    {
        if (getBattery()->getPrimaryPod() == Battery::Component::Right) return getEarDetection()->isPrimaryInEar();
        else return getEarDetection()->isSecondaryInEar();
    }
    bool isLeftPodInCase() const
    {
        const bool primaryIsLeft = getBattery()->getPrimaryPod() == Battery::Component::Left;
        return (primaryIsLeft ? getEarDetection()->getprimaryStatus()
                              : getEarDetection()->getsecondaryStatus()) == EarDetection::EarDetectionStatus::InCase;
    }
    bool isRightPodInCase() const
    {
        const bool primaryIsLeft = getBattery()->getPrimaryPod() == Battery::Component::Left;
        return (primaryIsLeft ? getEarDetection()->getsecondaryStatus()
                              : getEarDetection()->getprimaryStatus()) == EarDetection::EarDetectionStatus::InCase;
    }

    bool adaptiveModeActive() const { return noiseControlMode() == NoiseControlMode::Adaptive; }

    int podsInCase() const { return m_podsInCase; }
    bool caseLidOpen() const { return m_caseLidOpen; }
    QString connectionState() const { return m_connectionState; }

    void setConnectionState(const QString &state)
    {
        if (state != m_connectionState)
        {
            m_connectionState = state;
            emit proximityStateChanged();
        }
    }

    void updateProximityState(bool onePodInCase, bool bothPodsInCase, bool lidOpen)
    {
        int count = bothPodsInCase ? 2 : (onePodInCase ? 1 : 0);
        if (count != m_podsInCase || lidOpen != m_caseLidOpen)
        {
            m_podsInCase = count;
            m_caseLidOpen = lidOpen;
            emit proximityStateChanged();
        }
    }

    QString serialNumber() const { return m_serialNumber; }
    QString leftSerialNumber() const { return m_leftSerialNumber; }
    QString rightSerialNumber() const { return m_rightSerialNumber; }
    QString firmwareVersion() const { return m_firmwareVersion; }
    QString hardwareRevision() const { return m_hardwareRevision; }

    // Set the identity strings decoded from the information packet in one
    // shot so QML re-evaluates once rather than per field.
    void setDeviceIdentity(const QString &serial, const QString &leftSerial,
                           const QString &rightSerial, const QString &firmware,
                           const QString &hwRev)
    {
        if (serial != m_serialNumber || leftSerial != m_leftSerialNumber ||
            rightSerial != m_rightSerialNumber || firmware != m_firmwareVersion ||
            hwRev != m_hardwareRevision)
        {
            m_serialNumber = serial;
            m_leftSerialNumber = leftSerial;
            m_rightSerialNumber = rightSerial;
            m_firmwareVersion = firmware;
            m_hardwareRevision = hwRev;
            emit deviceIdentityChanged();
        }
    }

    bool headTrackingEnabled() const { return m_headTrackingEnabled; }
    void setHeadTrackingEnabled(bool enabled)
    {
        if (m_headTrackingEnabled != enabled)
        {
            m_headTrackingEnabled = enabled;
            emit headTrackingChanged();
        }
    }
    double headYaw() const { return m_headYaw; }
    double headPitch() const { return m_headPitch; }
    void setHeadOrientation(double yaw, double pitch)
    {
        if (!qFuzzyCompare(yaw, m_headYaw) || !qFuzzyCompare(pitch, m_headPitch))
        {
            m_headYaw = yaw;
            m_headPitch = pitch;
            emit headTrackingChanged();
        }
    }

    QStringList connectedDevices() const { return m_connectedDevices; }
    void setConnectedDevices(const QStringList &devices)
    {
        if (devices != m_connectedDevices)
        {
            m_connectedDevices = devices;
            emit connectedDevicesChanged();
        }
    }

    bool customEqEnabled() const { return m_customEqEnabled; }
    int customEqLow() const { return m_customEqLow; }
    int customEqMid() const { return m_customEqMid; }
    int customEqHigh() const { return m_customEqHigh; }
    void setCustomEq(bool enabled, int low, int mid, int high)
    {
        if (enabled != m_customEqEnabled || low != m_customEqLow ||
            mid != m_customEqMid || high != m_customEqHigh)
        {
            m_customEqEnabled = enabled;
            m_customEqLow = low;
            m_customEqMid = mid;
            m_customEqHigh = high;
            emit customEqChanged();
        }
    }

    // ---- pod-side control-command state ----
    int micMode() const { return m_micMode; }
    void setMicMode(int v) { if (v != m_micMode) { m_micMode = v; emit podConfigChanged(); } }
    bool podEarDetection() const { return m_podEarDetection; }
    void setPodEarDetection(bool v) { if (v != m_podEarDetection) { m_podEarDetection = v; emit podConfigChanged(); } }
    int pressSpeed() const { return m_pressSpeed; }
    void setPressSpeed(int v) { if (v != m_pressSpeed) { m_pressSpeed = v; emit podConfigChanged(); } }
    int holdDuration() const { return m_holdDuration; }
    void setHoldDuration(int v) { if (v != m_holdDuration) { m_holdDuration = v; emit podConfigChanged(); } }
    int listeningModeCycle() const { return m_listeningModeCycle; }
    void setListeningModeCycle(int v) { if (v != m_listeningModeCycle) { m_listeningModeCycle = v; emit podConfigChanged(); } }
    int chimeVolume() const { return m_chimeVolume; }
    void setChimeVolume(int v) { if (v != m_chimeVolume) { m_chimeVolume = v; emit podConfigChanged(); } }
    bool automaticConnection() const { return m_automaticConnection; }
    void setAutomaticConnection(bool v) { if (v != m_automaticConnection) { m_automaticConnection = v; emit podConfigChanged(); } }
    bool muteOnSinglePress() const { return m_muteOnSinglePress; }
    void setMuteOnSinglePress(bool v) { if (v != m_muteOnSinglePress) { m_muteOnSinglePress = v; emit podConfigChanged(); } }
    bool inCaseTone() const { return m_inCaseTone; }
    void setInCaseTone(bool v) { if (v != m_inCaseTone) { m_inCaseTone = v; emit podConfigChanged(); } }
    bool siriMultitone() const { return m_siriMultitone; }
    void setSiriMultitone(bool v) { if (v != m_siriMultitone) { m_siriMultitone = v; emit podConfigChanged(); } }
    bool sleepDetection() const { return m_sleepDetection; }
    void setSleepDetection(bool v) { if (v != m_sleepDetection) { m_sleepDetection = v; emit podConfigChanged(); } }
    bool loudSoundReduction() const { return m_loudSoundReduction; }
    void setLoudSoundReduction(bool v) { if (v != m_loudSoundReduction) { m_loudSoundReduction = v; emit podConfigChanged(); } }
    int ppeCapLevel() const { return m_ppeCapLevel; }
    void setPpeCapLevel(int v) { if (v != m_ppeCapLevel) { m_ppeCapLevel = v; emit podConfigChanged(); } }
    bool optimizedCharging() const { return m_optimizedCharging; }
    void setOptimizedCharging(bool v) { if (v != m_optimizedCharging) { m_optimizedCharging = v; emit podConfigChanged(); } }
    bool hearingGainSwipe() const { return m_hearingGainSwipe; }
    void setHearingGainSwipe(bool v) { if (v != m_hearingGainSwipe) { m_hearingGainSwipe = v; emit podConfigChanged(); } }
    bool allowOffOption() const { return m_allowOffOption; }
    void setAllowOffOption(bool v) { if (v != m_allowOffOption) { m_allowOffOption = v; emit podConfigChanged(); } }
    int crownDirection() const { return m_crownDirection; }
    void setCrownDirection(int v) { if (v != m_crownDirection) { m_crownDirection = v; emit podConfigChanged(); } }
    bool volumeSwipeEnabled() const { return m_volumeSwipeEnabled; }
    void setVolumeSwipeEnabled(bool v) { if (v != m_volumeSwipeEnabled) { m_volumeSwipeEnabled = v; emit podConfigChanged(); } }
    int volumeSwipeInterval() const { return m_volumeSwipeInterval; }
    void setVolumeSwipeInterval(int v) { if (v != m_volumeSwipeInterval) { m_volumeSwipeInterval = v; emit podConfigChanged(); } }
    bool adaptiveVolume() const { return m_adaptiveVolume; }
    void setAdaptiveVolume(bool v) { if (v != m_adaptiveVolume) { m_adaptiveVolume = v; emit podConfigChanged(); } }
    int hearingAssistMode() const { return m_hearingAssistMode; }
    void setHearingAssistMode(int v) { if (v != m_hearingAssistMode) { m_hearingAssistMode = v; emit podConfigChanged(); } }

    bool supportsSleepDetection() const
    {
        switch (m_model) {
        case AirPodsModel::AirPodsPro2Lightning:
        case AirPodsModel::AirPodsPro2USBC:
        case AirPodsModel::AirPods4ANC:
            return true;
        default:
            return m_model == AirPodsModel::Unknown;
        }
    }
    bool supportsLoudSoundReduction() const
    {
        switch (m_model) {
        case AirPodsModel::AirPodsPro2Lightning:
        case AirPodsModel::AirPodsPro2USBC:
            return true;
        default:
            return m_model == AirPodsModel::Unknown;
        }
    }
    bool supportsCaseSpeaker() const // Find-My-capable speaker in the case
    {
        switch (m_model) {
        case AirPodsModel::AirPodsPro2Lightning:
        case AirPodsModel::AirPodsPro2USBC:
        case AirPodsModel::AirPods4:
        case AirPodsModel::AirPods4ANC:
            return true;
        default:
            return m_model == AirPodsModel::Unknown;
        }
    }

    EarDetection *getEarDetection() const { return m_earDetection; }

    void reset(bool keepPhysicalState = false)
    {
        // Keep device identity (name/model/address): a disconnect doesn't
        // un-pair the device, and the UI should still show the remembered
        // AirPods. Only clear transient link/telemetry state — unless
        // keepPhysicalState is set: battery/ear/dock status describe the
        // pods rather than the link, survive the drop, and are refreshed
        // by BLE ads, so clearing them just flickers the UI.
        if (!keepPhysicalState)
        {
            m_battery->reset();
            getEarDetection()->reset();
            if (m_podsInCase != 0 || m_caseLidOpen || !m_connectionState.isEmpty())
            {
                m_podsInCase = 0;
                m_caseLidOpen = false;
                m_connectionState.clear();
                emit proximityStateChanged();
            }
        }
        setBatteryStatus("");
        setNoiseControlMode(NoiseControlMode::Off);
        setHearingAidEnabled(false);
    }

    void saveToSettings(QSettings &settings)
    {
        settings.beginGroup("DeviceInfo");
        settings.setValue("deviceName", deviceName());
        settings.setValue("model", static_cast<int>(model()));
        settings.setValue("bluetoothAddress", bluetoothAddress());
        settings.setValue("magicAccIRK", magicAccIRK());
        settings.setValue("magicAccEncKey", magicAccEncKey());
        settings.setValue("hearingAidEnabled", hearingAidEnabled());
        settings.setValue("serialNumber", serialNumber());
        settings.setValue("leftSerialNumber", leftSerialNumber());
        settings.setValue("rightSerialNumber", rightSerialNumber());
        settings.setValue("firmwareVersion", firmwareVersion());
        settings.setValue("hardwareRevision", hardwareRevision());
        settings.setValue("modelNumber", modelNumber());
        settings.endGroup();
    }
    void loadFromSettings(const QSettings &settings)
    {
        setDeviceName(settings.value("DeviceInfo/deviceName", "").toString());
        setModel(static_cast<AirPodsModel>(settings.value("DeviceInfo/model", (int)(AirPodsModel::Unknown)).toInt()));
        setBluetoothAddress(settings.value("DeviceInfo/bluetoothAddress", "").toString());
        setMagicAccIRK(settings.value("DeviceInfo/magicAccIRK", QByteArray()).toByteArray());
        setMagicAccEncKey(settings.value("DeviceInfo/magicAccEncKey", QByteArray()).toByteArray());
        setHearingAidEnabled(settings.value("DeviceInfo/hearingAidEnabled", false).toBool());
        setModelNumber(settings.value("DeviceInfo/modelNumber", "").toString());
        setDeviceIdentity(
            settings.value("DeviceInfo/serialNumber", "").toString(),
            settings.value("DeviceInfo/leftSerialNumber", "").toString(),
            settings.value("DeviceInfo/rightSerialNumber", "").toString(),
            settings.value("DeviceInfo/firmwareVersion", "").toString(),
            settings.value("DeviceInfo/hardwareRevision", "").toString());
    }

    void updateBatteryStatus()
    {
        if (getBattery()->getPrimaryPod() == Battery::Component::Headset) {
            int headsetLevel = getBattery()->getState(Battery::Component::Headset).level;
            setBatteryStatus(QString("Headset: %1%").arg(headsetLevel));
            return;
        }
        QStringList parts;
        const QPair<Battery::Component, QString> comps[] = {
            {Battery::Component::Left, "Left"},
            {Battery::Component::Right, "Right"},
            {Battery::Component::Case, "Case"},
        };
        for (const auto &c : comps) {
            auto state = getBattery()->getState(c.first);
            if (state.status != Battery::BatteryStatus::Disconnected)
                parts.append(QString("%1: %2%").arg(c.second).arg(state.level));
        }
        setBatteryStatus(parts.join(", "));
    }

signals:
    void batteryStatusChanged(const QString &status);
    void noiseControlModeChanged(NoiseControlMode mode);
    void noiseControlModeChangedInt(int mode);
    void conversationalAwarenessChanged(bool enabled);
    void hearingAidEnabledChanged(bool enabled);
    void adaptiveNoiseLevelChanged(int level);
    void deviceNameChanged(const QString &name);
    void primaryChanged();
    void oneBudANCModeChanged(bool enabled);
    void modelChanged();
    void bluetoothAddressChanged(const QString &address);
    void proximityStateChanged();
    void deviceIdentityChanged();
    void headTrackingChanged();
    void connectedDevicesChanged();
    void customEqChanged();
    void podConfigChanged();

private:
    QString m_batteryStatus;
    NoiseControlMode m_noiseControlMode = NoiseControlMode::Transparency;
    bool m_conversationalAwareness = false;
    bool m_hearingAidEnabled = false;
    int m_adaptiveNoiseLevel = 50;
    QString m_deviceName;
    Battery *m_battery;
    QByteArray m_magicAccIRK;
    QByteArray m_magicAccEncKey;
    bool m_oneBudANCMode = false;
    AirPodsModel m_model = AirPodsModel::Unknown;
    QString m_modelNumber;
    QString m_manufacturer;
    QString m_bluetoothAddress;
    EarDetection *m_earDetection;
    int m_podsInCase = 0;
    bool m_caseLidOpen = false;
    QString m_connectionState;
    QString m_serialNumber;
    QString m_leftSerialNumber;
    QString m_rightSerialNumber;
    QString m_firmwareVersion;
    QString m_hardwareRevision;
    bool m_headTrackingEnabled = false;
    double m_headYaw = 0;
    double m_headPitch = 0;
    QStringList m_connectedDevices;
    bool m_customEqEnabled = false;
    int m_customEqLow = 50, m_customEqMid = 50, m_customEqHigh = 50;
    int m_micMode = 0;
    bool m_podEarDetection = true;
    int m_pressSpeed = 0;
    int m_holdDuration = 0;
    int m_listeningModeCycle = 0;
    int m_chimeVolume = 75;
    bool m_automaticConnection = false;
    bool m_muteOnSinglePress = false;
    bool m_inCaseTone = false;
    bool m_siriMultitone = false;
    bool m_sleepDetection = false;
    bool m_loudSoundReduction = false;
    int m_ppeCapLevel = 82;
    bool m_optimizedCharging = false;
    bool m_hearingGainSwipe = false;
    bool m_allowOffOption = true;
    int m_crownDirection = 0;
    bool m_volumeSwipeEnabled = false;
    int m_volumeSwipeInterval = 1;
    bool m_adaptiveVolume = false;
    int m_hearingAssistMode = 0;
};
