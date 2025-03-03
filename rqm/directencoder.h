#include "media/video/video_base.h"
#include "media/media_encoder.h"
#include <optional>

#include "media_device.h"

#include <memory>

using namespace sip_core;

class DirectEncoder : public sip_core::video::VideoFramePassiveReader
{
public:
    DirectEncoder(const std::string& rtp,
                  const sip_core::DeviceParams& params,
                  std::optional<unsigned> bitrate = std::nullopt);

    ~DirectEncoder() {};

    // as VideoFramePassiveReader
    void update(Observable<std::shared_ptr<MediaFrame>>* obs,
                const std::shared_ptr<MediaFrame>& frame_p) override;

private:
    MediaEncoder videoEncoder_;

    static constexpr int KEYFRAMES_AT_START {1}; // Number of keyframes to enforce at stream startup
    static constexpr unsigned KEY_FRAME_PERIOD {2}; // seconds before forcing a keyframe

    std::shared_ptr<AccountVideoCodecInfo> accountVideoCodec_;

    int64_t frameNumber_ = 0;

    std::atomic<int> forceKeyFrame_ {KEYFRAMES_AT_START};
    int keyFrameFreq_ {0}; // Set keyframe rate, 0 to disable auto-keyframe. Computed in constructor
};