#include <iostream>
#include <vector>
#include <map>
#include <string>
#include <sstream>
#include <algorithm>
<<<<<<< HEAD
#include <unistd.h>
#include <regex>
=======
>>>>>>> 0ffc415ee (chore: example CallController implemented)

#include "CallController.h"

#ifdef _WIN32
#include <conio.h>  // Windows-specific for _getch()
#include <windows.h>
#define PATH_MAX MAX_PATH
#else
#include <climits>
#include <termios.h>
#include <unistd.h>
#endif

<<<<<<< HEAD
using namespace std;
#define ACCAUNT_ID "test_acc"
=======
#define ACCAUNT_ID "test_acc"

// getInput("Enter username: ");
// getInput("Enter domain: ");
// getPassword();
>>>>>>> 0ffc415ee (chore: example CallController implemented)

std::string username = "user";
std::string password = "pass";
std::string domain = "192.168.92.27";

std::string getPassword(const std::string& prompt);
std::string getInput(const std::string& prompt);
std::vector<std::string> split(const std::string &s);

int main() {

    std::cout << "SIP core Console App" << std::endl;
    std::cout << "Available commands: call <callee>, switch <device>, hangup, capOn, capOff, video, exit" << std::endl;

    CallController controller(ACCAUNT_ID);
    
    if(!controller.Init()) {
        std::cerr << "Error: can't initialize sip." << std::endl;
        return 1;
    }

    if(!controller.Register(username, password, domain)) {
        std::cerr << "Error: unable to send register for current account." << std::endl;
        return 1;
    }

<<<<<<< HEAD
    // Get list of input devices
    //auto inputs = sip_core::Manager::instance().getAudioInputDeviceList();
    // Get list of output devices
    //auto outputs = sip_core::Manager::instance().getAudioOutputDeviceList();
    //sip_core::Manager::instance().setAudioDevice(1, sip_core::AudioDeviceType::CAPTURE);
    //sip_core::Manager::instance().setAudioDevice(1, sip_core::AudioDeviceType::PLAYBACK);

    auto cameras = sip_core::Manager::instance().getVideoManager().videoDeviceMonitor.getDeviceList();
    for(auto camera : cameras) {
        std::cout << "Camera: " << camera << std::endl;
    }

    string line;
=======
    std::string line;
>>>>>>> 0ffc415ee (chore: example CallController implemented)
    while (true) {
        getline(std::cin, line);
        std::vector<std::string> tokens = split(line);
        if (tokens.empty()) continue;

        std::string command = tokens[0];
        transform(command.begin(), command.end(), command.begin(), ::tolower);

        if (command == "exit") {
            break;
        }
        else if (command == "call") {
            if (tokens.size() != 2) {
                std::cerr << "Error: Usage - call <callee>" << std::endl;
                continue;
            }
<<<<<<< HEAD
            
            // build media list settings according to settings
            vector<map<string, string>> mediaList;
            if(g_isAudioOn)
            mediaList.push_back(g_mediaAudio);
            if(g_isVideoOn)
            mediaList.push_back(g_mediaVideo);
            
            string callee = tokens[1];
            active_call = libsip_core::placeCallWithMedia(ACCAUNT_ID,
                toSipUri(callee, domain),
                mediaList);
                
            cout << "Call connected: " << username << " -> " << callee << "\n CallID = " << active_call << endl;
        } else if (command == "switch") {
            if (tokens.size() != 2) {
                cerr << "Error: Usage - switch <device>\n device could be of type:\n  display://:(screen_number)\n  camera://(camera_name)\n  default" << endl;
=======

            std::string callee = tokens[1];
            if(controller.Call(callee)) {
                std::cerr << "Error: already in an active call state" << std::endl;
>>>>>>> 0ffc415ee (chore: example CallController implemented)
                continue;
            }
                
            std::cout << "Call connected: " << username << " -> " << callee << "\n CallID = " << controller.getActiveCall() << std::endl;
        } else if (command == "switch") {
            if (tokens.size() != 2) {
                std::cerr << "Error: Usage - switch <device>\n device could be of type:\n  display://:(screen_number)\n  camera://(camera_name)\n  default" << std::endl;
                continue;
            }
            std::string device = tokens[1];
            if (!controller.setVideoDevice(device)) {
                std::cerr << "Error: Usage - switch <device>\n device could be of type:\n  display://:(screen_number)\n  camera://(camera_name)\n  default" << std::endl;
                continue;
            }

        } else if (command == "hangup") {
            if (!controller.hasActiveCall()) {
                std::cerr << "Error: no active call" << std::endl;
                continue;
            }
            if(!controller.HangUp()) {
                std::cerr << "Error: failed to hangup call: " <<  controller.getActiveCall() << std::endl;
                continue;
            }
        } else if (command == "capon") {
            if (!controller.hasActiveCall()) {
                std::cerr << "Error: no active call" << std::endl;
                continue;
            }
            
            if(controller.isCaptureInProgress()) {
                std::cerr << "Error: already recording" << std::endl;
            }

            if(!controller.startCallCapture()) {
                std::cerr << "Error: failed to start recording" << std::endl;
                continue;
            }
        } else if (command == "capoff") {
            if(controller.hasActiveCall()){
                std::cerr << "Error: no active call" << std::endl;
                continue;
            }
            if(!controller.isCaptureInProgress()) {
                std::cerr << "Error: nothing recording" << std::endl;
                continue;
            }
            if(!controller.stopCallCapture()) {
                std::cerr << "Error: failed to stop recording" << std::endl;
                continue;
            }
        } else if (command == "video") {
            if(controller.isVideoEnabled()) std::cout << "Disabling video..." << std::endl;
            else std::cout << "Enabling video..." << std::endl;

            controller.toggleVideo();
        } else {
            std::cerr << "Error: Unknown command. \nFull list of commands:\n call <callee> - initiates call with given ID,\n switch <device> - switches video source for an active call.\n hangup - hangup current call.\n capOn - start capture of active call in a local file.\n capOff - stops capture of video.\n video - enables video transfer.\n exit - exit program." << std::endl;
        }
    }

    return 0;
}

<<<<<<< HEAD
bool init_sip()
{
    const sip_core::SignalHandlerMap sigMap = {
        libsip_core::exportable_callback<libsip_core::ConfigurationSignal::RegistrationStateChanged>(&registrationStateChanged),
        libsip_core::exportable_callback<libsip_core::ConfigurationSignal::VolatileDetailsChanged>(&volatileDetailsChanged),
        libsip_core::exportable_callback<libsip_core::CallSignal::StateChange>(&callStateChanged),
        libsip_core::exportable_callback<libsip_core::CallSignal::IncomingCall>(&incomingCall),
        libsip_core::exportable_callback<libsip_core::CallSignal::IncomingCallWithMedia>(&incomingCallWithMedia),
        libsip_core::exportable_callback<libsip_core::CallSignal::MediaNegotiationStatus>(&mediaNegotiationStatus),
        libsip_core::exportable_callback<libsip_core::AudioSignal::DeviceEvent>(&audioDeviceEvent),
        libsip_core::exportable_callback<libsip_core::VideoSignal::StartCapture>(&startCapture),
        libsip_core::exportable_callback<libsip_core::VideoSignal::DecodingStarted>(&decodingStarted),
        libsip_core::exportable_callback<libsip_core::CallSignal::MediaChangeRequested>(&mediaChange)
    };

    libsip_core::registerSignalHandlers(sigMap);
    
    if (!libsip_core::init(static_cast<libsip_core::InitFlag>(0)))
            return false;

    string cwd;
    char buffer[PATH_MAX];
    if (getcwd(buffer, sizeof(buffer)) != nullptr) {
        cwd = buffer;
    }
    
    if(!libsip_core::start(cwd + "/test.yaml", ""))
        return false;

    return true;
}

bool register_accaunt()
{
    map<string, string> account;

    account["Account.type"] = "SIP";
    account["Account.upnpEnabled"] = "false";
    account["Account.username"] = username; // getInput("Enter username: ");
    account["Account.hostname"] = domain; // getInput("Enter domain: ");
    account["Account.password"] = password; // getPassword();
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

    bool needToCreateNew = true;
    auto accounts = libsip_core::getAccountList();
    for(auto acc : accounts) {
        if(acc == ACCAUNT_ID) {
            needToCreateNew = false;
            break;
        }
    }
    if(needToCreateNew) libsip_core::addAccount(account, ACCAUNT_ID);
    else libsip_core::setAccountDetails(ACCAUNT_ID, account);
    
    libsip_core::sendRegister(ACCAUNT_ID, true);

    return true;
}

string toSipUri(const string& number, const string& domainName) {
    smatch match;
    regex pattern;

    // Case 1: Already full SIP URI with domain (sip:X@Y)
    pattern = regex(R"(^sip:(.+@.+))");
    if (regex_search(number, match, pattern)) {
        return number;
    }

    // Case 2: SIP URI without domain (sip:X)
    pattern = regex(R"(^sip:(.+))");
    if (regex_search(number, match, pattern)) {
        return "sip:" + match[1].str() + "@" + domainName;
    }

    // Case 3: Already has user@domain but no SIP prefix (X@Y)
    pattern = regex(R"(^(.+@.+))");
    if (regex_search(number, match, pattern)) {
        return "sip:" + number;
    }

    // Case 4: Partial user@ format (X@)
    pattern = regex(R"(^(.+)@)");
    if (regex_search(number, match, pattern)) {
        return "sip:" + number + domainName;
    }

    // Default case: Simple username
    return "sip:" + number + "@" + domainName;
}

=======
>>>>>>> 0ffc415ee (chore: example CallController implemented)
// Function to hide password input (cross-platform)
std::string getPassword(const std::string& prompt = "Enter password: ") {
    std::string password;
    std::cout << prompt;
    
    #ifdef _WIN32
    // Windows implementation (no echo)
    char ch;
    while ((ch = _getch()) != '\r') {  // Enter key
        if (ch == '\b') {  // Backspace
            if (!password.empty()) {
                password.pop_back();
                std::cout << "\b \b";  // Erase asterisk
            }
        } else {
            password.push_back(ch);
            std::cout << '*';
        }
    }
    #else
    // Linux/macOS implementation (no echo)
    termios oldt;
    tcgetattr(STDIN_FILENO, &oldt);
    termios newt = oldt;
    newt.c_lflag &= ~ECHO;
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);
    
    getline(std::cin, password);
    
    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);  // Restore terminal settings
    #endif

    std::cout << std::endl;
    return password;
}

std::string getInput(const std::string& prompt) {
    std::string input;
    std::cout << prompt;
    getline(std::cin, input);
    return input;
}

std::vector<std::string> split(const std::string &s) {
    std::vector<std::string> tokens;
    std::string token;
    std::istringstream iss(s);
    while (iss >> token) {
        tokens.push_back(token);
    }
    return tokens;
}