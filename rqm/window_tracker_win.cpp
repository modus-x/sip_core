#include "window_tracker_win.h"

WindowTrackerWindows::WindowTrackerWindows(WindowChangeCallback callback)
    : WindowTracker(callback),
      m_isTracking(false),
      m_currentWindow(NULL) {
}

WindowTrackerWindows::~WindowTrackerWindows() {
    stopTracking();
}

bool WindowTrackerWindows::startTracking() {
    if (m_isTracking) {
        return true; // Already tracking
    }
    
    m_isTracking = true;
    m_trackingThread = std::thread(&WindowTrackerWindows::trackingThreadFunc, this);
    
    return true;
}

void WindowTrackerWindows::stopTracking() {
    if (!m_isTracking) {
        return; // Not tracking
    }
    
    m_isTracking = false;
    
    if (m_trackingThread.joinable()) {
        m_trackingThread.join();
    }
}

WindowInfo WindowTrackerWindows::getCurrentWindowInfo() {
    HWND foregroundWindow = GetForegroundWindow();
    if (foregroundWindow == NULL) {
        return WindowInfo();
    }
    
    return getWindowInfoFromHandle(foregroundWindow);
}

void WindowTrackerWindows::trackingThreadFunc() {
    HWND lastForegroundWindow = NULL;
    
    while (m_isTracking) {
        HWND currentForegroundWindow = GetForegroundWindow();
        
        if (currentForegroundWindow != NULL && currentForegroundWindow != lastForegroundWindow) {
            // Window has changed
            lastForegroundWindow = currentForegroundWindow;
            m_currentWindow = currentForegroundWindow;
            
            // Get window info and notify through callback
            WindowInfo info = getWindowInfoFromHandle(currentForegroundWindow);
            notifyWindowChange(info);
        }
        
        // Sleep to avoid high CPU usage (adjust as needed)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

WindowInfo WindowTrackerWindows::getWindowInfoFromHandle(HWND hwnd) {
    WindowInfo info;
    
    // Get current time
    info.time = std::time(nullptr);
    
    // Get window title
    char windowTitle[256] = { 0 };
    GetWindowTextA(hwnd, windowTitle, sizeof(windowTitle));
    info.windowName = windowTitle;
    
    // Get process name
    info.processName = getProcessNameFromWindow(hwnd);
    
    // Get window rectangle
    info.windowRectangle = getWindowRectangleString(hwnd);
    
    return info;
}

std::string WindowTrackerWindows::getProcessNameFromWindow(HWND hwnd) {
    DWORD processId;
    GetWindowThreadProcessId(hwnd, &processId);
    
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, processId);
    if (hProcess == NULL) {
        return "";
    }
    
    char processPath[MAX_PATH] = { 0 };
    if (GetModuleFileNameExA(hProcess, NULL, processPath, MAX_PATH) == 0) {
        CloseHandle(hProcess);
        return "";
    }
    
    CloseHandle(hProcess);
    
    // Extract just the filename from the full path
    std::string fullPath(processPath);
    size_t lastSlash = fullPath.find_last_of("\\/");
    if (lastSlash != std::string::npos) {
        return fullPath.substr(lastSlash + 1);
    }
    
    return fullPath;
}

std::string WindowTrackerWindows::getWindowRectangleString(HWND hwnd) {
    RECT rect;
    if (!GetWindowRect(hwnd, &rect)) {
        return "";
    }
    
    // Format as "x1.y1-x2.y2-x3.y3-x4.y4"
    std::stringstream ss;
    ss << rect.left << "." << rect.top << "-"
       << rect.right << "." << rect.top << "-"
       << rect.right << "." << rect.bottom << "-"
       << rect.left << "." << rect.bottom;
    
    return ss.str();
}