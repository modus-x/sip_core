#ifndef WINDOW_TRACKER_LINUX_H
#define WINDOW_TRACKER_LINUX_H

#include "window_tracker.h"
#include <thread>
#include <atomic>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>

class WindowTrackerLinux : public WindowTracker {
public:
    WindowTrackerLinux(WindowChangeCallback callback);
    virtual ~WindowTrackerLinux();
    
    // Implementation of abstract methods
    virtual bool startTracking() override;
    virtual void stopTracking() override;
    virtual WindowInfo getCurrentWindowInfo() override;

private:
    // X11 display connection
    Display* m_display;
    
    // Thread that polls for window changes
    std::thread m_trackingThread;
    
    // Flag to control the tracking thread
    std::atomic<bool> m_isTracking;
    
    // Current active window
    Window m_currentWindow;
    
    // Common atoms that we'll need
    Atom m_atomActiveWindow;
    Atom m_atomWmName;
    Atom m_atomUtf8String;
    Atom m_atomPid;
    
    // Tracking thread main function
    void trackingThreadFunc();
    
    // Helper function to get window information from a window ID
    WindowInfo getWindowInfoFromWindow(Window window);
    
    // Helper to get process name from a window ID
    std::string getProcessNameFromWindow(Window window);
    
    // Helper to get window rectangle as a string
    std::string getWindowRectangleString(Window window);
    
    // Helper to get window property
    bool getWindowProperty(Window window, Atom property, Atom type, 
                          unsigned char** result, unsigned long* resultLength);
    
    // Helper to get window name
    std::string getWindowName(Window window);
};

#endif // WINDOW_TRACKER_LINUX_H