/*
 *  Copyright (C) 2004-2022 Savoir-faire Linux Inc.
 *
 *  Author: Guillaume Roguez <Guillaume.Roguez@savoirfairelinux.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "sip_core.h"
#include <string>

#include <ciso646> // fix windows compiler bug

#ifndef SIP_CORE_REVISION
#define SIP_CORE_REVISION ""
#endif

#ifndef SIP_CORE_DIRTY_REPO
#define SIP_CORE_DIRTY_REPO ""
#endif

#ifndef PACKAGE_VERSION
#define PACKAGE_VERSION "unknown"
#endif

namespace libsip_core {

const char* SIP_CORE_VERSION = "0.9.10";

const char*
version() noexcept
{
    return SIP_CORE_VERSION;
//    return SIP_CORE_REVISION[0] and SIP_CORE_DIRTY_REPO[0]
//               ? PACKAGE_VERSION "-" SIP_CORE_REVISION "-" SIP_CORE_DIRTY_REPO
//               : (SIP_CORE_REVISION[0] ? PACKAGE_VERSION "-" SIP_CORE_REVISION : PACKAGE_VERSION);
}

const char*
platform() noexcept
{
#ifdef __linux__
    #if defined(__ANDROID__)
        return "android";
    #else
        return "linux";
    #endif
#elif defined(_WIN32)
    return "win32";
#elif defined(__APPLE__)
    #ifdef TARGET_OS_IOS
        return "iOS";
    #else
        return "macOS";
    #endif
#else
    #error "Unknown OS"
#endif
}
} // namespace libsip_core
