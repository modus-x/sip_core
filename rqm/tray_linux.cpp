#include "tray_linux.h"
#include <iostream>

TrayIconLinux::TrayIconLinux(const std::string& title, const std::string& iconPath)
    : TrayIcon(title, iconPath),
      m_statusIcon(nullptr),
      m_menu(nullptr),
      m_isRunning(false) {
}

TrayIconLinux::~TrayIconLinux() {
    // Stop the GTK thread
    m_isRunning = false;
    
    // Wait for the thread to finish
    if (m_gtkThread.joinable()) {
        m_gtkThread.join();
    }
}

bool TrayIconLinux::init() {
    // Initialize GTK
    if (!initGtk()) {
        return false;
    }
    
    // Create the status icon
    if (!createStatusIcon()) {
        return false;
    }
    
    // Create the menu
    if (!createMenu()) {
        return false;
    }
    
    // Start the GTK main loop in a separate thread
    m_isRunning = true;
    m_gtkThread = std::thread(&TrayIconLinux::gtkThreadFunc, this);
    
    return true;
}

void TrayIconLinux::showNotification(const std::string& title, const std::string& message) {
    // Ensure we're in the GTK thread or use gdk_threads_add_idle
    GNotification* notification = g_notification_new(title.c_str());
    g_notification_set_body(notification, message.c_str());
    
    GApplication* app = g_application_get_default();
    if (app) {
        g_application_send_notification(app, nullptr, notification);
    }
    
    g_object_unref(notification);
}

bool TrayIconLinux::processMessages() {
    // The GTK main loop is running in a separate thread,
    // so we just need to check if it's still running
    return m_isRunning;
}

bool TrayIconLinux::initGtk() {
    // Initialize GTK
    if (!gtk_init_check(nullptr, nullptr)) {
        std::cerr << "Failed to initialize GTK" << std::endl;
        return false;
    }
    
    return true;
}

bool TrayIconLinux::createStatusIcon() {
    // Create a status icon
    m_statusIcon = gtk_status_icon_new();
    
    if (!m_statusIcon) {
        return false;
    }
    
    // Set the tooltip
    gtk_status_icon_set_tooltip_text(m_statusIcon, m_title.c_str());
    
    // Try to load the custom icon if specified
    if (!m_iconPath.empty()) {
        gtk_status_icon_set_from_file(m_statusIcon, m_iconPath.c_str());
    } else {
        // Fall back to a standard icon
        gtk_status_icon_set_from_icon_name(m_statusIcon, "application-x-executable");
    }
    
    // Connect the popup menu signal
    g_signal_connect(m_statusIcon, "popup-menu", G_CALLBACK(onPopupMenu), this);
    
    // Show the icon
    gtk_status_icon_set_visible(m_statusIcon, TRUE);
    
    return true;
}

bool TrayIconLinux::createMenu() {
    // Create the menu
    m_menu = gtk_menu_new();
    
    if (!m_menu) {
        return false;
    }
    
    // Create the exit menu item
    GtkWidget* exitItem = gtk_menu_item_new_with_label("Exit");
    
    if (!exitItem) {
        return false;
    }
    
    // Connect the exit signal
    g_signal_connect(exitItem, "activate", G_CALLBACK(onMenuExit), this);
    
    // Add the item to the menu
    gtk_menu_shell_append(GTK_MENU_SHELL(m_menu), exitItem);
    
    // Show all menu items
    gtk_widget_show_all(m_menu);
    
    return true;
}

void TrayIconLinux::gtkThreadFunc() {
    // Create a main loop
    GMainLoop* loop = g_main_loop_new(nullptr, FALSE);
    
    // Run the main loop until m_isRunning is false
    while (m_isRunning) {
        // Process GTK events
        while (gtk_events_pending() && m_isRunning) {
            gtk_main_iteration_do(FALSE);
        }
        
        // Sleep a bit to avoid high CPU usage
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    
    // Clean up
    g_main_loop_unref(loop);
    
    // Clean up the status icon
    if (m_statusIcon) {
        g_object_unref(m_statusIcon);
        m_statusIcon = nullptr;
    }
    
    // Clean up the menu
    if (m_menu) {
        gtk_widget_destroy(m_menu);
        m_menu = nullptr;
    }
}

void TrayIconLinux::onPopupMenu(GtkStatusIcon* status_icon, guint button, guint activate_time, gpointer user_data) {
    TrayIconLinux* self = static_cast<TrayIconLinux*>(user_data);
    
    // Show the menu
    gtk_menu_popup(GTK_MENU(self->m_menu), nullptr, nullptr, gtk_status_icon_position_menu, self->m_statusIcon, button, activate_time);
}

void TrayIconLinux::onMenuExit(GtkMenuItem* item, gpointer user_data) {
    TrayIconLinux* self = static_cast<TrayIconLinux*>(user_data);
    
    // Stop the application
    self->m_isRunning = false;
    
    // Quit the GTK main loop
    GApplication* app = g_application_get_default();
    if (app) {
        g_application_quit(app);
    } else {
        // Alternative: post a quit message to the main loop
        g_main_loop_quit(g_main_loop_new(nullptr, FALSE));
    }
}