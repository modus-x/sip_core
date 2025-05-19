#include <iostream>

#include "CallController.h"
#include "manager.h"

void CallController::callStateChanged(const std::string& accountId, const std::string& callId, const std::string& state, const int32_t detailCode)
{
    std::cout << "Call state: " << state << "." << std::endl;
    if (state == "OVER")
        m_activeCall = "";
}

void CallController::registrationStateChanged(const std::string& accountId, const std::string& state, const int32_t code, const std::string& detailStr)
{
    std::cout << "Registration state - " << state << "..." << std::endl;
}

void CallController::volatileDetailsChanged(const std::string& account_id, const std::map<std::string, std::string>& details)
{
    std::cout << "volatileDetailsChanged" << std::endl;
}

void CallController::audioDeviceEvent()
{
    std::cout << "audioDeviceEvent" << std::endl;
}

void CallController::incomingCall(const std::string& accountId, const std::string& callId, const std::string& from)
{
    std::cout << "Incoming call form user: " << from << ".\nAccapting..." << std::endl;

    std::vector<std::map<std::string, std::string>> answerMediaList;
    answerMediaList.push_back(m_mediaAudio);
    if(m_isVideoEnabled)
        answerMediaList.push_back(m_mediaVideo);

    libsip_core::acceptWithMedia(accountId, callId, answerMediaList); 
}

void CallController::incomingCallWithMedia( const std::string &accountId, const std::string &callId, const std::string &from, const std::vector<::std::map<::std::string, std::string>> &mediaList, const std::map<::std::string, std::string> &headers)
{
    std::cout << "Incoming call with media form user: " << from << ".\nAccapting..." << std::endl;

    std::vector<std::map<std::string, std::string>> answerMediaList;
    answerMediaList.push_back(m_mediaAudio);
    if(m_isVideoEnabled)
        answerMediaList.push_back(m_mediaVideo);
        
    libsip_core::acceptWithMedia(accountId, callId, answerMediaList);
}

void CallController::mediaNegotiationStatus(const ::std::string &callId, const ::std::string &event, const ::std::vector<::std::map<::std::string, ::std::string>> &mediaList)
{
    std::cout << "mediaNegotiationStatus event" << event << std::endl;
}

void CallController::startCapture(const std::string& camid)
{
    std::cout << "startCapture for - " << camid << std::endl;
}

void CallController::stopCapture(const std::string& camid)
{
    std::cout << "stopCapture for -" << camid << std::endl;
}

void CallController::decodingStarted(const std::string& id, const std::string& shmPath, const int32_t w, const int32_t h, const bool isMixer)
{
    std::cout << "decodingStarted for id - " << id << std::endl;

    if(!OpenVideoPrievew(id, w, h)) {
        std::cerr << "Error: failed to create window for " << id << "." << std::endl;
        return;
    }

    auto it = m_previewWindow.find(id);
    if(it == m_previewWindow.end())
        return;

    auto ptrWindow = it->second;
    libsip_core::SinkTarget target;
    target.preferredFormat = AV_PIX_FMT_RGBA;
    target.push = [ptrWindow] (libsip_core::FrameBuffer frame) {
        ptrWindow->renderFrame(frame);
    };

    if(!libsip_core::registerSinkTarget(id, target)) {
        std::cerr << "Error: unable to register sink target for: " << id << "." << std::endl;
    }
}

void CallController::decodingStopped(const std::string& id, const std::string& shmPath, const bool isMixer)
{
    std::cout << "decodingStopped for id - " << id << std::endl;
    CloseVideoPreview(id);
}