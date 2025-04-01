#include "SignalHandlers.h"
#include <iostream>

void callStateChanged(const std::string& accountId, const std::string& callId, const std::string& state, const int32_t detailCode)
{
    std::cout << "callStateChanged" << std::endl;
}

void registrationStateChanged(const std::string& accountId, const std::string& state, const int32_t code, const std::string& detailStr)
{
    std::cout << "registrationStateChanged" << std::endl;
}

void volatileDetailsChanged(const std::string& account_id, const std::map<std::string, std::string>& details)
{
    std::cout << "volatileDetailsChanged" << std::endl;
}

void audioDeviceEvent()
{
    std::cout << "audioDeviceEvent" << std::endl;
}

void incomingCall(const std::string& accountId, const std::string& callId, const std::string& from)
{
    std::cout << "incomingCall" << std::endl;
}

void incomingCallWithMedia( const std::string &accountId, const std::string &callId, const std::string &from, const std::vector<::std::map<::std::string, std::string>> &mediaList, const std::map<::std::string, std::string> &headers)
{
    std::cout << "incomingCallWithMedia" << std::endl;
}

void mediaNegotiationStatus(const ::std::string &callId, const ::std::string &event, const ::std::vector<::std::map<::std::string, ::std::string>> &mediaList)
{
    std::cout << "mediaNegotiationStatus" << std::endl;
}

void startCapture(const std::string& camid)
{
    std::cout << "startCapture" << std::endl;
}

void stopCapture(const std::string& camid)
{
    std::cout << "stopCapture" << std::endl;
}

void decodingStarted(const std::string& id, const std::string& shmPath, const int32_t w, const int32_t h, const bool isMixer)
{
    std::cout << "decodingStarted" << std::endl;
}

void decodingStopped(const std::string& id, const std::string& shmPath, const bool isMixer)
{
    std::cout << "decodingStopped" << std::endl;
}