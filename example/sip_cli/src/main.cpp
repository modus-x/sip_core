#include <iostream>
#include <vector>
#include <map>
#include <string>
#include <sstream>
#include <algorithm>
#include <unistd.h>
#include <climits>
#include <regex>

#include "SignalHandlers.h"
#include "sip_core/callmanager_interface.h"
#include "sip_core/configurationmanager_interface.h"
#include "client/ring_signal.h"
#include "manager.h"

#ifdef _WIN32
#include <conio.h>  // Windows-specific for _getch()
#else
#include <termios.h>
#include <unistd.h>
#endif

#define ACCAUNT_ID "test_acc"

using namespace std;

string username = "your_user_name";
string password = "your_password";
string domain = "your_domain";

vector<map<string, string>> mediaList {
    {
        { "MEDIA_TYPE", "MEDIA_TYPE_AUDIO"},
        { "ENABLED", "true" },
        { "MUTED", "false" },
        { "LABEL", "audio_0" }
    },
    {
        { "MEDIA_TYPE", "MEDIA_TYPE_VIDEO"},
        { "ENABLED", "true" },
        { "MUTED", "false" },
        { "LABEL", "video_0" }
    }
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
    cout << "Available commands: call <callee>, hangup, cstart, cend, exit" << endl;

    if(!init_sip()) {
        cerr << "Error: can't initialize sip." << endl;
        return 1;
    }

    if(!register_accaunt()) {
        cerr << "Error: unable to register account" << endl;
        return 1;
    }

    string line;
    while (true) {
        cout << "> ";
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
                cout << "Error: Usage - call <callee>" << endl;
                continue;
            }
            if(!active_call.empty()) {
                cout << "Error: already in an active call state" << endl;
                continue;
            }

            string callee = tokens[1];
            active_call = libsip_core::placeCallWithMedia(ACCAUNT_ID,
                toSipUri(callee, domain),
                mediaList);

            cout << "Call connected: " << username << " -> " << callee << "\n CallID = " << active_call << endl;
        } else if (command == "hangup") {
            if (active_call.empty()) {
                cout << "Error: no active call" << endl;
                continue;
            }
            if(!libsip_core::hangUp(ACCAUNT_ID, active_call)) {
                cout << "Error: failed to hangup call: " <<  active_call << endl;
                continue;
            }
            active_call = "";

        } else if (command == "cstart") {
            if(active_call.empty()){
                cout << "Error: no active call" << endl;
                continue;
            }
            if(libsip_core::getIsRecording(ACCAUNT_ID, active_call)) {
                cout << "Error: already recording" << endl;
                continue;
            }
            if(!libsip_core::toggleRecording(ACCAUNT_ID, active_call)) {
                cout << "Error: failed to start recording" << endl;
                continue;
            }
        } else if (command == "cend") {
            if(active_call.empty()){
                cout << "Error: no active call" << endl;
                continue;
            }
            if(!libsip_core::getIsRecording(ACCAUNT_ID, active_call)) {
                cout << "Error: nothing recodring" << endl;
                continue;
            }
            if(!libsip_core::toggleRecording(ACCAUNT_ID, active_call)) {
                cout << "Error: failed to stop recording" << endl;
                continue;
            }
        } else {
            cout << "Error: Unknown command. \nFull list of commands:\ncall <callee> - initiates call with given ID,\n hangup - hangup current call.\n cstart - start capture of video in a local file.\n cend - stops capture of video.\n exit - exit program." << endl;
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
        libsip_core::exportable_callback<libsip_core::CallSignal::StateChange>(&callStateChanged),
        libsip_core::exportable_callback<libsip_core::CallSignal::IncomingCall>(&incomingCall),
        libsip_core::exportable_callback<libsip_core::CallSignal::IncomingCallWithMedia>(&incomingCallWithMedia),
        libsip_core::exportable_callback<libsip_core::ConfigurationSignal::VolatileDetailsChanged>(&volatileDetailsChanged),
        libsip_core::exportable_callback<libsip_core::AudioSignal::DeviceEvent>(&audioDeviceEvent),
        libsip_core::exportable_callback<libsip_core::VideoSignal::StartCapture>(&startCapture),
        libsip_core::exportable_callback<libsip_core::CallSignal::MediaNegotiationStatus>(&mediaNegotiationStatus),
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
    
    if(!libsip_core::start(cwd, ""))
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
