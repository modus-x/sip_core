#include "media/media_device.h"
#include "sip_core/device_const.h"

namespace sip_core {

DeviceParams::DeviceParams(const libsip_core::DeviceMap& deviceMap)
{
    std::pair<bool, int> pairInt;
    pairInt = getIntValue(deviceMap, libsip_core::Device::DeviceAttributeKey::HEIGHT);
    if (pairInt.first && pairInt.second > 0)
        height = pairInt.second;

    pairInt = getIntValue(deviceMap, libsip_core::Device::DeviceAttributeKey::WIDTH);
    if (pairInt.first && pairInt.second > 0)
        width = pairInt.second;
}

libsip_core::DeviceMap
DeviceParams::toDeviceMap(const DeviceParams& params)
{
    libsip_core::DeviceMap map;

    map.emplace(libsip_core::Device::DeviceAttributeKey::HEIGHT, std::to_string(params.height));
    map.emplace(libsip_core::Device::DeviceAttributeKey::WIDTH, std::to_string(params.width));

    return map;
}

std::string
DeviceParams::toJson() const
{
    Json::Value val = {};
    val[libsip_core::Device::DeviceAttributeKey::HEIGHT] = height;
    val[libsip_core::Device::DeviceAttributeKey::WIDTH] = width;
    return Json::writeString(Json::StreamWriterBuilder {}, val);
}

DeviceParams::DeviceParams(const std::string& msg)
{
    Json::Value json;
    std::string err;
    Json::CharReaderBuilder rbuilder;
    auto reader = std::unique_ptr<Json::CharReader>(rbuilder.newCharReader());
    if (reader->parse(msg.data(), msg.data() + msg.size(), &json, &err)) {
        if (json.isObject()) {
            if (json.isMember(libsip_core::Device::DeviceAttributeKey::HEIGHT)) {
                height = json[libsip_core::Device::DeviceAttributeKey::HEIGHT].asInt();
            }
            if (json.isMember(libsip_core::Device::DeviceAttributeKey::WIDTH))
                width = json[libsip_core::Device::DeviceAttributeKey::WIDTH].asInt();
        }
    }
}

std::pair<bool, int>
DeviceParams::getIntValue(const libsip_core::DeviceMap& map, const std::string& key)
{
    const auto& iter = map.find(key);
    if (iter == map.end()) {
        return {false, {}};
    }

    return {true, std::stoi(iter->second)};
}

} // namespace sip_core
