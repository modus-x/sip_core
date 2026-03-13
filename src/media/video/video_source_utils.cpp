#include "video_source_utils.h"

#include "sip_core/media_const.h"

#include <algorithm>

namespace {

constexpr std::string_view kSeparator = libsip_core::Media::VideoProtocolPrefix::SEPARATOR;

} // namespace

namespace sip_core {
namespace video {

bool
containsExactDeviceId(const std::vector<std::string>& deviceIds, std::string_view id)
{
    if (id.empty())
        return false;

    return std::any_of(deviceIds.begin(), deviceIds.end(), [id](const std::string& deviceId) {
        return deviceId == id;
    });
}

std::string
chooseDefaultDeviceId(std::string_view currentDefault,
                      const std::vector<std::string>& physicalDeviceIds)
{
    if (containsExactDeviceId(physicalDeviceIds, currentDefault))
        return std::string(currentDefault);

    if (!physicalDeviceIds.empty())
        return physicalDeviceIds.front();

    return {};
}

bool
isValidVideoSwitchSource(std::string_view source,
                         const std::vector<std::string>& availableCameraIds)
{
    if (source.empty())
        return true;

    const auto pos = source.find(kSeparator);
    if (pos == std::string_view::npos)
        return false;

    const auto prefix = source.substr(0, pos);
    const auto suffix = source.substr(pos + kSeparator.size());
    if (suffix.empty())
        return false;

    if (prefix == libsip_core::Media::VideoProtocolPrefix::CAMERA)
        return containsExactDeviceId(availableCameraIds, suffix);

    if (prefix == libsip_core::Media::VideoProtocolPrefix::DISPLAY
        || prefix == libsip_core::Media::VideoProtocolPrefix::FILE)
        return true;

    return false;
}

} // namespace video
} // namespace sip_core
