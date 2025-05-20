#include <iostream>

#include "CallController.h"
#include "manager.h"

void CallController::callStateChanged(const std::string& accountId, const std::string& callId, const std::string& state, const int32_t detailCode)
{
    std::lock_guard<std::mutex> lock(m_mtxEvents);
    std::cout << "Call state: " << state << "." << std::endl;
    if (state == "OVER")
        m_activeCall = "";
}

void CallController::registrationStateChanged(const std::string& accountId, const std::string& state, const int32_t code, const std::string& detailStr)
{
    std::lock_guard<std::mutex> lock(m_mtxEvents);
    std::cout << "Registration state - " << state << "..." << std::endl;
}

void CallController::volatileDetailsChanged(const std::string& account_id, const std::map<std::string, std::string>& details)
{
    std::lock_guard<std::mutex> lock(m_mtxEvents);
    std::cout << "volatileDetailsChanged" << std::endl;
}

void CallController::audioDeviceEvent()
{
    std::lock_guard<std::mutex> lock(m_mtxEvents);
    std::cout << "audioDeviceEvent" << std::endl;
}

void CallController::incomingCall(const std::string& accountId, const std::string& callId, const std::string& from)
{
    std::lock_guard<std::mutex> lock(m_mtxEvents);
    std::cout << "Incoming call form user: " << from << ".\nAccapting..." << std::endl;

    std::vector<std::map<std::string, std::string>> answerMediaList;
    answerMediaList.push_back(m_mediaAudio);

    libsip_core::acceptWithMedia(accountId, callId, answerMediaList); 
}

void CallController::incomingCallWithMedia( const std::string &accountId, const std::string &callId, const std::string &from, const std::vector<::std::map<::std::string, std::string>> &mediaList, const std::map<::std::string, std::string> &headers)
{
    std::lock_guard<std::mutex> lock(m_mtxEvents);
    std::cout << "Incoming call with media form user: " << from << ".\nAccapting..." << std::endl;

    bool hasVideo = false;
    for(auto media : mediaList) {
        auto type = media.find("MEDIA_TYPE");
        if(type != media.end()) {
            if(type->second == "VIDEO") {
                hasVideo = true;
                continue;
            }
        }
    }

    std::vector<std::map<std::string, std::string>> answerMediaList;
    answerMediaList.push_back(m_mediaAudio);
    if(m_isVideoEnabled && hasVideo)
        answerMediaList.push_back(m_mediaVideo);
        
    libsip_core::acceptWithMedia(accountId, callId, answerMediaList);
}

void CallController::mediaNegotiationStatus(const ::std::string &callId, const ::std::string &event, const ::std::vector<::std::map<::std::string, ::std::string>> &mediaList)
{
    std::lock_guard<std::mutex> lock(m_mtxEvents);
    std::cout << "mediaNegotiationStatus event - " << event << std::endl;
}

void CallController::startCapture(const std::string& camid)
{
    std::lock_guard<std::mutex> lock(m_mtxEvents);
    std::cout << "startCapture for - " << camid << std::endl;
}

void CallController::stopCapture(const std::string& camid)
{
    std::lock_guard<std::mutex> lock(m_mtxEvents);
    std::cout << "stopCapture for -" << camid << std::endl;
}

void CallController::decodingStarted(const std::string& id, const std::string& shmPath, const int32_t w, const int32_t h, const bool isMixer)
{
    std::lock_guard<std::mutex> lock(m_mtxEvents);
    std::cout << "decodingStarted for id - " << id << std::endl;

    CreateNewPreviewArgs* args = new CreateNewPreviewArgs { id, w, h };
    SDL_Event event;
    SDL_zero(event);
    event.type = EVENT_CREATE_PREVIEW;
    event.user.code = 1;
    event.user.data1 = (void*)args;
    if(!SDL_PushEvent(&event))
        delete args;
}

void CallController::decodingStopped(const std::string& id, const std::string& shmPath, const bool isMixer)
{
    std::lock_guard<std::mutex> lock(m_mtxEvents);
    std::cout << "decodingStopped for id - " << id << std::endl;

    std::string* args = new std::string(id);
    SDL_Event event;
    SDL_zero(event);
    event.type = EVENT_DESTROY_PREVIEW;
    event.user.code = 1;
    event.user.data1 = (void*)args;
    if(!SDL_PushEvent(&event))
        delete args;
}