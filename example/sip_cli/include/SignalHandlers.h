#pragma once

#include <string>
#include <map>
#include <vector>

// this events functions gonna be called from event thread.
class ISignals
{
    virtual void audioDeviceEvent() = 0;
    virtual void callStateChanged(const std::string& accountId,
                                  const std::string& callId,
                                  const std::string& state,
                                  const int32_t detailCode)
        = 0;
    virtual void registrationStateChanged(const std::string& accountId,
                                          const std::string& state,
                                          const int32_t code,
                                          const std::string& detailStr)
        = 0;
    virtual void volatileDetailsChanged(const std::string& account_id,
                                        const std::map<std::string, std::string>& details)
        = 0;
    virtual void incomingCall(const std::string& accountId,
                              const std::string& callId,
                              const std::string& from)
        = 0;
    virtual void incomingCallWithMedia(
        const std::string& accountId,
        const std::string& callId,
        const std::string& from,
        const std::vector<std::map<std::string, std::string>>& mediaList,
        const std::map<std::string, std::string>& headers)
        = 0;
    virtual void mediaNegotiationStatus(
        const ::std::string& callId,
        const ::std::string& event,
        const ::std::vector<::std::map<::std::string, ::std::string>>& mediaList)
        = 0;
    virtual void mediaChangeRequest(
        const std::string& accountId, 
        const std::string& callId, 
        const std::vector<std::map<std::string, std::string>>& remoteMediaList)
        = 0;
    virtual void startCapture(const std::string& camid) = 0;
    virtual void stopCapture(const std::string& camid) = 0;
    virtual void decodingStarted(const std::string& id,
                                 const std::string& shmPath,
                                 const int32_t w,
                                 const int32_t h,
                                 const bool isMixer)
        = 0;
    virtual void decodingStopped(const std::string& id,
                                 const std::string& shmPath,
                                 const bool isMixer)
        = 0;
    virtual void conferenceCreated(const std::string& accountId, const std::string& confId) = 0;
    virtual void conferenceChanged(const std::string& accountId,
                                   const std::string& confId,
                                   const std::string& state)
        = 0;
    virtual void conferenceRemoved(const std::string& accountId, const std::string& confId) = 0;
    virtual void confInfoChanged(const std::string& callId,
                                 const std::vector<std::map<std::string, std::string>>& confInfos)
        = 0;
};
