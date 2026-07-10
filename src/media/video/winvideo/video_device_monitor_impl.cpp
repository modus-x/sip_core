/*
 *  Copyright (C) 2004-2022 Savoir-faire Linux Inc.
 *
 *  Author: Edric Milaret <edric.ladent-milaret@savoirfairelinux.com>
 *  Author: Andreas Traczyk <andreas.traczyk@savoirfairelinux.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA.
 */

#include "../video_device_monitor.h"
#include "logger.h"
#include "noncopyable.h"
#include "string_utils.h"

#include <dshow.h>
#include <dbt.h>
#include <SetupAPI.h>

#include <algorithm>
#include <condition_variable>
#include <string>
#include <thread>
#include <vector>
#include <cctype>
#include <mutex>

namespace sip_core {
namespace video {

constexpr GUID guidCamera
    = {0xe5323777, 0xf976, 0x4f5b, 0x9b, 0x55, 0xb9, 0x46, 0x99, 0xc4, 0x6e, 0x44};

class VideoDeviceMonitorImpl
{
public:
    VideoDeviceMonitorImpl(VideoDeviceMonitor* monitor);
    ~VideoDeviceMonitorImpl();

    void start();

private:
    NON_COPYABLE(VideoDeviceMonitorImpl);

    VideoDeviceMonitor* monitor_;

    void run();
    void notifyInitialSnapshotApplied();
    void reconcileCurrentDevices();

    std::vector<std::string> enumerateVideoInputDevices();

    std::thread thread_;
    HWND hWnd_ {nullptr};
    std::mutex stateMutex_;
    std::condition_variable stateCv_;
    bool initialSnapshotApplied_ {false};
    static LRESULT CALLBACK WinProcCallback(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);
};

VideoDeviceMonitorImpl::VideoDeviceMonitorImpl(VideoDeviceMonitor* monitor)
    : monitor_(monitor)
    , thread_()
{}

void
VideoDeviceMonitorImpl::start()
{
    thread_ = std::thread(&VideoDeviceMonitorImpl::run, this);

    std::unique_lock<std::mutex> lk(stateMutex_);
    stateCv_.wait(lk, [this] { return initialSnapshotApplied_; });
}

VideoDeviceMonitorImpl::~VideoDeviceMonitorImpl()
{
    if (hWnd_)
        PostMessage(hWnd_, WM_CLOSE, 0, 0);
    if (thread_.joinable())
        thread_.join();
}

void
VideoDeviceMonitorImpl::notifyInitialSnapshotApplied()
{
    {
        std::lock_guard<std::mutex> lk(stateMutex_);
        initialSnapshotApplied_ = true;
    }
    stateCv_.notify_all();
}

bool
registerDeviceInterfaceToHwnd(HWND hWnd, HDEVNOTIFY* hDeviceNotify)
{
    // Use a guid for cameras specifically in order to not get spammed
    // with device messages.
    // These are pertinent GUIDs for media devices:
    //
    // usb interfaces   { 0xa5dcbf10l, 0x6530, 0x11d2, 0x90, 0x1f, 0x00, 0xc0, 0x4f, 0xb9, 0x51,
    // 0xed }; image devices    { 0x6bdd1fc6,  0x810f, 0x11d0, 0xbe, 0xc7, 0x08, 0x00, 0x2b, 0xe2,
    // 0x09, 0x2f }; capture devices  { 0x65e8773d,  0x8f56, 0x11d0, 0xa3, 0xb9, 0x00, 0xa0, 0xc9,
    // 0x22, 0x31, 0x96 }; camera devices   { 0xe5323777,  0xf976, 0x4f5b, 0x9b, 0x55, 0xb9, 0x46,
    // 0x99, 0xc4, 0x6e, 0x44 }; audio devices    { 0x6994ad04,  0x93ef, 0x11d0, 0xa3, 0xcc, 0x00,
    // 0xa0, 0xc9, 0x22, 0x31, 0x96 };

    DEV_BROADCAST_DEVICEINTERFACE NotificationFilter;

    ZeroMemory(&NotificationFilter, sizeof(NotificationFilter));
    NotificationFilter.dbcc_size = sizeof(DEV_BROADCAST_DEVICEINTERFACE);
    NotificationFilter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    NotificationFilter.dbcc_classguid = guidCamera;

    *hDeviceNotify = RegisterDeviceNotification(hWnd,
                                                &NotificationFilter,
                                                DEVICE_NOTIFY_WINDOW_HANDLE);

    if (nullptr == *hDeviceNotify) {
        return false;
    }

    return true;
}

LRESULT CALLBACK
VideoDeviceMonitorImpl::WinProcCallback(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    LRESULT lRet = 1;
    static HDEVNOTIFY hDeviceNotify;
    auto* pThis = reinterpret_cast<VideoDeviceMonitorImpl*>(GetWindowLongPtr(hWnd, GWLP_USERDATA));

    switch (message) {
    case WM_CREATE: {
        // Store object pointer passed from CreateWindowEx.
        auto createParams = reinterpret_cast<CREATESTRUCT*>(lParam)->lpCreateParams;
        pThis = static_cast<VideoDeviceMonitorImpl*>(createParams);
        SetLastError(0);
        SetWindowLongPtr(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pThis));

        if (!registerDeviceInterfaceToHwnd(hWnd, &hDeviceNotify)) {
            SIP_CORE_ERR() << "Cannot register for device change notifications";
            SendMessage(hWnd, WM_DESTROY, 0, 0);
        }
    } break;

    case WM_DEVICECHANGE: {
        switch (wParam) {
        case DBT_DEVICEREMOVECOMPLETE:
        case DBT_DEVICEARRIVAL:
            if (pThis) {
                SIP_CORE_DBG() << "Camera device change detected, refreshing snapshot";
                pThis->reconcileCurrentDevices();
            }
            break;
        default:
            break;
        }
        break;
    } break;

    case WM_CLOSE:
        UnregisterDeviceNotification(hDeviceNotify);
        DestroyWindow(hWnd);
        break;

    case WM_DESTROY:
        PostQuitMessage(0);
        break;

    default:
        lRet = DefWindowProc(hWnd, message, wParam, lParam);
        break;
    }

    return lRet;
}

void
VideoDeviceMonitorImpl::run()
{
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool comInitialized = SUCCEEDED(hr);
    if (!comInitialized) {
        SIP_CORE_ERR() << "Cannot initialize COM";
        notifyInitialSnapshotApplied();
        return;
    }

    // Create a dummy window with the sole purpose to receive device change messages.
    static const wchar_t* className = L"Message";
    static const wchar_t* windowName = L"devicenotifications";
    WNDCLASSEX wx = {};
    wx.cbSize = sizeof(WNDCLASSEX);
    wx.lpfnWndProc = WinProcCallback;
    auto instance = reinterpret_cast<HINSTANCE>(GetModuleHandle(nullptr));
    wx.hInstance = instance;
    wx.lpszClassName = className;

    const ATOM classAtom = RegisterClassEx(&wx);
    if (!classAtom && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        SIP_CORE_ERR() << "Cannot register device monitor window class";
        notifyInitialSnapshotApplied();
        CoUninitialize();
        return;
    }

    // Pass this as lpParam so WinProcCallback can access members of VideoDeviceMonitorImpl.
    hWnd_ = CreateWindowEx(
        0, className, windowName, 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, this);
    if (!hWnd_) {
        SIP_CORE_ERR() << "Cannot create device monitor window";
        notifyInitialSnapshotApplied();
        CoUninitialize();
        return;
    }

    reconcileCurrentDevices();
    notifyInitialSnapshotApplied();

    // Run the message loop that will finish once a WM_DESTROY message
    // has been sent, allowing the thread to join.
    MSG msg;
    int retVal;
    while ((retVal = GetMessage(&msg, NULL, 0, 0)) != 0) {
        if (retVal != -1) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    CoUninitialize();
}

void
VideoDeviceMonitorImpl::reconcileCurrentDevices()
{
    monitor_->reconcileDevices(enumerateVideoInputDevices());
}

std::vector<std::string>
VideoDeviceMonitorImpl::enumerateVideoInputDevices()
{
    std::vector<std::string> deviceList;

    ICreateDevEnum* pDevEnum;
    HRESULT hr = CoCreateInstance(CLSID_SystemDeviceEnum,
                                  NULL,
                                  CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&pDevEnum));

    if (FAILED(hr)) {
        SIP_CORE_ERR() << "Can't enumerate webcams";
        return {};
    }

    IEnumMoniker* pEnum = nullptr;
    hr = pDevEnum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &pEnum, 0);
    if (hr == S_FALSE) {
        hr = VFW_E_NOT_FOUND;
    }
    pDevEnum->Release();
    if (FAILED(hr) || pEnum == nullptr) {
        SIP_CORE_ERR() << "No webcam found";
        return {};
    }

    IMoniker* pMoniker = NULL;
    while (pEnum->Next(1, &pMoniker, NULL) == S_OK) {
        IPropertyBag* pPropBag;
        HRESULT hr = pMoniker->BindToStorage(0, 0, IID_PPV_ARGS(&pPropBag));
        if (FAILED(hr)) {
            pMoniker->Release();
            continue;
        }

        IBindCtx* bind_ctx = NULL;
        LPOLESTR olestr = NULL;

        hr = CreateBindCtx(0, &bind_ctx);
        if (hr != S_OK) {
            pPropBag->Release();
            pMoniker->Release();
            continue;
        }
        hr = pMoniker->GetDisplayName(bind_ctx, NULL, &olestr);
        if (hr != S_OK) {
            bind_ctx->Release();
            pPropBag->Release();
            pMoniker->Release();
            continue;
        }
        auto unique_name = to_string(olestr);
        if (!unique_name.empty()) {
            // replace ':' with '_' since ffmpeg uses : to delineate between sources
            std::replace(unique_name.begin(), unique_name.end(), ':', '_');
            deviceList.push_back(std::string("video=") + unique_name);
        }

        CoTaskMemFree(olestr);
        bind_ctx->Release();
        pPropBag->Release();
        pMoniker->Release();
    }
    pEnum->Release();

    return deviceList;
}

VideoDeviceMonitor::VideoDeviceMonitor()
    : preferences_()
    , devices_()
    , monitorImpl_(new VideoDeviceMonitorImpl(this))
{
    monitorImpl_->start();
    addDevice(DEVICE_DESKTOP, {});
}

VideoDeviceMonitor::~VideoDeviceMonitor() {}

} // namespace video
} // namespace sip_core
