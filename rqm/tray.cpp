#include "tray.h"

TrayIcon::TrayIcon(const std::string& title, const std::string& iconPath)
    : m_title(title), m_iconPath(iconPath) {
}

TrayIcon::~TrayIcon() {
    // Base destructor doesn't need to do anything
}
