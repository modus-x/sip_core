#include <vector>
#include <csignal>

#include "manager.h"
#include "videomanager.h"
#include "sip/sipaccount.h"
#include "sip/sipaccount_config.h"

using namespace sip_core;

const std::string RQM_ACCOUNT_ID = "RQM";

auto& manager = Manager::instance();

// Signal flag to detect Ctrl+C
static std::atomic<bool> keepRunning(true);

// Signal handler for Ctrl+C (SIGINT)
void
signalHandler(int signum)
{
    keepRunning = false;
}

// Function to wait for Ctrl+C or 'q'
void
waitForExit()
{
    // Register signal handler for Ctrl+C
    std::signal(SIGINT, signalHandler);

    // Set up non-blocking input (optional, but keeps it simple)
    while (keepRunning) {
        std::cout << "keep running..." << std::endl;
        // Check for 'q' without blocking too long
        if (std::cin.peek() == 'q') {
            keepRunning = false;
        }
        // Small sleep to avoid busy-waiting
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
                sip_core::Manager::instance().answerCall(accountId, callId, mediaList);
            }),
    };

    registerSignalHandlers(handlers);

    sip_core::Manager::instance().getVideoManager().videoDeviceMonitor.setDefaultDevice("desktop");

    auto config = rqm->config();

    std::vector<SipAccountConfig::Credentials> creds {
        SipAccountConfig::Credentials("*", config.username, "1qazxsw2")};

    rqm->setCredentials(creds);

    rqm->doRegister();

    waitForExit();

    return 0;
}
