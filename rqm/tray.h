// TrayIcon.h
#ifndef TRAY_ICON_H
#define TRAY_ICON_H

#include <string>

// Abstract base class for system tray functionality
class TrayIcon {
public:
    // Constructor takes the icon title and optionally an icon file path
    TrayIcon(const std::string& title, const std::string& iconPath = "");
    
    // Virtual destructor for proper cleanup in derived classes
    virtual ~TrayIcon();
    
    // Initialize the tray icon and add it to the system tray
    virtual bool init() = 0;
    
    // Show a notification/balloon tip from the tray icon
    virtual void showNotification(const std::string& title, const std::string& message) = 0;
    
    // Process messages/events (this is needed for message loops)
    virtual bool processMessages() = 0;

protected:
    // Common properties
    std::string m_title;      // Title/tooltip for the tray icon
    std::string m_iconPath;   // Path to the icon file
};

#endif

