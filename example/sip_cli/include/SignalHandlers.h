#include <string>
#include <map>
#include <vector>

void audioDeviceEvent();

void callStateChanged(const std::string& accountId, const std::string& callId, const std::string& state, const int32_t detailCode);

void registrationStateChanged(const std::string& accountId, const std::string& state, const int32_t code, const std::string& detailStr);    

void volatileDetailsChanged(const std::string& account_id, const std::map<std::string, std::string>& details);

void incomingCall(const std::string& accountId, const std::string& callId, const std::string& from);

void incomingCallWithMedia( const std::string &accountId, const std::string &callId, const std::string &from, const std::vector<std::map<std::string, std::string>> &mediaList, const std::map<std::string, std::string> &headers);

void mediaNegotiationStatus(const ::std::string &callId, const ::std::string &event, const ::std::vector<::std::map<::std::string, ::std::string>> &mediaList);

void startCapture(const std::string& camid);

void stopCapture(const std::string& camid);

void decodingStarted(const std::string& id, const std::string& shmPath, const int32_t w, const int32_t h, const bool isMixer);

void decodingStopped(const std::string& id, const std::string& shmPath, const bool isMixer);