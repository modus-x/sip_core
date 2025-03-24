

#include "tray_win.h"
#include "tools.h"
#include "resource.h"

#include <string.h>

#define WM_TRAYICON (WM_USER + 1)
#define ID_TRAY_EXIT 1001

// Store a global pointer to the current instance for use in the static window procedure
TrayIconWindows* g_trayInstance = nullptr;

TrayIconWindows::TrayIconWindows(const std::string& title, const std::string& iconPath)
    : TrayIcon(title, iconPath),
      m_hwnd(NULL),
      m_menu(NULL),
      m_isInitialized(false) {
    
    // Initialize the icon data structure
    ZeroMemory(&m_iconData, sizeof(m_iconData));
    
    // Store the instance for use in the static window procedure
    g_trayInstance = this;
}

TrayIconWindows::~TrayIconWindows() {
    // Remove the icon from the tray if initialized
    if (m_isInitialized) {
        Shell_NotifyIcon(NIM_DELETE, &m_iconData);
    }
    
    // Destroy the menu if it exists
    if (m_menu) {
        DestroyMenu(m_menu);
    }
    
    // Destroy the window if it exists
    if (m_hwnd) {
        DestroyWindow(m_hwnd);
    }
    
    // Reset the global instance pointer
    if (g_trayInstance == this) {
        g_trayInstance = nullptr;
    }
}

bool TrayIconWindows::init() {
    // If already initialized, return success
    if (m_isInitialized) {
        return true;
    }
    
    // Create the hidden window
    if (!createHiddenWindow()) {
        return false;
    }
    
    // Create the popup menu
    m_menu = CreatePopupMenu();
    if (!m_menu) {
        return false;
    }
    
    // Add the "Exit" menu item
    AppendMenu(m_menu, MF_STRING, ID_TRAY_EXIT, L"Выход");
    
    // Set up the tray icon data
    m_iconData.cbSize = sizeof(NOTIFYICONDATA);
    m_iconData.hWnd = m_hwnd;
    m_iconData.uID = 1;
    m_iconData.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    m_iconData.uCallbackMessage = WM_TRAYICON;
    
    // Set the icon
    m_iconData.hIcon = loadIcon();

    // Set the tooltip
    wcsncpy_s(m_iconData.szTip, to_wstring(m_title).c_str(), sizeof(m_iconData.szTip) - 1);
    
    // Add the icon to the system tray
    m_isInitialized = Shell_NotifyIcon(NIM_ADD, &m_iconData) == TRUE;
    
    return m_isInitialized;
}

void TrayIconWindows::showNotification(const std::string& title, const std::string& message) {
    if (!m_isInitialized) {
        return;
    }
    
    // Update the notification fields
    m_iconData.uFlags |= NIF_INFO;
    wcsncpy_s(m_iconData.szInfoTitle, to_wstring(title).c_str(), sizeof(m_iconData.szInfoTitle) - 1);
    wcsncpy_s(m_iconData.szInfo, to_wstring(message).c_str(), sizeof(m_iconData.szInfo) - 1);
    m_iconData.dwInfoFlags = NIIF_INFO;
    
    // Show the notification
    Shell_NotifyIcon(NIM_MODIFY, &m_iconData);
}

bool TrayIconWindows::processMessages() {
    // Process all pending messages
    MSG msg;
    while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
        // Check for quit message
        if (msg.message == WM_QUIT) {
            return false;
        }
        
        // Process message
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    
    return true;
}

bool TrayIconWindows::registerWindowClass() {
    WNDCLASSEXA wc = { 0 };
    wc.cbSize = sizeof(WNDCLASSEXA);
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = "TrayIconWindowClass";
    
    return RegisterClassExA(&wc) != 0;
}

bool TrayIconWindows::createHiddenWindow() {
    // Register the window class
    if (!registerWindowClass()) {
        return false;
    }
    
    // Create the hidden window
    m_hwnd = CreateWindowExA(
        0,
        "TrayIconWindowClass",
        "TrayIconWindow",
        0,
        CW_USEDEFAULT, CW_USEDEFAULT,
        CW_USEDEFAULT, CW_USEDEFAULT,
        NULL, NULL,
        GetModuleHandle(NULL),
        NULL
    );
    
    return m_hwnd != NULL;
}

HICON TrayIconWindows::loadIcon() {
    HINSTANCE hInstance = GetModuleHandle(NULL); // Handle to the current executable
    return (HICON)LoadIcon(hInstance, MAKEINTRESOURCE(IDI_ICON1)); // Load the icon from resources
}

LRESULT CALLBACK TrayIconWindows::WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    // Forward tray icon messages to the instance method
    if (uMsg == WM_TRAYICON && g_trayInstance) {
        return g_trayInstance->handleTrayMessage(hwnd, uMsg, wParam, lParam);
    }
    
    // Handle other messages
    switch (uMsg) {
        case WM_COMMAND:
            // Handle menu commands
            if (LOWORD(wParam) == ID_TRAY_EXIT) {
                PostQuitMessage(0);
                return 0;
            }
            break;
    }
    
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

LRESULT TrayIconWindows::handleTrayMessage(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    // Only handle messages from our tray icon
    if (wParam != 1) {
        return 0;
    }
    
    // Handle the different tray icon messages
    switch (LOWORD(lParam)) {
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU:
            {
                // Get the cursor position
                POINT pt;
                GetCursorPos(&pt);
                
                // Set the foreground window to handle menu events correctly
                SetForegroundWindow(hwnd);
                
                // Display the popup menu
                TrackPopupMenu(m_menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
                
                // Required for proper menu dismissal
                PostMessage(hwnd, WM_NULL, 0, 0);
            }
            break;
    }
    
    return 0;
}