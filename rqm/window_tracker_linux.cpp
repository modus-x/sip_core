

#include "window_tracker_linux.h"
#include <fstream>
#include <sstream>
#include <unistd.h>
#include <iostream>

WindowTrackerLinux::WindowTrackerLinux(WindowChangeCallback callback)
    : WindowTracker(callback),
      m_isTracking(false),
      m_currentWindow(0) {
    
    // Open connection to X server
    m_display = XOpenDisplay(NULL);
    if (!m_display) {
        std::cerr << "Cannot open display" << std::endl;
        return;
    }
    
    // Initialize atoms we'll need
    m_atomActiveWindow = XInternAtom(m_display, "_NET_ACTIVE_WINDOW", False);
    m_atomWmName = XInternAtom(m_display, "_NET_WM_NAME", False);
    m_atomUtf8String = XInternAtom(m_display, "UTF8_STRING", False);
    m_atomPid = XInternAtom(m_display, "_NET_WM_PID", False);
}

WindowTrackerLinux::~WindowTrackerLinux() {
    stopTracking();
    
    if (m_display) {
        XCloseDisplay(m_display);
        m_display = NULL;
    }
}

bool WindowTrackerLinux::startTracking() {
    if (!m_display || m_isTracking) {
        return false;
    }
    
    m_isTracking = true;
    m_trackingThread = std::thread(&WindowTrackerLinux::trackingThreadFunc, this);
    
    return true;
}

void WindowTrackerLinux::stopTracking() {
    if (!m_isTracking) {
        return;
    }
    
    m_isTracking = false;
    
    if (m_trackingThread.joinable()) {
        m_trackingThread.join();
    }
}

WindowInfo WindowTrackerLinux::getCurrentWindowInfo() {
    if (!m_display) {
        return WindowInfo();
    }
    
    Window activeWindow = 0;
    
    // Get the active window from root window property
    Atom actualType;
    int actualFormat;
    unsigned long itemCount, bytesAfter;
    unsigned char* propertyData = NULL;
    
    Window rootWindow = DefaultRootWindow(m_display);
    
    if (XGetWindowProperty(m_display, rootWindow, m_atomActiveWindow, 
                         0, 1, False, XA_WINDOW, &actualType, &actualFormat, 
                         &itemCount, &bytesAfter, &propertyData) == Success) {
        if (propertyData && actualType == XA_WINDOW && actualFormat == 32 && itemCount == 1) {
            activeWindow = *reinterpret_cast<Window*>(propertyData);
        }
        
        if (propertyData) {
            XFree(propertyData);
        }
    }
    
    if (activeWindow == 0) {
        return WindowInfo();
    }
    
    return getWindowInfoFromWindow(activeWindow);
}

void WindowTrackerLinux::trackingThreadFunc() {
    Window lastActiveWindow = 0;
    
    while (m_isTracking && m_display) {
        Window activeWindow = 0;
        
        // Get the active window from root window property
        Window rootWindow = DefaultRootWindow(m_display);
        unsigned char* propertyData = NULL;
        unsigned long itemCount;
        
        if (getWindowProperty(rootWindow, m_atomActiveWindow, XA_WINDOW, 
                            &propertyData, &itemCount)) {
            if (propertyData && itemCount == 1) {
                activeWindow = *reinterpret_cast<Window*>(propertyData);
            }
            
            if (propertyData) {
                XFree(propertyData);
            }
        }
        
        if (activeWindow != 0 && activeWindow != lastActiveWindow) {
            // Window has changed
            lastActiveWindow = activeWindow;
            m_currentWindow = activeWindow;
            
            // Get window info and notify through callback
            WindowInfo info = getWindowInfoFromWindow(activeWindow);
            notifyWindowChange(info);
        }
        
        // Sleep to avoid high CPU usage (adjust as needed)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

WindowInfo WindowTrackerLinux::getWindowInfoFromWindow(Window window) {
    WindowInfo info;
    
    // Get current time
    info.time = std::time(nullptr);
    
    // Get window name
    info.windowName = getWindowName(window);
    
    // Get process name
    info.processName = getProcessNameFromWindow(window);
    
    // Get window rectangle
    info.windowRectangle = getWindowRectangleString(window);
    
    return info;
}

bool WindowTrackerLinux::getWindowProperty(Window window, Atom property, Atom type, 
                                           unsigned char** result, unsigned long* resultLength) {
    Atom actualType;
    int actualFormat;
    unsigned long bytesAfter;
    
    if (XGetWindowProperty(m_display, window, property, 
                         0, ~0L, False, type, &actualType, &actualFormat, 
                         resultLength, &bytesAfter, result) != Success) {
        return false;
    }
    
    if (actualType != type || *resultLength == 0) {
        if (*result) {
            XFree(*result);
            *result = NULL;
        }
        return false;
    }
    
    return true;
}

std::string WindowTrackerLinux::getWindowName(Window window) {
    // Try _NET_WM_NAME first (UTF-8)
    unsigned char* propertyData = NULL;
    unsigned long itemCount;
    
    if (getWindowProperty(window, m_atomWmName, m_atomUtf8String, &propertyData, &itemCount)) {
        std::string name(reinterpret_cast<char*>(propertyData));
        XFree(propertyData);
        return name;
    }
    
    // Fall back to WM_NAME (older, non-UTF8)
    XTextProperty textProperty;
    if (XGetWMName(m_display, window, &textProperty) && textProperty.value) {
        std::string name(reinterpret_cast<char*>(textProperty.value));
        XFree(textProperty.value);
        return name;
    }
    
    return "";
}

std::string WindowTrackerLinux::getProcessNameFromWindow(Window window) {
    // Get PID of the window
    unsigned char* propertyData = NULL;
    unsigned long itemCount;
    pid_t pid = 0;
    
    if (getWindowProperty(window, m_atomPid, XA_CARDINAL, &propertyData, &itemCount)) {
        if (propertyData && itemCount == 1) {
            pid = *reinterpret_cast<pid_t*>(propertyData);
        }
        
        if (propertyData) {
            XFree(propertyData);
        }
    }
    
    if (pid == 0) {
        return "";
    }
    
    // Get process name from /proc filesystem
    std::string procCmdlinePath = "/proc/" + std::to_string(pid) + "/cmdline";
    std::ifstream cmdlineFile(procCmdlinePath);
    if (!cmdlineFile.is_open()) {
        return "";
    }
    
    std::string cmdline;
    std::getline(cmdlineFile, cmdline);
    
    // Extract executable name from command line
    // cmdline may have null bytes, find first one or use the whole string
    size_t nullPos = cmdline.find('\0');
    std::string command = (nullPos != std::string::npos) ? 
                          cmdline.substr(0, nullPos) : cmdline;
    
    // Extract basename (executable name without path)
    size_t lastSlash = command.find_last_of('/');
    if (lastSlash != std::string::npos) {
        return command.substr(lastSlash + 1);
    }
    
    return command;
}

std::string WindowTrackerLinux::getWindowRectangleString(Window window) {
    XWindowAttributes attrs;
    if (!XGetWindowAttributes(m_display, window, &attrs)) {
        return "";
    }
    
    int x = attrs.x;
    int y = attrs.y;
    int width = attrs.width;
    int height = attrs.height;
    
    // Convert to absolute screen coordinates if needed
    if (attrs.map_state == IsViewable) {
        Window child;
        XTranslateCoordinates(m_display, window, DefaultRootWindow(m_display),
                              0, 0, &x, &y, &child);
    }
    
    // Format as "x1.y1-x2.y2-x3.y3-x4.y4"
    std::stringstream ss;
    ss << x << "." << y << "-"
       << (x + width) << "." << y << "-"
       << (x + width) << "." << (y + height) << "-"
       << x << "." << (y + height);
    
    return ss.str();
}
