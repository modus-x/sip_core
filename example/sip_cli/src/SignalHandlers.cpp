#include <iostream>

#include "SignalHandlers.h"
#include "manager.h"
#include "sip/sipcall.h"
#include "sip_core/callmanager_interface.h"
#include <memory>

void callStateChanged(const std::string& accountId, const std::string& callId, const std::string& state, const int32_t detailCode)
{
    std::cout << "Call state: " << state << "." << std::endl;
    if (state == "OVER")
        active_call = "";
}

void registrationStateChanged(const std::string& accountId, const std::string& state, const int32_t code, const std::string& detailStr)
{
    std::cout << "Registration state - " << state << "..." << std::endl;
}

void volatileDetailsChanged(const std::string& account_id, const std::map<std::string, std::string>& details)
{
    //std::cout << "volatileDetailsChanged" << std::endl;
}

void audioDeviceEvent()
{
    std::cout << "audioDeviceEvent" << std::endl;
}

void incomingCall(const std::string& accountId, const std::string& callId, const std::string& from)
{
    std::cout << "Incoming call form user: " << from << ".\nAccapting..." << std::endl;

    std::vector<std::map<std::string, std::string>> answerMediaList;
    if(g_isAudioOn)
        answerMediaList.push_back(g_mediaAudio);
    if(g_isVideoOn)
        answerMediaList.push_back(g_mediaVideo);

    active_call = callId;
    if(!libsip_core::acceptWithMedia(accountId, callId, answerMediaList)) {
        std::cerr << "Error: unable to except incoming call.";
        return;
    } 
}

void incomingCallWithMedia( const std::string &accountId, const std::string &callId, const std::string &from, const std::vector<::std::map<::std::string, std::string>> &mediaList, const std::map<::std::string, std::string> &headers)
{
    std::cout << "Incoming call with media form user: " << from << ".\nAccapting..." << std::endl;

    std::this_thread::sleep_for(std::chrono::seconds(1));
    std::vector<std::map<std::string, std::string>> answerMediaList;
    if(g_isAudioOn)
        answerMediaList.push_back(g_mediaAudio);
    if(g_isVideoOn)
        answerMediaList.push_back(g_mediaVideo);
    
    assert(mediaList.size() > 0);
    if (mediaList.size() > answerMediaList.size()) {
        for (auto idx = answerMediaList.size(); idx < mediaList.size(); idx++) {
            answerMediaList.emplace_back(mediaList[idx]);
        }
    }
    
    active_call = callId;
    if(!libsip_core::acceptWithMedia(accountId, callId, answerMediaList)) {
        std::cerr << "Error: unable to except incoming call.";
        return;
    }
}

void mediaNegotiationStatus(const ::std::string &callId, const ::std::string &event, const ::std::vector<::std::map<::std::string, ::std::string>> &mediaList)
{
    //std::cout << "mediaNegotiationStatus" << std::endl;
}

void startCapture(const std::string& camid)
{
    //std::cout << "startCapture" << std::endl;
}

void stopCapture(const std::string& camid)
{
    //std::cout << "stopCapture" << std::endl;
}

void decodingStarted(const std::string& id, const std::string& shmPath, const int32_t w, const int32_t h, const bool isMixer)
{
    //std::cout << "decodingStarted" << std::endl;

    libsip_core::SinkTarget target;
    target.preferredFormat = AV_PIX_FMT_RGBA;
    target.push = [] (libsip_core::FrameBuffer frame) {
        //std::cout << "SinkTarget Push" << std::endl;
    };

    if(!libsip_core::registerSinkTarget(id, target)) {
        std::cerr << "Error: unable to register sink target for: " << id << "." << std::endl;
    }
}

void decodingStopped(const std::string& id, const std::string& shmPath, const bool isMixer)
{
    //std::cout << "decodingStopped" << std::endl;
}

void mediaChange(const std::string &accountId, const std::string &callId, const std::vector<std::map<std::string, std::string>> &remoteMediaList)
{
    std::cout << "reINVITE occured" << std::endl;

    std::vector<std::map<std::string, std::string>> newMediaList;
    if(g_isAudioOn)
        newMediaList.push_back(g_mediaAudio);
    if(g_isVideoOn)
        newMediaList.push_back(g_mediaVideo);
    
    assert(remoteMediaList.size() > 0);
    if (remoteMediaList.size() > newMediaList.size()) {
        for (auto idx = newMediaList.size(); idx < remoteMediaList.size(); idx++) {
            newMediaList.emplace_back(remoteMediaList[idx]);
        }
    }

    if(!sip_core::answerMediaChangeRequest(accountId, callId, remoteMediaList)) {
        std::cerr << "Error: reINVITE for non existing call..." << std::endl;
        return;
    }
}