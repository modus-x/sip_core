#include "fmt/format.h"
#include "media/system_codec_container.h"

#include "direct_encoder.h"

DirectEncoder::DirectEncoder(const std::string& rtp,
                             const sip_core::DeviceParams& params,
                             std::optional<unsigned> bitrate)
{
    // use h264

    auto h264Codec = std::static_pointer_cast<SystemVideoCodecInfo>(
                         getSystemCodecContainer()->searchCodecByName("H264"))
                         .get();

    accountVideoCodec_ = std::make_shared<AccountVideoCodecInfo>(AccountVideoCodecInfo(*h264Codec));

    auto encoderBitrate = accountVideoCodec_->bitrate;

    if (bitrate.has_value())
        encoderBitrate = bitrate.value();

    auto ms = MediaStream {fmt::format("video device test sender"),
                           AV_PIX_FMT_YUV420P,
                           1 / static_cast<rational<int>>(params.framerate),
                           static_cast<int>(params.width),
                           static_cast<int>(params.height),
                           static_cast<int>(encoderBitrate),
                           static_cast<rational<int>>(params.framerate)

    };

    videoEncoder_.openOutput(fmt::format("rtp://{}", rtp), "rtp");
    videoEncoder_.setOptions(ms);
    videoEncoder_.setOptions({MediaDescription {
        MediaType::MEDIA_VIDEO,
        true,
        false,
        MediaDirection::SENDONLY,
        {},
        {},
        accountVideoCodec_,

        // used
        accountVideoCodec_->payloadType,
        {},
        {},
        {},
        {},
        {},

        // used - extra video params
        {},

        // used - video mode
        RateMode::CRF_CONSTRAINED,

        // used - video linkableHW
        true,

    }});

    videoEncoder_.addStream(accountVideoCodec_->systemCodecInfo);
    videoEncoder_.setInitSeqVal(0);

    keyFrameFreq_ = ms.frameRate.numerator() * KEY_FRAME_PERIOD;

    std::cout << "keyframe is " << keyFrameFreq_ << std::endl;
}

void
DirectEncoder::update(Observable<std::shared_ptr<MediaFrame>>* /*obs*/,
                      const std::shared_ptr<MediaFrame>& frame_p)
{
    auto input_frame = std::dynamic_pointer_cast<VideoFrame>(frame_p);
    bool is_keyframe = forceKeyFrame_ > 0
                       or (keyFrameFreq_ > 0 and (frameNumber_ % keyFrameFreq_) == 0);

    if (is_keyframe)
        --forceKeyFrame_;

    if (videoEncoder_.encode(input_frame, is_keyframe, frameNumber_++) < 0)
        SIP_CORE_ERR("encoding failed");
}