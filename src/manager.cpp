/*
 *  Copyright (C) 2004-2022 Savoir-faire Linux Inc.
 *
 *  Author: Alexandre Bourget <alexandre.bourget@savoirfairelinux.com>
 *  Author: Yan Morin <yan.morin@savoirfairelinux.com>
 *  Author: Laurielle Lea <laurielle.lea@savoirfairelinux.com>
 *  Author: Emmanuel Milou <emmanuel.milou@savoirfairelinux.com>
 *  Author: Alexandre Savard <alexandre.savard@savoirfairelinux.com>
 *  Author: Guillaume Carmel-Archambault <guillaume.carmel-archambault@savoirfairelinux.com>
 *  Author: Tristan Matthews <tristan.matthews@savoirfairelinux.com>
 *  Author: Guillaume Roguez <guillaume.roguez@savoirfairelinux.com>
 *  Author: Adrien Béraud <adrien.beraud@savoirfairelinux.com>
 *  Author: Philippe Gorley <philippe.gorley@savoirfairelinux.com>
 *  Author: Aline Gondim Santos <aline.gondimsantos@savoirfairelinux.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "manager.h"

#include "logger.h"
#include "account_schema.h"

#include "fileutils.h"
#include "map_utils.h"
#include "account.h"
#include "string_utils.h"
#include <csignal> // For signal handling
#include "account.h"

#include "call_factory.h"

#include "connectivity/sip_utils.h"
#include "sip/sipvoiplink.h"
#include "sip/sipaccount_config.h"
#include "sip/sipaccount.h"

#include "im/instant_messaging.h"

#include "config/yamlparser.h"

#if HAVE_ALSA
#include "audio/alsa/alsalayer.h"
#endif

#include "media/localrecordermanager.h"
#include "audio/sound/tonelist.h"
#include "audio/sound/dtmf.h"
#include "audio/ringbufferpool.h"

#include "client/videomanager.h"

#include "conference.h"

#include "client/ring_signal.h"
#include "sip_core/call_const.h"
#include "sip_core/account_const.h"
#include "sip/sipcall.h"
#include "media/audio/audio_rtp_session.h"
#include "media/audio/audio_receive_thread.h"

#include "libav_utils.h"
#ifdef ENABLE_VIDEO
#include "video/video_scaler.h"
#include "video/sinkclient.h"
#include "video/video_base.h"
#include "media/video/video_mixer.h"
#endif
#include "audio/tonecontrol.h"

#include "sip_core/media_const.h"

#include <libavutil/ffversion.h>

#ifndef WIN32
#include <sys/time.h>
#include <sys/resource.h>
#endif

#ifdef TARGET_OS_IOS
#include <CoreFoundation/CoreFoundation.h>
#endif

#include <cerrno>
#include <ctime>
#include <cstdlib>
#include <iostream>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <memory>
#include <mutex>
#include <list>
#include <random>

#if (defined(TARGET_OS_IOS) && TARGET_OS_IOS)
#include "media/audio/coreaudio/ios/corelayer.h"
#endif

namespace sip_core {

/** To store uniquely a list of Call ids */
using CallIDSet = std::set<std::string>;

std::atomic_bool Manager::initialized = {false};

#if TARGET_OS_IOS
bool Manager::isIOSExtension = {false};
#endif

void
check_rename(const std::string& old_dir, const std::string& new_dir)
{
    if (old_dir == new_dir or not fileutils::isDirectory(old_dir))
        return;

    if (not fileutils::isDirectory(new_dir)) {
        SIP_CORE_WARN() << "Migrating " << old_dir << " to " << new_dir;
        std::rename(old_dir.c_str(), new_dir.c_str());
    } else {
        for (const auto& file : fileutils::readDirectory(old_dir)) {
            auto old_dest = fileutils::getFullPath(old_dir, file);
            auto new_dest = fileutils::getFullPath(new_dir, file);
            if (fileutils::isDirectory(old_dest) and fileutils::isDirectory(new_dest)) {
                check_rename(old_dest, new_dest);
            } else {
                SIP_CORE_WARN() << "Migrating " << old_dest << " to " << new_dest;
                std::rename(old_dest.c_str(), new_dest.c_str());
            }
        }
        fileutils::removeAll(old_dir);
    }
}

/**
 * Set pjsip's log level based on the SIPLOGLEVEL environment variable.
 * SIPLOGLEVEL = 0 minimum logging
 * SIPLOGLEVEL = 6 maximum logging
 */

/** Environment variable used to set pjsip's logging level */
static constexpr const char* SIPLOGLEVEL = "SIPLOGLEVEL";

static void
setSipLogLevel()
{
    int level = PJ_LOG_MAX_LEVEL;

    pj_log_set_level(level);
    pj_log_set_log_func([](int level, const char* data, int /*len*/) {
        if (level < 2)
            SIP_CORE_ERR() << data;
        else if (level < 4)
            SIP_CORE_WARN() << data;
        else
            SIP_CORE_DBG() << data;
    });
}

struct Manager::ManagerPimpl
{
    explicit ManagerPimpl(Manager& base);

    bool parseConfiguration();

    /*
     * Play one tone
     * @return false if the driver is uninitialize
     */
    void playATone(Tone::ToneId toneId);

    int getCurrentDeviceIndex(AudioDeviceType type);

    /**
     * Process remaining participant given a conference and the current call id.
     * Mainly called when a participant is detached or hagned up
     * @param current call id
     * @param conference pointer
     */
    void processRemainingParticipants(Conference& conf);

    /**
     * Create config directory in home user and return configuration file path
     */
    std::string retrieveConfigPath() const;

    void unsetCurrentCall();

    void switchCall(const std::string& id);

    /**
     * Add incoming callid to the waiting list
     * @param id std::string to add
     */
    void addWaitingCall(const std::string& id);

    /**
     * Remove incoming callid to the waiting list
     * @param id std::string to remove
     */
    void removeWaitingCall(const std::string& id);

    void loadAccount(const YAML::Node& item, int& errorCount);

    void sendTextMessageToConference(const Conference& conf,
                                     const std::map<std::string, std::string>& messages,
                                     const std::string& from) const noexcept;

    void bindCallToConference(Call& call, Conference& conf);

    void addMainParticipant(Conference& conf);

    bool hangupConference(Conference& conf);

    template<class T>
    std::shared_ptr<T> findAccount(const std::function<bool(const std::shared_ptr<T>&)>&);

    void initAudioDriver();

    /**
     * Check if any account has an active conference.
     */
    bool hasActiveConference() const;

    void processIncomingCall(const std::string& accountId,
                             Call& incomCall,
                             const std::map<std::string, std::string>& headers = {});
    static void stripSipPrefix(Call& incomCall);

    /**
     * Tracks an incoming call that arrived with an Alert-Info header.
     * Until either the client pushes a custom ringtone via
     * setRingtoneForIncomingCall() or the scheduled fallback fires, the
     * default ringtone is intentionally NOT played.
     */
    struct PendingAlertInfoCall
    {
        std::string accountId;
        std::shared_ptr<Task> fallbackTask;
        bool delivered {false};
    };

    /// Lookup an incoming-call header by name in a case-insensitive manner.
    /// PJSIP preserves the original casing of received headers, so the
    /// remote PBX may emit "Alert-Info" in any case.
    static std::string findHeaderCaseInsensitive(
        const std::map<std::string, std::string>& headers,
        std::string_view name);

    /// Called from the scheduler when the Alert-Info wait window expired
    /// without the client pushing a custom ringtone. Plays the default
    /// ringtone and logs an ERROR.
    void onAlertInfoTimeout(const std::string& accountId, const std::string& callId);

    /// Drop any pending Alert-Info entry for the given call id and cancel
    /// the scheduled fallback if it has not run yet. Safe to call for
    /// calls that never had a pending entry.
    void clearPendingAlertInfoCall(const std::string& callId);

    Manager& base_; // pimpl back-pointer

    /** Main scheduler */
    ScheduledExecutor scheduler_ {"manager"};

    /** Application wide tone controller */
    ToneControl toneCtrl_;
    std::unique_ptr<AudioDeviceGuard> toneDeviceGuard_;

    /** Current Call ID */
    std::string currentCall_;

    /** Protected current call access */
    std::mutex currentCallMutex_;

    /** Protected sinks access */
    std::mutex sinksMutex_;

    /** Audio layer */
    std::shared_ptr<AudioLayer> audiodriver_ {nullptr};
    std::array<std::atomic_uint, 3> audioStreamUsers_ {};
    // Per-stream pending stop task: scheduled when the user count drops
    // to 0, cancelled when a new guard reclaims the stream within the
    // linger window. Lets us coalesce rapid destroy/recreate cycles that
    // would otherwise stall PulseAudio's virtual xrdp-source (Linux over
    // xrdp on Astra 1.8): the source accepts the new connection but its
    // read callback never fires when destroyed and recreated within ms.
    std::array<std::shared_ptr<Task>, 3> audioStreamStopTask_ {};
    std::mutex audioStreamMutex_ {};

    // Main thread
    std::unique_ptr<DTMF> dtmfKey_;

    /** Buffer to generate DTMF */
    AudioBuffer dtmfBuf_;

    /** Buffer to play local files */
    std::unique_ptr<AudioFile> currentFile_;

    // To handle volume control
    // short speakerVolume_;
    // short micVolume_;
    // End of sound variable

    /**
     * Mutex used to protect audio layer
     */
    std::mutex audioLayerMutex_;

    /**
     * Waiting Call Vectors
     */
    CallIDSet waitingCalls_;

    /**
     * Protect waiting call list, access by many voip/audio threads
     */
    std::mutex waitingCallsMutex_;

    /**
     * Path of the ConfigFile
     */
    std::string path_;

    /**
     * Path of the app assets root
     */
    std::optional<std::string> data_path_;

    /**
     * Instance of the RingBufferPool for the whole application
     *
     * In order to send signal to other parts of the application, one must pass through the
     * RingBufferMananger. Audio instances must be registered into the RingBufferMananger and bound
     * together via the Manager.
     *
     */
    std::unique_ptr<RingBufferPool> ringbufferpool_;

    std::atomic_bool finished_ {false};
    std::atomic_bool shuttingDown_ {false};

    /// Protects pendingAlertInfoCalls_.
    std::mutex pendingAlertInfoMutex_;

    /// Active incoming calls that received an Alert-Info header and are
    /// awaiting a custom ringtone from the client. Keyed by call id.
    std::map<std::string, PendingAlertInfoCall> pendingAlertInfoCalls_;

    /* Sink ID mapping */
    std::map<std::string, std::weak_ptr<video::SinkClient>> sinkMap_;

    std::unique_ptr<VideoManager> videoManager_;

    std::unique_ptr<SIPVoIPLink> sipLink_;
};

Manager::ManagerPimpl::ManagerPimpl(Manager& base)
    : base_(base)
    , toneCtrl_(base.preferences)
    , dtmfBuf_(0, AudioFormat::MONO())
    , ringbufferpool_(new RingBufferPool)
#ifdef ENABLE_VIDEO
    , videoManager_(new VideoManager)
#endif
{
    sip_core::libav_utils::av_init();
}

bool
Manager::ManagerPimpl::parseConfiguration()
{
    bool result = true;

    try {
        std::ifstream file = fileutils::ifstream(path_);
        YAML::Node parsedFile = YAML::Load(file);
        file.close();
        const int error_count = base_.loadAccountMap(parsedFile);

        if (error_count > 0) {
            SIP_CORE_WARN("Errors while parsing %s", path_.c_str());
            result = false;
        }
    } catch (const YAML::BadFile& e) {
        SIP_CORE_WARN("Could not open configuration file");
        result = false;
    }

    return result;
}

/**
 * Multi Thread
 */
void
Manager::ManagerPimpl::playATone(Tone::ToneId toneId)
{
    if (not base_.voipPreferences.getPlayTones())
        return;

    std::lock_guard<std::mutex> lock(audioLayerMutex_);
    if (not audiodriver_) {
        SIP_CORE_ERR("Audio layer not initialized");
        return;
    }

    auto oldGuard = std::move(toneDeviceGuard_);
    toneDeviceGuard_ = base_.startAudioStream(AudioDeviceType::PLAYBACK);
    audiodriver_->flushUrgent();
    toneCtrl_.play(toneId);
}

int
Manager::ManagerPimpl::getCurrentDeviceIndex(AudioDeviceType type)
{
    if (not audiodriver_)
        return -1;
    switch (type) {
    case AudioDeviceType::PLAYBACK:
        return audiodriver_->getIndexPlayback();
    case AudioDeviceType::RINGTONE:
        return audiodriver_->getIndexRingtone();
    case AudioDeviceType::CAPTURE:
        return audiodriver_->getIndexCapture();
    default:
        return -1;
    }
}

void
Manager::ManagerPimpl::processRemainingParticipants(Conference& conf)
{
    const std::string current_callId(base_.getCurrentCallId());
    ParticipantSet participants(conf.getParticipantList());
    const size_t n = participants.size();
    SIP_CORE_DBG("Process remaining %zu participant(s) from conference %s",
                 n,
                 conf.getConfId().c_str());

    if (n > 1) {
        // Reset ringbuffer's readpointers
        for (const auto& p : participants)
            base_.getRingBufferPool().flush(p);

        base_.getRingBufferPool().flush(RingBufferPool::DEFAULT_ID);
    } else if (n == 1) {
        // this call is the last participant, hence
        // the conference is over
        auto p = participants.begin();
        if (auto call = base_.getCallFromCallID(*p)) {
            // if we are not listening to this conference and not a rendez-vous
            auto w = call->getAccount();
            auto account = w.lock();
            if (!account) {
                SIP_CORE_ERR("No account detected");
                return;
            }

            // Stay in a conference if 1 participants for swarm and rendezvous
            if (account->isRendezVous())
                return;

            // Else go in 1:1
            if (current_callId != conf.getConfId())
                base_.onHoldCall(account->getAccountID(), call->getCallId());
            else
                switchCall(call->getCallId());
        }

        SIP_CORE_DBG("No remaining participants, remove conference");
        if (auto account = conf.getAccount())
            account->removeConference(conf.getConfId());
    } else {
        SIP_CORE_DBG("No remaining participants, remove conference");
        if (auto account = conf.getAccount())
            account->removeConference(conf.getConfId());
        unsetCurrentCall();
    }
}

/**
 * Initialization: Main Thread
 */
std::string
Manager::ManagerPimpl::retrieveConfigPath() const
{
    return fileutils::get_config_dir() + DIR_SEPARATOR_STR + "sip.yaml";
}

void
Manager::ManagerPimpl::unsetCurrentCall()
{
    currentCall_ = "";
}

void
Manager::ManagerPimpl::switchCall(const std::string& id)
{
    std::lock_guard<std::mutex> m(currentCallMutex_);
    SIP_CORE_DBG("----- Switch current call id to '%s' -----", not id.empty() ? id.c_str() : "none");
    currentCall_ = id;
}

void
Manager::ManagerPimpl::addWaitingCall(const std::string& id)
{
    std::lock_guard<std::mutex> m(waitingCallsMutex_);
    // Enable incoming call beep if needed.
    if (audiodriver_ and waitingCalls_.empty() and not currentCall_.empty())
        audiodriver_->playIncomingCallNotification(true);
    waitingCalls_.insert(id);
}

void
Manager::ManagerPimpl::removeWaitingCall(const std::string& id)
{
    std::lock_guard<std::mutex> m(waitingCallsMutex_);
    waitingCalls_.erase(id);
    if (audiodriver_ and waitingCalls_.empty())
        audiodriver_->playIncomingCallNotification(false);
}

void
Manager::ManagerPimpl::loadAccount(const YAML::Node& node, int& errorCount)
{
    using yaml_utils::parseValue;
    using yaml_utils::parseValueOptional;

    std::string accountid;
    parseValue(node, "id", accountid);

    std::string accountType(ACCOUNT_TYPE_SIP);
    parseValue(node, "type", accountType);

    if (!accountid.empty()) {
        if (auto a = base_.accountFactory.createAccount(accountType, accountid)) {
            auto config = a->buildConfig();
            config->unserialize(node);
            a->setConfig(std::move(config));
        } else {
            SIP_CORE_ERROR("Failed to create account of type \"{:s}\"", accountType);
            ++errorCount;
        }
    }
}

// THREAD=VoIP
void
Manager::ManagerPimpl::sendTextMessageToConference(const Conference& conf,
                                                   const std::map<std::string, std::string>& messages,
                                                   const std::string& from) const noexcept
{
    ParticipantSet participants(conf.getParticipantList());
    for (const auto& callId : participants) {
        try {
            auto call = base_.getCallFromCallID(callId);
            if (not call)
                throw std::runtime_error("no associated call");
            call->sendTextMessage(messages, from);
        } catch (const std::exception& e) {
            SIP_CORE_ERR("Failed to send message to conference participant %s: %s",
                         callId.c_str(),
                         e.what());
        }
    }
}

void
Manager::ManagerPimpl::bindCallToConference(Call& call, Conference& conf)
{
    const auto& callId = call.getCallId();
    const auto& confId = conf.getConfId();
    const auto& state = call.getStateStr();

    // ensure that calls are only in one conference at a time
    if (call.isConferenceParticipant())
        base_.detachParticipant(callId);

    SIP_CORE_DBG("[call:%s] bind to conference %s (callState=%s)",
                 callId.c_str(),
                 confId.c_str(),
                 state.c_str());

    base_.getRingBufferPool().unBindAll(callId);
    // unBindAll only removes bindings where callId is the reader.
    // The 1-to-1 call's AudioReceiveThread::setup() created a half-duplex
    // binding where DEFAULT_ID reads from rb_callId — that lives in
    // readBindingsMap_[DEFAULT_ID] and is NOT cleaned by unBindAll(callId).
    // Remove it explicitly so the conference can establish its own bindings
    // (which respect localPlaybackMuted_).
    base_.getRingBufferPool().unBindHalfDuplexOut(RingBufferPool::DEFAULT_ID, callId);

    conf.addParticipant(callId);

    if (state == "HOLD") {
        conf.bindParticipant(callId);
        base_.offHoldCall(call.getAccountId(), callId);
    } else if (state == "INCOMING") {
        conf.bindParticipant(callId);
        base_.answerCall(call);
    } else if (state == "CURRENT") {
        conf.bindParticipant(callId);
    } else if (state == "INACTIVE") {
        conf.bindParticipant(callId);
        base_.answerCall(call);
    } else
        SIP_CORE_WARN("[call:%s] call state %s not recognized for conference",
                      callId.c_str(),
                      state.c_str());
}

bool
Manager::ManagerPimpl::hasActiveConference() const
{
    for (const auto& account : base_.getAllAccounts())
        if (!account->getConferenceList().empty())
            return true;
    return false;
}

//==============================================================================

Manager&
Manager::instance()
{
    // Leak-on-exit singleton (not a Meyers singleton): a plain
    // `static Manager instance` is destroyed during `__cxa_finalize_ranges`
    // at process exit, but other globals (notably `bindings::ClientImpl` and
    // its owned `InstanceHandler<DirectRenderer>` map) are finalised AFTER
    // us and still call `Manager::getSinkClient()` / similar from their
    // destructors. Touching the already-destroyed `callSinksMap_` (or any
    // other member) dereferences zeroed memory and the macOS app crashes on
    // every Cmd-Q.
    //
    // Allocate once on the heap and never free. The singleton outlives every
    // atexit handler, so late destructors can still reach Manager safely.
    // The OS reclaims memory at process exit anyway. Same pattern as the
    // `common_glue::TypeRepository` leak-on-exit fix.
    static Manager* instance = new Manager();

    // This will give a warning that can be ignored the first time instance()
    // is called...subsequent warnings are more serious
    if (not Manager::initialized)
        SIP_CORE_DBG("Not initialized");

    return *instance;
}

Manager::Manager()
    : preferences()
    , voipPreferences()
    , audioPreference()
#ifdef ENABLE_VIDEO
    , videoPreferences()
#endif
    , callFactory()
    , accountFactory()
    , rand_ {std::random_device {}()}
    , pimpl_(new ManagerPimpl(*this))
{}

Manager::~Manager() {}

void
Manager::setAutoAnswer(const std::string& accountId, bool enable) const
{
    if (auto account = getAccount(accountId)) {
        account->setAutoAnswer(enable);
    }
}

bool
Manager::setRingtone(const std::string& accountId, const std::string& ringtone)
{
    if (auto account = getAccount(accountId)) {
        return account->setRingtone(ringtone);
    }
    return false;
}

int
Manager::getKeepAliveInterval(const std::string& accountId)
{
    if (auto account = getAccount(accountId)) {
        if (account->config().type == ACCOUNT_TYPE_SIP) {
            auto sipAccount = std::static_pointer_cast<SIPAccount>(account);
            return sipAccount->config().keepAliveInterval;
        }
    }
    return 0;
}

void
Manager::setKeepAliveInterval(const std::string& accountId, int interval)
{
    if (auto account = getAccount(accountId)) {
        if (account->config().type == ACCOUNT_TYPE_SIP) {
            auto sipAccount = std::static_pointer_cast<SIPAccount>(account);
            const bool restoreMainRouteFastProbe = sipAccount->isUsingBackupRoute()
                                                   && sipAccount->isMainRouteFastProbeEnabled()
                                                   && interval > 0;
            const bool restoreActiveNoRouteFastProbe
                = SIPAccount::shouldUseOptionsForKeepAlive(
                      sipAccount->config().keepAliveType,
                      sipAccount->getTransportType() == PJSIP_TRANSPORT_UDP)
                  && sipAccount->isActiveNoRouteFastProbeEnabled() && interval > 0;
            sipAccount->editConfig(
                [&](SipAccountConfig& config) { config.keepAliveInterval = interval; });

            sipAccount->cancelKeepAliveTimer();
            sipAccount->cancelMainRouteKeepAliveTimer();
            sipAccount->cancelBackupRouteKeepAliveTimer();

            if (sipAccount->isUsable() && sipAccount->getTransport()) {
                sipAccount->registerKeepAliveTimer();
                if (restoreActiveNoRouteFastProbe)
                    sipAccount->enableActiveNoRouteFastProbe("manager-keepalive-interval-refresh");
                if (sipAccount->hasBackServiceRoutes()) {
                    if (sipAccount->isUsingBackupRoute()) {
                        sipAccount->registerMainRouteKeepAliveTimer();
                        if (restoreMainRouteFastProbe)
                            sipAccount->enableMainRouteFastProbe(
                                "manager-keepalive-interval-refresh");
                    } else {
                        sipAccount->registerBackupRouteKeepAliveTimer();
                    }
                }
            }
        }
    }
}

std::string
Manager::getRingtonePath(const std::string& accountId)
{
    if (auto account = getAccount(accountId)) {
        return account->getRingtonePath();
    }
    return "";
}

bool
Manager::getRingtoneEnabled(const std::string& accountId)
{
    if (auto account = getAccount(accountId)) {
        return account->getRingtoneEnabled();
    }
    return false;
}

void
Manager::setRingtoneEnabled(const std::string& accountId, bool enabled)
{
    if (auto account = getAccount(accountId)) {
        return account->setRingtoneEnabled(enabled);
    }
}

// Signal handler function — must be async-signal-safe.
// std::exit() is NOT safe here (runs atexit handlers / static dtors while
// potentially holding PJSIP locks).  _exit() is async-signal-safe.
static void
signalHandler(int signum)
{
    _exit(signum);
}

static void
beforeExit()
{
    Manager::instance().finish();
}

void
Manager::init(const std::string& config_file, const std::optional<std::string>& data_path)
{
    // FIXME: this is no good

    initialized = true;
    SIP_CORE_INFO("Using SIP core version %s for %s", libsip_core::version(), PJ_OS_NAME);

#ifndef WIN32
    // Set the max number of open files.
    struct rlimit nofiles;
    if (getrlimit(RLIMIT_NOFILE, &nofiles) == 0) {
        if (nofiles.rlim_cur < nofiles.rlim_max && nofiles.rlim_cur < 1024u) {
            nofiles.rlim_cur = std::min<rlim_t>(nofiles.rlim_max, 8192u);
            setrlimit(RLIMIT_NOFILE, &nofiles);
        }
    }
#endif

#define PJSIP_TRY(ret) \
    do { \
        if ((ret) != PJ_SUCCESS) \
            throw std::runtime_error(#ret " failed"); \
    } while (0)

    srand(time(nullptr)); // to get random number for RANDOM_PORT

    // Initialize PJSIP (SIP and ICE implementation)
    PJSIP_TRY(pj_init());
    setSipLogLevel();
    PJSIP_TRY(pjlib_util_init());
    PJSIP_TRY(pjnath_init());
#undef PJSIP_TRY

    SIP_CORE_DBG("Using PJSIP version %s for %s", pj_get_version(), PJ_OS_NAME);

    SIP_CORE_DBG("Using FFmpeg version %s", av_version_info());

    // Manager can restart without being recreated (Unit tests)
    // So only create the SipLink once
    pimpl_->sipLink_ = std::make_unique<SIPVoIPLink>();

    pimpl_->path_ = config_file.empty() ? pimpl_->retrieveConfigPath() : config_file;
    SIP_CORE_DBG("Configuration file path: %s", pimpl_->path_.c_str());

    pimpl_->data_path_ = data_path;

    // manager can restart without being recreated (Unit tests)
    pimpl_->finished_ = false;

    // call start explicitly if we trying to init core after finish().
    pimpl_->scheduler_.start();

    bool no_errors;

    try {
        no_errors = pimpl_->parseConfiguration();
    } catch (const YAML::Exception& e) {
        SIP_CORE_ERR("%s", e.what());
        no_errors = false;
    }

    {
        std::lock_guard<std::mutex> lock(pimpl_->audioLayerMutex_);
        pimpl_->initAudioDriver();
        if (pimpl_->audiodriver_) {
            pimpl_->toneCtrl_.setSampleRate(pimpl_->audiodriver_->getSampleRate());
            pimpl_->dtmfKey_.reset(new DTMF(getRingBufferPool().getInternalSamplingRate()));
        }
    }

    // Register atexit handler only once — repeated init/finish cycles must not
    // stack duplicate handlers.
    static bool atexitRegistered = false;
    if (!atexitRegistered) {
        atexit(beforeExit);
        atexitRegistered = true;
    }

    // Register the signal handler for common termination signals
    if (signal(SIGINT, signalHandler) == SIG_ERR) {
        std::cerr << "Failed to register signal handler for SIGINT" << std::endl;
    }
    if (signal(SIGTERM, signalHandler) == SIG_ERR) {
        std::cerr << "Failed to register signal handler for SIGTERM" << std::endl;
    }
}

void
Manager::finish() noexcept
{
    // return if not initialized
    if (not initialized)
        return;

    // return if already finished
    bool expected = false;
    if (not pimpl_->finished_.compare_exchange_strong(expected, true))
        return;

    try {
        SIP_CORE_DBG("Finishing started");

        // 1. Set global shutdown flag
        pimpl_->shuttingDown_ = true;

        // 2. Mark all SIP accounts as shutting down and cancel their timers.
        //    This disables transport recovery, reregistration, keepalive, and
        //    route-switching guards that are already scattered through SIPAccount.
        for (const auto& account : getAllAccounts<SIPAccount>()) {
            account->isShuttingDown_.store(true);
            account->cancelKeepAliveTimer();
            account->cancelMainRouteKeepAliveTimer();
            account->cancelBackupRouteKeepAliveTimer();
            // Cancel the auto-reregistration timer here too, while the PJSIP
            // endpoint is still alive. Otherwise ~SIPAccount (step 8, after the
            // endpoint is destroyed in step 7) would call
            // cancelAutoReregistrationTimer() -> pjsip_endpt_cancel_timer() on a
            // NULL/destroyed endpoint and crash (SIGSEGV in pjsip_endpt_cancel_timer).
            account->cancelAutoReregistrationTimer();
        }

        // 3. Hangup all remaining active calls
        SIP_CORE_DBG("Hangup %zu remaining call(s)", callFactory.callCount());
        for (const auto& call : callFactory.getAllCalls())
            hangupCall(call->getAccountId(), call->getCallId());
        callFactory.clear();

        // 4. Fire-and-forget UNREGISTER for every account.
        //    Each account sends the SIP UNREGISTER packet, then immediately
        //    destroys its regc (callback suppressed via pjsip_regc_destroy2).
        SIP_CORE_DBG("Fire-and-forget unregister for all accounts");
        unregisterAccountsImmediate();

        // 5. Stop the scheduler — prevents new callbacks from being dispatched
        //    while we tear down the SIP stack.
        pimpl_->scheduler_.stop();

        // 6. Detach transports from all accounts so that no account holds a
        //    shared_ptr<SipTransport> when the PJSIP endpoint is destroyed.
        //    Without this, ~SIPAccount would release the last transport
        //    reference AFTER pjsip_endpt_destroy, causing use-after-free.
        SIP_CORE_DBG("Detaching transports from all accounts");
        for (const auto& account : getAllAccounts<SIPAccount>()) {
            account->setTransport();
        }

        // 7. Shut down SIP stack: stop event loop, destroy transports, endpoint.
        //    NOTE: sipLink_->shutdown() calls sipTransportBroker->shutdown()
        //    which accesses Manager::instance().sipVoIPLink(), so the pointer
        //    MUST NOT be reset before shutdown() returns.
        if (pimpl_->sipLink_) {
            SIP_CORE_DBG("Shutting down sip voiplink");
            pimpl_->sipLink_->shutdown();
            SIP_CORE_DBG("Resetting sip voiplink");
            pimpl_->sipLink_.reset();
        }

        // 8. Destroy all accounts (triggers ~SIPAccount final cleanup).
        //    Safe now: event loop is stopped, scheduler is stopped, transports
        //    detached, no PJSIP callbacks can fire during account destruction.
        accountFactory.clear();

        // 9. Audio layer — no SIP dependencies, safe to tear down last.
        {
            std::lock_guard<std::mutex> lock(pimpl_->audioLayerMutex_);
            pimpl_->audiodriver_.reset();
        }

        SIP_CORE_DBG("pj_shutdown");
        pj_shutdown();

        SIP_CORE_DBG("Finishing completed");

    } catch (const VoipLinkException& err) {
        SIP_CORE_ERR("%s", err.what());
    }

    pimpl_->shuttingDown_ = false;
    pimpl_->finished_ = true;
    initialized = false;
}

void
Manager::monitor(bool continuous)
{
    Logger::setMonitorLog(true);
    SIP_CORE_DBG("############## START MONITORING ##############");
    SIP_CORE_DBG("Using PJSIP %s for %s", pj_get_version(), PJ_OS_NAME);

#ifdef __linux__
#if defined(__ANDROID__)
#else
    auto opened_files = fileutils::readDirectory("/proc/" + std::to_string(getpid()) + "/fd").size();
    SIP_CORE_DBG("Opened files: %lu", opened_files);
#endif
#endif

    for (const auto& call : callFactory.getAllCalls())
        call->monitor();
    SIP_CORE_DBG("############## END MONITORING ##############");
    Logger::setMonitorLog(continuous);
}

const std::optional<std::string>&
Manager::getDataPath() const
{
    return pimpl_->data_path_;
}

std::string
Manager::getConfigPath() const
{
    return pimpl_->path_;
}

bool
Manager::isCurrentCall(const Call& call) const
{
    return pimpl_->currentCall_ == call.getCallId();
}

bool
Manager::hasCurrentCall() const
{
    for (const auto& call : callFactory.getAllCalls()) {
        if (!call->isSubcall() && call->getStateStr() == libsip_core::Call::StateEvent::CURRENT)
            return true;
    }
    return false;
}

std::shared_ptr<Call>
Manager::getCurrentCall() const
{
    return getCallFromCallID(pimpl_->currentCall_);
}

const std::string&
Manager::getCurrentCallId() const
{
    return pimpl_->currentCall_;
}

void
Manager::unregisterAccounts()
{
    for (const auto& account : getAllAccounts()) {
        if (account->isEnabled()) {
            account->doUnregister();
        }
    }
}

void
Manager::unregisterAccountsImmediate()
{
    for (const auto& account : getAllAccounts<SIPAccount>()) {
        if (account->isEnabled()) {
            account->doUnregisterFireAndForget();
        }
    }
}

///////////////////////////////////////////////////////////////////////////////
// Management of events' IP-phone user
///////////////////////////////////////////////////////////////////////////////
/* Main Thread */

std::string
Manager::outgoingCall(const std::string& account_id,
                      const std::string& to,
                      const std::vector<libsip_core::MediaMap>& mediaList)
{
    SIP_CORE_DBG() << "try outgoing call to '" << to << "'"
                   << " with account '" << account_id << "'";

    std::shared_ptr<Call> call;

    try {
        call = newOutgoingCall(trim(to), account_id, mediaList);
    } catch (const std::exception& e) {
        SIP_CORE_ERR("%s", e.what());
        return {};
    }

    if (not call)
        return {};

    stopTone();

    pimpl_->switchCall(call->getCallId());

    return call->getCallId();
}

// THREAD=Main : for outgoing Call
bool
Manager::answerCall(const std::string& accountId,
                    const std::string& callId,
                    const std::vector<libsip_core::MediaMap>& mediaList)
{
    if (auto account = getAccount(accountId)) {
        if (auto call = account->getCall(callId)) {
            return answerCall(*call, mediaList);
        }
    }
    return false;
}

bool
Manager::switchTransport(const std::string& accountId, libsip_core::TransportType transportType)
{
    if (auto account = getAccount(accountId)) {
        return account->switchTransport(transportType);
    }
    return false;
}

bool
Manager::answerCall(Call& call, const std::vector<libsip_core::MediaMap>& mediaList)
{
    if (call.getCallType() != Call::CallType::INCOMING) {
        SIP_CORE_WARN("Ignoring answer request for non-incoming call %s", call.getCallId().c_str());
        return false;
    }
    SIP_CORE_INFO("Answer call %s", call.getCallId().c_str());

    if (call.getConnectionState() != Call::ConnectionState::RINGING) {
        // The call is already answered
        return true;
    }

    // If ringing
    stopTone();
    pimpl_->clearPendingAlertInfoCall(call.getCallId());
    pimpl_->removeWaitingCall(call.getCallId());

    try {
        call.answer(mediaList);
    } catch (const std::runtime_error& e) {
        SIP_CORE_ERR("%s", e.what());
        return false;
    }

    // if we dragged this call into a conference already
    if (auto conf = call.getConference())
        pimpl_->switchCall(conf->getConfId());
    else
        pimpl_->switchCall(call.getCallId());

    addAudio(call);

    // Start recording if set in preference
    if (audioPreference.getIsAlwaysRecording()) {
        auto recResult = call.toggleRecording();
        emitSignal<libsip_core::CallSignal::RecordPlaybackFilepath>(call.getCallId(),
                                                                    call.getPath());
        emitSignal<libsip_core::CallSignal::RecordingStateChanged>(call.getCallId(), recResult);
    }
    return true;
}

// THREAD=Main
bool
Manager::hangupCall(const std::string& accountId, const std::string& callId)
{
    auto account = getAccount(accountId);
    if (not account)
        return false;
    // store the current call id
    stopTone();
    pimpl_->clearPendingAlertInfoCall(callId);
    pimpl_->removeWaitingCall(callId);

    /* We often get here when the call was hungup before being created */
    auto call = account->getCall(callId);
    if (not call) {
        SIP_CORE_WARN("Could not hang up non-existant call %s", callId.c_str());
        return false;
    }

    // Disconnect streams
    removeAudio(*call);

    if (call->isConferenceParticipant()) {
        removeParticipant(*call);
    } else {
        // we are not participating in a conference, current call switched to ""
        if (isCurrentCall(*call))
            pimpl_->unsetCurrentCall();
    }

    try {
        call->hangup(0);
    } catch (const VoipLinkException& e) {
        SIP_CORE_ERR("%s", e.what());
        return false;
    }

    return true;
}

bool
Manager::hangupConference(const std::string& accountId, const std::string& confId)
{
    if (auto account = getAccount(accountId)) {
        if (auto conference = account->getConference(confId)) {
            return pimpl_->hangupConference(*conference);
        } else {
            SIP_CORE_ERR("No such conference %s", confId.c_str());
        }
    }
    return false;
}

// THREAD=Main
bool
Manager::onHoldCall(const std::string&, const std::string& callId)
{
    bool result = true;

    stopTone();

    std::string current_callId(getCurrentCallId());

    if (auto call = getCallFromCallID(callId)) {
        try {
            result = call->onhold([=](bool ok) {
                if (!ok) {
                    SIP_CORE_ERR("hold failed for call %s", callId.c_str());
                    return;
                }
                removeAudio(*call); // Unbind calls in main buffer
                // Remove call from the queue if it was still there
                pimpl_->removeWaitingCall(callId);

                // keeps current call id if the action is not holding this call
                // or a new outgoing call. This could happen in case of a conference
                if (current_callId == callId)
                    pimpl_->unsetCurrentCall();
            });
        } catch (const VoipLinkException& e) {
            SIP_CORE_ERR("%s", e.what());
            result = false;
        }
    } else {
        SIP_CORE_DBG("CallID %s doesn't exist in call onHold", callId.c_str());
        return false;
    }

    return result;
}

// THREAD=Main
bool
Manager::offHoldCall(const std::string&, const std::string& callId)
{
    bool result = true;

    stopTone();

    std::shared_ptr<Call> call = getCallFromCallID(callId);
    if (!call)
        return false;

    try {
        result = call->offhold([=](bool ok) {
            if (!ok) {
                SIP_CORE_ERR("off hold failed for call %s", callId.c_str());
                return;
            }

            if (auto conf = call->getConference())
                pimpl_->switchCall(conf->getConfId());
            else
                pimpl_->switchCall(call->getCallId());

            addAudio(*call);
        });
    } catch (const VoipLinkException& e) {
        SIP_CORE_ERR("%s", e.what());
        return false;
    }

    return result;
}

// THREAD=Main
bool
Manager::transferCall(const std::string& accountId, const std::string& callId, const std::string& to)
{
    auto account = getAccount(accountId);
    if (not account)
        return false;
    if (auto call = account->getCall(callId)) {
        if (call->isConferenceParticipant()) {
            removeParticipant(*call);
        } /*else if (not isConference(getCurrentCallId())) {
            pimpl_->unsetCurrentCall();
        }*/
        call->transfer(to);
    } else
        return false;

    // remove waiting call in case we make transfer without even answer
    pimpl_->removeWaitingCall(callId);

    return true;
}

void
Manager::transferFailed()
{
    emitSignal<libsip_core::CallSignal::TransferFailed>();
}

void
Manager::transferSucceeded()
{
    emitSignal<libsip_core::CallSignal::TransferSucceeded>();
}

// THREAD=Main : Call:Incoming
bool
Manager::refuseCall(const std::string& accountId, const std::string& id)
{
    if (auto account = getAccount(accountId)) {
        if (auto call = account->getCall(id)) {
            stopTone();
            pimpl_->clearPendingAlertInfoCall(id);
            call->refuse();
            pimpl_->removeWaitingCall(id);
            removeAudio(*call);
            return true;
        }
    }
    return false;
}

bool
Manager::holdConference(const std::string& accountId, const std::string& confId)
{
    SIP_CORE_INFO("Hold conference %s", confId.c_str());

    if (const auto account = getAccount(accountId)) {
        if (auto conf = account->getConference(confId)) {
            conf->detachLocalParticipant();
            emitSignal<libsip_core::CallSignal::ConferenceChanged>(accountId,
                                                                   conf->getConfId(),
                                                                   conf->getStateStr());
            return true;
        }
    }
    return false;
}

bool
Manager::unHoldConference(const std::string& accountId, const std::string& confId)
{
    SIP_CORE_DBG("[conf:%s] un-holding conference", confId.c_str());

    if (const auto account = getAccount(accountId)) {
        if (auto conf = account->getConference(confId)) {
            // Unhold conf only if it was in hold state otherwise...
            // all participants are restarted
            if (conf->getState() == Conference::State::HOLD) {
                for (const auto& item : conf->getParticipantList())
                    offHoldCall(accountId, item);

                pimpl_->switchCall(confId);
                conf->setState(Conference::State::ACTIVE_ATTACHED);
                emitSignal<libsip_core::CallSignal::ConferenceChanged>(accountId,
                                                                       conf->getConfId(),
                                                                       conf->getStateStr());
                return true;
            } else if (conf->getState() == Conference::State::ACTIVE_DETACHED) {
                pimpl_->addMainParticipant(*conf);
            }
        }
    }
    return false;
}

bool
Manager::addParticipant(const std::string& accountId,
                        const std::string& callId,
                        const std::string& account2Id,
                        const std::string& conferenceId)
{
    auto account = getAccount(accountId);
    auto account2 = getAccount(account2Id);
    if (account && account2) {
        auto call = account->getCall(callId);
        auto conf = account2->getConference(conferenceId);
        if (!call or !conf)
            return false;
        auto callConf = call->getConference();
        if (callConf != conf)
            return addParticipant(*call, *conf);
    }
    return false;
}

bool
Manager::addParticipant(Call& call, Conference& conference)
{
    // No-op if the call is already a conference participant
    /*if (call.getConfId() == conference.getConfId()) {
        SIP_CORE_WARN("Call %s already participant of conf %s", call.getCallId().c_str(),
    conference.getConfId().c_str()); return true;
    }*/

    SIP_CORE_DBG("Add participant %s to conference %s",
                 call.getCallId().c_str(),
                 conference.getConfId().c_str());

    // store the current call id (it will change in offHoldCall or in answerCall)
    pimpl_->bindCallToConference(call, conference);

    // Don't attach current user yet
    if (conference.getState() == Conference::State::ACTIVE_DETACHED)
        return true;

    // TODO: remove this ugly hack => There should be different calls when double clicking
    // a conference to add main participant to it, or (in this case) adding a participant
    // to conference
    pimpl_->unsetCurrentCall();
    pimpl_->addMainParticipant(conference);
    pimpl_->switchCall(conference.getConfId());
    addAudio(call);

    return true;
}

void
Manager::ManagerPimpl::addMainParticipant(Conference& conf)
{
    conf.attachLocalParticipant();
    emitSignal<libsip_core::CallSignal::ConferenceChanged>(conf.getAccountId(),
                                                           conf.getConfId(),
                                                           conf.getStateStr());
    switchCall(conf.getConfId());
}

bool
Manager::ManagerPimpl::hangupConference(Conference& conference)
{
    SIP_CORE_DBG("Hangup conference %s", conference.getConfId().c_str());
    ParticipantSet participants(conference.getParticipantList());
    for (const auto& callId : participants) {
        if (auto call = base_.getCallFromCallID(callId))
            base_.hangupCall(call->getAccountId(), callId);
    }
    unsetCurrentCall();
    return true;
}

bool
Manager::registerEventPackage(const std::string& eventPackage, int expires)
{
    return pimpl_->sipLink_->registerEventPackage(eventPackage, expires);
}

bool
Manager::addMainParticipant(const std::string& accountId, const std::string& conferenceId)
{
    SIP_CORE_INFO("Add main participant to conference %s", conferenceId.c_str());

    if (auto account = getAccount(accountId)) {
        if (auto conf = account->getConference(conferenceId)) {
            pimpl_->addMainParticipant(*conf);
            SIP_CORE_DBG("Successfully added main participant to conference %s",
                         conferenceId.c_str());
            return true;
        } else
            SIP_CORE_WARN("Failed to add main participant to conference %s", conferenceId.c_str());
    }
    return false;
}

std::shared_ptr<Call>
Manager::getCallFromCallID(const std::string& callID) const
{
    return callFactory.getCall(callID);
}

bool
Manager::joinParticipant(const std::string& accountId,
                         const std::string& callId1,
                         const std::string& account2Id,
                         const std::string& callId2,
                         bool attached,
                         bool muteLocalPlayback)
{
    SIP_CORE_INFO("JoinParticipant(%s, %s, %i)", callId1.c_str(), callId2.c_str(), attached);
    auto account = getAccount(accountId);
    auto account2 = getAccount(account2Id);
    if (not account or not account2) {
        return false;
    }

    SIP_CORE_INFO("Creating conference for participants %s and %s. Attach host [%s]",
                  callId1.c_str(),
                  callId2.c_str(),
                  attached ? "YES" : "NO");

    if (callId1 == callId2) {
        SIP_CORE_ERR("Cannot join participant %s to itself", callId1.c_str());
        return false;
    }

    bool attachLocalVideo = false;

    // Set corresponding conference ids for call 1
    auto call1 = account->getCall(callId1);
    if (!call1) {
        SIP_CORE_ERR("Could not find call %s", callId1.c_str());
        return false;
    }

    auto call1Media = call1->getMediaAttributeList();

    attachLocalVideo = std::any_of(call1Media.begin(),
                                   call1Media.end(),
                                   [](const MediaAttribute& media) {
                                       return media.hasValidVideo();
                                   });

    // use default source if not found
    std::string source;
    if (attachLocalVideo) {
        for (auto m : call1Media) {
            if (m.type_ == MediaType::MEDIA_VIDEO) {
                source = m.sourceUri_;
            }
        }
    }

    // Set corresponding conference details
    auto call2 = account2->getCall(callId2);
    if (!call2) {
        SIP_CORE_ERR("Could not find call %s", callId2.c_str());
        return false;
    }

    auto call2Media = call2->getMediaAttributeList();

    // is that true for call2 ?
    if (!attachLocalVideo) {
        attachLocalVideo = std::any_of(call2Media.begin(),
                                       call2Media.end(),
                                       [](const MediaAttribute& media) {
                                           return media.hasValidVideo();
                                       });
    }

    if (attachLocalVideo) {
        for (auto m : call2Media) {
            if (m.type_ == MediaType::MEDIA_VIDEO) {
                source = m.sourceUri_;
            }
        }
    }

    auto conf = std::make_shared<Conference>(account, "");

    // Set the local playback mute flag BEFORE any bindings are established.
    // attachLocalParticipant() and bindParticipant() already respect this flag,
    // using half-duplex (host-inaudible) bindings when it is true.
    if (muteLocalPlayback)
        conf->muteLocalPlayback(true);

    account->attach(conf);
    emitSignal<libsip_core::CallSignal::ConferenceCreated>(account->getAccountID(),
                                                           conf->getConfId());

    // Bind calls according to their state
    // if audio only, they will be added as AUDIO only sources
    pimpl_->bindCallToConference(*call1, *conf);
    pimpl_->bindCallToConference(*call2, *conf);

    // Switch current call id to this conference
    if (attached) {
        // attach local participant
        conf->setLocalHostDefaultMediaSource(attachLocalVideo, source);
        conf->attachLocalParticipant();
        pimpl_->switchCall(conf->getConfId());
        conf->setState(Conference::State::ACTIVE_ATTACHED);
    } else {
        conf->detachLocalParticipant();
    }
    emitSignal<libsip_core::CallSignal::ConferenceChanged>(account->getAccountID(),
                                                           conf->getConfId(),
                                                           conf->getStateStr());

    return true;
}

void
Manager::createConfFromParticipantList(const std::string& accountId,
                                       const std::vector<std::string>& participantList)
{
    auto account = getAccount(accountId);
    if (not account) {
        SIP_CORE_WARN("Can't find account");
        return;
    }

    // we must at least have 2 participant for a conference
    if (participantList.size() <= 1) {
        SIP_CORE_ERR("Participant number must be higher or equal to 2");
        return;
    }

    auto conf = std::make_shared<Conference>(account);

    unsigned successCounter = 0;
    for (const auto& numberaccount : participantList) {
        std::string tostr(numberaccount.substr(0, numberaccount.find(',')));
        std::string account(numberaccount.substr(numberaccount.find(',') + 1, numberaccount.size()));

        pimpl_->unsetCurrentCall();

        // Create call
        auto callId = outgoingCall(account, tostr, {});
        if (callId.empty())
            continue;

        // Manager methods may behave differently if the call id participates in a conference
        conf->addParticipant(callId);
        successCounter++;
    }

    // Create the conference if and only if at least 2 calls have been successfully created
    if (successCounter >= 2) {
        account->attach(conf);
        emitSignal<libsip_core::CallSignal::ConferenceCreated>(accountId, conf->getConfId());
    }
}

bool
Manager::detachLocalParticipant(const std::shared_ptr<Conference>& conf)
{
    if (not conf)
        return false;

    SIP_CORE_INFO("Detach local participant from conference %s", conf->getConfId().c_str());
    conf->detachLocalParticipant();
    emitSignal<libsip_core::CallSignal::ConferenceChanged>(conf->getAccountId(),
                                                           conf->getConfId(),
                                                           conf->getStateStr());
    pimpl_->unsetCurrentCall();
    return true;
}

bool
Manager::detachParticipant(const std::string& callId)
{
    SIP_CORE_DBG("Detach participant %s", callId.c_str());

    auto call = getCallFromCallID(callId);
    if (!call) {
        SIP_CORE_ERR("Could not find call %s", callId.c_str());
        return false;
    }

    removeParticipant(*call);
    return true;
}

void
Manager::removeParticipant(Call& call)
{
    SIP_CORE_DBG("Remove participant %s", call.getCallId().c_str());

    auto conf = call.getConference();
    if (not conf) {
        SIP_CORE_ERR("No conference, cannot remove participant");
        return;
    }

    conf->removeParticipant(call.getCallId());

    removeAudio(call);

    emitSignal<libsip_core::CallSignal::ConferenceChanged>(conf->getAccountId(),
                                                           conf->getConfId(),
                                                           conf->getStateStr());

    pimpl_->processRemainingParticipants(*conf);
}

bool
Manager::joinConference(const std::string& accountId,
                        const std::string& confId1,
                        const std::string& account2Id,
                        const std::string& confId2)
{
    auto account = getAccount(accountId);
    auto account2 = getAccount(account2Id);
    if (not account) {
        SIP_CORE_ERR("Can't find account: %s", accountId.c_str());
        return false;
    }
    if (not account2) {
        SIP_CORE_ERR("Can't find account: %s", account2Id.c_str());
        return false;
    }

    auto conf = account->getConference(confId1);
    if (not conf) {
        SIP_CORE_ERR("Not a valid conference ID: %s", confId1.c_str());
        return false;
    }

    auto conf2 = account2->getConference(confId2);
    if (not conf2) {
        SIP_CORE_ERR("Not a valid conference ID: %s", confId2.c_str());
        return false;
    }

    ParticipantSet participants(conf->getParticipantList());

    std::vector<std::shared_ptr<Call>> calls;
    calls.reserve(participants.size());

    // Detach and remove all participant from conf1 before add
    // ... to conf2
    for (const auto& p : participants) {
        SIP_CORE_DBG("Detach participant %s", p.c_str());
        if (auto call = account->getCall(p)) {
            conf->removeParticipant(p);
            removeAudio(*call);
            calls.emplace_back(std::move(call));
        } else {
            SIP_CORE_ERR("Could not find call %s", p.c_str());
        }
    }
    // Remove conf1
    account->removeConference(confId1);

    for (const auto& c : calls)
        addParticipant(*c, *conf2);

    return true;
}

void
Manager::addAudio(Call& call)
{
    const auto& callId = call.getCallId();
    SIP_CORE_INFO("Add audio to call %s", callId.c_str());

    if (call.isConferenceParticipant()) {
        SIP_CORE_DBG("[conf:%s] Attach local audio", callId.c_str());

        // bind to conference participant
        /*auto iter = pimpl_->conferenceMap_.find(callId);
        if (iter != pimpl_->conferenceMap_.end() and iter->second) {
            iter->second->bindParticipant(callId);
        }*/
    } else {
        SIP_CORE_DBG("[call:%s] Attach audio", callId.c_str());

        // bind to main
        getRingBufferPool().bindCallID(callId, RingBufferPool::DEFAULT_ID);
        auto oldGuard = std::move(call.audioGuard);
        call.audioGuard = startAudioStream(AudioDeviceType::PLAYBACK);

        // Pin the capture stream for the call's lifetime. AudioRtpSession::stop()
        // (called on every re-invite via stopAllMedia/startAllMedia) drops the
        // AudioInput, which drops its own AudioDeviceGuard(CAPTURE). Without
        // this anchor the user count would hit 0, PulseLayer::stopStream(CAPTURE)
        // would tear the xrdp-source stream down, and on xrdp the recreated
        // stream silently fails to deliver samples for many seconds. Holding
        // a second guard here keeps the count >=1 across stop()/start().
        auto oldCaptureGuard = std::move(call.audioCaptureGuard);
        call.audioCaptureGuard = startAudioStream(AudioDeviceType::CAPTURE);

        std::lock_guard<std::mutex> lock(pimpl_->audioLayerMutex_);
        if (!pimpl_->audiodriver_) {
            SIP_CORE_ERR("Audio driver not initialized");
            return;
        }
        pimpl_->audiodriver_->flushUrgent();
        getRingBufferPool().flushAllBuffers();
    }
}

void
Manager::removeAudio(Call& call)
{
    const auto& callId = call.getCallId();
    SIP_CORE_DBG("[call:%s] Remove local audio", callId.c_str());
    getRingBufferPool().unBindAll(callId);
    call.audioGuard.reset();
    call.audioCaptureGuard.reset();
}

ScheduledExecutor&
Manager::scheduler()
{
    return pimpl_->scheduler_;
}

std::shared_ptr<Task>
Manager::scheduleTask(std::function<void()>&& task,
                      std::chrono::steady_clock::time_point when,
                      const char* filename,
                      uint32_t linum)
{
    return pimpl_->scheduler_.schedule(std::move(task), when, filename, linum);
}

std::shared_ptr<Task>
Manager::scheduleTaskIn(std::function<void()>&& task,
                        std::chrono::steady_clock::duration timeout,
                        const char* filename,
                        uint32_t linum)
{
    return pimpl_->scheduler_.scheduleIn(std::move(task), timeout, filename, linum);
}

void
Manager::saveConfig(const std::shared_ptr<Account>& acc)
{
    saveConfig();
}

void
Manager::setCaptureGain(double gain)
{
    audioPreference.setVolumemic(gain);
    pimpl_->audiodriver_->setCaptureGain(gain);
    saveConfig();
}

double
Manager::getCaptureGain() const
{
    return audioPreference.getVolumemic();
}

void
Manager::setPlaybackGain(double gain)
{
    audioPreference.setVolumespkr(gain);
    pimpl_->audiodriver_->setPlaybackGain(gain);
    saveConfig();
}

double
Manager::getPlaybackGain() const
{
    return audioPreference.getVolumespkr();
}

void
Manager::saveConfig()
{
    SIP_CORE_DBG("Saving Configuration to DATA directory %s", pimpl_->path_.c_str());

    if (pimpl_->audiodriver_) {
        audioPreference.setVolumemic(pimpl_->audiodriver_->getCaptureGain());
        audioPreference.setVolumespkr(pimpl_->audiodriver_->getPlaybackGain());
        audioPreference.setCaptureMuted(pimpl_->audiodriver_->isCaptureMuted());
        audioPreference.setPlaybackMuted(pimpl_->audiodriver_->isPlaybackMuted());
    }

    try {
        YAML::Emitter out;

        // FIXME maybe move this into accountFactory?
        out << YAML::BeginMap << YAML::Key << "accounts";
        out << YAML::Value << YAML::BeginSeq;

        for (const auto& account : accountFactory.getAllAccounts()) {
            account->config().serialize(out);
        }
        out << YAML::EndSeq;

        // FIXME: this is a hack until we get rid of accountOrder
        preferences.verifyAccountOrder(getAccountList());
        preferences.serialize(out);
        voipPreferences.serialize(out);
        audioPreference.serialize(out);
#ifdef ENABLE_VIDEO
        videoPreferences.serialize(out);
#endif
        std::lock_guard<std::mutex> lock(fileutils::getFileLock(pimpl_->path_));
        std::ofstream fout = fileutils::ofstream(pimpl_->path_);
        fout.write(out.c_str(), out.size());
    } catch (const YAML::Exception& e) {
        SIP_CORE_ERR("%s", e.what());
    } catch (const std::runtime_error& e) {
        SIP_CORE_ERR("%s", e.what());
    }
}

// THREAD=Main | VoIPLink
void
Manager::playDtmf(char code)
{
    stopTone();

    if (not voipPreferences.getPlayDtmf()) {
        SIP_CORE_DBG("Do not have to play a tone...");
        return;
    }

    // length in milliseconds
    int pulselen = voipPreferences.getPulseLength();

    if (pulselen == 0) {
        SIP_CORE_DBG("Pulse length is not set...");
        return;
    }

    std::lock_guard<std::mutex> lock(pimpl_->audioLayerMutex_);

    // fast return, no sound, so no dtmf
    if (not pimpl_->audiodriver_ or not pimpl_->dtmfKey_) {
        SIP_CORE_DBG("No audio layer...");
        return;
    }

    std::shared_ptr<AudioDeviceGuard> audioGuard = startAudioStream(AudioDeviceType::PLAYBACK);
    if (not pimpl_->audiodriver_->waitForStart(std::chrono::seconds(1))) {
        SIP_CORE_ERR("Failed to start audio layer...");
        return;
    }

    // number of data sampling in one pulselen depends on samplerate
    // size (n sampling) = time_ms * sampling/s
    //                     ---------------------
    //                            ms/s
    int size = (int) ((pulselen * (float) pimpl_->audiodriver_->getSampleRate()) / 1000);
    pimpl_->dtmfBuf_.resize(size);

    // Handle dtmf
    pimpl_->dtmfKey_->startTone(code);

    // copy the sound
    if (pimpl_->dtmfKey_->generateDTMF(*pimpl_->dtmfBuf_.getChannel(0))) {
        // Put buffer to urgentRingBuffer
        // put the size in bytes...
        // so size * 1 channel (mono) * sizeof (bytes for the data)
        // audiolayer->flushUrgent();

        pimpl_->audiodriver_->putUrgent(pimpl_->dtmfBuf_);
    }

    scheduler().scheduleIn([audioGuard] { SIP_CORE_WARN("End of dtmf"); },
                           std::chrono::milliseconds(pulselen));

    // TODO Cache the DTMF
}

// Multi-thread
bool
Manager::incomingCallsWaiting()
{
    std::lock_guard<std::mutex> m(pimpl_->waitingCallsMutex_);
    return not pimpl_->waitingCalls_.empty();
}

void
Manager::incomingCall(const std::string& accountId,
                      Call& call,
                      const std::map<std::string, std::string>& headers)
{
    if (not accountId.empty()) {
        pimpl_->stripSipPrefix(call);
    }

    std::string from("<" + call.getPeerNumber() + ">");

    auto const& account = getAccount(accountId);
    if (not account) {
        SIP_CORE_ERR("Incoming call %s on unknown account %s",
                     call.getCallId().c_str(),
                     accountId.c_str());
        return;
    }

    // Process the call.
    pimpl_->processIncomingCall(accountId, call, headers);
}

void
Manager::incomingMessage(const std::string& accountId,
                         const std::string& callId,
                         const std::string& from,
                         const std::map<std::string, std::string>& messages)
{
    auto account = getAccount(accountId);
    if (not account) {
        return;
    }
    if (auto call = account->getCall(callId)) {
        if (call->isConferenceParticipant()) {
            if (auto conf = call->getConference()) {
                SIP_CORE_DBG("Is a conference, send incoming message to everyone");
                // filter out vcards messages  as they could be resent by master as its own vcard
                // TODO. Implement a protocol to handle vcard messages
                bool sendToOtherParicipants = true;
                for (auto& message : messages) {
                    if (message.first.find("x-ring/ring.profile.vcard") != std::string::npos) {
                        sendToOtherParicipants = false;
                    }
                }
                if (sendToOtherParicipants) {
                    pimpl_->sendTextMessageToConference(*conf, messages, from);
                }

                // in case of a conference we must notify client using conference id
                emitSignal<libsip_core::CallSignal::IncomingMessage>(accountId,
                                                                     conf->getConfId(),
                                                                     from,
                                                                     messages);
            } else {
                SIP_CORE_ERR("no conference associated to ID %s", callId.c_str());
            }
        } else {
            emitSignal<libsip_core::CallSignal::IncomingMessage>(accountId, callId, from, messages);
        }
    }
}

void
Manager::sendCallTextMessage(const std::string& accountId,
                             const std::string& callID,
                             const std::map<std::string, std::string>& messages,
                             const std::string& from,
                             bool /*isMixed TODO: use it */)
{
    auto account = getAccount(accountId);
    if (not account) {
        return;
    }

    if (auto conf = account->getConference(callID)) {
        SIP_CORE_DBG("Is a conference, send instant message to everyone");
        pimpl_->sendTextMessageToConference(*conf, messages, from);
    } else if (auto call = account->getCall(callID)) {
        if (call->isConferenceParticipant()) {
            if (auto conf = call->getConference()) {
                SIP_CORE_DBG(
                    "Call is participant in a conference, send instant message to everyone");
                pimpl_->sendTextMessageToConference(*conf, messages, from);
            } else {
                SIP_CORE_ERR("no conference associated to call ID %s", callID.c_str());
            }
        } else {
            try {
                call->sendTextMessage(messages, from);
            } catch (const im::InstantMessageException& e) {
                SIP_CORE_ERR("Failed to send message to call %s: %s",
                             call->getCallId().c_str(),
                             e.what());
            }
        }
    } else {
        SIP_CORE_ERR("Failed to send message to %s: inexistent call ID", callID.c_str());
    }
}

// THREAD=Main
void
Manager::onCallEarlyMedia(Call& call)
{
    SIP_CORE_DBG("[call:%s] Early media started, enabling playback", call.getCallId().c_str());

    // Start the playback device BEFORE stopping the tone so the refcount
    // never drops to zero (avoids a brief playback-stream gap).
    auto oldGuard = std::move(call.audioGuard);
    call.audioGuard = startAudioStream(AudioDeviceType::PLAYBACK);

    // Stop any local ringback tone — the server is now providing audio.
    // Always stop regardless of current-call status because the tone is
    // global and now gets mixed into every active audio stream.
    stopTone();

    if (pimpl_->audiodriver_) {
        std::lock_guard<std::mutex> lock(pimpl_->audioLayerMutex_);
        pimpl_->audiodriver_->flushUrgent();
    }
}

// THREAD=VoIP CALL=Outgoing
void
Manager::peerAnsweredCall(Call& call)
{
    const auto& callId = call.getCallId();
    SIP_CORE_DBG("[call:%s] Peer answered", callId.c_str());

    // Always stop the ringback tone — it is global and now gets mixed into
    // every active audio stream, so it must be silenced as soon as any
    // outgoing call is answered.
    stopTone();

    addAudio(call);

    if (pimpl_->audiodriver_) {
        std::lock_guard<std::mutex> lock(pimpl_->audioLayerMutex_);
        getRingBufferPool().flushAllBuffers();
        pimpl_->audiodriver_->flushUrgent();
    }

    if (audioPreference.getIsAlwaysRecording()) {
        auto result = call.toggleRecording();
        emitSignal<libsip_core::CallSignal::RecordPlaybackFilepath>(callId, call.getPath());
        emitSignal<libsip_core::CallSignal::RecordingStateChanged>(callId, result);
    }
}

// THREAD=VoIP Call=Outgoing
void
Manager::peerRingingCall(Call& call)
{
    SIP_CORE_DBG("[call:%s] Peer ringing!!!", call.getCallId().c_str());

    // Always play the ringback tone.  AudioLayer::getToPlay() now mixes
    // the tone with any active call / conference audio instead of choosing
    // one over the other, so the ringback is audible without muting the
    // ongoing conversation.
    ringback();
}

// THREAD=VoIP Call=Outgoing/Ingoing
void
Manager::peerHungupCall(Call& call)
{
    const auto& callId = call.getCallId();
    SIP_CORE_DBG("[call:%s] Peer hung up", callId.c_str());

    pimpl_->clearPendingAlertInfoCall(callId);

    if (call.isConferenceParticipant()) {
        removeParticipant(call);
    } else if (isCurrentCall(call)) {
        stopTone();
        pimpl_->unsetCurrentCall();
    }

    call.peerHungup();

    pimpl_->removeWaitingCall(callId);
    if (not incomingCallsWaiting())
        stopTone();

    removeAudio(call);
}

// THREAD=VoIP
void
Manager::callBusy(Call& call)
{
    SIP_CORE_DBG("[call:%s] Busy", call.getCallId().c_str());

    pimpl_->clearPendingAlertInfoCall(call.getCallId());

    if (isCurrentCall(call)) {
        pimpl_->unsetCurrentCall();
    }

    pimpl_->removeWaitingCall(call.getCallId());
    if (not incomingCallsWaiting())
        stopTone();
}

// THREAD=VoIP
void
Manager::callFailure(Call& call)
{
    SIP_CORE_DBG("[call:%s] %s failed",
                 call.getCallId().c_str(),
                 call.isSubcall() ? "Sub-call" : "Parent call");

    pimpl_->clearPendingAlertInfoCall(call.getCallId());

    if (isCurrentCall(call)) {
        pimpl_->unsetCurrentCall();
    }

    if (call.isConferenceParticipant()) {
        SIP_CORE_DBG("[call %s] Participating in a conference. Remove", call.getCallId().c_str());
        // remove this participant
        removeParticipant(call);
    }

    pimpl_->removeWaitingCall(call.getCallId());
    if (not incomingCallsWaiting())
        stopTone();
    removeAudio(call);
}

/**
 * Multi Thread
 */
void
Manager::stopTone()
{
    if (not voipPreferences.getPlayTones())
        return;

    pimpl_->toneCtrl_.stop();
    pimpl_->toneDeviceGuard_.reset();
}

/**
 * Multi Thread
 */
void
Manager::playTone()
{
    pimpl_->playATone(Tone::ToneId::DIALTONE);
}

/**
 * Multi Thread
 */
void
Manager::playToneWithMessage()
{
    pimpl_->playATone(Tone::ToneId::CONGESTION);
}

/**
 * Multi Thread
 */
void
Manager::congestion()
{
    pimpl_->playATone(Tone::ToneId::CONGESTION);
}

/**
 * Multi Thread
 */
void
Manager::ringback()
{
    pimpl_->playATone(Tone::ToneId::RINGTONE);
}

/**
 * Multi Thread
 */
void
Manager::playRingtone(const std::string& accountID)
{
    const auto account = getAccount(accountID);
    if (!account) {
        SIP_CORE_WARN("Invalid account in ringtone");
        return;
    }

    if (account->isAutoAnswerEnabled()) {
        return;
    }

    if (!account->getRingtoneEnabled()) {
        ringback();
        return;
    }

    {
        std::lock_guard<std::mutex> lock(pimpl_->audioLayerMutex_);

        if (not pimpl_->audiodriver_) {
            SIP_CORE_ERR("no audio layer in ringtone");
            return;
        }
        // start audio if not started AND flush all buffers (main and urgent)
        auto oldGuard = std::move(pimpl_->toneDeviceGuard_);
        pimpl_->toneDeviceGuard_ = startAudioStream(AudioDeviceType::RINGTONE);
        pimpl_->toneCtrl_.setSampleRate(pimpl_->audiodriver_->getSampleRate());
    }

    if (not pimpl_->toneCtrl_.setAudioFile(account->getRingtonePath()))
        ringback();
}

std::shared_ptr<AudioLoop>
Manager::getTelephoneTone()
{
    return pimpl_->toneCtrl_.getTelephoneTone();
}

std::shared_ptr<AudioLoop>
Manager::getTelephoneFile()
{
    return pimpl_->toneCtrl_.getTelephoneFile();
}

/**
 * Set input audio plugin
 */
void
Manager::setAudioPlugin(const std::string& audioPlugin)
{
    {
        std::lock_guard<std::mutex> lock(pimpl_->audioLayerMutex_);
        audioPreference.setAlsaPlugin(audioPlugin);
        pimpl_->audiodriver_.reset();
        pimpl_->initAudioDriver();
    }
    // Recreate audio driver with new settings
    saveConfig();
}

/**
 * Set audio output device
 */
void
Manager::setAudioDevice(int index, AudioDeviceType type)
{
    SIP_CORE_INFO() << "Setting audio device " << index << " type " << static_cast<int>(type);

    std::lock_guard<std::mutex> lock(pimpl_->audioLayerMutex_);

    if (not pimpl_->audiodriver_) {
        SIP_CORE_ERR("Audio driver not initialized");
        return;
    }
    if (pimpl_->getCurrentDeviceIndex(type) == index) {
        SIP_CORE_WARN("Audio device already selected ; doing nothing.");
        return;
    }

    pimpl_->audiodriver_->updatePreference(audioPreference, index, type);

    // Recreate audio driver with new settings
    pimpl_->audiodriver_.reset();
    pimpl_->initAudioDriver();
    saveConfig();
}

/**
 * Get list of supported audio output device
 */
std::vector<std::string>
Manager::getAudioOutputDeviceList()
{
    std::lock_guard<std::mutex> lock(pimpl_->audioLayerMutex_);

    if (not pimpl_->audiodriver_) {
        SIP_CORE_ERR("Audio layer not initialized");
        return {};
    }

    return pimpl_->audiodriver_->getPlaybackDeviceList();
}

/**
 * Get list of supported audio input device
 */
std::vector<std::string>
Manager::getAudioInputDeviceList()
{
    std::lock_guard<std::mutex> lock(pimpl_->audioLayerMutex_);

    if (not pimpl_->audiodriver_) {
        SIP_CORE_ERR("Audio layer not initialized");
        return {};
    }

    return pimpl_->audiodriver_->getCaptureDeviceList();
}

/**
 * Get string array representing integer indexes of output and input device
 */
std::vector<int>
Manager::getCurrentAudioDevicesIndex()
{
    std::lock_guard<std::mutex> lock(pimpl_->audioLayerMutex_);
    if (not pimpl_->audiodriver_) {
        SIP_CORE_ERR("Audio layer not initialized");
        return {};
    }

    return {pimpl_->audiodriver_->getIndexPlayback(),
            pimpl_->audiodriver_->getIndexCapture(),
            pimpl_->audiodriver_->getIndexRingtone()};
}

std::string
Manager::getHomePath()
{
    return fileutils::get_home_dir();
}

void
Manager::startAudio()
{
#if (defined(TARGET_OS_IOS) && TARGET_OS_IOS)
    SIP_CORE_INFO("ios -> startAudio");

    // Recreate audio driver with new settings
    pimpl_->audiodriver_.reset(pimpl_->base_.audioPreference.createAudioLayer());

    constexpr std::array<AudioDeviceType, 2> TYPES {AudioDeviceType::CAPTURE,
                                                    AudioDeviceType::PLAYBACK};

    for (const auto& type : TYPES)
        if (pimpl_->audioStreamUsers_[(unsigned) type])
            pimpl_->audiodriver_->startStream(type);
#endif
}
void
Manager::recoverAudioDevices()
{
    if (!initialized || pimpl_->finished_ || pimpl_->shuttingDown_)
        return;

    std::shared_ptr<AudioLayer> refreshedDriver;
    {
        std::lock_guard<std::mutex> lock(pimpl_->audioLayerMutex_);
        SIP_CORE_WARN("Audio devices changed, refreshing audio layer");
        pimpl_->audiodriver_.reset();
        pimpl_->initAudioDriver();
        refreshedDriver = pimpl_->audiodriver_;
    }

    if (refreshedDriver) {
        refreshedDriver->notifyDevicesChanged();
    } else {
        SIP_CORE_ERR("Audio devices changed, but audio layer could not be recreated");
        onAudioDevicesChanged();
    }
}

AudioDeviceGuard::AudioDeviceGuard(Manager& manager, AudioDeviceType type)
    : manager_(manager)
    , type_(type)
{
    auto streamId = (unsigned) type;
    if (streamId >= manager_.pimpl_->audioStreamUsers_.size())
        throw std::invalid_argument("Invalid audio device type");
    std::lock_guard<std::mutex> lk(manager_.pimpl_->audioStreamMutex_);
    if (manager_.pimpl_->audioStreamUsers_[streamId]++ == 0) {
        // If a deferred stop is pending the underlying device stream is
        // still alive — cancel the task and reuse it without touching
        // PulseAudio (xrdp-source on Astra 1.8 silently stalls when
        // destroyed and recreated within the same instant, e.g. on the
        // hold→outgoing→hangup→unhold cycle). Otherwise the stream is
        // really stopped and must be started from scratch.
        if (auto& pending = manager_.pimpl_->audioStreamStopTask_[streamId]) {
            pending->cancel();
            pending.reset();
        } else if (auto layer = manager_.getAudioDriver()) {
            layer->startStream(type);
        }
    }
}

AudioDeviceGuard::~AudioDeviceGuard()
{
    auto streamId = (unsigned) type_;
    std::lock_guard<std::mutex> lk(manager_.pimpl_->audioStreamMutex_);
    if (--manager_.pimpl_->audioStreamUsers_[streamId] != 0)
        return;

    // Defer the actual stopStream so a fresh guard within the linger
    // window can reuse the existing device stream. The hold→outgoing→
    // hangup→unhold path tears the capture guard down and re-acquires
    // it within ~2 ms; PulseAudio's xrdp-source enters a deaf state in
    // that window. A 750 ms linger comfortably covers re-invite media
    // renegotiation and call-to-call transitions.
    auto& manager = manager_;
    auto streamType = type_;
    auto streamIdx = streamId;
    manager_.pimpl_->audioStreamStopTask_[streamId] = manager.scheduleTaskIn(
        [&manager, streamType, streamIdx]() {
            std::lock_guard<std::mutex> lk(manager.pimpl_->audioStreamMutex_);
            // Clear the slot first so a re-entrant ctor on this same
            // thread doesn't try to cancel a now-firing task.
            manager.pimpl_->audioStreamStopTask_[streamIdx].reset();
            if (manager.pimpl_->audioStreamUsers_[streamIdx].load() != 0)
                return; // a new guard reclaimed the stream
            if (auto layer = manager.getAudioDriver())
                layer->stopStream(streamType);
        },
        std::chrono::milliseconds(750),
        __FILE__, __LINE__);
}

bool
Manager::getIsAlwaysRecording() const
{
    return audioPreference.getIsAlwaysRecording();
}

void
Manager::setIsAlwaysRecording(bool isAlwaysRec)
{
    audioPreference.setIsAlwaysRecording(isAlwaysRec);
    saveConfig();
}

bool
Manager::toggleRecordingCall(const std::string& accountId, const std::string& id)
{
    bool result = false;
    if (auto account = getAccount(accountId)) {
        std::shared_ptr<Recordable> rec;
        if (auto conf = account->getConference(id)) {
            SIP_CORE_DBG("toggle recording for conference %s", id.c_str());
            rec = conf;
        } else if (auto call = account->getCall(id)) {
            SIP_CORE_DBG("toggle recording for call %s", id.c_str());
            rec = call;
        } else {
            SIP_CORE_ERR("Could not find recordable instance %s", id.c_str());
            return false;
        }
        result = rec->toggleRecording();
        emitSignal<libsip_core::CallSignal::RecordPlaybackFilepath>(id, rec->getPath());
        emitSignal<libsip_core::CallSignal::RecordingStateChanged>(id, result);
    }
    return result;
}

bool
Manager::startRecordedFilePlayback(const std::string& filepath)
{
    auto data_path = Manager::instance().getDataPath();

    if (!data_path.has_value()) {
        return false;
    }

    auto soundDir = fmt::format("{}/{}", data_path.value(), "sounds");
    auto sound = fileutils::getFullPath(soundDir, filepath);
    SIP_CORE_DBG("Start recorded file playback %s", sound.c_str());

    std::lock_guard<std::mutex> lock(pimpl_->audioLayerMutex_);

    if (not pimpl_->audiodriver_) {
        SIP_CORE_ERR("No audio layer in start recorded file playback");
        return false;
    }

    std::shared_ptr<AudioDeviceGuard> audioGuard = startAudioStream(AudioDeviceType::PLAYBACK);

    if (not pimpl_->audiodriver_->waitForStart(std::chrono::seconds(1))) {
        SIP_CORE_ERR("Failed to start audio layer...");
        return false;
    }

    pimpl_->currentFile_.reset(new AudioFile(sound, pimpl_->audiodriver_->getSampleRate(), true));

    pimpl_->audiodriver_->putUrgentNoResize(*pimpl_->currentFile_->getBuffer());

    // todo: wait autio stop, then stop audio layer
    scheduler().scheduleIn([audioGuard] { SIP_CORE_WARN("End of dtmf"); }, std::chrono::seconds(3));

    return true;
}

void
Manager::recordingPlaybackSeek(const double value)
{
    pimpl_->toneCtrl_.seek(value);
}

void
Manager::stopRecordedFilePlayback()
{
    SIP_CORE_DBG("Stop recorded file playback");

    pimpl_->toneCtrl_.stopAudioFile();
    pimpl_->toneDeviceGuard_.reset();
}

void
Manager::setHistoryLimit(int days)
{
    SIP_CORE_DBG("Set history limit");
    preferences.setHistoryLimit(days);
    saveConfig();
}

int
Manager::getHistoryLimit() const
{
    return preferences.getHistoryLimit();
}

void
Manager::setRingingTimeout(int timeout)
{
    SIP_CORE_DBG("Set ringing timeout");
    preferences.setRingingTimeout(timeout);
    saveConfig();
}

int
Manager::getRingingTimeout() const
{
    return preferences.getRingingTimeout();
}

bool
Manager::setAudioManager(const std::string& api)
{
    {
        std::lock_guard<std::mutex> lock(pimpl_->audioLayerMutex_);

        if (not pimpl_->audiodriver_)
            return false;

        if (api == audioPreference.getAudioApi()) {
            SIP_CORE_DBG("Audio manager chosen already in use. No changes made. ");
            return true;
        }
    }

    {
        std::lock_guard<std::mutex> lock(pimpl_->audioLayerMutex_);
        audioPreference.setAudioApi(api);
        pimpl_->audiodriver_.reset();
        pimpl_->initAudioDriver();
    }

    saveConfig();

    // ensure that we completed the transition (i.e. no fallback was used)
    return api == audioPreference.getAudioApi();
}

std::string
Manager::getAudioManager() const
{
    return audioPreference.getAudioApi();
}

std::string
Manager::getAudioProcessor() const
{
    return audioPreference.getAudioProcessor();
}

int
Manager::getAudioInputDeviceIndex(const std::string& name)
{
    std::lock_guard<std::mutex> lock(pimpl_->audioLayerMutex_);

    if (not pimpl_->audiodriver_) {
        SIP_CORE_ERR("Audio layer not initialized");
        return 0;
    }

    return pimpl_->audiodriver_->getAudioDeviceIndex(name, AudioDeviceType::CAPTURE);
}

int
Manager::getAudioOutputDeviceIndex(const std::string& name)
{
    std::lock_guard<std::mutex> lock(pimpl_->audioLayerMutex_);

    if (not pimpl_->audiodriver_) {
        SIP_CORE_ERR("Audio layer not initialized");
        return 0;
    }

    return pimpl_->audiodriver_->getAudioDeviceIndex(name, AudioDeviceType::PLAYBACK);
}

std::string
Manager::getCurrentAudioOutputPlugin() const
{
    return audioPreference.getAlsaPlugin();
}

std::string
Manager::getNoiseSuppressState() const
{
    return audioPreference.getNoiseReduce();
}

void
Manager::setNoiseSuppressState(const std::string& state)
{
    {
        std::lock_guard<std::mutex> lock(pimpl_->audioLayerMutex_);
        audioPreference.setNoiseReduce(state);
        pimpl_->audiodriver_.reset();
        pimpl_->initAudioDriver();
    }

    saveConfig();
}

std::string
Manager::getEchoCancellerState() const
{
    return audioPreference.getEchoCanceller();
}

void
Manager::setEchoCancellerState(const std::string& state)
{
    {
        std::lock_guard<std::mutex> lock(pimpl_->audioLayerMutex_);
        audioPreference.setEchoCancel(state);
        pimpl_->audiodriver_.reset();
        pimpl_->initAudioDriver();
    }

    saveConfig();
}

void
Manager::setWebRtcParams(const libsip_core::WebRtcParams& params)

{
    {
        std::lock_guard<std::mutex> lock(pimpl_->audioLayerMutex_);

        // check if active && type
        if (pimpl_->audiodriver_->getAudioProcessor()
            && audioPreference.getAudioProcessor() == "webrtc") {
            // needs reload
            if (params.experimentalNs != audioPreference.getWebRtcParams().experimentalNs) {
                // save prefs
                audioPreference.setWebRtcParams(params);
                pimpl_->audiodriver_.reset();
                pimpl_->initAudioDriver();

            } else {
                // live update
                pimpl_->audiodriver_->setWebRtcParams(params);
            }
        }
    }

    audioPreference.setWebRtcParams(params);

    // TODO do not save it now
}

const libsip_core::WebRtcParams&
Manager::getWebRtcParams()
{
    return audioPreference.getWebRtcParams();
}

bool
Manager::isAGCEnabled() const
{
    return audioPreference.isAGCEnabled();
}

void
Manager::setAGCState(bool state)
{
    {
        std::lock_guard<std::mutex> lock(pimpl_->audioLayerMutex_);
        audioPreference.setAGCState(state);
        pimpl_->audiodriver_.reset();
        pimpl_->initAudioDriver();
    }

    saveConfig();
}

bool
Manager::isVADEnabled() const
{
    return audioPreference.getVadEnabled();
}

int32_t
Manager::getVADSensitivity() const
{
    return audioPreference.getVoiceActivitySensitivity();
}

void
Manager::setVADSensitivity(int32_t sensitivity)
{
    audioPreference.setVoiceActivitySensitivity(sensitivity);
    const auto clampedSensitivity = audioPreference.getVoiceActivitySensitivity();

    {
        std::lock_guard<std::mutex> lock(pimpl_->audioLayerMutex_);
        if (pimpl_->audiodriver_)
            pimpl_->audiodriver_->setVadSensitivity(clampedSensitivity);
    }

    for (auto& call : callFactory.getAllCalls()) {
        if (auto sipCall = std::dynamic_pointer_cast<SIPCall>(call)) {
            for (auto& audioRtp : sipCall->getRtpSessionList(MediaType::MEDIA_AUDIO)) {
                auto& recv = std::static_pointer_cast<AudioRtpSession>(audioRtp)->getAudioReceive();
                if (recv)
                    recv->setVadSensitivity(clampedSensitivity);
            }
        }
    }

    saveConfig();
}

int32_t
Manager::getConferenceVoiceInactiveHoldMs() const
{
#ifdef ENABLE_VIDEO
    return videoPreferences.getConferenceVoiceInactiveHoldMs();
#else
    return 0;
#endif
}

void
Manager::setConferenceVoiceInactiveHoldMs(int32_t holdMs)
{
#ifdef ENABLE_VIDEO
    videoPreferences.setConferenceVoiceInactiveHoldMs(holdMs);
    const auto clampedHoldMs = videoPreferences.getConferenceVoiceInactiveHoldMs();

    for (const auto& account : getAllAccounts()) {
        for (const auto& confId : account->getConferenceList()) {
            if (auto conf = account->getConference(confId))
                conf->setVoiceInactiveHoldMs(clampedHoldMs);
        }
    }
#else
    (void) holdMs;
#endif
    saveConfig();
}

void
Manager::setAudioProcessor(const std::string& processor)
{
    {
        std::lock_guard<std::mutex> lock(pimpl_->audioLayerMutex_);
        audioPreference.setAudioProcessor(processor);
        pimpl_->audiodriver_.reset();
        pimpl_->initAudioDriver();
    }

    if (audioPreference.getVadEnabled()) {
        for (auto& call : callFactory.getAllCalls()) {
            if (auto sipCall = std::dynamic_pointer_cast<SIPCall>(call)) {
                for (auto& audioRtp : sipCall->getRtpSessionList(MediaType::MEDIA_AUDIO)) {
                    auto& recv = std::static_pointer_cast<AudioRtpSession>(audioRtp)
                                     ->getAudioReceive();
                    recv->setVAD(false);
                    recv->setVAD(true);
                }
            }
        }
    }

    saveConfig();
}

void
Manager::setVADState(bool state)
{
    {
        std::lock_guard<std::mutex> lock(pimpl_->audioLayerMutex_);
        audioPreference.setVad(state);
        pimpl_->audiodriver_.reset();
        pimpl_->initAudioDriver();
    }

    for (auto& call : callFactory.getAllCalls()) {
        if (auto sipCall = std::dynamic_pointer_cast<SIPCall>(call)) {
            for (auto& audioRtp : sipCall->getRtpSessionList(MediaType::MEDIA_AUDIO)) {
                auto& recv = std::static_pointer_cast<AudioRtpSession>(audioRtp)->getAudioReceive();
                recv->setVAD(state);
            }
        }
    }

    saveConfig();
}

/**
 * Initialization: Main Thread
 */
void
Manager::ManagerPimpl::initAudioDriver()
{
    audiodriver_.reset(base_.audioPreference.createAudioLayer());
    if (!audiodriver_) {
        SIP_CORE_ERR("Unable to initialize audio driver");
        return;
    }
    constexpr std::array<AudioDeviceType, 3> TYPES {AudioDeviceType::CAPTURE,
                                                    AudioDeviceType::PLAYBACK,
                                                    AudioDeviceType::RINGTONE};
    for (const auto& type : TYPES)
        if (audioStreamUsers_[(unsigned) type])
            audiodriver_->startStream(type);
}

// Internal helper method
void
Manager::ManagerPimpl::stripSipPrefix(Call& incomCall)
{
    // strip sip: which is not required and bring confusion with ip to ip calls
    // when placing new call from history.
    std::string peerNumber(incomCall.getPeerNumber());

    const char SIP_PREFIX[] = "sip:";
    size_t startIndex = peerNumber.find(SIP_PREFIX);

    if (startIndex != std::string::npos)
        incomCall.setPeerNumber(peerNumber.substr(startIndex + sizeof(SIP_PREFIX) - 1));
}

std::string
Manager::ManagerPimpl::findHeaderCaseInsensitive(
    const std::map<std::string, std::string>& headers, std::string_view name)
{
    auto equalIgnoreCase = [](std::string_view a, std::string_view b) {
        if (a.size() != b.size())
            return false;
        for (size_t i = 0; i < a.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(a[i]))
                != std::tolower(static_cast<unsigned char>(b[i])))
                return false;
        }
        return true;
    };
    for (const auto& [k, v] : headers) {
        if (equalIgnoreCase(k, name))
            return v;
    }
    return {};
}

void
Manager::ManagerPimpl::onAlertInfoTimeout(const std::string& accountId,
                                          const std::string& callId)
{
    bool needFallback = false;
    {
        std::lock_guard<std::mutex> lock(pendingAlertInfoMutex_);
        auto it = pendingAlertInfoCalls_.find(callId);
        if (it == pendingAlertInfoCalls_.end()) {
            // Already handled (delivered or cleared).
            return;
        }
        if (!it->second.delivered) {
            needFallback = true;
        }
        pendingAlertInfoCalls_.erase(it);
    }

    if (needFallback) {
        SIP_CORE_ERR(
            "[call:%s] Alert-Info wait timed out, falling back to default ringtone",
            callId.c_str());
        base_.playRingtone(accountId);
    }
}

void
Manager::ManagerPimpl::clearPendingAlertInfoCall(const std::string& callId)
{
    std::shared_ptr<Task> taskToCancel;
    {
        std::lock_guard<std::mutex> lock(pendingAlertInfoMutex_);
        auto it = pendingAlertInfoCalls_.find(callId);
        if (it == pendingAlertInfoCalls_.end())
            return;
        taskToCancel = std::move(it->second.fallbackTask);
        pendingAlertInfoCalls_.erase(it);
    }
    // Cancel outside the lock to avoid potential reentrancy if the task
    // somehow runs synchronously on cancel().
    if (taskToCancel)
        taskToCancel->cancel();
}

bool
Manager::setRingtoneForIncomingCall(const std::string& accountId,
                                    const std::string& callId,
                                    const std::string& ringtonePath)
{
    std::shared_ptr<Task> taskToCancel;
    {
        std::lock_guard<std::mutex> lock(pimpl_->pendingAlertInfoMutex_);
        auto it = pimpl_->pendingAlertInfoCalls_.find(callId);
        if (it == pimpl_->pendingAlertInfoCalls_.end()) {
            SIP_CORE_WARN(
                "setRingtoneForIncomingCall: no pending Alert-Info call %s on account %s",
                callId.c_str(),
                accountId.c_str());
            return false;
        }
        if (it->second.delivered) {
            SIP_CORE_WARN(
                "setRingtoneForIncomingCall: ringtone already delivered for call %s",
                callId.c_str());
            return false;
        }
        // Mark delivered + remove the entry (we own everything we need locally now).
        taskToCancel = std::move(it->second.fallbackTask);
        it->second.delivered = true;
        pimpl_->pendingAlertInfoCalls_.erase(it);
    }

    if (taskToCancel)
        taskToCancel->cancel();

    auto account = getAccount(accountId);
    if (!account) {
        SIP_CORE_ERR("setRingtoneForIncomingCall: unknown account %s", accountId.c_str());
        return false;
    }

    if (account->isAutoAnswerEnabled())
        return true; // intentionally do not play any ringtone

    if (!account->getRingtoneEnabled()) {
        ringback();
        return true;
    }

    {
        std::lock_guard<std::mutex> lock(pimpl_->audioLayerMutex_);
        if (not pimpl_->audiodriver_) {
            SIP_CORE_ERR("setRingtoneForIncomingCall: no audio layer for call %s",
                         callId.c_str());
            return false;
        }
        auto oldGuard = std::move(pimpl_->toneDeviceGuard_);
        pimpl_->toneDeviceGuard_ = startAudioStream(AudioDeviceType::RINGTONE);
        pimpl_->toneCtrl_.setSampleRate(pimpl_->audiodriver_->getSampleRate());
    }

    if (not pimpl_->toneCtrl_.setAudioFile(ringtonePath)) {
        SIP_CORE_ERR(
            "setRingtoneForIncomingCall: failed to play custom ringtone '%s' for call %s — "
            "falling back to default ringtone",
            ringtonePath.c_str(),
            callId.c_str());
        playRingtone(accountId);
        return false;
    }

    SIP_CORE_INFO("[call:%s] Playing custom ringtone '%s'",
                  callId.c_str(),
                  ringtonePath.c_str());
    return true;
}

// Internal helper method
void
Manager::ManagerPimpl::processIncomingCall(const std::string& accountId,
                                           Call& incomCall,
                                           const std::map<std::string, std::string>& headers)
{
    base_.stopTone();

    auto incomCallId = incomCall.getCallId();
    auto currentCall = base_.getCurrentCall();

    if (currentCall
        && (currentCall->isConferenceParticipant()
            || currentCall->isRemoteConferenceParticipant())) {
        incomCall.refuse();
        return;
    }

    auto w = incomCall.getAccount();
    auto account = w.lock();
    if (!account) {
        SIP_CORE_ERR("No account detected");
        return;
    }

    auto const& mediaList = MediaAttribute::mediaAttributesToMediaMaps(
        incomCall.getMediaAttributeList());

    if (mediaList.empty())
        SIP_CORE_WARN("Incoming call %s has an empty media list", incomCallId.c_str());

    SIP_CORE_INFO("Incoming call %s on account %s with %lu media",
                  incomCallId.c_str(),
                  accountId.c_str(),
                  mediaList.size());

    // Look up the Alert-Info header (case-insensitive) BEFORE we emit the
    // signal so that, if present, we can install a pending entry first and
    // a fast-responding client cannot push a custom ringtone before we are
    // ready to honor it.
    const std::string alertInfo = findHeaderCaseInsensitive(headers, "Alert-Info");
    const bool hasAlertInfo = !alertInfo.empty();

    if (hasAlertInfo) {
        const int pauseSec = std::max(0, account->getPauseAfterAlertInfo());
        SIP_CORE_INFO("[call:%s] Alert-Info present (%s) — postponing default ringtone for %d s",
                      incomCallId.c_str(),
                      alertInfo.c_str(),
                      pauseSec);

        std::lock_guard<std::mutex> lock(pendingAlertInfoMutex_);
        // Replace any stale entry for this id (paranoia).
        pendingAlertInfoCalls_.erase(incomCallId);
        auto& entry = pendingAlertInfoCalls_[incomCallId];
        entry.accountId = accountId;
        entry.delivered = false;
        entry.fallbackTask = base_.scheduler().scheduleIn(
            [this, accountId, incomCallId] { this->onAlertInfoTimeout(accountId, incomCallId); },
            std::chrono::seconds(pauseSec));
    }

    emitSignal<libsip_core::CallSignal::IncomingCallWithMedia>(accountId,
                                                               incomCallId,
                                                               incomCall.getPeerNumber(),
                                                               mediaList,
                                                               headers);

    if (not base_.hasCurrentCall()) {
        incomCall.setState(Call::ConnectionState::RINGING);
#if !defined(RING_UWP) && !(defined(TARGET_OS_IOS) && TARGET_OS_IOS)
        if (not account->isRendezVous() && incomCall.getPeerNumber().find("__callback") == -1
            && incomCall.getPeerNumber().find("_supervise") == -1) {
            // When Alert-Info is present, the ringtone is played either by
            // setRingtoneForIncomingCall() or by the scheduled timeout.
            if (not hasAlertInfo) {
                base_.playRingtone(accountId);
            }
        }

#endif
    }

    addWaitingCall(incomCallId);

    if (currentCall && currentCall->getCallId() != incomCallId) {
        // Test if already calling this person
        if (currentCall->getAccountId() == account->getAccountID()
            && currentCall->getPeerNumber() == incomCall.getPeerNumber()) {
            auto device_uid = account->getUsername();
            if (device_uid.find("ring:") == 0) {
                // NOTE: in case of a SIP call it's already ready to compare
                device_uid = device_uid.substr(5); // after ring:
            }
            auto answerToCall = false;
            auto downgradeToAudioOnly = currentCall->isAudioOnly() != incomCall.isAudioOnly();
            if (downgradeToAudioOnly)
                // Accept the incoming audio only
                answerToCall = incomCall.isAudioOnly();
            else
                // Accept the incoming call from the higher id number
                answerToCall = (device_uid.compare(incomCall.getPeerNumber()) < 0);

            if (answerToCall) {
                runOnMainThread([accountId = currentCall->getAccountId(),
                                 currentCallID = currentCall->getCallId(),
                                 incomCall = incomCall.shared_from_this()] {
                    auto& mgr = Manager::instance();
                    mgr.answerCall(*incomCall);
                    mgr.hangupCall(accountId, currentCallID);
                });
            }
        }
    }
}

AudioFormat
Manager::hardwareAudioFormatChanged(AudioFormat format)
{
    return audioFormatUsed(format);
}

AudioFormat
Manager::audioFormatUsed(AudioFormat format)
{
    AudioFormat currentFormat = pimpl_->ringbufferpool_->getInternalAudioFormat();
    format.nb_channels = std::max(currentFormat.nb_channels,
                                  std::min(format.nb_channels, 2u)); // max 2 channels.
    format.sample_rate = std::max(currentFormat.sample_rate, format.sample_rate);

    if (currentFormat == format)
        return format;

    SIP_CORE_DBG("Audio format changed: %s -> %s",
                 currentFormat.toString().c_str(),
                 format.toString().c_str());

    pimpl_->ringbufferpool_->setInternalAudioFormat(format);
    pimpl_->toneCtrl_.setSampleRate(format.sample_rate);
    pimpl_->dtmfKey_.reset(new DTMF(format.sample_rate));

    return format;
}

void
Manager::setAccountsOrder(const std::string& order)
{
    SIP_CORE_DBG("Set accounts order : %s", order.c_str());
    // Set the new config

    preferences.setAccountOrder(order);

    saveConfig();

    emitSignal<libsip_core::ConfigurationSignal::AccountsChanged>();
}

std::vector<std::string>
Manager::getAccountList() const
{
    // Concatenate all account pointers in a single map
    std::vector<std::string> v;
    v.reserve(accountCount());
    for (const auto& account : getAllAccounts()) {
        v.emplace_back(account->getAccountID());
    }

    return v;
}

std::map<std::string, std::string>
Manager::getAccountDetails(const std::string& accountID) const
{
    const auto account = getAccount(accountID);

    if (account) {
        return account->getAccountDetails();
    } else {
        SIP_CORE_ERR("Could not get account details on a non-existing accountID %s",
                     accountID.c_str());
        // return an empty map since we can't throw an exception to D-Bus
        return std::map<std::string, std::string>();
    }
}

std::map<std::string, std::string>
Manager::getVolatileAccountDetails(const std::string& accountID) const
{
    const auto account = getAccount(accountID);

    if (account) {
        return account->getVolatileAccountDetails();
    } else {
        SIP_CORE_ERR("Could not get volatile account details on a non-existing accountID %s",
                     accountID.c_str());
        return {};
    }
}

void
Manager::setAccountDetails(const std::string& accountID,
                           const std::map<std::string, std::string>& details)
{
    SIP_CORE_DBG("Set account details for %s", accountID.c_str());

    auto account = getAccount(accountID);
    if (not account) {
        SIP_CORE_ERR("Could not find account %s", accountID.c_str());
        return;
    }

    // Ignore if nothing has changed
    if (details == account->getAccountDetails())
        return;

    account->setAccountDetails(details);
}

std::string
Manager::getNewAccountId()
{
    std::string random_id;
    do {
        random_id = to_hex_string(std::uniform_int_distribution<uint64_t>()(rand_));
    } while (getAccount(random_id));
    return random_id;
}

std::string
Manager::addAccount(const std::map<std::string, std::string>& details, const std::string& accountId)
{
    /** @todo Deal with both the accountMap_ and the Configuration */
    auto newAccountID = accountId.empty() ? getNewAccountId() : accountId;

    // Get the type
    std::string_view accountType;
    auto typeIt = details.find(Conf::CONFIG_ACCOUNT_TYPE);
    if (typeIt != details.end())
        accountType = typeIt->second;
    else
        accountType = AccountFactory::DEFAULT_ACCOUNT_TYPE;

    SIP_CORE_DEBUG("Adding account {:s} with type {}", newAccountID, accountType);

    auto newAccount = accountFactory.createAccount(accountType, newAccountID);
    if (!newAccount) {
        SIP_CORE_ERROR("Unknown {:s} param when calling addAccount(): {:s}",
                       Conf::CONFIG_ACCOUNT_TYPE,
                       accountType);
        return "";
    }

    newAccount->setAccountDetails(details);
    preferences.addAccount(newAccountID);

    saveConfig();

    emitSignal<libsip_core::ConfigurationSignal::AccountsChanged>();

    return newAccountID;
}

void
Manager::removeAccount(const std::string& accountID, bool flush)
{
    // Get it down and dying
    if (const auto& remAccount = getAccount(accountID)) {
        // Unregister explicitely
        // remAccount->doUnregister();
        if (flush)
            remAccount->flush();
        accountFactory.removeAccount(*remAccount);
    }

    preferences.removeAccount(accountID);

    saveConfig();

    emitSignal<libsip_core::ConfigurationSignal::AccountsChanged>();
}

void
Manager::removeAccounts()
{
    for (const auto& acc : getAccountList())
        removeAccount(acc);
}

std::vector<std::string_view>
Manager::loadAccountOrder() const
{
    return split_string(preferences.getAccountOrder(), '/');
}

int
Manager::loadAccountMap(const YAML::Node& node)
{
    int errorCount = 0;
    try {
        // build preferences
        preferences.unserialize(node);
        voipPreferences.unserialize(node);
        audioPreference.unserialize(node);
#ifdef ENABLE_VIDEO
        videoPreferences.unserialize(node);
#endif
    } catch (const YAML::Exception& e) {
        SIP_CORE_ERR("Preferences node unserialize YAML exception: %s", e.what());
        ++errorCount;
    } catch (const std::exception& e) {
        SIP_CORE_ERR("Preferences node unserialize standard exception: %s", e.what());
        ++errorCount;
    } catch (...) {
        SIP_CORE_ERR("Preferences node unserialize unknown exception");
        ++errorCount;
    }

    const std::string accountOrder = preferences.getAccountOrder();

    // load saved preferences for IP2IP account from configuration file
    const auto& accountList = node["accounts"];

    for (auto& a : accountList) {
        pimpl_->loadAccount(a, errorCount);
    }

    return errorCount;
}

std::vector<std::string>
Manager::getCallList() const
{
    std::vector<std::string> results;
    for (const auto& call : callFactory.getAllCalls()) {
        if (!call->isSubcall())
            results.push_back(call->getCallId());
    }
    return results;
}

void
Manager::registerAccounts()
{
    auto allAccounts(getAccountList());

    for (auto& item : allAccounts) {
        const auto a = getAccount(item);

        if (!a)
            continue;

        a->loadConfig();

        if (a->isUsable())
            a->doRegister();
    }
}

void
Manager::sendRegister(const std::string& accountID, bool enable)
{
    const auto acc = getAccount(accountID);
    if (!acc)
        return;

    acc->setEnabled(enable);

    if (acc->isEnabled()) {
        acc->doRegister();
    } else
        acc->doUnregister();
}

uint64_t
Manager::sendTextMessage(const std::string& accountID,
                         const std::string& to,
                         const std::map<std::string, std::string>& payloads,
                         const bool fromPlugin)
{
    if (const auto acc = getAccount(accountID)) {
        try {
            return acc->sendTextMessage(to, payloads);
        } catch (const std::exception& e) {
            SIP_CORE_ERR("Exception during text message sending: %s", e.what());
        }
    }
    return 0;
}

int
statusFromImStatus(im::MessageStatus status)
{
    switch (status) {
    case im::MessageStatus::IDLE:
    case im::MessageStatus::SENDING:
        return static_cast<int>(libsip_core::Account::MessageStates::SENDING);
    case im::MessageStatus::SENT:
        return static_cast<int>(libsip_core::Account::MessageStates::SENT);
    case im::MessageStatus::DISPLAYED:
        return static_cast<int>(libsip_core::Account::MessageStates::DISPLAYED);
    case im::MessageStatus::FAILURE:
        return static_cast<int>(libsip_core::Account::MessageStates::FAILURE);
    default:
        return static_cast<int>(libsip_core::Account::MessageStates::UNKNOWN);
    }
}

int
Manager::getMessageStatus(uint64_t id) const
{
    const auto& allAccounts = accountFactory.getAllAccounts();
    for (const auto& acc : allAccounts) {
        auto status = acc->getMessageStatus(id);
        if (status != im::MessageStatus::UNKNOWN)
            return statusFromImStatus(status);
    }
    return static_cast<int>(libsip_core::Account::MessageStates::UNKNOWN);
}

int
Manager::getMessageStatus(const std::string& accountID, uint64_t id) const
{
    if (const auto acc = getAccount(accountID))
        return statusFromImStatus(acc->getMessageStatus(id));
    return static_cast<int>(libsip_core::Account::MessageStates::UNKNOWN);
}

void
Manager::setAccountActive(const std::string& accountID, bool active, bool shutdownConnections)
{
    const auto acc = getAccount(accountID);
    if (!acc || acc->isActive() == active)
        return;
    acc->setActive(active);
    if (acc->isEnabled()) {
        if (active) {
            acc->doRegister();
        } else {
            acc->doUnregister();
        }
    }
}

std::shared_ptr<AudioLayer>
Manager::getAudioDriver()
{
    return pimpl_->audiodriver_;
}

void
Manager::onAudioDevicesChanged()
{
    SIP_CORE_DBG("Audio devices changed, restarting media senders for active calls");
    for (const auto& call : callFactory.getAllCalls()) {
        if (call->isSubcall())
            continue;
        if (call->getConnectionState() != Call::ConnectionState::CONNECTED)
            continue;
        if (call->getState() != Call::CallState::ACTIVE)
            continue;
        call->restartMediaSender();
    }
}

#ifdef ENABLE_VIDEO
void
Manager::onVideoDevicesChanged()
{
    SIP_CORE_DBG("Video devices changed, checking for video inputs to restart");
    for (const auto& call : callFactory.getAllCalls()) {
        if (call->isSubcall())
            continue;
        if (call->getConnectionState() != Call::ConnectionState::CONNECTED)
            continue;
        if (call->getState() != Call::CallState::ACTIVE)
            continue;

        auto* sipCall = dynamic_cast<SIPCall*>(call.get());
        if (!sipCall)
            continue;

        for (const auto& rtpSession : sipCall->getRtpSessionList(MediaType::MEDIA_VIDEO)) {
            auto videoRtp = std::dynamic_pointer_cast<video::VideoRtpSession>(rtpSession);
            if (!videoRtp)
                continue;
            auto& videoLocal = videoRtp->getVideoLocal();
            if (videoLocal && videoLocal->wasStoppedByDeviceDisconnect()) {
                SIP_CORE_DBG("Restarting video input for call %s", call->getCallId().c_str());
                videoLocal->restart();
                videoRtp->restartSender();
            }
        }
    }
}
#endif

std::shared_ptr<Call>
Manager::newOutgoingCall(std::string_view toUrl,
                         const std::string& accountId,
                         const std::vector<libsip_core::MediaMap>& mediaList)
{
    auto account = getAccount(accountId);
    if (not account) {
        SIP_CORE_WARN("No account matches ID %s", accountId.c_str());
        return {};
    }

    if (not account->isUsable()) {
        SIP_CORE_WARN("Account %s is not usable", accountId.c_str());
        return {};
    }

    return account->newOutgoingCall(toUrl, mediaList);
}

#ifdef ENABLE_VIDEO
std::shared_ptr<video::SinkClient>
Manager::createSinkClient(const std::string& id, bool mixer)
{
    const auto& iter = pimpl_->sinkMap_.find(id);
    if (iter != std::end(pimpl_->sinkMap_)) {
        if (auto sink = iter->second.lock())
            return sink;
        pimpl_->sinkMap_.erase(iter); // remove expired weak_ptr
    }

    auto sink = std::make_shared<video::SinkClient>(id, mixer);
    pimpl_->sinkMap_.emplace(id, sink);
    return sink;
}

void
Manager::createSinkClients(
    const std::string& callId,
    const ConfInfo& infos,
    const std::vector<std::shared_ptr<video::VideoFrameActiveWriter>>& videoStreams,
    std::map<std::string, std::shared_ptr<video::SinkClient>>& sinksMap,
    const std::string& accountId)
{
    std::lock_guard<std::mutex> lk(pimpl_->sinksMutex_);
    std::set<std::string> sinkIdsList {};

    // create video sinks
    for (const auto& participant : infos) {
        std::string sinkId = participant.sinkId;
        if (sinkId.empty()) {
            sinkId = callId;
            sinkId += string_remove_suffix(participant.uri, '@') + participant.device;
        }
        if (participant.w && participant.h && !participant.videoMuted) {
            auto currentSink = getSinkClient(sinkId);
            if (!accountId.empty() && currentSink
                && string_remove_suffix(participant.uri, '@') == getAccount(accountId)->getUsername()
                && participant.device
                       == Manager::instance()
                              .getVideoManager()
                              .videoDeviceMonitor.getMRLForDefaultDevice()) {
                // This is a local sink that must already exist
                continue;
            }
            if (currentSink) {
                // If sink exists, update it
                currentSink->setCrop(participant.x, participant.y, participant.w, participant.h);
                sinkIdsList.emplace(sinkId);
                continue;
            }
            auto newSink = createSinkClient(sinkId);
            newSink->start();
            newSink->setCrop(participant.x, participant.y, participant.w, participant.h);
            newSink->setFrameSize(participant.w, participant.h);

            for (auto& videoStream : videoStreams)
                videoStream->attach(newSink.get());

            sinksMap.emplace(sinkId, newSink);
            sinkIdsList.emplace(sinkId);
        } else {
            sinkIdsList.erase(sinkId);
        }
    }

    // remove any non used video sink
    for (auto it = sinksMap.begin(); it != sinksMap.end();) {
        if (sinkIdsList.find(it->first) == sinkIdsList.end()) {
            for (auto& videoStream : videoStreams)
                videoStream->detach(it->second.get());
            it->second->stop();
            it = sinksMap.erase(it);
        } else {
            it++;
        }
    }
}

std::shared_ptr<video::SinkClient>
Manager::getSinkClient(const std::string& id)
{
    const auto& iter = pimpl_->sinkMap_.find(id);
    if (iter != std::end(pimpl_->sinkMap_))
        if (auto sink = iter->second.lock())
            return sink;
    return nullptr;
}
#endif // ENABLE_VIDEO

RingBufferPool&
Manager::getRingBufferPool()
{
    return *pimpl_->ringbufferpool_;
}

bool
Manager::hasAccount(const std::string& accountID)
{
    return accountFactory.hasAccount(accountID);
}

VideoManager&
Manager::getVideoManager() const
{
    return *pimpl_->videoManager_;
}

std::vector<libsip_core::Message>
Manager::getLastMessages(const std::string& accountID, const uint64_t& base_timestamp)
{
    if (const auto acc = getAccount(accountID))
        return acc->getLastMessages(base_timestamp);
    return {};
}

SIPVoIPLink&
Manager::sipVoIPLink() const
{
    return *pimpl_->sipLink_;
}

void
Manager::setDefaultModerator(const std::string& accountID, const std::string& peerURI, bool state)
{
    auto acc = getAccount(accountID);
    if (!acc) {
        SIP_CORE_ERR("Fail to change default moderator, account %s not found", accountID.c_str());
        return;
    }

    if (state)
        acc->addDefaultModerator(peerURI);
    else
        acc->removeDefaultModerator(peerURI);
    saveConfig(acc);
}

std::vector<std::string>
Manager::getDefaultModerators(const std::string& accountID)
{
    auto acc = getAccount(accountID);
    if (!acc) {
        SIP_CORE_ERR("Fail to get default moderators, account %s not found", accountID.c_str());
        return {};
    }

    auto set = acc->getDefaultModerators();
    return std::vector<std::string>(set.begin(), set.end());
}

void
Manager::enableLocalModerators(const std::string& accountID, bool isModEnabled)
{
    if (auto acc = getAccount(accountID))
        acc->editConfig(
            [&](AccountConfig& config) { config.localModeratorsEnabled = isModEnabled; });
}

bool
Manager::isLocalModeratorsEnabled(const std::string& accountID)
{
    auto acc = getAccount(accountID);
    if (!acc) {
        SIP_CORE_ERR("Fail to get local moderators, account %s not found", accountID.c_str());
        return true; // Default value
    }
    return acc->isLocalModeratorsEnabled();
}

void
Manager::setAllModerators(const std::string& accountID, bool allModerators)
{
    if (auto acc = getAccount(accountID))
        acc->editConfig([&](AccountConfig& config) { config.allModeratorsEnabled = allModerators; });
}

bool
Manager::isAllModerators(const std::string& accountID)
{
    auto acc = getAccount(accountID);
    if (!acc) {
        SIP_CORE_ERR("Fail to get all moderators, account %s not found", accountID.c_str());
        return true; // Default value
    }
    return acc->isAllModerators();
}

void
Manager::setTsxTimers(const uint32_t t1, const uint32_t t2, const uint32_t t4, const uint32_t td)
{
    pjsip_tsx_set_timers(t1, t2, t4, td);
}

} // namespace sip_core
