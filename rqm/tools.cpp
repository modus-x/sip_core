#include "tools.h"

#ifdef _WIN32
#include <windows.h>
#endif

std::wstring to_wstring(const std::string& str) {
    #ifdef _WIN32
        int size_needed = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, NULL, 0);
        std::wstring wstr(size_needed, 0);
        MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, &wstr[0], size_needed);
        return wstr;
    #else
        // just return as is
        return str;
    #endif
}