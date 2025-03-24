#ifndef WINDOW_TRACKER_H
#define WINDOW_TRACKER_H

#include <string>
#include <functional>
#include <ctime>

// A struct that contains all window information we want to track
struct WindowInfo {
    std::string windowName;      // Title of the window
    std::string processName;     // Name of the process that created the window
    std::string windowRectangle; // Rectangle coordinates in format "x1.y1-x2.y2-x3.y3-x4.y4"
    time_t time;                 // Timestamp when the switch occurred (Unix timestamp)
    
    // Constructor for easy initialization
    WindowInfo(const std::string& wName = "", 
               const std::string& pName = "", 
               const std::string& wRect = "", 
               time_t t = 0) 
        : windowName(wName), processName(pName), windowRectangle(wRect), time(t) {}
};

// Callback function type for window change notifications
using WindowChangeCallback = std::function<void(const WindowInfo&)>;

// Abstract base class for window tracking functionality
class WindowTracker {
public:
    // Constructor that takes a callback function
    WindowTracker(WindowChangeCallback callback) : m_callback(callback) {}
    
    // Virtual destructor for proper cleanup in derived classes
    virtual ~WindowTracker() {}
    
    // Start tracking window changes
    virtual bool startTracking() = 0;
    
    // Stop tracking window changes
    virtual void stopTracking() = 0;
    
    // Get the current active window info (non-blocking)
    virtual WindowInfo getCurrentWindowInfo() = 0;

protected:
    // Callback to notify when window changes
    WindowChangeCallback m_callback;
    
    // Utility method to notify about window changes
    void notifyWindowChange(const WindowInfo& info) {
        if (m_callback) {
            m_callback(info);
        }
    }
};

#endif // WINDOW_TRACKER_H