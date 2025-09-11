#include <iostream>

#include "CallController.h"
#include "manager.h"

void
CallController::callStateChanged(const std::string& accountId,
                                 const std::string& callId,
                                 const std::string& state,
                                 const int32_t detailCode)
{
    std::lock_guard<std::recursive_mutex> lock(m_mtxEvents);
    std::cout << "Call state: " << state << ", id - " << callId << "." << std::endl;
    if (state == "OVER") {
        auto it = std::find_if(m_activeCalls.begin(),
                               m_activeCalls.end(),
                               [&callId](const std::pair<std::string, std::string>& item) {
                                   return item.second == callId;
                               });

        if (it != m_activeCalls.end()) {
            m_activeCalls.erase(it);
        }
    }
}

void
CallController::registrationStateChanged(const std::string& accountId,
                                         const std::string& state,
                                         const int32_t code,
                                         const std::string& detailStr)
{
    std::lock_guard<std::recursive_mutex> lock(m_mtxEvents);
    std::cout << "Registration state - " << state << "..." << std::endl;
}

void
CallController::volatileDetailsChanged(const std::string& account_id,
                                       const std::map<std::string, std::string>& details)
{
    std::lock_guard<std::recursive_mutex> lock(m_mtxEvents);
    std::cout << "volatileDetailsChanged" << std::endl;
}

void
CallController::audioDeviceEvent()
{
    std::lock_guard<std::recursive_mutex> lock(m_mtxEvents);
    std::cout << "audioDeviceEvent" << std::endl;
}

void
CallController::incomingCall(const std::string& accountId,
                             const std::string& callId,
                             const std::string& from)
{
    std::lock_guard<std::recursive_mutex> lock(m_mtxEvents);
    
    if(m_activeCalls.empty()) {
        std::cout << "Incoming call form user: " << from << ".\nAccapting..." << std::endl;
        std::vector<std::map<std::string, std::string>> answerMediaList;
        answerMediaList.push_back(m_mediaAudio);

        std::this_thread::sleep_for(1s);

        if (libsip_core::acceptWithMedia(accountId, callId, answerMediaList))
            m_activeCalls[callId] = callId;
    } else {
        std::cout << "Incoming call form user: " << from << ".\nDenied..." << std::endl;
        libsip_core::refuse(accountId, callId);
    }
}

void
CallController::incomingCallWithMedia(
    const std::string& accountId,
    const std::string& callId,
    const std::string& from,
    const std::vector<::std::map<::std::string, std::string>>& mediaList,
    const std::map<::std::string, std::string>& headers)
{
    std::lock_guard<std::recursive_mutex> lock(m_mtxEvents);
    
    if(m_activeCalls.empty()) {
        std::cout << "Incoming call with media form user: " << from << ".\nAccapting..." << std::endl;

        std::vector<std::map<std::string, std::string>> answerMediaList;
        answerMediaList.push_back(m_mediaAudio);

        bool incomingWithVideo = false;
        for (auto media : mediaList) {
            auto type = media.find("MEDIA_TYPE");
            if (type != media.end()) {
                if (type->second == "MEDIA_TYPE_VIDEO") {
                    incomingWithVideo = true;
                    continue;
                }
            }
        }

#ifdef ENABLE_VIDEO
        if (incomingWithVideo) {
            if (m_isVideoEnabled) {
                m_mediaVideo["ENABLED"] = "true";
                m_mediaVideo["MUTED"] = "false";
                answerMediaList.push_back(m_mediaVideo);
            }
            else {
                m_mediaVideo["ENABLED"] = "true";
                m_mediaVideo["MUTED"] = "true";
                answerMediaList.push_back(m_mediaVideo);
            }
        }
        else {
            if (m_isVideoEnabled) {
                m_mediaVideo["ENABLED"] = "true";
                m_mediaVideo["MUTED"] = "false";
                answerMediaList.push_back(m_mediaVideo);
            }
        }
#elif
        if (incomingWithVideo) {
            libsip_core::MediaMap videoMedia = {
                { "MEDIA_TYPE", "MEDIA_TYPE_VIDEO"},
                { "MUTED", "true" },
                { "ENABLED", "false" },
                { "LABEL", "video_0" }};
            answerMediaList.push_back(videoMedia);
        }
#endif

        std::this_thread::sleep_for(1s);

        if (libsip_core::acceptWithMedia(accountId, callId, answerMediaList))
            m_activeCalls[callId] = callId;
    } else {
        std::cout << "Incoming call form user: " << from << ".\nDenied..." << std::endl;
        libsip_core::refuse(accountId, callId);
    }
}

void
CallController::mediaNegotiationStatus(
    const ::std::string& callId,
    const ::std::string& event,
    const ::std::vector<::std::map<::std::string, ::std::string>>& mediaList)
{
    std::lock_guard<std::recursive_mutex> lock(m_mtxEvents);
    std::cout << "mediaNegotiationStatus event - " << event << std::endl;
}

void CallController::mediaChangeRequest(
    const std::string& accountId, 
    const std::string& callId, 
    const std::vector<std::map<std::string, std::string>>& remoteMediaList)
{
    std::lock_guard<std::recursive_mutex> lock(m_mtxEvents);
    std::cout << "mediaChangeRequest event for call - " << callId << std::endl;

    libsip_core::answerMediaChangeRequest(accountId, callId, remoteMediaList);
}

void
CallController::startCapture(const std::string& camid)
{
    std::lock_guard<std::recursive_mutex> lock(m_mtxEvents);
    std::cout << "startCapture for - " << camid << std::endl;
}

void
CallController::stopCapture(const std::string& camid)
{
    std::lock_guard<std::recursive_mutex> lock(m_mtxEvents);
    std::cout << "stopCapture for -" << camid << std::endl;
}

void
CallController::decodingStarted(const std::string& id,
                                const std::string& shmPath,
                                const int32_t w,
                                const int32_t h,
                                const bool isMixer)
{
    std::lock_guard<std::recursive_mutex> lock(m_mtxEvents);
    std::cout << "decodingStarted for id - " << id << std::endl;

    CreateNewPreviewArgs* args = new CreateNewPreviewArgs {id, w, h};
    SDL_Event event;
    SDL_zero(event);
    event.type = EVENT_CREATE_PREVIEW;
    event.user.code = 1;
    event.user.data1 = (void*) args;
    if (!SDL_PushEvent(&event))
        delete args;
}

void
CallController::decodingStopped(const std::string& id,
                                const std::string& shmPath,
                                const bool isMixer)
{
    std::lock_guard<std::recursive_mutex> lock(m_mtxEvents);
    std::cout << "decodingStopped for id - " << id << std::endl;

    std::string* args = new std::string(id);
    SDL_Event event;
    SDL_zero(event);
    event.type = EVENT_DESTROY_PREVIEW;
    event.user.code = 1;
    event.user.data1 = (void*) args;
    if (!SDL_PushEvent(&event))
        delete args;
}

void
CallController::conferenceCreated(const std::string& accountId, const std::string& confId)
{
    std::lock_guard<std::recursive_mutex> lock(m_mtxEvents);
    std::cout << "Conference created with id - " << confId << "." << std::endl;

    // libsip_core::setActiveStream(accountId, confId, "", "", "host_video_0", true);
    libsip_core::addMainParticipant(accountId, confId);
    m_activeConfirence = confId;
}

void
CallController::conferenceChanged(const std::string& accountId,
                                  const std::string& confId,
                                  const std::string& state)
{
    std::lock_guard<std::recursive_mutex> lock(m_mtxEvents);
    std::cout << "Conference changed; id - " << confId << ". State - " << state << "." << std::endl;
}

void
CallController::conferenceRemoved(const std::string& accountId, const std::string& confId)
{
    std::lock_guard<std::recursive_mutex> lock(m_mtxEvents);
    std::cout << "Conference removed; id - " << confId << "." << std::endl;

    m_activeConfirence = "";
}

void
CallController::confInfoChanged(const std::string& callId,
                                const std::vector<std::map<std::string, std::string>>& confInfos)
{
    std::lock_guard<std::recursive_mutex> lock(m_mtxEvents);
    std::cout << "Conference infos changed." << std::endl;
}
