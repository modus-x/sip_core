#include <iostream>
#include <vector>
#include <map>
#include <string>
#include <sstream>
#include <algorithm>
#include <queue>
#include <mutex>
#include <atomic>
#include <thread>

#include "CallController.h"

#ifdef _WIN32
#include <conio.h> // Windows-specific for _getch()
#else
#include <termios.h>
#include <unistd.h>
#endif

#define ACCOUNT_ID "test_acc"

// getInput("Enter username: ");
// getInput("Enter domain: ");
// getPassword();

std::string username = "dev_user";
std::string password = "!QAZxsw2";
std::string domain = "192.168.92.27";

std::atomic_bool g_needFinish(false);
std::queue<std::vector<std::string>> g_command_queue;
std::mutex g_queue_mutex;

void consoleInputLoop();
std::string getPassword(const std::string& prompt);
std::string getInput(const std::string& prompt);
std::vector<std::string> split(const std::string& s);

int
main()
{
    std::cout << "SIP core Console App" << std::endl;
    std::cout << "Available commands: call <callee>, add <callee>, del <callee>, move <from> <to>, "
                 "conf <callee1> ... <calleeN>, switch <device>, hold, resume, hangup, capOn, capOff, video, reregister, unregister, subscribe, unsubscribe, exit"
              << std::endl;

    CallController controller(ACCOUNT_ID);
    std::thread input_thread(consoleInputLoop);

    if (!controller.init()) {
        std::cerr << "Error: can't initialize sip." << std::endl;
        return 1;
    }

    // controller.setAudioCaptureDevice(1);
    // print audio captures
    auto captures = controller.getAudioCaptureDeviceList();
    if (captures.empty())
        std::cout << "\nNo avaliable audio capture devices found." << std::endl;
    else {
        std::cout << "\nAvaliable audio capture devices: \n";
        for (auto dev : captures) {
            std::cout << "\t" << dev << "\n";
        }
        std::cout << std::endl;
    }

    // print audio playbacks
    auto playbacks = controller.getAudioPlaybackDeviceList();
    if (playbacks.empty())
        std::cout << "\nNo avaliable audio playback devices found." << std::endl;
    else {
        std::cout << "\nAvaliable audio playback devices:\n";
        for (auto dev : playbacks) {
            std::cout << "\t" << dev << "\n";
        }
        std::cout << std::endl;
    }

    // print video cameras info
    auto devices = controller.getVideoDeviceList();
    if (devices.empty())
        std::cout << "\nNo avaliable video devices found." << std::endl;
    else {
        std::cout << "\nAvaliable video devices:\n";
        for (auto dev : devices) {
            std::cout << "\t" << dev << "\n";
        }
        std::cout << std::endl;
    }

    if (!controller.sendRegister(username, password, domain)) {
        std::cerr << "Error: unable to send register for current account." << std::endl;
        return 1;
    }

    while (true) {
        controller.proccesEvents();

        std::vector<std::string> tokens;
        {
            std::lock_guard<std::mutex> lock(g_queue_mutex);
            if (g_command_queue.empty())
                continue;

            tokens = g_command_queue.front();
            g_command_queue.pop();
        }

        std::string command = tokens[0];
        transform(command.begin(), command.end(), command.begin(), ::tolower);

        if (command == "exit") {
            break;
        } else if (command == "call") {
            if (tokens.size() != 2) {
                std::cerr << "Error: Usage - call <callee>" << std::endl;
                continue;
            }

            std::string callee = tokens[1];
            if (!controller.call(callee)) {
                std::cerr << "Error: already in an active call state" << std::endl;
                continue;
            }

            std::cout << "Call started: " << username << " -> " << callee
                      << "\n CallID = " << controller.getActiveCall() << std::endl;
        } else if (command == "add") {
            if (tokens.size() != 2) {
                std::cerr << "Error: Usage - add <callee>\n adds new participant to a current call."
                          << std::endl;
                continue;
            }

            if (!controller.hasActiveCall()) {
                std::cerr << "Error: No active call." << std::endl;
                continue;
                ;
            }

            if (!controller.addParticipant(tokens[1]))
                std::cerr << "Error: failed to add participant." << std::endl;

        } else if (command == "del") {
            if (tokens.size() != 2) {
                std::cerr
                    << "Error: Usage - del <callee>\n removes participant from current conference."
                    << std::endl;
                continue;
            }

            if (!controller.hasActiveCall()) {
                std::cerr << "Error: No active call." << std::endl;
                continue;
                ;
            }

            controller.addParticipant(tokens[1]);
            std::cerr << "Error: failed to remove participant." << std::endl;

            controller.removeParticipant(tokens[1]);

        } else if (command == "move") {
            if (tokens.size() != 3) {
                std::cerr << "Error: Usage - move <from> <to>" << std::endl;
                continue;
            }

            if (!controller.moveParticipant(std::stoi(tokens[1]), std::stoi(tokens[2])))
                std::cerr << "Error: failed to move particiant." << std::endl;

        } else if (command == "conf") {
            if (tokens.size() < 4) {
                std::cerr << "Error: Usage - conf <callee1> ... <calleeN>\n new conference should "
                             "have at least 3 valid members."
                          << std::endl;
                continue;
            }

            std::vector<std::string> calleeList;
            for (int i = 1; i < tokens.size(); i++) {
                calleeList.push_back(tokens[i]);
            }

            if (!controller.createConfirence(calleeList))
                std::cerr << "Error: failed to create conference." << std::endl;

        } else if (command == "switch") {
            if (tokens.size() != 2 || !controller.setVideoDevice(tokens[1])) {
                std::cerr << "Error: Usage - switch <device>\n device could be of type:\n  "
                             "display://:(screen_number)\n  camera://(camera_name)\n  default"
                          << std::endl;
                continue;
            }
        } else if (command == "hangup") {
            if (!controller.hasActiveCall()) {
                std::cerr << "Error: no active call" << std::endl;
                continue;
            }
            if (!controller.hangUp()) {
                std::cerr << "Error: failed to hangup call: " << controller.getActiveCall()
                          << std::endl;
                continue;
            }
        } else if (command == "hold") {
            if (!controller.hasActiveCall()) {
                std::cerr << "Error: no active call" << std::endl;
                continue;
            }
            if (!controller.hold()) {
                std::cerr << "Error: failed to hold call: " << controller.getActiveCall()
                          << std::endl;
                continue;
            }
        } else if (command == "resume") {
            if (!controller.hasActiveCall()) {
                std::cerr << "Error: no active call" << std::endl;
                continue;
            }
            if (!controller.resume()) {
                std::cerr << "Error: failed to resume call: " << controller.getActiveCall()
                          << std::endl;
                continue;
            }
        } else if (command == "capon") {
            if (controller.isCaptureInProgress()) {
                std::cerr << "Error: already recording" << std::endl;
            }

            if (!controller.hasActiveCall()) {
                std::cerr << "Error: no active call" << std::endl;
                continue;
            }

            if (!controller.startCallCapture()) {
                std::cerr << "Error: failed to start recording" << std::endl;
                continue;
            }
        } else if (command == "capoff") {
            if (!controller.isCaptureInProgress()) {
                std::cerr << "Error: nothing recording" << std::endl;
                continue;
            }

            if (!controller.hasActiveCall()) {
                std::cerr << "Error: no active call" << std::endl;
                continue;
            }

            if (!controller.stopCallCapture()) {
                std::cerr << "Error: failed to stop recording" << std::endl;
                continue;
            }
        } else if (command == "video") {
            if (controller.isVideoEnabled())
                std::cout << "Disabling video..." << std::endl;
            else
                std::cout << "Enabling video..." << std::endl;

            controller.toggleVideo();
        } else if (command == "info") {
            if (!controller.hasActiveCall()) {
                std::cerr << "No active call..." << std::endl;
                continue;
            }

            auto info = controller.getCallDetails(controller.getActiveCall());
            std::cout << "Current call info:\n";
            for (auto it = info.begin(); it != info.end(); ++it) {
                std::cout << " " << it->first << " : " << it->second << std::endl;
            }
} else if (command == "unregister") {
            if (!controller.unregister()) {
                std::cerr << "Error: unable to send unregister for current account." << std::endl;
            } else {
                std::cout << "Unregister successfully sent" << std::endl;
            }
        } else if (command == "subscribe") {
            if (tokens.size() < 2) {
                std::cerr << "Error: Usage - subscribe <uri1> <uri2> ..." << std::endl;
                continue;
            }
            std::vector<std::string> uris;
            for (size_t i = 1; i < tokens.size(); ++i) {
                uris.push_back(tokens[i]);
            }
            controller.subscribe(uris);
            std::cout << "Subscribe successfully sent" << std::endl;
        } else if (command == "unsubscribe") {
            if (tokens.size() < 2) {
                std::cerr << "Error: Usage - unsubscribe <uri1> <uri2> ..." << std::endl;
                continue;
            }
            std::vector<std::string> uris;
            for (size_t i = 1; i < tokens.size(); ++i) {
                uris.push_back(tokens[i]);
            }
            controller.unsubscribe(uris);
            std::cout << "Unsubscribe successfully sent" << std::endl;
        } else if (command == "reregister") {
            if (!controller.sendRegister(username, password, domain)) {
                std::cerr << "Error: unable to send reregister for current account." << std::endl;
            } else {
                std::cout << "Reregister successfully sent" << std::endl;
            }
        } else {
            std::cerr << "Error: Unknown command. \nFull list of commands:\n call <callee> - "
                         "initiates call with given ID,\n add <callee> - adds new participant to "
                         "current call,\n del <callee> - remove participant from conference,\n "
                         "move <from> <to> - move conference participant position in grid,\n conf "
                         "<callee1> ... <calleeN> - creates conference with given participants "
                         "(>=3),\n switch <device> - switches video source for an active call.\n "
                         "hold - put current call on hold.\n resume - resume current call.\n "
                         "hangup - hangup current call.\n capOn - start capture of active call in "
                         "a local file.\n capOff - stops capture of video.\n video - enables video "
                         "transfer.\n info - get current call infos.\n reregister - force "
                         "reregistration.\n unregister - unregister user.\n subscribe <uri1>... - subscribe to events.\n unsubscribe <uri1>... - unsubscribe from events.\n exit - exit program."
                      << std::endl;
        }
    }

    g_needFinish.store(true);
    std::cout << "Press enter to exit..." << std::endl;

    if (input_thread.joinable())
        input_thread.join();

    return 0;
}

void
consoleInputLoop()
{
    std::string line;
    while (!g_needFinish.load()) {
        getline(std::cin, line);
        std::vector<std::string> tokens = split(line);
        if (tokens.empty())
            continue;

        {
            std::lock_guard<std::mutex> lock(g_queue_mutex);
            g_command_queue.push(tokens);
        }
    }
}

// Function to hide password input (cross-platform)
std::string
getPassword(const std::string& prompt = "Enter password: ")
{
    std::string password;
    std::cout << prompt;

#ifdef _WIN32
    // Windows implementation (no echo)
    char ch;
    while ((ch = _getch()) != '\r') { // Enter key
        if (ch == '\b') {             // Backspace
            if (!password.empty()) {
                password.pop_back();
                std::cout << "\b \b"; // Erase asterisk
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

    tcsetattr(STDIN_FILENO, TCSANOW, &oldt); // Restore terminal settings
#endif

    std::cout << std::endl;
    return password;
}

std::string
getInput(const std::string& prompt)
{
    std::string input;
    std::cout << prompt;
    getline(std::cin, input);
    return input;
}

std::vector<std::string>
split(const std::string& s)
{
    std::vector<std::string> tokens;
    std::string token;
    std::istringstream iss(s);
    while (iss >> token) {
        tokens.push_back(token);
    }
    return tokens;
}