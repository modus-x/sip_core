#pragma once 

#include <map>
#include <string>
#include <memory>
#include <SDL3/SDL.h>

#include "SignalHandlers.h"
#include "SDLVideoRenderer.h"

class CallController final : private ISignals
{
private:
    struct CreateNewPreviewArgs 
    {
        std::string id;
        int w, h;
    };

public:
    CallController(const std::string& accountId);
    ~CallController();

    bool init();
    bool sendRegister(const std::string& user, const std::string& pass, const std::string& domain);

    bool call(const std::string& callTo);
    bool hangUp();
    bool hasActiveCall() const;
    const std::string getActiveCall() const;

    bool addParticipant(const std::string& newParticipant);
    bool removeParticipant(const std::string& participant);
    bool createConfirence(const std::vector<std::string>& participantsList);
    bool moveParticipant(size_t from_index, size_t to_index);

    bool isCaptureInProgress();
    bool startCallCapture();
    bool stopCallCapture();

    void toggleVideo();
    bool isVideoEnabled() const;
    bool setVideoDevice(const std::string& videoDevice);
    std::vector<std::string> getVideoDeviceList() const;
    const std::string getVideoDevice() const;
    std::map<std::string, std::string> getCallDetails(const std::string& callId);

    std::vector<std::string> getAudioCaptureDeviceList() const;
    std::vector<std::string> getAudioPlaybackDeviceList() const;

    void setAudioCaptureDevice(int index);
    void setAudioPlaybackDevice(int index);
    
    void proccesEvents();

private:
    virtual void audioDeviceEvent();
    virtual void callStateChanged(const std::string& accountId, const std::string& callId, const std::string& state, const int32_t detailCode) override;
    virtual void registrationStateChanged(const std::string& accountId, const std::string& state, const int32_t code, const std::string& detailStr) override;
    virtual void volatileDetailsChanged(const std::string& account_id, const std::map<std::string, std::string>& details) override;
    virtual void incomingCall(const std::string& accountId, const std::string& callId, const std::string& from) override;
    virtual void incomingCallWithMedia( const std::string &accountId, const std::string &callId, const std::string &from, const std::vector<std::map<std::string, std::string>> &mediaList, const std::map<std::string, std::string> &headers) override;
    virtual void mediaNegotiationStatus(const ::std::string &callId, const ::std::string &event, const ::std::vector<::std::map<::std::string, ::std::string>> &mediaList) override;
    virtual void startCapture(const std::string& camid) override;
    virtual void stopCapture(const std::string& camid) override;
    virtual void decodingStarted(const std::string& id, const std::string& shmPath, const int32_t w, const int32_t h, const bool isMixer) override;
    virtual void decodingStopped(const std::string& id, const std::string& shmPath, const bool isMixer) override;
    virtual void conferenceCreated(const std::string& accountId, const std::string& confId) override;
    virtual void conferenceChanged(const std::string& accountId, const std::string& confId, const std::string& state) override;
    virtual void conferenceRemoved(const std::string& accountId, const std::string& confId) override;
    virtual void confInfoChanged(const std::string& callId, const std::vector<std::map<std::string, std::string>>& confInfos) override;

    std::string toSipUri(const std::string& number, const std::string& domainName);
    bool OpenVideoPrievew(const std::string& id, int width, int height);
    void CloseVideoPreview(const std::string& id);

    void createConfFromParticipantList(const std::string& accountId,
                                       const std::vector<std::string>& participantList);

    mutable std::recursive_mutex m_mtxEvents;

    bool m_isVideoEnabled;
    std::map<std::string, std::string> m_mediaAudio;
    std::map<std::string, std::string> m_mediaVideo;

    std::string m_user;
    std::string m_domain;
    const std::string m_accontId;
    std::string m_activeConfirence;
    std::map<std::string, std::string> m_activeCalls;

    Uint32 EVENT_CREATE_PREVIEW;
    Uint32 EVENT_DESTROY_PREVIEW;
    Uint32 EVENT_FRAME_READY;

    std::map<std::string, std::shared_ptr<SDLVideoRenderer>> m_previewWindows;
};