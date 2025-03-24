#ifndef WINDOW_TRACKER_WINDOWS_H
#define WINDOW_TRACKER_WINDOWS_H

#include "window_tracker.h"
#include <windows.h>
#include <thread>
#include <atomic>
#include <psapi.h>
#include <sstream>

class WindowTrackerWindows : public WindowTracker {
public:
    WindowTrackerWindows(WindowChangeCallback callback);
    virtual ~WindowTrackerWindows();
    
    // Implementation of abstract methods
    virtual bool startTracking() override;
    virtual void stopTracking() override;
    virtual WindowInfo getCurrentWindowInfo() override;

private:
    // Thread that polls for window changes
    std::thread m_trackingThread;
    
    // Flag to control the tracking thread
    std::atomic<bool> m_isTracking;
    
    // Current window handle
    HWND m_currentWindow;
    
    // Tracking thread main function
    void trackingThreadFunc();
    
    // Helper function to get window information from a handle
    WindowInfo getWindowInfoFromHandle(HWND hwnd);
    
    // Helper to get process name from a handle
    std::string getProcessNameFromWindow(HWND hwnd);
    
    // Helper to get window rectangle as a string
    std::string getWindowRectangleString(HWND hwnd);
};

#endif