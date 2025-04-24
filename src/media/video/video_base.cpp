/*
 *  Copyright (C) 2004-2022 Savoir-faire Linux Inc.
 *
 *  Author: Guillaume Roguez <Guillaume.Roguez@savoirfairelinux.com>
 *  Author: Adrien Béraud <adrien.beraud@savoirfairelinux.com>
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

#include "libav_deps.h" // MUST BE INCLUDED FIRST
#include "video_base.h"
#include "media_buffer.h"
#include "string_utils.h"
#include "logger.h"
#include "connectivity/utf8_utils.h"

#include <cassert>

namespace sip_core {
namespace video {

/*=== VideoGenerator =========================================================*/

VideoFrame&
VideoGenerator::getNewFrame()
{
    std::lock_guard<std::mutex> lk(mutex_);
    writableFrame_.reset(new VideoFrame());
    return *writableFrame_.get();
}

void
VideoGenerator::publishFrame()
{
    std::lock_guard<std::mutex> lk(mutex_);
    lastFrame_ = std::move(writableFrame_);
    notify(std::static_pointer_cast<MediaFrame>(lastFrame_));
}

void
VideoGenerator::publishFrame(std::shared_ptr<VideoFrame> frame)
{
    std::lock_guard<std::mutex> lk(mutex_);
    lastFrame_ = std::move(frame);
    notify(std::static_pointer_cast<MediaFrame>(lastFrame_));
}

void
VideoGenerator::flushFrames()
{
    std::lock_guard<std::mutex> lk(mutex_);
    writableFrame_.reset();
    lastFrame_.reset();
}

std::shared_ptr<VideoFrame>
VideoGenerator::obtainLastFrame()
{
    std::lock_guard<std::mutex> lk(mutex_);
    return lastFrame_;
}

/*=== VideoSettings =========================================================*/

static std::string
extractString(const std::map<std::string, std::string>& settings, const std::string& key)
{
    auto i = settings.find(key);
    if (i != settings.cend())
        return i->second;
    return {};
}

VideoSettings::VideoSettings(const std::map<std::string, std::string>& settings)
{
    name = extractString(settings, "name");
    unique_id = extractString(settings, "id");
    input = extractString(settings, "input");
    if (input.empty()) {
        input = unique_id;
    }
    channel = extractString(settings, "channel");
    video_size = extractString(settings, "size");
    framerate = extractString(settings, "rate");
}

std::map<std::string, std::string>
VideoSettings::to_map() const
{
    return {{"name", utf8_make_valid(name)},
            {"id", unique_id},
            {"input", input},
            {"size", video_size},
            {"channel", channel},
            {"rate", framerate},
            {"quality",
            std::to_string(quality)},
            {"no_color",
            std::to_string(no_color)},
            {"down_scale_factor",
            std::to_string(down_scale_factor)}};
}

} // namespace video
} // namespace sip_core

namespace YAML {

Node
convert<sip_core::video::VideoSettings>::encode(const sip_core::video::VideoSettings& rhs)
{
    Node node;
    node["name"] = rhs.name;
    node["id"] = rhs.unique_id;
    node["input"] = rhs.input;
    node["video_size"] = rhs.video_size;
    node["channel"] = rhs.channel;
    node["framerate"] = rhs.framerate;
    node["quality"] = rhs.quality;
    node["no_color"] = rhs.no_color;
    node["down_scale_factor"] = rhs.down_scale_factor;
    return node;
}

bool
convert<sip_core::video::VideoSettings>::decode(const Node& node,
                                                sip_core::video::VideoSettings& rhs)
{
    if (not node.IsMap()) {
        SIP_CORE_WARN("Can't decode VideoSettings YAML node");
        return false;
    }
    rhs.name = node["name"].as<std::string>();
    rhs.unique_id = node["id"].as<std::string>();
    rhs.input = node["input"].as<std::string>();
    rhs.video_size = node["video_size"].as<std::string>();
    rhs.channel = node["channel"].as<std::string>();
    rhs.framerate = node["framerate"].as<std::string>();
    rhs.quality = node["quality"].as<int>();
    rhs.no_color = node["no_color"].as<bool>();
    rhs.down_scale_factor = node["down_scale_factor"].as<int>();
    return true;
}

Emitter&
operator<<(Emitter& out, const sip_core::video::VideoSettings& v)
{
    out << convert<sip_core::video::VideoSettings>::encode(v);
    return out;
}

} // namespace YAML
