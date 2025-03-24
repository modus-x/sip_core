// TrayIconLinux.h
#ifndef TRAY_ICON_LINUX_H
#define TRAY_ICON_LINUX_H

#include "tray.h"
#include <gtk/gtk.h>
#include <thread>
#include <atomic>

// Linux implementation of the tray icon using GTK
class TrayIconLinux : public TrayIcon {
public:
    TrayIconLinux(const std::string& title, const std::string& iconPath = "");
    virtual ~TrayIconLinux();
    
    // Implementation of abstract methods
    virtual bool init() override;
    virtual void showNotification(const std::string& title, const std::string& message) override;
    virtual bool processMessages() override;

private:
    // GTK-specific members
    GtkStatusIcon* m_statusIcon;
    GtkWidget* m_menu;
    std::thread m_gtkThread;
    std::atomic<bool> m_isRunning;
    
    // Initialize GTK library
    bool initGtk();
    
    // Create the status icon
    bool createStatusIcon();
    
    // Create the popup menu
    bool createMenu();
    
    // GTK main thread function
    void gtkThreadFunc();
    
    // Callback functions
    static void onPopupMenu(GtkStatusIcon* status_icon, guint button, guint activate_time, gpointer user_data);
    static void onMenuExit(GtkMenuItem* item, gpointer user_data);
};

#endif