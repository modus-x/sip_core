#include "CallController.h"
#include "callmanager_interface.h"
#include "configurationmanager_interface.h"
#include "presencemanager_interface.h"
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

CallController::CallController(const std::string& accountId, bool enableVideo /* = true */)
    : m_mtxEvents()
#ifdef ENABLE_VIDEO
    , m_isVideoEnabled(enableVideo)
    , m_mediaVideo {{"MEDIA_TYPE", "MEDIA_TYPE_VIDEO"},
                    {"ENABLED", "true"},
                    {"MUTED", "false"},
                    {"LABEL", "video_0"}}
#endif
    , m_mediaAudio {{"MEDIA_TYPE", "MEDIA_TYPE_AUDIO"},
                    {"ENABLED", "true"},
                    {"MUTED", "false"},
                    {"LABEL", "audio_0"}}
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
        libsip_core::exportable_callback<libsip_core::CallSignal::MediaChangeRequested>(
            std::bind(&CallController::mediaChangeRequest, 
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

    if (!libsip_core::init(static_cast<libsip_core::InitFlag>(0)))
        return false;

    std::string cwd;
    char buffer[PATH_MAX];
    if (getcwd(buffer, sizeof(buffer)) != nullptr) {
        cwd = buffer;
    }

    if (!libsip_core::start(cwd + "/test.yaml", ""))
        return false;
    
    const std::vector<unsigned> active_codecs = 
    {
        // audio
        AV_CODEC_ID_OPUS, AV_CODEC_ID_PCM_ALAW, AV_CODEC_ID_PCM_MULAW,
        // video 
        AV_CODEC_ID_H264, AV_CODEC_ID_VP8, AV_CODEC_ID_VP9 
    };
    libsip_core::setActiveCodecList(m_accountId, active_codecs);

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

void
CallController::publishPresence(bool available, const std::string& note)
{
    std::lock_guard<std::recursive_mutex> lock(m_mtxEvents);
    if (!libsip_core::initialized())
        return;

    libsip_core::publish(m_accountId, available, note);
}

bool
CallController::sendRegister(const std::string& user,
                             const std::string& pass,
                             const std::string& domain,
                             const std::string& binding)
{
    std::lock_guard<std::recursive_mutex> lock(m_mtxEvents);

    if (!libsip_core::initialized())
        return false;

    std::string actualUser = user;
    std::string actualPass = pass;
    std::string actualDomain = domain;
    if(pass.empty()) {
        std::cout << "Warn: some of the requeired fields are not set.\n" <<
                     "Using defautl account:" <<
                     "\n\tusername - " << m_defualt_username <<
                     "\n\tdomain - " << m_default_domain << std::endl << std::endl;
        
        actualUser = m_defualt_username;
        actualPass = m_default_password;
        actualDomain = m_default_domain;
    }

    bool needToCreateNew = true;
    auto accounts = libsip_core::getAccountList();
    for (auto acc : accounts) {
        if (acc == m_accountId) {
            needToCreateNew = false;
            break;
        }
    }

    if (needToCreateNew) {
        std::map<std::string, std::string> account;
        account["Account.type"] = "SIP";
        account["Account.upnpEnabled"] = "false";
        account["Account.username"] = actualUser;
        account["Account.hostname"] = actualDomain;
        account["Account.password"] = actualPass;
        account["Account.bindAddress"] = binding;
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
        if(not actualUser.empty())
            details["Account.username"] = actualUser;
        if(not actualDomain.empty())
            details["Account.hostname"] = actualDomain;
        if (not actualPass.empty())
            details["Account.password"] = actualPass;
        if(not binding.empty())
            details["Account.bindAddress"] = binding;
        libsip_core::setAccountDetails(m_accountId, details);
    }

    m_user = actualUser;
    m_domain = actualDomain;

    libsip_core::registerEventPackage("x-lostcalls", 600);

    if(not m_user.empty() && not m_domain.empty()) {
        std::cout << "Registering user - " << actualUser << "..." << std::endl;
        libsip_core::sendRegister(m_accountId, true);
    }
    else {
        std::cerr << "Error: no valid username or domain address given.\n" 
                     "Use --user, --domain, --pass options or edit test.yaml directly...\n\n";
        return false;
    }

    return true;
}

bool
CallController::unregister()
{
    std::lock_guard<std::recursive_mutex> lock(m_mtxEvents);

    if (!libsip_core::initialized())
        return false;

    std::cout << "Unregistering user - " << m_user << "..." << std::endl;

    libsip_core::sendRegister(m_accountId, false);

    return true;
}

void
CallController::subscribe(const std::vector<std::string>& uris)
{
    std::lock_guard<std::recursive_mutex> lock(m_mtxEvents);
    if (!libsip_core::initialized())
        return;

    std::vector<std::string> sipUris;
    for (const auto& uri : uris) {
        sipUris.push_back(toSipUri(uri, m_domain));
    }

    std::cout << "Subscribing to events for specified URIs..." << std::endl;
    for (const auto& uri : sipUris) {
        libsip_core::subscribeToEvents(m_accountId, uri, "x-lostcalls", true);
    }
}

void
CallController::unsubscribe(const std::vector<std::string>& uris)
{
    std::lock_guard<std::recursive_mutex> lock(m_mtxEvents);
    if (!libsip_core::initialized())
        return;

    std::vector<std::string> sipUris;
    for (const auto& uri : uris) {
        sipUris.push_back(toSipUri(uri, m_domain));
    }

    std::cout << "Unsubscribing from events for specified URIs..." << std::endl;
    for (const auto& uri : sipUris) {
        libsip_core::subscribeToEvents(m_accountId, uri, "x-lostcalls", false);
    }
}

bool
CallController::call(const std::string& callTo)
{
    std::lock_guard<std::recursive_mutex> lock(m_mtxEvents);
    if (hasActiveCall())
        return false;

    // build media list settings according to settings
    std::vector<std::map<std::string, std::string>> mediaList;
    mediaList.push_back(m_mediaAudio);
#ifdef ENABLE_VIDEO
    // if (m_isVideoEnabled) 
    //     mediaList.push_back(m_mediaVideo);
    if (m_isVideoEnabled) 
        m_mediaVideo["ENABLED"] = "true";
    else 
        m_mediaVideo["ENABLED"] = "false";
    mediaList.push_back(m_mediaVideo);
#endif

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
    std::lock_guard<std::recursive_mutex> lock(m_mtxEvents);
    return !m_activeConfirence.empty() || m_activeCalls.size() != 0;
}

const std::string
CallController::getActiveCall() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mtxEvents);
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
    std::lock_guard<std::recursive_mutex> lock(m_mtxEvents);
    if (m_activeCalls.empty())
        return false;

    // build media list settings according to settings
    std::vector<std::map<std::string, std::string>> mediaList;
    mediaList.push_back(m_mediaAudio);
#ifdef ENABLE_VIDEO
    // if (m_isVideoEnabled) 
    //     mediaList.push_back(m_mediaVideo);
    if (m_isVideoEnabled) 
        m_mediaVideo["ENABLED"] = "true";
    else 
        m_mediaVideo["ENABLED"] = "false";
    mediaList.push_back(m_mediaVideo);
#endif

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
    std::lock_guard<std::recursive_mutex> lock(m_mtxEvents);
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

        std::lock_guard<std::recursive_mutex> lock(m_mtxEvents);
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
CallController::moveParticipant(size_t from_index, size_t to_index)
{
    return libsip_core::moveParticipant(m_accountId, m_activeConfirence, from_index, to_index);
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

bool
CallController::hold()
{
    std::lock_guard<std::recursive_mutex> lock(m_mtxEvents);
    if (!hasActiveCall())
        return false;

    if (m_activeConfirence.empty()) {
        return libsip_core::hold(m_accountId, m_activeCalls.begin()->second);
    } else {
        return libsip_core::holdConference(m_accountId, m_activeConfirence);
    }
}

bool
CallController::resume()
{
    std::lock_guard<std::recursive_mutex> lock(m_mtxEvents);
    if (!hasActiveCall())
        return false;

    if (m_activeConfirence.empty()) {
        return libsip_core::unhold(m_accountId, m_activeCalls.begin()->second);
    } else {
        return libsip_core::unholdConference(m_accountId, m_activeConfirence);
    }
}

void
CallController::enableVideo(bool enabled)
{
#ifdef ENABLE_VIDEO
    std::lock_guard<std::recursive_mutex> lock(m_mtxEvents);
    if(m_isVideoEnabled == enabled)
        return;

    m_isVideoEnabled = enabled;

    if (hasActiveCall()) {
        // build media list settings according to settings
        std::vector<std::map<std::string, std::string>> mediaList;
        mediaList.push_back(m_mediaAudio);
        // if (m_isVideoEnabled) 
        //     mediaList.push_back(m_mediaVideo);
        if (m_isVideoEnabled) 
            m_mediaVideo["ENABLED"] = "true";
        else 
            m_mediaVideo["ENABLED"] = "false";
        mediaList.push_back(m_mediaVideo);
        
        libsip_core::requestMediaChange(m_accountId, getActiveCall(), mediaList);
    }

#elif
    std::cerr << "Video is unsupported by a kernel build." << std::endl;
#endif
}

bool
CallController::isVideoEnabled() const
{
#ifdef ENABLE_VIDEO
    std::lock_guard<std::recursive_mutex> lock(m_mtxEvents);
    return m_isVideoEnabled;
#elif
    return false;
#endif
}

void
CallController::enableHWAccel(bool enabled)
{
#ifdef RING_ACCEL
    std::lock_guard<std::recursive_mutex> lock(m_mtxEvents);
    if(isHWAccelEnabled() == enabled) 
        return;

    if(enabled) {
        sip_core::Manager::instance().videoPreferences.setDecodingAccelerated(true);
        sip_core::Manager::instance().videoPreferences.setEncodingAccelerated(true);
    } else {
        sip_core::Manager::instance().videoPreferences.setDecodingAccelerated(false);
        sip_core::Manager::instance().videoPreferences.setEncodingAccelerated(false);
    }

    sip_core::Manager::instance().saveConfig();
#elif
    std::cerr << "Video hardware acceleration is unsupported by a kernel build." << std::endl;
#endif
}

bool
CallController::isHWAccelEnabled() const
{
#ifdef RING_ACCEL
    std::lock_guard<std::recursive_mutex> lock(m_mtxEvents);
    return sip_core::Manager::instance().videoPreferences.getDecodingAccelerated() && sip_core::Manager::instance().videoPreferences.getEncodingAccelerated();
#elif
    return false;
#endif
}

bool
CallController::setVideoDevice(const std::string& videoDevice)
{
    std::lock_guard<std::recursive_mutex> lock(m_mtxEvents);
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
    std::lock_guard<std::recursive_mutex> lock(m_mtxEvents);
    return libsip_core::getDeviceList();
}

const std::string
CallController::getVideoDevice() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mtxEvents);
    static std::string source;
    source = m_mediaVideo.at("SOURCE");
    return source;
}

std::map<std::string, std::string>
CallController::getCallDetails(const std::string& callId)
{
    return libsip_core::getCallDetails(m_accountId, callId);
}

std::vector<std::string>
CallController::getAudioCaptureDeviceList() const
{
    return sip_core::Manager::instance().getAudioInputDeviceList();
}

std::vector<std::string>
CallController::getAudioPlaybackDeviceList() const
{
    return sip_core::Manager::instance().getAudioOutputDeviceList();
}

void
CallController::setAudioCaptureDevice(int index)
{
    sip_core::Manager::instance().setAudioDevice(index, sip_core::AudioDeviceType::CAPTURE);
}

void
CallController::setAudioPlaybackDevice(int index)
{
    sip_core::Manager::instance().setAudioDevice(index, sip_core::AudioDeviceType::PLAYBACK);
}

bool
CallController::hangUp()
{
    if (!hasActiveCall())
        return true;

    std::lock_guard<std::recursive_mutex> lock(m_mtxEvents);
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

            std::lock_guard<std::recursive_mutex> lock(m_mtxEvents);

            auto it = m_previewWindows.find(*args);
            if (it == m_previewWindows.end())
                return;

            auto ptrWindow = it->second;
            ptrWindow->render();
        } else if (event.type == EVENT_CREATE_PREVIEW) {
            std::unique_ptr<CreateNewPreviewArgs> args((CreateNewPreviewArgs*) event.user.data1);

            std::lock_guard<std::recursive_mutex> lock(m_mtxEvents);
            if (m_previewWindows.find(args->id) == m_previewWindows.end()) {
                if (!OpenVideoPrievew(args->id, args->w, args->h)) {
                    std::cerr << "Error: failed to create window for " << args->id << "."
                              << std::endl;
                    return;
                }
            }

            auto ptrWindow = m_previewWindows.find(args->id)->second;
            libsip_core::SinkTarget target;
            target.preferredFormat = AV_PIX_FMT_ARGB;

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

            std::lock_guard<std::recursive_mutex> lock(m_mtxEvents);
            CloseVideoPreview(*args);
        }
    }
}

std::string
CallController::toSipUri(const std::string& number, const std::string& domainName)
{
    std::smatch match;
    std::regex pattern;

    // Case 0. Subscription to domain!
    if (number == domainName) {
        return "sip:" + number;
    }

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
