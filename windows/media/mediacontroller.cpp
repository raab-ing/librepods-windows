#include "mediacontroller.h"
#include "logger.h"
#include "eardetection.hpp"

#include <QPointer>
#include <QMetaObject>
#include <QTimer>

#include <windows.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propidl.h>
#include <propvarutil.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Control.h>

// PKEY_DeviceInterface_Bluetooth_DeviceAddress; named uniquely here because
// the symbol name varies across Windows SDK versions.
static const PROPERTYKEY kBluetoothDeviceAddressKey = {
    {0x2BD67D8B, 0x8BEB, 0x48D5, {0x87, 0xE0, 0x6C, 0xDA, 0x34, 0x28, 0x04, 0x0A}}, 2};

using namespace winrt;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Media::Control;
using winrt::com_ptr;

namespace
{

com_ptr<IMMDevice> defaultRenderDevice()
{
    com_ptr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator), enumerator.put_void())))
    {
        return nullptr;
    }
    com_ptr<IMMDevice> device;
    if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, device.put())))
    {
        return nullptr;
    }
    return device;
}

QString propString(IPropertyStore *store, const PROPERTYKEY &key)
{
    PROPVARIANT v;
    PropVariantInit(&v);
    QString result;
    if (SUCCEEDED(store->GetValue(key, &v)) && v.vt == VT_LPWSTR && v.pwszVal)
    {
        result = QString::fromWCharArray(v.pwszVal);
    }
    PropVariantClear(&v);
    return result;
}

} // namespace

struct MediaController::Impl
{
    GlobalSystemMediaTransportControlsSessionManager sessionManager{nullptr};
    GlobalSystemMediaTransportControlsSessionManager::SessionsChanged_revoker sessionsChangedRevoker;
};

MediaController::MediaController(QObject *parent) : QObject(parent), impl(new Impl)
{
}

MediaController::~MediaController()
{
    delete impl;
}

void MediaController::followMediaChanges()
{
    QPointer<MediaController> guard(this);
    GlobalSystemMediaTransportControlsSessionManager::RequestAsync().Completed(
        [guard](IAsyncOperation<GlobalSystemMediaTransportControlsSessionManager> const &op,
                AsyncStatus status)
        {
            if (!guard || status != AsyncStatus::Completed)
            {
                return;
            }
            auto manager = op.GetResults();
            if (manager == nullptr)
            {
                return;
            }
            QMetaObject::invokeMethod(guard, [guard, manager]() {
                guard->impl->sessionManager = manager;
                guard->impl->sessionsChangedRevoker = manager.SessionsChanged(winrt::auto_revoke,
                    [guard](GlobalSystemMediaTransportControlsSessionManager const &,
                            SessionsChangedEventArgs const &)
                    {
                        QMetaObject::invokeMethod(guard, &MediaController::refreshPlaybackState,
                                                  Qt::QueuedConnection);
                    });
                guard->refreshPlaybackState();
            }, Qt::QueuedConnection);
        });
}

void MediaController::refreshPlaybackState()
{
    MediaState state = getCurrentMediaState();
    if (state != m_lastState)
    {
        m_lastState = state;
        emit mediaStateChanged(state);
    }
}

MediaController::MediaState MediaController::getCurrentMediaState() const
{
    if (impl->sessionManager == nullptr)
    {
        return Stopped;
    }
    try
    {
        for (auto const &session : impl->sessionManager.GetSessions())
        {
            auto status = session.GetPlaybackInfo().PlaybackStatus();
            if (status == GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing)
            {
                return Playing;
            }
        }
        for (auto const &session : impl->sessionManager.GetSessions())
        {
            auto status = session.GetPlaybackInfo().PlaybackStatus();
            if (status == GlobalSystemMediaTransportControlsSessionPlaybackStatus::Paused)
            {
                return Paused;
            }
        }
    }
    catch (const hresult_error &)
    {
    }
    return Stopped;
}

void MediaController::pause()
{
    if (impl->sessionManager == nullptr)
    {
        return;
    }
    pausedByAppServices.clear();
    try
    {
        for (auto const &session : impl->sessionManager.GetSessions())
        {
            if (session.GetPlaybackInfo().PlaybackStatus() ==
                GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing)
            {
                QString appId = QString::fromWCharArray(session.SourceAppUserModelId().c_str());
                auto async = session.TryPauseAsync();
                async.Completed([appId](IAsyncOperation<bool> const &op, AsyncStatus status) {
                    if (status == AsyncStatus::Completed && op.GetResults())
                    {
                        LOG_DEBUG("Paused media session: " << appId);
                    }
                });
                pausedByAppServices << appId;
            }
        }
    }
    catch (const hresult_error &)
    {
    }
}

void MediaController::play()
{
    if (impl->sessionManager == nullptr || pausedByAppServices.isEmpty())
    {
        return;
    }
    try
    {
        for (auto const &session : impl->sessionManager.GetSessions())
        {
            QString appId = QString::fromWCharArray(session.SourceAppUserModelId().c_str());
            if (pausedByAppServices.contains(appId))
            {
                auto async = session.TryPlayAsync();
                async.Completed([appId](IAsyncOperation<bool> const &op, AsyncStatus status) {
                    if (status == AsyncStatus::Completed && op.GetResults())
                    {
                        LOG_DEBUG("Resumed media session: " << appId);
                    }
                });
            }
        }
        pausedByAppServices.clear();
    }
    catch (const hresult_error &)
    {
    }
}

void MediaController::togglePlayPause()
{
    if (impl->sessionManager == nullptr)
    {
        return;
    }
    try
    {
        for (auto const &session : impl->sessionManager.GetSessions())
        {
            const auto status = session.GetPlaybackInfo().PlaybackStatus();
            if (status == GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing)
            {
                session.TryPauseAsync();
                return;
            }
        }
        // Nothing playing — resume the first paused session.
        for (auto const &session : impl->sessionManager.GetSessions())
        {
            if (session.GetPlaybackInfo().PlaybackStatus() ==
                GlobalSystemMediaTransportControlsSessionPlaybackStatus::Paused)
            {
                session.TryPlayAsync();
                return;
            }
        }
    }
    catch (const hresult_error &)
    {
    }
}

void MediaController::nextTrack()
{
    if (impl->sessionManager == nullptr)
    {
        return;
    }
    try
    {
        for (auto const &session : impl->sessionManager.GetSessions())
        {
            const auto status = session.GetPlaybackInfo().PlaybackStatus();
            if (status == GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing ||
                status == GlobalSystemMediaTransportControlsSessionPlaybackStatus::Paused)
            {
                session.TrySkipNextAsync();
                return;
            }
        }
    }
    catch (const hresult_error &)
    {
    }
}

void MediaController::previousTrack()
{
    if (impl->sessionManager == nullptr)
    {
        return;
    }
    try
    {
        for (auto const &session : impl->sessionManager.GetSessions())
        {
            const auto status = session.GetPlaybackInfo().PlaybackStatus();
            if (status == GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing ||
                status == GlobalSystemMediaTransportControlsSessionPlaybackStatus::Paused)
            {
                session.TrySkipPreviousAsync();
                return;
            }
        }
    }
    catch (const hresult_error &)
    {
    }
}

void MediaController::volumeUp()
{
    const float current = getDefaultEndpointVolume();
    if (current >= 0.0f)
    {
        setDefaultEndpointVolume(qMin(1.0f, current + 0.05f));
    }
}

void MediaController::volumeDown()
{
    const float current = getDefaultEndpointVolume();
    if (current >= 0.0f)
    {
        setDefaultEndpointVolume(qMax(0.0f, current - 0.05f));
    }
}

bool MediaController::isActiveOutputDeviceAirPods()
{
    if (connectedDeviceMacAddress.isEmpty())
    {
        return false;
    }

    com_ptr<IMMDevice> device = defaultRenderDevice();
    if (!device)
    {
        return false;
    }

    com_ptr<IPropertyStore> store;
    if (FAILED(device->OpenPropertyStore(STGM_READ, store.put())))
    {
        return false;
    }

    // Preferred check: the Bluetooth address property on the endpoint.
    QString endpointBtAddress = propString(store.get(), kBluetoothDeviceAddressKey);
    if (!endpointBtAddress.isEmpty())
    {
        QString normalizedEndpoint = endpointBtAddress;
        normalizedEndpoint.remove(':').remove('-');
        QString normalizedLocal = connectedDeviceMacAddress;
        normalizedLocal.remove(':').remove('_').remove('-');
        if (normalizedEndpoint.compare(normalizedLocal, Qt::CaseInsensitive) == 0)
        {
            return true;
        }
    }

    // Fallback: friendly-name match against the advertised device name.
    QString friendlyName = propString(store.get(), PKEY_Device_FriendlyName);
    if (!connectedDeviceName.isEmpty() && friendlyName.contains(connectedDeviceName, Qt::CaseInsensitive))
    {
        return true;
    }
    return friendlyName.contains(QStringLiteral("airpods"), Qt::CaseInsensitive);
}

float MediaController::getDefaultEndpointVolume()
{
    com_ptr<IMMDevice> device = defaultRenderDevice();
    if (!device)
    {
        return -1.0f;
    }
    com_ptr<IAudioEndpointVolume> endpointVolume;
    if (FAILED(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                                endpointVolume.put_void())))
    {
        return -1.0f;
    }
    float volume = 0.0f;
    if (FAILED(endpointVolume->GetMasterVolumeLevelScalar(&volume)))
    {
        return -1.0f;
    }
    return volume;
}

bool MediaController::setDefaultEndpointVolume(float volume)
{
    com_ptr<IMMDevice> device = defaultRenderDevice();
    if (!device)
    {
        return false;
    }
    com_ptr<IAudioEndpointVolume> endpointVolume;
    if (FAILED(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                                endpointVolume.put_void())))
    {
        return false;
    }
    return SUCCEEDED(endpointVolume->SetMasterVolumeLevelScalar(volume, nullptr));
}

void MediaController::handleConversationalAwareness(const QByteArray &data)
{
    if (data.size() < 10)
    {
        LOG_ERROR("Invalid conversational awareness packet");
        return;
    }

    uint8_t flag = (uint8_t)data[9];

    switch (flag)
    {
    case 0x01:
        LOG_INFO("Conversational awareness event: voice detected");

        if (initialVolume < 0.0f && isActiveOutputDeviceAirPods())
        {
            initialVolume = getDefaultEndpointVolume();
            LOG_DEBUG("Initial volume saved: " << initialVolume);
        }

        if (initialVolume >= 0.0f)
        {
            float target = initialVolume * 0.20f;
            setDefaultEndpointVolume(target);
            LOG_INFO("Volume lowered to " << target * 100.0f << "%");
        }
        break;

    case 0x08:
        LOG_INFO("Conversational awareness disabled");
        initialVolume = -1.0f;
        break;

    case 0x09:
        LOG_INFO("Conversational awareness enabled");
        break;

    default:
        LOG_INFO("Conversational awareness event: voice ended");

        if (initialVolume >= 0.0f && isActiveOutputDeviceAirPods())
        {
            setDefaultEndpointVolume(initialVolume);
            LOG_INFO("Volume restored to " << initialVolume * 100.0f << "%");
            initialVolume = -1.0f;
        }
        break;
    }
}

bool MediaController::earPauseDue(EarDetection *earDetection) const
{
    const bool primaryInEar = earDetection->isPrimaryInEar();
    const bool secondaryInEar = earDetection->isSecondaryInEar();
    if (earDetectionBehavior == PauseWhenOneRemoved)
        return !primaryInEar || !secondaryInEar;
    if (earDetectionBehavior == PauseWhenBothRemoved)
        return !primaryInEar && !secondaryInEar;
    return false;
}

void MediaController::handleEarDetection(EarDetection *earDetection)
{
    if (earDetectionBehavior == Disabled)
    {
        LOG_DEBUG("Ear detection is disabled, ignoring status");
        return;
    }

    bool primaryInEar = earDetection->isPrimaryInEar();
    bool secondaryInEar = earDetection->isSecondaryInEar();

    const bool shouldPause = earPauseDue(earDetection);
    bool shouldResume = false;

    if (earDetectionBehavior == PauseWhenOneRemoved)
    {
        shouldResume = primaryInEar && secondaryInEar;
    }
    else if (earDetectionBehavior == PauseWhenBothRemoved)
    {
        shouldResume = primaryInEar || secondaryInEar;
    }

    if (shouldPause && isActiveOutputDeviceAirPods())
    {
        // Pods emit transient NotInEar flickers while worn (sensor noise,
        // fit shift) — pausing on the first report makes media stop and
        // auto-resume "by itself". Require the state to persist 1.5s; a
        // revert before then cancels the pending pause entirely.
        m_earPauseSource = earDetection;
        if (!m_earPauseTimer)
        {
            m_earPauseTimer = new QTimer(this);
            m_earPauseTimer->setSingleShot(true);
            connect(m_earPauseTimer, &QTimer::timeout, this, [this]() {
                if (m_earPauseSource && earPauseDue(m_earPauseSource)
                    && isActiveOutputDeviceAirPods()
                    && getCurrentMediaState() == Playing)
                {
                    LOG_INFO("Pausing playback for ear detection (confirmed)");
                    pause();
                }
            });
        }
        if (getCurrentMediaState() == Playing && !m_earPauseTimer->isActive())
        {
            LOG_DEBUG("Ear-detection pause pending debounce");
            m_earPauseTimer->start(1500);
        }
    }
    else if (m_earPauseTimer)
    {
        m_earPauseTimer->stop();
    }

    if (primaryInEar || secondaryInEar)
    {
        LOG_INFO("At least one AirPod is in ear");
        activateA2dpProfile();

        if (shouldResume && !pausedByAppServices.isEmpty() && isActiveOutputDeviceAirPods())
        {
            play();
        }
    }
    else
    {
        LOG_INFO("Both AirPods are out of ear");
        removeAudioOutputDevice();
    }
}

void MediaController::setEarDetectionBehavior(EarDetectionBehavior behavior)
{
    earDetectionBehavior = behavior;
    LOG_INFO("Set ear detection behavior to: " << behavior);
}

void MediaController::setConnectedDeviceMacAddress(const QString &macAddress)
{
    connectedDeviceMacAddress = macAddress;
}

void MediaController::setConnectedDeviceName(const QString &name)
{
    connectedDeviceName = name;
}

// Windows manages Bluetooth audio profiles at the OS level; there is no
// per-application API to switch a device between A2DP and HFP. These profile
// helpers are retained for API parity with the Linux build.
void MediaController::activateA2dpProfile()
{
    LOG_DEBUG("activateA2dpProfile: Windows manages A2DP automatically, nothing to do");
}

void MediaController::removeAudioOutputDevice()
{
    LOG_DEBUG("removeAudioOutputDevice: not supported on Windows, nothing to do");
}

bool MediaController::isA2dpProfileAvailable()
{
    return !connectedDeviceMacAddress.isEmpty();
}

QString MediaController::getPreferredA2dpProfile()
{
    return QStringLiteral("a2dp-sink");
}

bool MediaController::restartWirePlumber()
{
    return false;
}
