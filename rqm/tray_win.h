#ifndef TRAY_ICON_WINDOWS_H
#define TRAY_ICON_WINDOWS_H

#include "tray.h"
#include <windows.h>
#include <shellapi.h>
#include <memory>

// Windows implementation of the tray icon
class TrayIconWindows : public TrayIcon {
public:
    TrayIconWindows(const std::string& title, const std::string& iconPath = "");
    virtual ~TrayIconWindows();
    
    // Implementation of abstract methods
    virtual bool init() override;
    virtual void showNotification(const std::string& title, const std::string& message) override;
    virtual bool processMessages() override;

private:
    // Windows-specific members
    HWND m_hwnd;                 // Hidden window handle
    NOTIFYICONDATA m_iconData;   // Tray icon data
    HMENU m_menu;                // Popup menu
    bool m_isInitialized;        // Initialization state
    
    // Static window procedure for the hidden window
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
    
    // Message handler for tray icon events
    LRESULT handleTrayMessage(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
    
    // Initialize the hidden window
    bool createHiddenWindow();
    
    // Create and register the window class
    bool registerWindowClass();
    
    // Load icon from file or use default
    HICON loadIcon();
};

#endif