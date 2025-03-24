#include <vector>
#include <csignal>

#include "manager.h"
#include "videomanager.h"
#include "sip/sipcall.h"
#include "sip/sipaccount.h"
#include "sip/sipaccount_config.h"

#include "direct_encoder.h"
#ifdef _WIN32
#include "window_tracker_win.h"
#include "tray_win.h"
#else
#include "window_tracker_linux.h"
#include "tray_linux.h"
#endif

using namespace sip_core;

const std::string RQM_APP_NAME = "RQM Desktop Recorder";

const std::string RQM_ACCOUNT_ID = "RQM";

auto& manager = Manager::instance();

static std::weak_ptr<SIPCall> currentCall;

std::unique_ptr<DirectEncoder>  directEncoder;

std::unique_ptr<WindowTracker> tracker;

std::unique_ptr<TrayIcon> tray;

static const std::string test_rtp = "192.168.92.45:5060";

bool test_video = false;


std::string
getWindowInfo(const std::string& windowName,
              const std::string& processName,
              const std::string& coords,
              const time_t time)
{
    Json::Value root;
    root["windowRectangle"] = coords;
    root["processName"] = processName;
    root["windowName"] = windowName;
    root["time"] = time;
    auto result = Json::writeString(Json::StreamWriterBuilder {}, root);
    return result;
}

// Callback function that will be called when window focus changes
void windowChangedCallback(const WindowInfo& info) {
    if (auto call = currentCall.lock()) {
        auto json = getWindowInfo(info.windowName,
                                    info.processName,
                                    info.windowRectangle,
                                    info.time);
        std::cout << "Sending json " << json << std::endl;
        call->sendSIPInfo(json, "json");
    }
}

int
main(int argc, char const* argv[])
{

    if (!libsip_core::init(
            static_cast<InitFlag>(LIBSIP_CORE_FLAG_DEBUG | LIBSIP_CORE_FLAG_CONSOLE_LOG))) {
        return 1;
    }

    sip_core::Logger::setFileLog("rqm.log");

    if (!libsip_core::start(".", std::nullopt)) {
        return 1;
    }


    // find the RQM account
    auto rqm = manager.findAccount<sip_core::SIPAccount>(
        [](const std::shared_ptr<sip_core::SIPAccount>& acc) {
            auto ok = acc->getAccountID() == RQM_ACCOUNT_ID;
            return ok;
        });

    if (!rqm) {
        return 1;
    }



#ifdef _WIN32
    tracker = std::make_unique<WindowTrackerWindows>(windowChangedCallback);
#else
    tracker = std::make_unique<WindowTrackerLinux>(windowChangedCallback);
#endif

#ifdef _WIN32
    tray = std::make_unique<TrayIconWindows>(RQM_APP_NAME, "rqm.ico");
#else
    tray = std::make_unique<TrayIconLinux>(RQM_APP_NAME, "rqm.png");
#endif

    // Initialize the tray icon
    if (!tray->init()) {
        std::cerr << "Failed to initialize tray icon!" << std::endl;
        return 1;
    }

    tray->showNotification("Внимание!", "Записыватель экрана работает из трея");

    SignalHandlerMap handlers = {
        exportable_callback<libsip_core::ConfigurationSignal::RegistrationStateChanged>(
            [](const std::string& accountId,
               const std::string& state,
               int details,
               const std::string& detailsStr) {
                std::cout << "RegistrationStateChanged: "
                          << " " << details << " " << state << std::endl;
            }),
        exportable_callback<libsip_core::CallSignal::IncomingCallWithMedia>(
            [](const std::string& accountId,
               const std::string& callId,
               const std::string& peerNumber,
               const std::vector<std::map<std::string, std::string>>& mediaList,
               const std::map<std::string, std::string>& headers) {
                std::cout << "IncomingCallWithMedia: "
                          << " from " << peerNumber << ", medias " << mediaList.capacity()
                          << std::endl;
                manager.answerCall(accountId, callId, mediaList);

                tracker->startTracking();

                currentCall = manager.callFactory.getCall<SIPCall>(callId);

                currentCall.lock()->addStateListener(
                    [](Call::CallState new_state, Call::ConnectionState new_cstate, int /* code */) {
                    if (new_state == Call::CallState::OVER)
                        tracker->stopTracking();
                    return true;
                    });
            }),
    };

    registerSignalHandlers(handlers);

    manager.getVideoManager().videoDeviceMonitor.setDefaultDevice("desktop");

    auto config = rqm->config();

    std::vector<SipAccountConfig::Credentials> creds {
        SipAccountConfig::Credentials("*", config.username, "1")};

    rqm->setCredentials(creds);

    if (test_video) {
        // desktop video input decoder.
        auto videoInput = getVideoInput(
            manager.getVideoManager().videoDeviceMonitor.getMRLForDefaultDevice());

        auto params = videoInput->getParams().get();

        directEncoder = std::make_unique<DirectEncoder>(test_rtp, params);

        // let the buffers go to our test renderers
        videoInput->attach(directEncoder.get());
    } else {
        rqm->doRegister();
    }

    // Main message loop
    while (tray->processMessages()) {
        // Sleep a bit to avoid high CPU usage
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    tracker->stopTracking();

    return 0;
}
