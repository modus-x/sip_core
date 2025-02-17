/*
 *  Copyright (C) 2004-2022 Savoir-faire Linux Inc.
 *
 *  Author: Philippe Proulx <philippe.proulx@savoirfairelinux.com>
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
#include <string>
#include <vector>
#include <map>
#include <cstdlib>

#include <optional>

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "manager.h"
#include "logger.h"
#include "sip_core.h"
#include "client/ring_signal.h"

#ifdef ENABLE_VIDEO
#include "client/videomanager.h"
#endif // ENABLE_VIDEO

namespace libsip_core {

bool
init(enum InitFlag flags) noexcept
{
    try {
        sip_core::Logger::setDebugMode(LIBSIP_CORE_FLAG_DEBUG == (flags & LIBSIP_CORE_FLAG_DEBUG));

        sip_core::Logger::setSysLog(true);
        sip_core::Logger::setConsoleLog(LIBSIP_CORE_FLAG_CONSOLE_LOG
                                        == (flags & LIBSIP_CORE_FLAG_CONSOLE_LOG));

        const char* log_file = getenv("SIP_CORE_INFO_FILE");

        if (log_file) {
            sip_core::Logger::setFileLog(log_file);
        }

        // Following function create a local static variable inside
        // This var must have the same live as Manager.
        // So we call it now to create this var.
        sip_core::getSignalHandlers();

        return true;
    } catch (...) {
        return false;
    }
}

bool
start(const std::string& config_file, const std::optional<std::string>& data_path) noexcept
{
    try {
        sip_core::Manager::instance().init(config_file, data_path);
    } catch (...) {
        return false;
    }
    return true;
}

bool
initialized() noexcept
{
    return sip_core::Manager::initialized;
}

void
fini() noexcept
{
    sip_core::Manager::instance().finish();
    sip_core::Logger::fini();
}

void
logging(const std::string& whom, const std::string& action) noexcept
{
    if ("syslog" == whom) {
        sip_core::Logger::setSysLog(not action.empty());
    } else if ("console" == whom) {
        sip_core::Logger::setConsoleLog(not action.empty());
    } else if ("monitor" == whom) {
        sip_core::Logger::setMonitorLog(not action.empty());
    } else if ("file" == whom) {
        sip_core::Logger::setFileLog(action);
    } else {
        SIP_CORE_ERR("Bad log handler %s", whom.c_str());
    }
}

} // namespace libsip_core
