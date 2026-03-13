#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace sip_core {
namespace video {

bool containsExactDeviceId(const std::vector<std::string>& deviceIds, std::string_view id);

std::string chooseDefaultDeviceId(std::string_view currentDefault,
                                  const std::vector<std::string>& physicalDeviceIds);

bool isValidVideoSwitchSource(std::string_view source,
                              const std::vector<std::string>& availableCameraIds);

} // namespace video
} // namespace sip_core
