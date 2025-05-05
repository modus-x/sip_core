#include <iostream>
#include <vector>
#include <map>
#include <string>
#include <sstream>
#include <algorithm>
#include <unistd.h>
#include <regex>

#include "SignalHandlers.h"
#include "sip_core/callmanager_interface.h"
#include "sip_core/configurationmanager_interface.h"
#include "client/ring_signal.h"
#include "client/videomanager.h"
#include "manager.h"

#ifdef _WIN32
#include <conio.h>  // Windows-specific for _getch()
#include <windows.h>
#define PATH_MAX MAX_PATH
#else
#include <climits>
#include <termios.h>
#include <unistd.h>
#endif

#define ACCAUNT_ID "test_acc"

using namespace std;

string username = "username";
string password = "password";
string domain = "192.168.92.27";


bool g_isAudioOn = true;
bool g_isVideoOn = false;

map<string, string> g_mediaAudio
{
    { "MEDIA_TYPE", "MEDIA_TYPE_AUDIO"},
    { "ENABLED", "true" },
    { "MUTED", "false" },
    { "LABEL", "audio_0" }
};

map<string, string> g_mediaVideo
{
    { "MEDIA_TYPE", "MEDIA_TYPE_VIDEO"},
    { "ENABLED", "true" },
    { "MUTED", "false" },
    //{ "SOURCE", "display://:0.0" },
    { "LABEL", "video_0" }
};

string toSipUri(const string& number, const string& domainName);
std::string getPassword(const std::string& prompt);
string getInput(const string& prompt);
vector<string> split(const string &s);
bool init_sip();
bool register_accaunt();

string active_call {};

int main() {

    cout << "SIP core Console App" << endl;
    cout << "Available commands: call <callee>, switch <device>, hangup, capOn, capOff, video, audio, exit" << endl;

    if(!init_sip()) {
        cerr << "Error: can't initialize sip." << endl;
        return 1;
    }

    if(!register_accaunt()) {
        cerr << "Error: unable to register account" << endl;
        return 1;
    }

    // Get list of input devices
    //auto inputs = sip_core::Manager::instance().getAudioInputDeviceList();
    // Get list of output devices
    //auto outputs = sip_core::Manager::instance().getAudioOutputDeviceList();
    //sip_core::Manager::instance().setAudioDevice(1, sip_core::AudioDeviceType::CAPTURE);
    //sip_core::Manager::instance().setAudioDevice(1, sip_core::AudioDeviceType::PLAYBACK);

    string line;
    while (true) {
        getline(cin, line);
        vector<string> tokens = split(line);
        if (tokens.empty()) continue;

        string command = tokens[0];
        transform(command.begin(), command.end(), command.begin(), ::tolower);

        if (command == "exit") {
            break;
        }
        else if (command == "call") {
            if (tokens.size() != 2) {
                cerr << "Error: Usage - call <callee>" << endl;
                continue;
            }
            if(!active_call.empty()) {
                cerr << "Error: already in an active call state" << endl;
                continue;
            }

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
                continue;
            }
            string device = tokens[1];
            if (device.rfind("display://") == 0 || device.rfind("camera://") == 0) {
                g_mediaVideo["SOURCE"] = device;
            } else if(device == "default") {
                g_mediaVideo["SOURCE"] = libsip_core::getDefaultDevice();
            } else {
                cerr << "Error: Usage - switch <device>\n device could be of type:\n  display://:(screen_number)\n  camera://(camera_name)\n  default" << endl;
                continue;
            }

        } else if (command == "hangup") {
            if (active_call.empty()) {
                cerr << "Error: no active call" << endl;
                continue;
            }
            if(!libsip_core::hangUp(ACCAUNT_ID, active_call)) {
                cerr << "Error: failed to hangup call: " <<  active_call << endl;
                continue;
            }
            
            active_call = "";
        } else if (command == "capon") {
            if(active_call.empty()){
                cerr << "Error: no active call" << endl;
                continue;
            }
            
            if(libsip_core::getIsRecording(ACCAUNT_ID, active_call)) {
                cerr << "Error: already recording" << endl;
            }

            if(!libsip_core::toggleRecording(ACCAUNT_ID, active_call)) {
                cerr << "Error: failed to start recording" << endl;
                continue;
            }
        } else if (command == "capoff") {
            if(active_call.empty()){
                cerr << "Error: no active call" << endl;
                continue;
            }
            if(!libsip_core::getIsRecording(ACCAUNT_ID, active_call)) {
                cerr << "Error: nothing recodring" << endl;
                continue;
            }
            if(!libsip_core::toggleRecording(ACCAUNT_ID, active_call)) {
                cerr << "Error: failed to stop recording" << endl;
                continue;
                
            }
        } else if (command == "audio") {
            if(g_isAudioOn) cout << "Disabling audio..." << endl;
            else cout << "Enabling audio..." << endl;
            g_isAudioOn = !g_isAudioOn;

            if(!active_call.empty()) {
                // build media list settings according to settings
                vector<map<string, string>> mediaList;
                if(g_isAudioOn) mediaList.push_back(g_mediaAudio);
                if(g_isVideoOn) mediaList.push_back(g_mediaVideo);

                libsip_core::requestMediaChange(ACCAUNT_ID, active_call, mediaList);
            }
        } else if (command == "video") {
            if(g_isVideoOn) cout << "Disabling video..." << endl;
            else cout << "Enabling video..." << endl;
            g_isVideoOn = !g_isVideoOn;

            if(!active_call.empty()) {
                // build media list settings according to settings
                vector<map<string, string>> mediaList;
                if(g_isAudioOn) mediaList.push_back(g_mediaAudio);
                if(g_isVideoOn) mediaList.push_back(g_mediaVideo);

                libsip_core::requestMediaChange(ACCAUNT_ID, active_call, mediaList);
            }
        } else {
            cerr << "Error: Unknown command. \nFull list of commands:\n call <callee> - initiates call with given ID,\n switch <device> - switches video source for an active call.\n hangup - hangup current call.\n capOn - start capture of active call in a local file.\n capOff - stops capture of video.\n video - enables video transfer.\n audio -enables audio transfer.\n exit - exit program." << endl;
        }
    }

    if(libsip_core::initialized()) {
        libsip_core::fini();
    }

    return 0;
}

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

// Function to hide password input (cross-platform)
string getPassword(const string& prompt = "Enter password: ") {
    string password;
    cout << prompt;
    
    #ifdef _WIN32
    // Windows implementation (no echo)
    char ch;
    while ((ch = _getch()) != '\r') {  // Enter key
        if (ch == '\b') {  // Backspace
            if (!password.empty()) {
                password.pop_back();
                cout << "\b \b";  // Erase asterisk
            }
        } else {
            password.push_back(ch);
            cout << '*';
        }
    }
    #else
    // Linux/macOS implementation (no echo)
    termios oldt;
    tcgetattr(STDIN_FILENO, &oldt);
    termios newt = oldt;
    newt.c_lflag &= ~ECHO;
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);
    
    getline(cin, password);
    
    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);  // Restore terminal settings
    #endif

    cout << endl;
    return password;
}

string getInput(const string& prompt) {
    string input;
    cout << prompt;
    getline(std::cin, input);
    return input;
}

vector<string> split(const string &s) {
    vector<string> tokens;
    string token;
    istringstream iss(s);
    while (iss >> token) {
        tokens.push_back(token);
    }
    return tokens;
}
