/*
 *  Copyright (C) 2004-2024 Savoir-faire Linux Inc.
 *
 *  Native Windows (WASAPI) audio backend. Shared-mode, event-driven capture and
 *  render, RDP-safe. Minimum target OS: Windows 8 (IAudioClient / IAudioClient2;
 *  no IAudioClient3 dependency).
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 3 of the License, or
 *  (at your option) any later version.
 */

#include "wasapilayer.h"
#include "wasapi_convert.h"
#include "manager.h"
#include "noncopyable.h"
#include "audio/ringbufferpool.h"
#include "audio/ringbuffer.h"
#include "libav_deps.h"
#include "client/ring_signal.h"
#include "logger.h"

#include <windows.h>
#include <mmreg.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ksmedia.h>
#include <avrt.h>
#include <dbt.h>
#include <wrl/client.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace sip_core {

namespace {

// 100-ns reference-time units per millisecond.
constexpr REFERENCE_TIME REFTIMES_PER_MS = 10000;
// Larger buffer over RDP to absorb the redirection channel's network jitter.
// This trades ~200 ms of mouth-to-ear latency for glitch-free playback over the
// remote session — a deliberate choice for requirement 1.3; revisit if latency
// complaints arise.
constexpr REFERENCE_TIME RDP_BUFFER_DURATION = 200 * REFTIMES_PER_MS;
constexpr REFERENCE_TIME LOCAL_MIN_BUFFER_DURATION = 30 * REFTIMES_PER_MS;
constexpr auto DEVICE_CHANGE_DEBOUNCE = std::chrono::milliseconds(250);

const GUID GUID_DEVINTERFACE_AUDIO_RENDER_LOCAL
    = {0xe6327cad, 0xdcec, 0x4949, {0xae, 0x8a, 0x99, 0x1e, 0x97, 0x6a, 0x79, 0xd2}};
const GUID GUID_DEVINTERFACE_AUDIO_CAPTURE_LOCAL
    = {0x2eef81be, 0x33fa, 0x4800, {0x96, 0x70, 0x1c, 0xd4, 0x74, 0x97, 0x2c, 0x3f}};

bool
isRemoteSession()
{
    return GetSystemMetrics(SM_REMOTESESSION) != 0;
}

// Balanced COM apartment initializer. Leaves the apartment untouched (and does
// not uninitialize) when the calling thread already picked a different mode.
struct ComScope
{
    HRESULT hr;
    ComScope() { hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED); }
    ~ComScope()
    {
        if (SUCCEEDED(hr))
            CoUninitialize();
    }
    NON_COPYABLE(ComScope);
};

std::string
utf16ToUtf8(const wchar_t* w)
{
    if (!w)
        return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1)
        return {};
    std::string out(static_cast<size_t>(len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), len, nullptr, nullptr);
    return out;
}

// Classify the shared-mode mix format. WASAPI shared mode is virtually always
// 32-bit float and always interleaved (no planar path needed); Pcm16/Pcm32 are
// defensive. Non-16/32-bit PCM (e.g. 24-bit) -> Unsupported -> silence.
wasapi::WaveSampleType
classifyFormat(const WAVEFORMATEX* wf)
{
    if (!wf)
        return wasapi::WaveSampleType::Unsupported;
    WORD tag = wf->wFormatTag;
    const GUID* sub = nullptr;
    if (tag == WAVE_FORMAT_EXTENSIBLE && wf->cbSize >= 22) {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wf);
        sub = &ext->SubFormat;
        tag = (*sub == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) ? WAVE_FORMAT_IEEE_FLOAT
                                                        : WAVE_FORMAT_PCM;
    }
    if (tag == WAVE_FORMAT_IEEE_FLOAT && wf->wBitsPerSample == 32)
        return wasapi::WaveSampleType::Float32;
    if (tag == WAVE_FORMAT_PCM && wf->wBitsPerSample == 16)
        return wasapi::WaveSampleType::Pcm16;
    if (tag == WAVE_FORMAT_PCM && wf->wBitsPerSample == 32)
        return wasapi::WaveSampleType::Pcm32;
    return wasapi::WaveSampleType::Unsupported;
}

//==================================================================================================
// Device-change monitor. Physical (un)plug is reported via WM_DEVICECHANGE on a
// message window; default-device (role) changes and endpoint enable/disable are
// reported ONLY via IMMNotificationClient. Both feed the same debounced recovery.
// Ported verbatim from the PortAudio backend (behavior must be preserved).
//==================================================================================================

void
scheduleWasapiDeviceRecovery(const std::shared_ptr<std::atomic_bool>& scheduled)
{
    if (!Manager::initialized)
        return;
    bool expected = false;
    if (!scheduled->compare_exchange_strong(expected, true))
        return;
    Manager::instance().scheduleTaskIn(
        [scheduled] {
            // Reopen the coalescing gate only AFTER the (slow) rebuild completes,
            // so device events arriving during recovery — common in an RDP
            // reconnect storm — stay coalesced instead of scheduling a second,
            // redundant recovery (an extra audio interruption).
            Manager::instance().recoverAudioDevices();
            scheduled->store(false);
        },
        DEVICE_CHANGE_DEBOUNCE);
}

bool
registerAudioDeviceInterfaceToHwnd(HWND hWnd, const GUID& guid, HDEVNOTIFY* hDeviceNotify)
{
    DEV_BROADCAST_DEVICEINTERFACE filter;
    ZeroMemory(&filter, sizeof(filter));
    filter.dbcc_size = sizeof(DEV_BROADCAST_DEVICEINTERFACE);
    filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    filter.dbcc_classguid = guid;
    *hDeviceNotify = RegisterDeviceNotification(hWnd, &filter, DEVICE_NOTIFY_WINDOW_HANDLE);
    return *hDeviceNotify != nullptr;
}

class DefaultDeviceListener : public IMMNotificationClient
{
public:
    explicit DefaultDeviceListener(std::function<void()>&& callback)
        : callback_(std::move(callback))
    {}

    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refCount_); }
    ULONG STDMETHODCALLTYPE Release() override
    {
        ULONG count = InterlockedDecrement(&refCount_);
        if (count == 0)
            delete this;
        return count;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv)
            return E_POINTER;
        if (riid == IID_IUnknown || riid == __uuidof(IMMNotificationClient)) {
            *ppv = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow, ERole role, LPCWSTR) override
    {
        if (role == eConsole || role == eCommunications) {
            SIP_CORE_DBG() << "Windows default audio device changed";
            if (callback_)
                callback_();
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override
    {
        SIP_CORE_DBG() << "Windows audio endpoint state changed";
        if (callback_)
            callback_();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override
    {
        return S_OK;
    }

private:
    NON_COPYABLE(DefaultDeviceListener);
    virtual ~DefaultDeviceListener() = default;

    std::function<void()> callback_;
    LONG refCount_ {1};
};

class WindowsAudioDeviceMonitor
{
public:
    explicit WindowsAudioDeviceMonitor(std::function<void()>&& callback)
        : callback_(std::move(callback))
    {}

    ~WindowsAudioDeviceMonitor()
    {
        if (hWnd_)
            PostMessageW(hWnd_, WM_CLOSE, 0, 0);
        if (thread_.joinable())
            thread_.join();
    }

    void start()
    {
        thread_ = std::thread(&WindowsAudioDeviceMonitor::run, this);
        std::unique_lock<std::mutex> lk(stateMutex_);
        stateCv_.wait(lk, [this] { return ready_; });
    }

private:
    NON_COPYABLE(WindowsAudioDeviceMonitor);

    void notifyReady()
    {
        {
            std::lock_guard<std::mutex> lk(stateMutex_);
            ready_ = true;
        }
        stateCv_.notify_all();
    }

    void notifyDeviceChange()
    {
        SIP_CORE_DBG() << "Windows audio device change detected";
        if (callback_)
            callback_();
    }

    void unregisterNotifications()
    {
        for (auto& notification : deviceNotifications_) {
            if (notification) {
                UnregisterDeviceNotification(notification);
                notification = nullptr;
            }
        }
    }

    static LRESULT CALLBACK WinProcCallback(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        auto* pThis = reinterpret_cast<WindowsAudioDeviceMonitor*>(
            GetWindowLongPtrW(hWnd, GWLP_USERDATA));

        switch (message) {
        case WM_CREATE: {
            auto createParams = reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams;
            pThis = static_cast<WindowsAudioDeviceMonitor*>(createParams);
            SetLastError(0);
            SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pThis));
            if (!registerAudioDeviceInterfaceToHwnd(hWnd,
                                                    GUID_DEVINTERFACE_AUDIO_CAPTURE_LOCAL,
                                                    &pThis->deviceNotifications_[0]))
                SIP_CORE_ERR() << "Cannot register for audio capture device notifications";
            if (!registerAudioDeviceInterfaceToHwnd(hWnd,
                                                    GUID_DEVINTERFACE_AUDIO_RENDER_LOCAL,
                                                    &pThis->deviceNotifications_[1]))
                SIP_CORE_ERR() << "Cannot register for audio render device notifications";
        } break;
        case WM_DEVICECHANGE:
            switch (wParam) {
            case DBT_DEVICEARRIVAL:
            case DBT_DEVICEREMOVECOMPLETE:
            case DBT_DEVNODES_CHANGED:
                if (pThis)
                    pThis->notifyDeviceChange();
                break;
            default:
                break;
            }
            break;
        case WM_CLOSE:
            if (pThis)
                pThis->unregisterNotifications();
            DestroyWindow(hWnd);
            break;
        case WM_DESTROY:
            PostQuitMessage(0);
            break;
        default:
            return DefWindowProcW(hWnd, message, wParam, lParam);
        }
        return 0;
    }

    void run()
    {
        const HRESULT comInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ComPtr<IMMDeviceEnumerator> enumerator;
        DefaultDeviceListener* defaultListener {nullptr};
        if (SUCCEEDED(comInit)) {
            if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator),
                                           nullptr,
                                           CLSCTX_ALL,
                                           IID_PPV_ARGS(&enumerator)))) {
                defaultListener = new DefaultDeviceListener([this] { notifyDeviceChange(); });
                if (FAILED(enumerator->RegisterEndpointNotificationCallback(defaultListener))) {
                    SIP_CORE_ERR() << "Cannot register for default audio device notifications";
                    defaultListener->Release();
                    defaultListener = nullptr;
                    enumerator.Reset();
                }
            } else {
                SIP_CORE_ERR() << "Cannot create MMDeviceEnumerator for audio notifications";
            }
        } else {
            SIP_CORE_ERR() << "Cannot initialize COM for audio device notifications";
        }

        auto comCleanup = [&] {
            if (enumerator && defaultListener)
                enumerator->UnregisterEndpointNotificationCallback(defaultListener);
            if (defaultListener)
                defaultListener->Release();
            enumerator.Reset();
            if (SUCCEEDED(comInit))
                CoUninitialize();
        };

        static const wchar_t* className = L"SipCoreAudioDeviceNotifications";
        WNDCLASSEXW wx = {};
        wx.cbSize = sizeof(WNDCLASSEXW);
        wx.lpfnWndProc = WinProcCallback;
        auto instance = reinterpret_cast<HINSTANCE>(GetModuleHandleW(nullptr));
        wx.hInstance = instance;
        wx.lpszClassName = className;

        const ATOM classAtom = RegisterClassExW(&wx);
        if (!classAtom && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            SIP_CORE_ERR() << "Cannot register audio device monitor window class";
            notifyReady();
            comCleanup();
            return;
        }

        hWnd_ = CreateWindowExW(0, className, L"sip-core-audio-device-notifications", 0, 0, 0, 0,
                                0, HWND_MESSAGE, nullptr, instance, this);
        if (!hWnd_) {
            SIP_CORE_ERR() << "Cannot create audio device monitor window";
            notifyReady();
            comCleanup();
            return;
        }

        notifyReady();

        MSG msg;
        int retVal;
        while ((retVal = GetMessageW(&msg, nullptr, 0, 0)) != 0) {
            if (retVal != -1) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
        comCleanup();
    }

    std::function<void()> callback_;
    std::thread thread_;
    HWND hWnd_ {nullptr};
    std::array<HDEVNOTIFY, 2> deviceNotifications_ {nullptr, nullptr};
    std::mutex stateMutex_;
    std::condition_variable stateCv_;
    bool ready_ {false};
};

//==================================================================================================
// One shared-mode, event-driven WASAPI stream (capture OR render), on its own
// MMCSS-boosted thread. Decoupled from AudioLayer internals through the two
// std::function seams (pull/push), which the layer fills in with getPlayback /
// putRecorded access.
//==================================================================================================

class WasapiStream
{
public:
    // Render: pull(dstS16, frames) fills interleaved S16 for `frames`*channels
    //         samples; returns false when there is nothing to play (-> silence).
    using PullFn = std::function<bool(int16_t* dst, unsigned frames)>;
    // Capture: push(srcS16OrNull, frames); null -> feed silence.
    using PushFn = std::function<void(const int16_t* src, unsigned frames)>;

    WasapiStream() = default;
    ~WasapiStream() { stop(); }
    NON_COPYABLE(WasapiStream);

    unsigned channels() const { return channels_; }
    unsigned sampleRate() const { return sampleRate_; }

    // Open + start. Returns false on failure (caller emits DeviceOpenError).
    bool start(IMMDevice* device,
               bool render,
               bool rdp,
               std::function<void()> onInvalidated,
               PullFn pull,
               PushFn push)
    {
        // Idempotent: a joinable thread_ means this stream is already running.
        // WasapiLayer maps RINGTONE and PLAYBACK onto one render stream, so a
        // second startStream can reach here; move-assigning over a joinable
        // std::thread (thread_ = std::thread(...)) calls std::terminate(). Treat
        // a start on an already-running stream as a no-op success.
        if (thread_.joinable())
            return true;

        render_ = render;
        onInvalidated_ = std::move(onInvalidated);
        pull_ = std::move(pull);
        push_ = std::move(push);

        if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client_))) {
            SIP_CORE_ERR() << "WASAPI: IAudioClient Activate failed";
            return false;
        }

        WAVEFORMATEX* mix = nullptr;
        if (FAILED(client_->GetMixFormat(&mix)) || !mix) {
            SIP_CORE_ERR() << "WASAPI: GetMixFormat failed";
            return false;
        }
        waveType_ = classifyFormat(mix);
        channels_ = mix->nChannels;
        sampleRate_ = mix->nSamplesPerSec;
        if (waveType_ == wasapi::WaveSampleType::Unsupported) {
            // Shared-mode GetMixFormat is effectively always 32-bit float, so
            // this is near-unreachable; surface it rather than fail silently.
            SIP_CORE_WARN() << "WASAPI: unsupported mix format (bits="
                            << mix->wBitsPerSample << ", tag=" << mix->wFormatTag
                            << "), audio will be silent";
            emitSignal<libsip_core::ConfigurationSignal::DeviceOpenError>(
                "Unsupported WASAPI shared-mode format", render_);
        }

        // Win8+: hint the engine (and the RDP stack) that this is a VoIP stream.
        ComPtr<IAudioClient2> client2;
        if (SUCCEEDED(client_.As(&client2)) && client2) {
            AudioClientProperties props {};
            props.cbSize = sizeof(props);
            props.bIsOffload = FALSE;
            props.eCategory = AudioCategory_Communications;
            client2->SetClientProperties(&props); // best-effort
        }

        REFERENCE_TIME defPeriod = 0, minPeriod = 0;
        client_->GetDevicePeriod(&defPeriod, &minPeriod);
        REFERENCE_TIME bufDuration = rdp ? RDP_BUFFER_DURATION
                                         : std::max<REFERENCE_TIME>(3 * defPeriod,
                                                                    LOCAL_MIN_BUFFER_DURATION);

        HRESULT hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                         AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                         bufDuration,
                                         0, // hnsPeriodicity must be 0 in shared mode
                                         mix,
                                         nullptr);
        CoTaskMemFree(mix);
        if (FAILED(hr)) {
            SIP_CORE_ERR() << "WASAPI: IAudioClient Initialize failed (0x" << std::hex << hr << ")";
            return false;
        }

        if (FAILED(client_->GetBufferSize(&bufferFrameCount_))) {
            SIP_CORE_ERR() << "WASAPI: GetBufferSize failed";
            return false;
        }

        hEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!hEvent_ || FAILED(client_->SetEventHandle(hEvent_))) {
            SIP_CORE_ERR() << "WASAPI: SetEventHandle failed";
            return false;
        }

        if (render_) {
            if (FAILED(client_->GetService(IID_PPV_ARGS(&renderClient_))))
                return false;
            // Pre-roll one buffer of silence so the first WaitForSingleObject
            // fires and the stream never starts starved.
            BYTE* data = nullptr;
            if (SUCCEEDED(renderClient_->GetBuffer(bufferFrameCount_, &data)))
                renderClient_->ReleaseBuffer(bufferFrameCount_, AUDCLNT_BUFFERFLAGS_SILENT);
        } else {
            if (FAILED(client_->GetService(IID_PPV_ARGS(&captureClient_))))
                return false;
        }

        // The watchdog MUST be shorter than the buffer duration: on a lost event
        // stream (RDP stall) the loop falls through to poll-and-refill before the
        // buffer drains, so there is no underrun. Scale it to ~half the actual
        // buffer (e.g. ~100 ms for the 200 ms RDP buffer, ~15 ms locally).
        const DWORD bufMs = static_cast<DWORD>(static_cast<unsigned long long>(bufferFrameCount_)
                                               * 1000ULL / (sampleRate_ ? sampleRate_ : 48000));
        watchdogMs_ = std::max<DWORD>(5, bufMs / 2);
        if (FAILED(client_->Start())) {
            SIP_CORE_ERR() << "WASAPI: IAudioClient Start failed";
            return false;
        }
        stop_ = false;
        thread_ = std::thread(&WasapiStream::run, this);
        SIP_CORE_INFO() << "WASAPI: started " << (render_ ? "render" : "capture") << " stream {"
                        << sampleRate_ << " Hz, " << channels_ << " ch, buf "
                        << (bufDuration / REFTIMES_PER_MS) << " ms}";
        return true;
    }

    void stop()
    {
        stop_ = true;
        if (hEvent_)
            SetEvent(hEvent_);
        if (thread_.joinable())
            thread_.join();
        if (client_)
            client_->Stop();
        renderClient_.Reset();
        captureClient_.Reset();
        client_.Reset();
        if (hEvent_) {
            CloseHandle(hEvent_);
            hEvent_ = nullptr;
        }
    }

private:
    void run()
    {
        ComScope com;
        DWORD taskIndex = 0;
        HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
        if (!mmcss)
            mmcss = AvSetMmThreadCharacteristicsW(L"Audio", &taskIndex);

        std::vector<int16_t> scratch(static_cast<size_t>(bufferFrameCount_) * channels_);

        while (!stop_) {
            DWORD w = WaitForSingleObject(hEvent_, watchdogMs_);
            if (stop_)
                break;
            (void) w; // process on both signal and watchdog timeout
            HRESULT hr = render_ ? serviceRender(scratch) : serviceCapture(scratch);
            if (hr == AUDCLNT_E_DEVICE_INVALIDATED || hr == AUDCLNT_E_RESOURCES_INVALIDATED) {
                SIP_CORE_WARN() << "WASAPI: device invalidated, triggering recovery";
                if (onInvalidated_)
                    onInvalidated_();
                break;
            }
        }

        if (mmcss)
            AvRevertMmThreadCharacteristics(mmcss);
    }

    HRESULT serviceRender(std::vector<int16_t>& scratch)
    {
        // stop()/device-recovery can Reset() the COM clients while this real-time
        // worker is still between run()'s stop_ check and here — observed null
        // client_ deref AV in serviceRender->GetCurrentPadding (dump
        // communicator.exe.23276). Bail to silence rather than dereference a reset
        // ComPtr; run()'s loop re-checks stop_ and exits promptly.
        if (!client_ || !renderClient_)
            return S_OK;
        UINT32 padding = 0;
        HRESULT hr = client_->GetCurrentPadding(&padding);
        if (FAILED(hr))
            return hr;
        // A conformant driver never reports padding > buffer size; guard anyway
        // so a misbehaving virtual/RDP endpoint can't wrap the unsigned subtract
        // into a huge GetBuffer request.
        UINT32 avail = (padding < bufferFrameCount_) ? (bufferFrameCount_ - padding) : 0;
        if (avail == 0)
            return S_OK;

        BYTE* data = nullptr;
        hr = renderClient_->GetBuffer(avail, &data);
        if (FAILED(hr))
            return hr;

        bool hasAudio = pull_ && pull_(scratch.data(), avail);
        if (hasAudio) {
            wasapi::convertS16ToDevice(scratch.data(), data, avail, channels_, waveType_);
            return renderClient_->ReleaseBuffer(avail, 0);
        }
        return renderClient_->ReleaseBuffer(avail, AUDCLNT_BUFFERFLAGS_SILENT);
    }

    HRESULT serviceCapture(std::vector<int16_t>& scratch)
    {
        // Same teardown race as serviceRender: the capture client can be reset
        // under this worker thread. Guard before dereferencing it.
        if (!client_ || !captureClient_)
            return S_OK;
        UINT32 packet = 0;
        HRESULT hr = captureClient_->GetNextPacketSize(&packet);
        if (FAILED(hr))
            return hr;

        // Bound the drain loop so a misbehaving virtual/RDP endpoint that keeps
        // reporting a non-zero packet size can't spin this real-time thread.
        int guard = 0;
        while (packet != 0 && ++guard <= 512) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            hr = captureClient_->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
            if (hr == AUDCLNT_S_BUFFER_EMPTY)
                break;
            if (FAILED(hr))
                return hr;

            if (frames > 0 && push_) {
                if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                    push_(nullptr, frames);
                } else {
                    // Reuse the preallocated scratch (frames <= bufferFrameCount_)
                    // so there is no heap allocation on the MMCSS real-time thread.
                    wasapi::convertDeviceToS16(data, scratch.data(), frames, channels_, waveType_);
                    push_(scratch.data(), frames);
                }
            }
            captureClient_->ReleaseBuffer(frames);
            hr = captureClient_->GetNextPacketSize(&packet);
            if (FAILED(hr))
                return hr;
        }
        return S_OK;
    }

    bool render_ {true};
    ComPtr<IAudioClient> client_;
    ComPtr<IAudioRenderClient> renderClient_;
    ComPtr<IAudioCaptureClient> captureClient_;
    HANDLE hEvent_ {nullptr};
    UINT32 bufferFrameCount_ {0};
    unsigned channels_ {2};
    unsigned sampleRate_ {48000};
    wasapi::WaveSampleType waveType_ {wasapi::WaveSampleType::Float32};
    DWORD watchdogMs_ {100};

    std::thread thread_;
    std::atomic_bool stop_ {true};
    std::function<void()> onInvalidated_;
    PullFn pull_;
    PushFn push_;
};

} // namespace

//==================================================================================================
// WasapiLayer::Impl — nested so it can reach AudioLayer's protected members
// (getPlayback / putRecorded / hardwareFormatAvailable / ...).
//==================================================================================================

struct WasapiLayer::Impl
{
    Impl(WasapiLayer& parent, const AudioPreference& pref);
    ~Impl();

    NON_COPYABLE(Impl);

    // COM / Core Audio helpers. Every public entry point that touches WASAPI
    // wraps its work in a ComScope and creates a short-lived enumerator, so the
    // behavior never depends on whether the host thread happens to be
    // COM-initialized (PortAudio cached device info and needed no COM at getter
    // time). The device-monitor thread holds the process MTA alive for our whole
    // lifetime, so these per-call enumerators and any opened IAudioClients stay
    // valid across the capture/render worker threads.
    static ComPtr<IMMDeviceEnumerator> makeEnumerator();
    static std::string friendlyName(IMMDevice* dev);
    static std::vector<std::string> rawNames(IMMDeviceEnumerator* e, EDataFlow flow);
    static std::string defaultName(IMMDeviceEnumerator* e, EDataFlow flow);
    static ComPtr<IMMDevice> firstActive(IMMDeviceEnumerator* e, EDataFlow flow);
    // Resolve a stored preference (friendly name; empty = default) to an IMMDevice,
    // with PortAudio's fallback chain: named -> eCommunications -> eConsole -> first active.
    static ComPtr<IMMDevice> resolveDevice(IMMDeviceEnumerator* e,
                                           EDataFlow flow,
                                           const std::string& pref);

    std::vector<std::string> deviceList(AudioDeviceType type) const;
    bool prefResolved(EDataFlow flow, const std::string& pref) const;

    bool startCapture(WasapiLayer& parent);
    bool startRender(WasapiLayer& parent);

    WasapiLayer& parent_;
    bool rdp_ {false};

    std::string deviceRecord_;
    std::string devicePlayback_;
    std::string deviceRingtone_;

    WasapiStream capture_;
    WasapiStream render_;
    // RINGTONE and PLAYBACK are distinct Manager stream-users that share the one
    // render_ stream; this counts how many are active so render_ is opened on the
    // first and torn down only on the last. Guarded by WasapiLayer::mutex_.
    int renderUsers_ {0};

    std::shared_ptr<std::atomic_bool> recoveryScheduled_ {
        std::make_shared<std::atomic_bool>(false)};
    std::unique_ptr<WindowsAudioDeviceMonitor> monitor_;
};

WasapiLayer::Impl::Impl(WasapiLayer& parent, const AudioPreference& pref)
    : parent_(parent)
    , deviceRecord_(pref.getPortAudioDeviceRecord())
    , devicePlayback_(pref.getPortAudioDevicePlayback())
    , deviceRingtone_(pref.getPortAudioDeviceRingtone())
{
    rdp_ = isRemoteSession();
    SIP_CORE_INFO() << "WasapiLayer: prefs {rec=" << deviceRecord_ << ", play=" << devicePlayback_
                    << ", ring=" << deviceRingtone_ << "}, RDP=" << rdp_;

    // The monitor thread holds a process-lifetime MTA CoInitializeEx, which
    // keeps every Core Audio object we create valid across our worker threads.
    monitor_ = std::make_unique<WindowsAudioDeviceMonitor>(
        [scheduled = recoveryScheduled_] { scheduleWasapiDeviceRecovery(scheduled); });
    monitor_->start();
}

WasapiLayer::Impl::~Impl()
{
    monitor_.reset();
    capture_.stop();
    render_.stop();
}

ComPtr<IMMDeviceEnumerator>
WasapiLayer::Impl::makeEnumerator()
{
    ComPtr<IMMDeviceEnumerator> e;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator),
                                nullptr,
                                CLSCTX_ALL,
                                IID_PPV_ARGS(&e))))
        SIP_CORE_ERR() << "WasapiLayer: failed to create MMDeviceEnumerator";
    return e;
}

std::string
WasapiLayer::Impl::friendlyName(IMMDevice* dev)
{
    if (!dev)
        return {};
    ComPtr<IPropertyStore> props;
    if (FAILED(dev->OpenPropertyStore(STGM_READ, &props)))
        return {};
    PROPVARIANT v;
    PropVariantInit(&v);
    std::string name;
    if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR)
        name = utf16ToUtf8(v.pwszVal);
    PropVariantClear(&v);
    return name;
}

std::vector<std::string>
WasapiLayer::Impl::rawNames(IMMDeviceEnumerator* e, EDataFlow flow)
{
    std::vector<std::string> names;
    ComPtr<IMMDeviceCollection> collection;
    if (!e || FAILED(e->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &collection)))
        return names;
    UINT count = 0;
    collection->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> dev;
        if (FAILED(collection->Item(i, &dev)))
            continue;
        auto name = friendlyName(dev.Get());
        if (!name.empty())
            names.push_back(std::move(name));
    }
    return names;
}

ComPtr<IMMDevice>
WasapiLayer::Impl::firstActive(IMMDeviceEnumerator* e, EDataFlow flow)
{
    ComPtr<IMMDevice> dev;
    ComPtr<IMMDeviceCollection> collection;
    if (e && SUCCEEDED(e->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &collection))) {
        UINT count = 0;
        collection->GetCount(&count);
        if (count > 0)
            collection->Item(0, &dev);
    }
    return dev;
}

std::string
WasapiLayer::Impl::defaultName(IMMDeviceEnumerator* e, EDataFlow flow)
{
    if (!e)
        return {};
    // VoIP tracks the eCommunications role; fall back to eConsole, then to the
    // first active endpoint — matching the label to what resolveDevice opens.
    ComPtr<IMMDevice> dev;
    if (FAILED(e->GetDefaultAudioEndpoint(flow, eCommunications, &dev)) || !dev)
        if (FAILED(e->GetDefaultAudioEndpoint(flow, eConsole, &dev)) || !dev)
            dev = firstActive(e, flow);
    return friendlyName(dev.Get());
}

std::vector<std::string>
WasapiLayer::Impl::deviceList(AudioDeviceType type) const
{
    const EDataFlow flow = (type == AudioDeviceType::CAPTURE) ? eCapture : eRender;
    ComScope com;
    auto e = makeEnumerator();
    return wasapi::buildDeviceList(rawNames(e.Get(), flow), defaultName(e.Get(), flow));
}

bool
WasapiLayer::Impl::prefResolved(EDataFlow flow, const std::string& pref) const
{
    if (pref.empty())
        return true;
    ComScope com;
    auto e = makeEnumerator();
    for (const auto& n : rawNames(e.Get(), flow))
        if (n == pref)
            return true;
    return false;
}

ComPtr<IMMDevice>
WasapiLayer::Impl::resolveDevice(IMMDeviceEnumerator* e, EDataFlow flow, const std::string& pref)
{
    ComPtr<IMMDevice> result;
    if (!e)
        return result;

    if (!pref.empty()) {
        ComPtr<IMMDeviceCollection> collection;
        if (SUCCEEDED(e->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &collection))) {
            UINT count = 0;
            collection->GetCount(&count);
            for (UINT i = 0; i < count; ++i) {
                ComPtr<IMMDevice> dev;
                if (FAILED(collection->Item(i, &dev)))
                    continue;
                if (friendlyName(dev.Get()) == pref)
                    return dev;
            }
        }
        SIP_CORE_WARN() << "WasapiLayer: configured device '" << pref
                        << "' not found, falling back to default";
    }

    // Empty preference or stale name: eCommunications -> eConsole -> first active,
    // so a machine with active endpoints but no default role still opens audio.
    if (FAILED(e->GetDefaultAudioEndpoint(flow, eCommunications, &result)) || !result)
        if (FAILED(e->GetDefaultAudioEndpoint(flow, eConsole, &result)) || !result)
            result = firstActive(e, flow);
    return result;
}

bool
WasapiLayer::Impl::startCapture(WasapiLayer& parent)
{
    // COM stays initialized for this whole scope: resolveDevice + the stream's
    // Activate/Initialize all run here on the caller thread.
    ComScope com;
    auto e = makeEnumerator();
    auto device = resolveDevice(e.Get(), eCapture, deviceRecord_);
    if (!device) {
        SIP_CORE_ERR() << "WasapiLayer: no capture device";
        emitSignal<libsip_core::ConfigurationSignal::DeviceOpenError>("No valid input device",
                                                                      false);
        return false;
    }
    auto push = [&parent](const int16_t* src, unsigned frames) {
        AudioFormat fmt {parent.pimpl_->capture_.sampleRate(),
                         parent.pimpl_->capture_.channels(),
                         AV_SAMPLE_FMT_S16};
        auto frame = std::make_shared<AudioFrame>(fmt, frames);
        if (!src || parent.isCaptureMuted_)
            libav_utils::fillWithSilence(frame->pointer());
        else
            std::memcpy(frame->pointer()->extended_data[0],
                        src,
                        static_cast<size_t>(frames) * fmt.nb_channels * sizeof(int16_t));
        parent.putRecorded(std::move(frame));
    };
    auto onInvalidated = [scheduled = recoveryScheduled_] {
        scheduleWasapiDeviceRecovery(scheduled);
    };
    if (!capture_.start(device.Get(), false, rdp_, onInvalidated, nullptr, push))
        return false;

    // Publish the capture format up to the mixer (S16 at the device mix rate/ch;
    // the ring buffer resamples to the internal format).
    parent.audioInputFormat_ = {capture_.sampleRate(), capture_.channels(), AV_SAMPLE_FMT_S16};
    parent.hardwareInputFormatAvailable(parent.audioInputFormat_);
    parent.recordChanged(true);
    return true;
}

bool
WasapiLayer::Impl::startRender(WasapiLayer& parent)
{
    ComScope com;
    auto e = makeEnumerator();
    auto device = resolveDevice(e.Get(), eRender, devicePlayback_);
    if (!device) {
        SIP_CORE_ERR() << "WasapiLayer: no playback device";
        emitSignal<libsip_core::ConfigurationSignal::DeviceOpenError>("No valid output device",
                                                                      true);
        return false;
    }
    auto pull = [&parent](int16_t* dst, unsigned frames) -> bool {
        AudioFormat fmt {parent.pimpl_->render_.sampleRate(),
                         parent.pimpl_->render_.channels(),
                         AV_SAMPLE_FMT_S16};
        auto toPlay = parent.getPlayback(fmt, frames);
        if (!toPlay)
            return false;
        // nb_samples is per-channel frames (not total samples); getToPlay resizes
        // it to exactly `frames`, so copyFrames == frames in the normal path.
        auto n = static_cast<unsigned>(toPlay->pointer()->nb_samples);
        unsigned copyFrames = std::min(n, frames);
        std::memcpy(dst,
                    toPlay->pointer()->extended_data[0],
                    static_cast<size_t>(copyFrames) * fmt.nb_channels * sizeof(int16_t));
        // Zero any tail if the pipeline under-delivered (defensive).
        if (copyFrames < frames)
            std::memset(dst + static_cast<size_t>(copyFrames) * fmt.nb_channels,
                        0,
                        static_cast<size_t>(frames - copyFrames) * fmt.nb_channels
                            * sizeof(int16_t));
        return true;
    };
    auto onInvalidated = [scheduled = recoveryScheduled_] {
        scheduleWasapiDeviceRecovery(scheduled);
    };
    if (!render_.start(device.Get(), true, rdp_, onInvalidated, pull, nullptr))
        return false;

    // Publish the playback format; store the negotiated internal format back.
    AudioFormat hw {render_.sampleRate(), render_.channels(), AV_SAMPLE_FMT_S16};
    parent.hardwareFormatAvailable(hw);
    parent.playbackChanged(true);
    return true;
}

//==================================================================================================
// WasapiLayer public interface.
//==================================================================================================

WasapiLayer::WasapiLayer(const AudioPreference& pref)
    : AudioLayer(pref)
    , pimpl_(std::make_unique<Impl>(*this, pref))
{
    setHasNativeAEC(false);
    setHasNativeNS(false);
}

WasapiLayer::~WasapiLayer()
{
    stopStream();
}

std::vector<std::string>
WasapiLayer::getCaptureDeviceList() const
{
    return pimpl_->deviceList(AudioDeviceType::CAPTURE);
}

std::vector<std::string>
WasapiLayer::getPlaybackDeviceList() const
{
    return pimpl_->deviceList(AudioDeviceType::PLAYBACK);
}

int
WasapiLayer::getAudioDeviceIndex(const std::string& name, AudioDeviceType type) const
{
    return wasapi::indexOfDevice(pimpl_->deviceList(type), name);
}

std::string
WasapiLayer::getAudioDeviceName(int, AudioDeviceType) const
{
    // Matches the previous backend: name resolution happens via index/list, not here.
    return {};
}

int
WasapiLayer::getIndexCapture() const
{
    return wasapi::resolvedIndex(pimpl_->deviceList(AudioDeviceType::CAPTURE), pimpl_->deviceRecord_);
}

int
WasapiLayer::getIndexPlayback() const
{
    return wasapi::resolvedIndex(pimpl_->deviceList(AudioDeviceType::PLAYBACK),
                                 pimpl_->devicePlayback_);
}

int
WasapiLayer::getIndexRingtone() const
{
    return wasapi::resolvedIndex(pimpl_->deviceList(AudioDeviceType::RINGTONE),
                                 pimpl_->deviceRingtone_);
}

bool
WasapiLayer::isPreferredDeviceResolved(AudioDeviceType type) const
{
    const std::string& pref = (type == AudioDeviceType::CAPTURE)
                                  ? pimpl_->deviceRecord_
                                  : (type == AudioDeviceType::PLAYBACK ? pimpl_->devicePlayback_
                                                                       : pimpl_->deviceRingtone_);
    const EDataFlow flow = (type == AudioDeviceType::CAPTURE) ? eCapture : eRender;
    return pimpl_->prefResolved(flow, pref);
}

void
WasapiLayer::startStream(AudioDeviceType stream)
{
    std::lock_guard<std::mutex> lock(mutex_);
    // Reference-count the shared render stream: open it for the first render user
    // (PLAYBACK or RINGTONE), and just bump the count for later ones — so a second
    // startStream never re-enters render_.start() on a live stream.
    auto startRenderRef = [this] {
        if (pimpl_->renderUsers_ > 0 || pimpl_->startRender(*this)) {
            pimpl_->renderUsers_++;
            status_.store(Status::Started);
        }
    };
    switch (stream) {
    case AudioDeviceType::ALL:
        pimpl_->startCapture(*this);
        startRenderRef();
        break;
    case AudioDeviceType::CAPTURE:
        pimpl_->startCapture(*this);
        break;
    case AudioDeviceType::PLAYBACK:
    case AudioDeviceType::RINGTONE:
        startRenderRef();
        break;
    }
}

void
WasapiLayer::stopStream(AudioDeviceType stream)
{
    // Lock hierarchy: this holds mutex_ across WasapiStream::stop()'s thread
    // join. The worker threads must therefore NEVER take mutex_ — they only
    // touch the ring buffers / audio processor via getPlayback/putRecorded
    // (which lock their own mutexes), so no join-vs-lock deadlock is possible.
    std::lock_guard<std::mutex> lock(mutex_);
    bool stoppedRender = false, stoppedCapture = false;
    // Only tear the shared render stream down when the LAST render user releases
    // it; otherwise the deferred stopStream(RINGTONE) fired ~750 ms after answer
    // would kill an active call's playback.
    auto stopRenderRef = [&] {
        if (pimpl_->renderUsers_ > 0 && --pimpl_->renderUsers_ == 0) {
            pimpl_->render_.stop();
            stoppedRender = true;
        }
    };
    switch (stream) {
    case AudioDeviceType::ALL:
        pimpl_->capture_.stop();
        pimpl_->renderUsers_ = 0;
        pimpl_->render_.stop();
        stoppedCapture = stoppedRender = true;
        break;
    case AudioDeviceType::CAPTURE:
        pimpl_->capture_.stop();
        stoppedCapture = true;
        break;
    case AudioDeviceType::PLAYBACK:
    case AudioDeviceType::RINGTONE:
        stopRenderRef();
        break;
    }
    if (stoppedRender && playbackStarted_) {
        playbackChanged(false);
        status_.store(Status::Idle);
    }
    if (stoppedCapture && recordStarted_)
        recordChanged(false);
    flushUrgent();
    flushMain();
}

void
WasapiLayer::updatePreference(AudioPreference& preference, int index, AudioDeviceType type)
{
    auto name = wasapi::nameForIndex(pimpl_->deviceList(type), index);
    switch (type) {
    case AudioDeviceType::PLAYBACK:
        preference.setPortAudioDevicePlayback(name);
        break;
    case AudioDeviceType::CAPTURE:
        preference.setPortAudioDeviceRecord(name);
        break;
    case AudioDeviceType::RINGTONE:
        preference.setPortAudioDeviceRingtone(name);
        break;
    default:
        break;
    }
}

} // namespace sip_core
