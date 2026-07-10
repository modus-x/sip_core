#include "sip/sdp.h"
#include "media/media_codec.h"

#include <pjlib.h>
#include <pjmedia/sdp.h>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

using namespace sip_core;

namespace {

void
fail(const std::string& message)
{
    std::cerr << "TEST FAILURE: " << message << "\n";
    std::exit(1);
}

void
expect_true(bool condition, const std::string& message)
{
    if (!condition) {
        fail(message);
    }
}

void
expect_false(bool condition, const std::string& message)
{
    expect_true(!condition, message);
}

std::string
base_sdp()
{
    return "v=0\r\n"
           "o=- 0 0 IN IP4 127.0.0.1\r\n"
           "s=-\r\n"
           "c=IN IP4 127.0.0.1\r\n"
           "t=0 0\r\n";
}

class SdpParser
{
public:
    SdpParser()
    {
        pj_status_t status = pj_init();
        if (status != PJ_SUCCESS) {
            fail("pj_init failed");
        }

        pj_caching_pool_init(&cp_, nullptr, 0);
        pool_ = pj_pool_create(&cp_.factory, "sdp-offer-validation-test", 4096, 4096, nullptr);
        if (!pool_) {
            pj_caching_pool_destroy(&cp_);
            pj_shutdown();
            fail("Failed to create PJ pool");
        }
    }

    ~SdpParser()
    {
        if (pool_) {
            pj_pool_release(pool_);
        }
        pj_caching_pool_destroy(&cp_);
        pj_shutdown();
    }

    bool hasNegotiableMedia(std::string sdp,
                            const std::vector<std::shared_ptr<AccountCodecInfo>>& audioCodecs,
                            const std::vector<std::shared_ptr<AccountCodecInfo>>& videoCodecs)
    {
        pjmedia_sdp_session* session = nullptr;
        auto status = pjmedia_sdp_parse(pool_, sdp.data(), sdp.size(), &session);
        if (status != PJ_SUCCESS || session == nullptr) {
            fail("Failed to parse SDP fixture");
        }

        return Sdp::hasNegotiableMedia(session, audioCodecs, videoCodecs);
    }

private:
    pj_caching_pool cp_ {};
    pj_pool_t* pool_ {nullptr};
};

struct LocalCodecs
{
    SystemAudioCodecInfo opusSys {1,
                                  1,
                                  "Opus",
                                  "opus",
                                  "libopus",
                                  CODEC_ENCODER_DECODER,
                                  0,
                                  48000,
                                  2,
                                  104};
    SystemAudioCodecInfo pcmaSys {2,
                                  2,
                                  "G.711a",
                                  "PCMA",
                                  "pcm_alaw",
                                  CODEC_ENCODER_DECODER,
                                  64,
                                  8000,
                                  1,
                                  8};
    std::vector<std::shared_ptr<AccountCodecInfo>> audio;
    std::vector<std::shared_ptr<AccountCodecInfo>> video;

    LocalCodecs()
    {
        auto opus = std::make_shared<AccountAudioCodecInfo>(opusSys);
        opus->isActive = true;
        auto pcma = std::make_shared<AccountAudioCodecInfo>(pcmaSys);
        pcma->isActive = true;
        audio.emplace_back(std::move(opus));
        audio.emplace_back(std::move(pcma));
    }
};

struct PcmuOnlyCodecs
{
    SystemAudioCodecInfo pcmuSys {3,
                                  3,
                                  "G.711u",
                                  "PCMU",
                                  "pcm_mulaw",
                                  CODEC_ENCODER_DECODER,
                                  64,
                                  8000,
                                  1,
                                  0};
    std::vector<std::shared_ptr<AccountCodecInfo>> audio;
    std::vector<std::shared_ptr<AccountCodecInfo>> video;

    PcmuOnlyCodecs()
    {
        auto pcmu = std::make_shared<AccountAudioCodecInfo>(pcmuSys);
        pcmu->isActive = true;
        audio.emplace_back(std::move(pcmu));
    }
};

void
test_t38_image_only_offer_is_not_negotiable(SdpParser& parser, const LocalCodecs& codecs)
{
    auto sdp = base_sdp()
               + "m=image 99 udptl t38\r\n"
                 "a=T38FaxVersion:0\r\n"
                 "a=T38MaxBitRate:14400\r\n";

    expect_false(parser.hasNegotiableMedia(sdp, codecs.audio, codecs.video),
                 "T.38 image-only offer must not be accepted as an initial call");
}

void
test_unknown_dynamic_audio_codec_is_not_negotiable(SdpParser& parser, const LocalCodecs& codecs)
{
    auto sdp = base_sdp()
               + "m=audio 4000 RTP/AVP 120\r\n"
                 "a=rtpmap:120 garbagecodec/8000\r\n"
                 "a=sendrecv\r\n";

    expect_false(parser.hasNegotiableMedia(sdp, codecs.audio, codecs.video),
                 "Unknown dynamic audio codec must not be negotiable");
}

void
test_long_unknown_audio_codec_name_is_not_negotiable(SdpParser& parser, const LocalCodecs& codecs)
{
    const auto longCodecName = std::string(128, 'x');
    auto sdp = base_sdp() + "m=audio 4000 RTP/AVP 120\r\n"
               + "a=rtpmap:120 " + longCodecName + "/8000\r\n"
               + "a=sendrecv\r\n";

    expect_false(parser.hasNegotiableMedia(sdp, codecs.audio, codecs.video),
                 "Long unsupported codec name must not be negotiable");
}

void
test_static_pcma_without_rtpmap_is_negotiable(SdpParser& parser, const LocalCodecs& codecs)
{
    auto sdp = base_sdp()
               + "m=audio 4000 RTP/AVP 8\r\n"
                 "a=sendrecv\r\n";

    expect_true(parser.hasNegotiableMedia(sdp, codecs.audio, codecs.video),
                "Static PCMA payload in m= line must be negotiable without rtpmap");
}

void
test_dynamic_opus_is_negotiable(SdpParser& parser, const LocalCodecs& codecs)
{
    auto sdp = base_sdp()
               + "m=audio 4000 RTP/AVP 111\r\n"
                 "a=rtpmap:111 opus/48000/2\r\n"
                 "a=sendrecv\r\n";

    expect_true(parser.hasNegotiableMedia(sdp, codecs.audio, codecs.video),
                "Dynamic Opus payload must be negotiable when name/rate/channels match");
}

void
test_valid_audio_plus_t38_image_is_negotiable(SdpParser& parser, const LocalCodecs& codecs)
{
    auto sdp = base_sdp()
               + "m=image 99 udptl t38\r\n"
                 "a=T38FaxVersion:0\r\n"
                 "a=T38MaxBitRate:14400\r\n"
                 "m=audio 4000 RTP/AVP 8\r\n"
                 "a=sendrecv\r\n";

    expect_true(parser.hasNegotiableMedia(sdp, codecs.audio, codecs.video),
                "Offer with valid audio plus unsupported T.38 image media must remain negotiable");
}

void
test_disabled_valid_audio_is_not_negotiable(SdpParser& parser, const LocalCodecs& codecs)
{
    auto sdp = base_sdp()
               + "m=audio 0 RTP/AVP 8\r\n"
                 "a=sendrecv\r\n";

    expect_false(parser.hasNegotiableMedia(sdp, codecs.audio, codecs.video),
                 "Disabled audio media must not make an offer negotiable");
}

// Regression: PCMU's static RFC 3551 payload type is 0. Until the helper was switched to
// std::optional, that 0 collided with the "not found" sentinel and PCMU-only configurations
// were dropped from the answer, producing a port-0 / disabled-stream 200 OK.
void
test_static_pcmu_only_without_rtpmap_is_negotiable(SdpParser& parser,
                                                   const PcmuOnlyCodecs& codecs)
{
    auto sdp = base_sdp()
               + "m=audio 4000 RTP/AVP 0\r\n"
                 "a=sendrecv\r\n";

    expect_true(parser.hasNegotiableMedia(sdp, codecs.audio, codecs.video),
                "Static PCMU payload (PT 0) in m= line must be negotiable for a PCMU-only "
                "local codec list, even without an explicit rtpmap entry");
}

// Regression: an offer that mirrors the original bug report (static PCMA + PCMU + dynamic codecs,
// no rtpmap for PCMA/PCMU) must still be accepted when the local list contains only PCMU.
void
test_pcmu_only_against_mixed_static_offer_is_negotiable(SdpParser& parser,
                                                        const PcmuOnlyCodecs& codecs)
{
    auto sdp = base_sdp()
               + "m=audio 15290 RTP/AVP 8 0 3 18 100 101\r\n"
                 "a=rtpmap:100 opus/48000/2\r\n"
                 "a=rtpmap:101 telephone-event/8000\r\n"
                 "a=sendrecv\r\n";

    expect_true(parser.hasNegotiableMedia(sdp, codecs.audio, codecs.video),
                "Mixed-codec offer must remain negotiable when the only local codec is PCMU");
}

} // namespace

int
main()
{
    SdpParser parser;
    LocalCodecs codecs;
    PcmuOnlyCodecs pcmuOnlyCodecs;

    test_t38_image_only_offer_is_not_negotiable(parser, codecs);
    test_unknown_dynamic_audio_codec_is_not_negotiable(parser, codecs);
    test_long_unknown_audio_codec_name_is_not_negotiable(parser, codecs);
    test_static_pcma_without_rtpmap_is_negotiable(parser, codecs);
    test_dynamic_opus_is_negotiable(parser, codecs);
    test_valid_audio_plus_t38_image_is_negotiable(parser, codecs);
    test_disabled_valid_audio_is_not_negotiable(parser, codecs);
    test_static_pcmu_only_without_rtpmap_is_negotiable(parser, pcmuOnlyCodecs);
    test_pcmu_only_against_mixed_static_offer_is_negotiable(parser, pcmuOnlyCodecs);

    std::cout << "All SDP offer validation tests passed.\n";
    return 0;
}
