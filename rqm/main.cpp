#include <vector>
#include <csignal>

#include "manager.h"
#include "videomanager.h"
#include "sip/sipcall.h"
#include "sip/sipaccount.h"
#include "sip/sipaccount_config.h"

#include "directencoder.h"

using namespace sip_core;

const std::string RQM_ACCOUNT_ID = "RQM";

auto& manager = Manager::instance();

// Signal flag to detect Ctrl+C
static std::atomic<bool> keepRunning(true);

static std::weak_ptr<SIPCall> currentCall;

static DirectEncoder* directEncoder;

static const std::string rtp = "192.168.92.45:5060";

bool test_video = true;

// Signal handler for Ctrl+C (SIGINT)
void
signalHandler(int signum)
{
    keepRunning = false;
}

std::string
getWindowInfo(const std::string& windowName,
              const std::string& processName,
              const std::string& coords)
{
    Json::Value root;
    root["windowRectangle"] = coords;
    root["processName"] = processName;
    root["windowName"] = windowName;
    auto now = std::chrono::system_clock::now();
    auto timestamp = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
    root["time"] = timestamp;
    auto result = Json::writeString(Json::StreamWriterBuilder {}, root);
    return result;
}

// Function to wait for Ctrl+C or 'q'
void
waitForExit()
{
    // Register signal handler for Ctrl+C
    std::signal(SIGINT, signalHandler);

    while (keepRunning) {
        auto symbol = std::cin.peek();

        std::cout << "peaked symbol: " << char(symbol) << std::endl;
        if (symbol != EOF && symbol != '\n') { // Process only meaningful characters
            std::cin.ignore(1);                // Consume the character
            std::cout << "received " << static_cast<char>(symbol) << std::endl;
            if (symbol == 'q') {
                keepRunning = false;
            } else if (symbol == 's') {
                if (auto call = currentCall.lock()) {
                    auto info = getWindowInfo("main.cpp - sip_core",
                                              "VS Code",
                                              "120.220-320.220-320.420-120.42");
                    std::cout << "Sending json " << info << std::endl;
                    call->sendSIPInfo(info, "json");
                }
            }
        } else if (symbol == '\n') {
            std::cin.ignore(1); // Consume the newline
        }
        // Small sleep to avoid busy-waiting when no input is processed
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    std::cout << "Exiting..." << std::endl;
}

void
startDesktopRecording()
{}

int
main(int argc, char const* argv[])
{
    if (!libsip_core::init(
            static_cast<InitFlag>(LIBSIP_CORE_FLAG_DEBUG | LIBSIP_CORE_FLAG_CONSOLE_LOG))) {
        return 1;
    }

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

                currentCall = manager.callFactory.getCall<SIPCall>(callId);
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

        directEncoder = new DirectEncoder(rtp, params);

        // let the buffers go to our test renderers
        videoInput->attach(directEncoder);

        waitForExit();

    } else {
        rqm->doRegister();

        waitForExit();
    }

    return 0;
}
