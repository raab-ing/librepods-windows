#include "audiospectrum.h"
#include "logger.h"

#include <QThread>
#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#endif

namespace
{
    // Iterative radix-2 FFT (Cooley-Turkey, in-place).
    void fft(std::vector<std::complex<float>> &a)
    {
        const int n = static_cast<int>(a.size());
        for (int i = 1, j = 0; i < n; ++i)
        {
            int bit = n >> 1;
            for (; j & bit; bit >>= 1)
                j ^= bit;
            j ^= bit;
            if (i < j)
                std::swap(a[i], a[j]);
        }
        for (int len = 2; len <= n; len <<= 1)
        {
            const double ang = -2.0 * M_PI / len;
            const std::complex<float> wl(static_cast<float>(std::cos(ang)),
                                         static_cast<float>(std::sin(ang)));
            for (int i = 0; i < n; i += len)
            {
                std::complex<float> w(1.0f, 0.0f);
                for (int j = 0; j < len / 2; ++j)
                {
                    const auto u = a[i + j];
                    const auto v = a[i + j + len / 2] * w;
                    a[i + j] = u + v;
                    a[i + j + len / 2] = u - v;
                    w *= wl;
                }
            }
        }
    }

    constexpr int kFftSize = 2048;
    constexpr float kDbFloor = -60.0f; // below this a band renders empty

    // Convert a mono frame into smoothed log-band magnitudes 0..1.
    void computeBands(const std::vector<float> &mono, int sampleRate,
                      std::array<float, AudioSpectrum::kBandCount> &out)
    {
        std::vector<std::complex<float>> bins(kFftSize);
        for (int i = 0; i < kFftSize; ++i)
        {
            // Hann window
            const float w = 0.5f * (1.0f - std::cos(2.0f * M_PI * i / (kFftSize - 1)));
            bins[i] = std::complex<float>(mono[i] * w, 0.0f);
        }
        fft(bins);

        const float fLow = 40.0f;
        const float fHigh = std::min(16000.0f, sampleRate * 0.5f);
        const float binHz = static_cast<float>(sampleRate) / kFftSize;

        for (int b = 0; b < AudioSpectrum::kBandCount; ++b)
        {
            const float t0 = static_cast<float>(b) / AudioSpectrum::kBandCount;
            const float t1 = static_cast<float>(b + 1) / AudioSpectrum::kBandCount;
            const float f0 = fLow * std::pow(fHigh / fLow, t0);
            const float f1 = fLow * std::pow(fHigh / fLow, t1);
            const int i0 = std::max(1, static_cast<int>(f0 / binHz));
            const int i1 = std::min(kFftSize / 2, std::max(i0 + 1, static_cast<int>(f1 / binHz)));

            float mag = 0.0f;
            for (int i = i0; i < i1; ++i)
                mag = std::max(mag, std::abs(bins[i]));

            const float db = 20.0f * std::log10(mag + 1e-7f);
            const float norm = std::clamp((db - kDbFloor) / -kDbFloor, 0.0f, 1.0f);
            // Smooth attack/release so the strip doesn't flicker.
            out[b] = norm > out[b] ? out[b] * 0.35f + norm * 0.65f
                                   : out[b] * 0.80f + norm * 0.20f;
        }
    }
}

AudioSpectrum::AudioSpectrum(QObject *parent) : QObject(parent)
{
    m_levelsCache.reserve(kBandCount);
    for (int i = 0; i < kBandCount; ++i)
        m_levelsCache.append(0.0);
}

AudioSpectrum::~AudioSpectrum()
{
    setEnabled(false);
}

void AudioSpectrum::setEnabled(bool on)
{
    if (on == m_running.load())
        return;

    if (on)
    {
        m_stopRequested = false;
        m_running = true;
        m_thread = QThread::create([this]() { workerMain(); });
        connect(m_thread, &QThread::finished, m_thread, &QObject::deleteLater);
        m_thread->start();
        LOG_INFO("Audio spectrum capture started");
    }
    else
    {
        m_stopRequested = true;
        if (m_thread)
        {
            m_thread->wait(3000);
            m_thread = nullptr;
        }
        m_running = false;
        std::lock_guard<std::mutex> lk(m_mutex);
        m_bands.fill(0.0f);
        m_levelsCache.clear();
        for (int i = 0; i < kBandCount; ++i)
            m_levelsCache.append(0.0);
        LOG_INFO("Audio spectrum capture stopped");
    }
    emit enabledChanged();
    emit levelsChanged();
}

QVariantList AudioSpectrum::levels() const
{
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_levelsCache;
}

void AudioSpectrum::publish(const std::array<float, kBandCount> &bands)
{
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_levelsCache.clear();
        for (float v : bands)
            m_levelsCache.append(v);
    }
    emit levelsChanged();
}

void AudioSpectrum::workerMain()
{
#ifdef Q_OS_WIN
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)))
    {
        LOG_ERROR("Spectrum: CoInitializeEx failed");
        return;
    }

    IMMDeviceEnumerator *enumerator = nullptr;
    IMMDevice *device = nullptr;
    IAudioClient *client = nullptr;
    IAudioCaptureClient *capture = nullptr;
    WAVEFORMATEX *format = nullptr;

    auto cleanup = [&]()
    {
        if (client)
            client->Stop();
        if (capture)
            capture->Release();
        if (client)
            client->Release();
        if (device)
            device->Release();
        if (enumerator)
            enumerator->Release();
        if (format)
            CoTaskMemFree(format);
        CoUninitialize();
    };

    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                              __uuidof(IMMDeviceEnumerator), reinterpret_cast<void **>(&enumerator))) ||
        FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device)) ||
        FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                reinterpret_cast<void **>(&client))))
    {
        LOG_ERROR("Spectrum: could not open default render endpoint");
        cleanup();
        return;
    }

    if (FAILED(client->GetMixFormat(&format)) || !format)
    {
        LOG_ERROR("Spectrum: GetMixFormat failed");
        cleanup();
        return;
    }

    const int sampleRate = static_cast<int>(format->nSamplesPerSec);
    const int channels = format->nChannels;
    const bool isFloat = format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                         (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE && format->cbSize >= 22 &&
                          reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(format)->SubFormat.Data1 ==
                              static_cast<DWORD>(WAVE_FORMAT_IEEE_FLOAT));

    // Shared-mode loopback: buffer ~100ms, poll every ~10ms.
    if (FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK,
                                  1000000, 0, format, nullptr)) ||
        FAILED(client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void **>(&capture))))
    {
        LOG_ERROR("Spectrum: loopback Initialize failed");
        cleanup();
        return;
    }
    if (FAILED(client->Start()))
    {
        LOG_ERROR("Spectrum: capture Start failed");
        cleanup();
        return;
    }

    LOG_INFO("Spectrum: loopback capturing " << sampleRate << "Hz x" << channels
             << (isFloat ? " float" : " pcm"));

    std::vector<float> mono;
    mono.reserve(kFftSize * 2);
    std::vector<float> windowBuf(kFftSize);
    std::array<float, kBandCount> bands{};

    while (!m_stopRequested.load())
    {
        UINT32 packetFrames = 0;
        if (FAILED(capture->GetNextPacketSize(&packetFrames)))
            break;
        if (packetFrames == 0)
        {
            Sleep(5);
            continue;
        }

        BYTE *data = nullptr;
        UINT32 frames = 0;
        DWORD flags = 0;
        if (FAILED(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr)))
            break;

        if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT) && data)
        {
            if (isFloat)
            {
                const float *f = reinterpret_cast<const float *>(data);
                for (UINT32 i = 0; i < frames; ++i)
                {
                    float s = 0.0f;
                    for (int c = 0; c < channels; ++c)
                        s += f[i * channels + c];
                    mono.push_back(s / channels);
                }
            }
            else
            {
                const qint16 *p = reinterpret_cast<const qint16 *>(data);
                for (UINT32 i = 0; i < frames; ++i)
                {
                    float s = 0.0f;
                    for (int c = 0; c < channels; ++c)
                        s += p[i * channels + c] / 32768.0f;
                    mono.push_back(s / channels);
                }
            }
        }
        capture->ReleaseBuffer(frames);

        // Process the newest full window each iteration (~23Hz at 48kHz
        // with typical ~210-frame packets); drop older samples to stay live.
        while (mono.size() >= static_cast<size_t>(kFftSize))
        {
            const size_t start = mono.size() - kFftSize;
            std::copy(mono.begin() + start, mono.end(), windowBuf.begin());
            computeBands(windowBuf, sampleRate, bands);
            publish(bands);
            mono.clear();
        }
    }

    cleanup();
    if (m_running.load())
        LOG_INFO("Spectrum: capture loop ended");
#else
    LOG_INFO("Spectrum: loopback capture not implemented on this platform");
#endif
}
