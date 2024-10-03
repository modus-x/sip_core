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

#pragma once

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "callmanager_interface.h"
#include "configurationmanager_interface.h"
#include "presencemanager_interface.h"

#ifdef ENABLE_VIDEO
#include "videomanager_interface.h"
#endif

#include "sip_core.h"
#include "logger.h"
#include "trace-tools.h"
#include "tracepoint.h"

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

#include <exception>
#include <memory>
#include <map>
#include <utility>
#include <string>
#include "scheduled_executor.h"
#include <memory.h>

namespace sip_core {

using SignalHandlerMap = std::map<std::string, std::shared_ptr<libsip_core::CallbackWrapperBase>>;
extern SignalHandlerMap& getSignalHandlers();
extern std::unique_ptr<ScheduledExecutor> eventScheduler;

template<typename Callback>
static void
runOnEventThread(Callback&& cb,
                 const char* filename = CURRENT_FILENAME(),
                 uint32_t linum = CURRENT_LINE())
{
    if (eventScheduler) {
        eventScheduler->run([cb = std::forward<Callback>(cb)]() mutable { cb(); }, filename, linum);
    }
}

/*
 * Find related user given callback and call it with given
 * arguments.
 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
template<typename Ts, typename... Args>
void
emitSignal(Args... args)
{
    sip_core_tracepoint_if_enabled(emit_signal, demangle<Ts>().c_str());

    const auto& handlers = getSignalHandlers();
    if (auto wrap = libsip_core::CallbackWrapper<typename Ts::cb_type>(handlers.at(Ts::name))) {
        try {
            sip_core_tracepoint(emit_signal_begin_callback, wrap.file_, wrap.linum_);
            auto cb = *wrap;
            cb(args...);
            sip_core_tracepoint(emit_signal_end_callback);
        } catch (std::exception& e) {
            SIP_CORE_ERR("Exception during emitting signal %s:\n%s", Ts::name, e.what());
        }
    } else {
        SIP_CORE_ERR("Wrap not found for signal %s", Ts::name);
    }

    sip_core_tracepoint(emit_signal_end);
}
#pragma GCC diagnostic pop

template<typename Ts>
std::pair<std::string, std::shared_ptr<libsip_core::CallbackWrapper<typename Ts::cb_type>>>
exported_callback()
{
    return std::make_pair((const std::string&) Ts::name,
                          std::make_shared<libsip_core::CallbackWrapper<typename Ts::cb_type>>());
}

} // namespace sip_core
