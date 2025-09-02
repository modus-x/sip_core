#include "CallController.h"
#include "configurationmanager_interface.h"
#include "manager.h"

#include <functional>
#include <regex>

#ifdef _WIN32
#define PATH_MAX MAX_PATH
#endif

// Get list of input devices
// auto inputs = sip_core::Manager::instance().getAudioInputDeviceList();
// Get list of output devices
// auto outputs = sip_core::Manager::instance().getAudioOutputDeviceList();
// sip_core::Manager::instance().setAudioDevice(1, sip_core::AudioDeviceType::CAPTURE);
// sip_core::Manager::instance().setAudioDevice(1, sip_core::AudioDeviceType::PLAYBACK);

CallController::CallController(const std::string& accountId)
    : m_mtxEvents()
    , m_isVideoEnabled(true)
    , m_mediaAudio {{"MEDIA_TYPE", "MEDIA_TYPE_AUDIO"},
                    {"ENABLED", "true"},
                    {"MUTED", "false"},
                    {"LABEL", "audio_0"}}
    , m_mediaVideo {{"MEDIA_TYPE", "MEDIA_TYPE_VIDEO"},
                    {"ENABLED", "true"},
                    {"MUTED", "false"},
                    // { "SOURCE", "display://desktop 640x480" }, //640x480
                    // { "SOURCE",
                    // R"(camera://video=@device_pnp_\\?\usb#vid_1bcf&pid_2284&mi_00#6&2e99a59a&0&0000#{65e8773d-8f56-11d0-a3b9-00a0c9223196}\global)"
                    // }, // 4k { "SOURCE",
                    // R"(camera://video=@device_pnp_\\?\usb#vid_09da&pid_2695&mi_00#6&26daa0e0&0&0000#{65e8773d-8f56-11d0-a3b9-00a0c9223196}\global)"
                    // }, // aux { "SOURCE",
                    // R"(camera://video=@device_pnp_\\?\usb#vid_04f2&pid_b76f&mi_00#6&330c68f9&0&0000#{65e8773d-8f56-11d0-a3b9-00a0c9223196}\global)"
                    // }, // front
                    {"LABEL", "video_0"}}
    , m_user()
    , m_domain()
    , m_accountId(accountId)
    , m_activeConfirence()
    , m_activeCalls()
    , EVENT_FRAME_READY(0)
    , EVENT_CREATE_PREVIEW(0)
    , EVENT_DESTROY_PREVIEW(0)
    , m_previewWindows()
{
    assert(!m_accountId.empty() && "Account id must not be empty");
}

CallController::~CallController()
{
    if (libsip_core::initialized()) {
        libsip_core::fini();
    }

    hangUp();
    SDL_Quit();
}

bool
CallController::init()
{
    const sip_core::SignalHandlerMap sigMap = {
        libsip_core::exportable_callback<libsip_core::ConfigurationSignal::RegistrationStateChanged>(
            std::bind(&CallController::registrationStateChanged,
                      this,
                      std::placeholders::_1,
                      std::placeholders::_2,
                      std::placeholders::_3,
                      std::placeholders::_4)),
        libsip_core::exportable_callback<libsip_core::ConfigurationSignal::VolatileDetailsChanged>(
            std::bind(&CallController::volatileDetailsChanged,
                      this,
                      std::placeholders::_1,
                      std::placeholders::_2)),
        libsip_core::exportable_callback<libsip_core::CallSignal::StateChange>(
            std::bind(&CallController::callStateChanged,
                      this,
                      std::placeholders::_1,
                      std::placeholders::_2,
                      std::placeholders::_3,
                      std::placeholders::_4)),
        libsip_core::exportable_callback<libsip_core::CallSignal::IncomingCall>(
            std::bind(&CallController::incomingCall,
                      this,
                      std::placeholders::_1,
                      std::placeholders::_2,
                      std::placeholders::_3)),
        libsip_core::exportable_callback<libsip_core::CallSignal::IncomingCallWithMedia>(
            std::bind(&CallController::incomingCallWithMedia,
                      this,
                      std::placeholders::_1,
                      std::placeholders::_2,
                      std::placeholders::_3,
                      std::placeholders::_4,
                      std::placeholders::_5)),
        libsip_core::exportable_callback<libsip_core::CallSignal::MediaNegotiationStatus>(
            std::bind(&CallController::mediaNegotiationStatus,
                      this,
                      std::placeholders::_1,
                      std::placeholders::_2,
                      std::placeholders::_3)),
        libsip_core::exportable_callback<libsip_core::AudioSignal::DeviceEvent>(
            std::bind(&CallController::audioDeviceEvent, this)),
        libsip_core::exportable_callback<libsip_core::VideoSignal::StartCapture>(
            std::bind(&CallController::startCapture, this, std::placeholders::_1)),
        libsip_core::exportable_callback<libsip_core::VideoSignal::DecodingStarted>(
            std::bind(&CallController::decodingStarted,
                      this,
                      std::placeholders::_1,
                      std::placeholders::_2,
                      std::placeholders::_3,
                      std::placeholders::_4,
                      std::placeholders::_5)),
        libsip_core::exportable_callback<libsip_core::VideoSignal::DecodingStopped>(
            std::bind(&CallController::decodingStopped,
                      this,
                      std::placeholders::_1,
                      std::placeholders::_2,
                      std::placeholders::_3)),
        libsip_core::exportable_callback<libsip_core::CallSignal::ConferenceCreated>(
            std::bind(&CallController::conferenceCreated,
                      this,
                      std::placeholders::_1,
                      std::placeholders::_2)),
        libsip_core::exportable_callback<libsip_core::CallSignal::ConferenceChanged>(
            std::bind(&CallController::conferenceChanged,
                      this,
                      std::placeholders::_1,
                      std::placeholders::_2,
                      std::placeholders::_3)),
        libsip_core::exportable_callback<libsip_core::CallSignal::ConferenceRemoved>(
            std::bind(&CallController::conferenceRemoved,
                      this,
                      std::placeholders::_1,
                      std::placeholders::_2)),
        libsip_core::exportable_callback<libsip_core::CallSignal::OnConferenceInfosUpdated>(
            std::bind(&CallController::confInfoChanged,
                      this,
                      std::placeholders::_1,
                      std::placeholders::_2)),
    };

    libsip_core::registerSignalHandlers(sigMap);

    if (!libsip_core::init(static_cast<libsip_core::InitFlag>(3)))
        return false;

    std::string cwd;
    char buffer[PATH_MAX];
    if (getcwd(buffer, sizeof(buffer)) != nullptr) {
        cwd = buffer;
    }

    if (!libsip_core::start(cwd + "/test.yaml", ""))
        return false;

    // Initialize SDL
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_Log("Failed to initialize SDL: %s", SDL_GetError());
        return false;
    }

    EVENT_CREATE_PREVIEW = SDL_RegisterEvents(3);
    if (EVENT_CREATE_PREVIEW == (Uint32) 0)
        return false;

    EVENT_FRAME_READY = EVENT_CREATE_PREVIEW + 1;
    EVENT_DESTROY_PREVIEW = EVENT_CREATE_PREVIEW + 2;

    return true;
}

bool
CallController::sendRegister(const std::string& user,
                             const std::string& pass,
                             const std::string& domain)
{
    std::lock_guard<std::mutex> lock(m_mtxEvents);

    if (!libsip_core::initialized())
        return false;

    bool needToCreateNew = true;
    auto accounts = libsip_core::getAccountList();
    for (auto acc : accounts) {
        if (acc == m_accountId) {
            needToCreateNew = false;
            break;
        }
    }

    std::string actualDomain;
    std::string actualUser;

    if (needToCreateNew) {
        std::map<std::string, std::string> account;
        account["Account.type"] = "SIP";
        account["Account.upnpEnabled"] = "false";

        actualUser = user;
        actualDomain = domain;

        account["Account.username"] = actualUser;
        account["Account.hostname"] = actualDomain;
        account["Account.password"] = pass;
        account["Account.localPort"] = "0";
        account["Account.localModeratorsEnabled"] = "true";
        account["Account.allModeratorsEnabled"] = "false";
        account["Account.allowIPAutoRewrite"] = "false";
        account["SRTP.keyExchange"] = "";
        account["Account.transport"] = "udp";
        account["Account.audioPortMin"] = "60000";
        account["Account.audioPortMax"] = "61000";
        account["Account.videoPortMin"] = "49152";
        account["Account.videoPortMax"] = "65534";
        libsip_core::addAccount(account, m_accountId);
    } else {
        auto details = libsip_core::getAccountDetails(m_accountId);
        actualUser = details["Account.username"];
        actualDomain = details["Account.hostname"];

        if (details["Account.password"].empty()) {
            std::cout << "No password set, inferring it..." << std::endl;
            details["Account.password"] = pass;
            libsip_core::setAccountDetails(m_accountId, details);
        }
    }

    std::cout << "Registring user - " << user << "..." << std::endl;

    libsip_core::sendRegister(m_accountId, true);

    m_user = actualUser;
    m_domain = actualDomain;
    return true;
}

bool
CallController::call(const std::string& callTo)
{
    std::lock_guard<std::mutex> lock(m_mtxEvents);
    if (!m_activeCalls.empty() || !m_activeConfirence.empty())
        return false;

    // build media list settings according to settings
    std::vector<std::map<std::string, std::string>> mediaList;
    mediaList.push_back(m_mediaAudio);
    if (m_isVideoEnabled)
        mediaList.push_back(m_mediaVideo);

    std::string id = libsip_core::placeCallWithMedia(m_accountId,
                                                     toSipUri(callTo, m_domain),
                                                     mediaList);

    if (id.empty())
        return false;

    m_activeCalls[callTo] = id;
    return true;
}

bool
CallController::hasActiveCall() const
{
    std::lock_guard<std::mutex> lock(m_mtxEvents);
    return !m_activeConfirence.empty() || m_activeCalls.size() != 0;
}

const std::string
CallController::getActiveCall() const
{
    std::lock_guard<std::mutex> lock(m_mtxEvents);
    if (!m_activeConfirence.empty())
        return m_activeConfirence;
    else if (m_activeCalls.size() != 0) {
        return m_activeCalls.begin()->second;
    } else
        return "";
}

bool
CallController::addParticipant(const std::string& newParticipant)
{
    std::lock_guard<std::mutex> lock(m_mtxEvents);
    if (m_activeCalls.empty())
        return false;

    // build media list settings according to settings
    std::vector<std::map<std::string, std::string>> mediaList;
    mediaList.push_back(m_mediaAudio);
    if (m_isVideoEnabled)
        mediaList.push_back(m_mediaVideo);

    // Create call
    auto callId = libsip_core::placeCallWithMedia(m_accountId, newParticipant, mediaList);
    if (callId.empty())
        return false;

    bool result;
    if (m_activeConfirence.empty())
        result = libsip_core::joinParticipant(m_accountId,
                                              m_activeCalls.begin()->second,
                                              m_accountId,
                                              callId,
                                              true);
    else
        result = libsip_core::addParticipant(m_accountId, callId, m_accountId, m_activeConfirence);

    if (!result) {
        libsip_core::hangUp(m_accountId, callId);
        return false;
    }

    m_activeCalls[newParticipant] = callId;
    return true;
}

bool
CallController::removeParticipant(const std::string& participant)
{
    std::lock_guard<std::mutex> lock(m_mtxEvents);
    if (m_activeConfirence.empty())
        return false;

    auto it = m_activeCalls.find(participant);
    if (it == m_activeCalls.end())
        return false;

    m_activeCalls.erase(it);
    return libsip_core::detachParticipant(m_accountId, it->second);
}

bool
CallController::createConfirence(const std::vector<std::string>& participantsList)
{
    if (hasActiveCall() || participantsList.size() < 2)
        return false;

    if (!call(participantsList[0]))
        return false;

    if (!libsip_core::joinParticipant(m_accountId,
                                      m_activeCalls.begin()->second,
                                      m_accountId,
                                      participantsList[0],
                                      true)) {
        hangUp();
        return false;
    }

    int count = 1;
    for (int i = 1; i < participantsList.size(); i++) {
        // Create call
        auto callId = libsip_core::placeCallWithMedia(m_accountId, participantsList[i], {});
        if (callId.empty())
            continue;

        if (!libsip_core::addParticipant(m_accountId, callId, m_accountId, m_activeConfirence))
            continue;

        std::lock_guard<std::mutex> lock(m_mtxEvents);
        m_activeCalls[participantsList[i]] = callId;
    }

    if (m_activeCalls.size() < 2) {
        m_activeCalls.clear();
        hangUp();
        return false;
    }

    return true;
}

bool
CallController::isCaptureInProgress()
{
    return libsip_core::getIsRecording(m_accountId, getActiveCall());
}

bool
CallController::startCallCapture()
{
    if (libsip_core::getIsRecording(m_accountId, getActiveCall()))
        return true;

    return libsip_core::toggleRecording(m_accountId, getActiveCall());
}

bool
CallController::stopCallCapture()
{
    if (!libsip_core::getIsRecording(m_accountId, getActiveCall()))
        return true;

    // returns fasle if recodring stopped
    return !libsip_core::toggleRecording(m_accountId, getActiveCall());
}

void
CallController::toggleVideo()
{
    if (hasActiveCall()) {
        std::lock_guard<std::mutex> lock(m_mtxEvents);
        // build media list settings according to settings
        std::vector<std::map<std::string, std::string>> mediaList;
        mediaList.push_back(m_mediaAudio);
        if (m_isVideoEnabled)
            mediaList.push_back(m_mediaVideo);

        libsip_core::requestMediaChange(m_accountId, getActiveCall(), mediaList);
    }

    std::lock_guard<std::mutex> lock(m_mtxEvents);
    m_isVideoEnabled = !m_isVideoEnabled;
}

bool
CallController::isVideoEnabled() const
{
    std::lock_guard<std::mutex> lock(m_mtxEvents);
    return m_isVideoEnabled;
}

bool
CallController::setVideoDevice(const std::string& videoDevice)
{
    std::lock_guard<std::mutex> lock(m_mtxEvents);
    if (videoDevice.rfind("display://") == 0 || videoDevice.rfind("camera://") == 0) {
        m_mediaVideo["SOURCE"] = videoDevice;
    } else if (videoDevice == "default") {
        m_mediaVideo["SOURCE"] = libsip_core::getDefaultDevice();
    } else
        return false;

    return true;
}

std::vector<std::string>
CallController::getVideoDeviceList() const
{
    std::lock_guard<std::mutex> lock(m_mtxEvents);
    return libsip_core::getDeviceList();
}

const std::string
CallController::getVideoDevice() const
{
    std::lock_guard<std::mutex> lock(m_mtxEvents);
    static std::string source;
    source = m_mediaVideo.at("SOURCE");
    return source;
}

bool
CallController::hangUp()
{
    if (!hasActiveCall())
        return true;

    std::lock_guard<std::mutex> lock(m_mtxEvents);
    if (m_activeConfirence.empty()) {
        if (!libsip_core::hangUp(m_accountId, m_activeCalls.begin()->second))
            return false;
    } else {
        m_activeCalls.clear();
        if (!libsip_core::hangUpConference(m_accountId, m_activeConfirence))
            return false;
    }

    m_activeCalls.clear();
    return true;
}

void
CallController::proccesEvents()
{
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == EVENT_FRAME_READY) {
            std::unique_ptr<std::string> args((std::string*) event.user.data1);

            std::lock_guard<std::mutex> lock(m_mtxEvents);

            auto it = m_previewWindows.find(*args);
            if (it == m_previewWindows.end())
                return;

            auto ptrWindow = it->second;
            ptrWindow->render();
        } else if (event.type == EVENT_CREATE_PREVIEW) {
            std::unique_ptr<CreateNewPreviewArgs> args((CreateNewPreviewArgs*) event.user.data1);

            std::lock_guard<std::mutex> lock(m_mtxEvents);
            if (m_previewWindows.find(args->id) == m_previewWindows.end()) {
                if (!OpenVideoPrievew(args->id, args->w, args->h)) {
                    std::cerr << "Error: failed to create window for " << args->id << "."
                              << std::endl;
                    return;
                }
            }

            auto ptrWindow = m_previewWindows.find(args->id)->second;
            libsip_core::SinkTarget target;
            target.preferredFormat = AV_PIX_FMT_RGBA;

            target.push = [this, id = args->id, ptrWindow](libsip_core::FrameBuffer frame) {
                ptrWindow->update(frame);

                std::string* new_args = new std::string(id);
                SDL_Event event;
                SDL_zero(event);
                event.type = EVENT_FRAME_READY;
                event.user.code = 1;
                event.user.data1 = (void*) new_args;
                if (!SDL_PushEvent(&event))
                    delete new_args;
            };

            if (!libsip_core::registerSinkTarget(args->id, target)) {
                std::cerr << "Error: unable to register sink target for: " << args->id << "."
                          << std::endl;
            }
        } else if (event.type == EVENT_DESTROY_PREVIEW) {
            std::unique_ptr<std::string> args((std::string*) event.user.data1);

            std::lock_guard<std::mutex> lock(m_mtxEvents);
            CloseVideoPreview(*args);
        }
    }
}

std::string
CallController::toSipUri(const std::string& number, const std::string& domainName)
{
    std::smatch match;
    std::regex pattern;

    // Case 1: Already full SIP URI with domain (sip:X@Y)
    pattern = std::regex(R"(^sip:(.+@.+))");
    if (regex_search(number, match, pattern)) {
        return number;
    }

    // Case 2: SIP URI without domain (sip:X)
    pattern = std::regex(R"(^sip:(.+))");
    if (regex_search(number, match, pattern)) {
        return "sip:" + match[1].str() + "@" + domainName;
    }

    // Case 3: Already has user@domain but no SIP prefix (X@Y)
    pattern = std::regex(R"(^(.+@.+))");
    if (regex_search(number, match, pattern)) {
        return "sip:" + number;
    }

    // Case 4: Partial user@ format (X@)
    pattern = std::regex(R"(^(.+)@)");
    if (regex_search(number, match, pattern)) {
        return "sip:" + number + domainName;
    }

    // Default case: Simple username
    return "sip:" + number + "@" + domainName;
}

bool
CallController::OpenVideoPrievew(const std::string& id, int width, int height)
{
    if (m_previewWindows.find(id) != m_previewWindows.end())
        return false;

    auto sdlWindow = std::shared_ptr<SDLVideoRenderer>(
        new SDLVideoRenderer(m_user + " - " + id, width, height));
    if (!sdlWindow->init())
        return false;

    m_previewWindows[id] = sdlWindow;

    return true;
}

void
CallController::CloseVideoPreview(const std::string& id)
{
    auto it = m_previewWindows.find(id);

    if (it != m_previewWindows.end()) {
        m_previewWindows.erase(it);
    }

    libsip_core::registerSinkTarget(id, {});
}
