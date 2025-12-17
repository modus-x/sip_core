/*
 *  Copyright (C) 2004-2022 Savoir-faire Linux Inc.
 *
 *  Author: Julien Bonjean <julien.bonjean@savoirfairelinux.com>
 *  Author: Guillaume Roguez <guillaume.roguez@savoirfairelinux.com>
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

#include <cstdio>
#include <cstring>
#include <cerrno>
#include <ctime>
#include <ciso646> // fix windows compiler bug

#include "client/ring_signal.h"

#include <fmt/core.h>
#include <fmt/format.h>
#include <fmt/compile.h>
#if __has_include(<fmt/std.h>)
#include <fmt/std.h>
#else
#include <fmt/ostream.h>
#endif

#ifdef _MSC_VER
#include <sys_time.h>
#else
#include <sys/time.h>
#endif

#include <atomic>
#include <condition_variable>
#include <functional>
#include <fstream>
#include <string>
#include <ios>
#include <mutex>
#include <thread>
#include <array>
#include <algorithm>

#include <zlib.h>

#ifdef _WIN32
#include <cwchar>
#endif

#include "fileutils.h"
#include "logger.h"

#ifdef __linux__
#include <unistd.h>
#include <syslog.h>
#include <sys/syscall.h>
#endif // __linux__

#ifdef __ANDROID__
#ifndef APP_NAME
#define APP_NAME "libsip_core"
#endif /* APP_NAME */
#endif

#define BLACK         "\033[22;30m"
#define GREEN         "\033[22;32m"
#define BROWN         "\033[22;33m"
#define BLUE          "\033[22;34m"
#define MAGENTA       "\033[22;35m"
#define GREY          "\033[22;37m"
#define DARK_GREY     "\033[01;30m"
#define LIGHT_RED     "\033[01;31m"
#define LIGHT_SCREEN  "\033[01;32m"
#define LIGHT_BLUE    "\033[01;34m"
#define LIGHT_MAGENTA "\033[01;35m"
#define LIGHT_CYAN    "\033[01;36m"
#define WHITE         "\033[01;37m"
#define END_COLOR     "\033[0m"

#ifndef _WIN32
#define RED    "\033[22;31m"
#define YELLOW "\033[01;33m"
#define CYAN   "\033[22;36m"
#else
#define FOREGROUND_WHITE 0x000f
#define RED              FOREGROUND_RED + 0x0008
#define YELLOW           FOREGROUND_RED + FOREGROUND_GREEN + 0x0008
#define CYAN             FOREGROUND_BLUE + FOREGROUND_GREEN + 0x0008
#define LIGHT_GREEN      FOREGROUND_GREEN + 0x0008
#endif // _WIN32


#ifdef UNICODE
#define LOGFILE L"sip_core"
#else
#define LOGFILE "sip_core"
#endif


namespace sip_core {

static constexpr auto ENDL = '\n';

#ifndef __GLIBC__
static const char*
check_error(int result, char* buffer)
{
    switch (result) {
    case 0:
        return buffer;

    case ERANGE: /* should never happen */
        return "unknown (too big to display)";

    default:
        return "unknown (invalid error number)";
    }
}

static const char*
check_error(char* result, char*)
{
    return result;
}
#endif

void
strErr()
{
#ifdef __GLIBC__
    SIP_CORE_ERR("%m");
#else
    char buf[1000];
    SIP_CORE_ERR("%s", check_error(strerror_r(errno, buf, sizeof(buf)), buf));
#endif
}

// extract the last component of a pathname (extract a filename from its dirname)
static const char*
stripDirName(const char* path)
{
    const char* occur = strrchr(path, DIR_SEPARATOR_CH);

    return occur ? occur + 1 : path;
}

static std::string
contextHeader(const char* const file, int line)
{
#ifdef __linux__
    auto tid = syscall(__NR_gettid) & 0xffff;
#else
    auto tid = std::this_thread::get_id();
#endif // __linux__

    unsigned int secs, milli;
    struct timeval tv;
    if (!gettimeofday(&tv, NULL)) {
        secs = tv.tv_sec;
        milli = tv.tv_usec / 1000; // suppose that milli < 1000
    } else {
        secs = time(NULL);
        milli = 0;
    }

    if (file) {
        return fmt::format(FMT_COMPILE("[{: >3d}.{:0<3d}|{: >4}|{: <24s}:{: <4d}]"),
                           secs,
                           milli,
                           tid,
                           stripDirName(file),
                           line);
    } else {
        return fmt::format(FMT_COMPILE("[{: >3d}.{:0<3d}|{: >4}] "), secs, milli, tid);
    }
}

std::string
formatPrintfArgs(const char* format, va_list ap)
{
    std::string ret;
    /* A good guess of what we might encounter. */
    static constexpr size_t default_buf_size = 80;

    ret.resize(default_buf_size);

    /* Necessary if we don't have enough space in buf. */
    va_list cp;
    va_copy(cp, ap);

    int size = vsnprintf(ret.data(), ret.size(), format, ap);

    /* Not enough space?  Well try again. */
    if ((size_t) size >= ret.size()) {
        ret.resize(size + 1);
        vsnprintf((char*) ret.data(), ret.size(), format, cp);
    }

    ret.resize(size);

    va_end(cp);

    return ret;
}

struct Logger::Msg
{
    Msg() = delete;

    Msg(int level, const char* file, int line, bool linefeed, std::string&& message)
        : payload_(std::move(message))
        , header_(contextHeader(file, line))
        , level_(level)
        , linefeed_(linefeed)
    {}

    Msg(int level, const char* file, int line, bool linefeed, const char* fmt, va_list ap)
        : payload_(formatPrintfArgs(fmt, ap))
        , header_(contextHeader(file, line))
        , level_(level)
        , linefeed_(linefeed)
    {}

    Msg(Msg&& other)
    {
        payload_ = std::move(other.payload_);
        header_ = std::move(other.header_);
        level_ = other.level_;
        linefeed_ = other.linefeed_;
    }

    std::string payload_;
    std::string header_;
    int level_;
    bool linefeed_;
};

class Logger::Handler
{
public:
    virtual ~Handler() = default;

    virtual void consume(Msg& msg) = 0;

    void enable(bool en) { enabled_.store(en, std::memory_order_relaxed); }
    bool isEnable() { return enabled_.load(std::memory_order_relaxed); }

private:
    std::atomic_bool enabled_ {false};
};

class ConsoleLog : public Logger::Handler
{
public:
    static ConsoleLog& instance()
    {
        // Intentional memory leak:
        // Some thread can still be logging even during static destructors.
        static ConsoleLog* self = new ConsoleLog();
        return *self;
    }

#ifdef _WIN32
    void printLogImpl(sip_core::Logger::Msg& msg, bool with_color)
    {
        WORD saved_attributes;
        static HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
        if (with_color) {
            static WORD color_header = CYAN;
            WORD color_prefix = LIGHT_GREEN;
            CONSOLE_SCREEN_BUFFER_INFO consoleInfo;

            switch (msg.level_) {
            case LOG_ERR:
                color_prefix = RED;
                break;

            case LOG_WARNING:
                color_prefix = YELLOW;
                break;
            }

            GetConsoleScreenBufferInfo(hConsole, &consoleInfo);
            saved_attributes = consoleInfo.wAttributes;
            SetConsoleTextAttribute(hConsole, color_header);

            fputs(msg.header_.c_str(), stderr);

            SetConsoleTextAttribute(hConsole, saved_attributes);
            SetConsoleTextAttribute(hConsole, color_prefix);
        } else {
            fputs(msg.header_.c_str(), stderr);
        }

        fputs(msg.payload_.c_str(), stderr);

        if (msg.linefeed_) {
            putc(ENDL, stderr);
        }

        if (with_color) {
            SetConsoleTextAttribute(hConsole, saved_attributes);
        }
    }
#else
    void printLogImpl(sip_core::Logger::Msg& msg, bool with_color)
    {
        if (with_color) {
            const char* color_header = CYAN;
            const char* color_prefix = "";

            switch (msg.level_) {
            case LOG_ERR:
                color_prefix = RED;
                break;

            case LOG_WARNING:
                color_prefix = YELLOW;
                break;
            }

            fputs(color_header, stderr);
            fputs(msg.header_.c_str(), stderr);
            fputs(END_COLOR, stderr);
            fputs(color_prefix, stderr);
        } else {
            fputs(msg.header_.c_str(), stderr);
        }

        fputs(msg.payload_.c_str(), stderr);

        if (msg.linefeed_) {
            putc(ENDL, stderr);
        }

        if (with_color) {
            fputs(END_COLOR, stderr);
        }
    }
#endif /* _WIN32 */

    virtual void consume(sip_core::Logger::Msg& msg) override
    {
        static bool with_color = !(getenv("NO_COLOR") || getenv("NO_COLORS") || getenv("NO_COLOUR")
                                   || getenv("NO_COLOURS"));

        printLogImpl(msg, with_color);
    }
};

void
Logger::setConsoleLog(bool en)
{
    ConsoleLog::instance().enable(en);
}

class SysLog : public Logger::Handler
{
public:
    static SysLog& instance()
    {
        // Intentional memory leak:
        // Some thread can still be logging even during static destructors.
        static SysLog* self = new SysLog();
        return *self;
    }

    SysLog()
    {
#ifdef _WIN32
        //::openlog(LOGFILE, WINLOG_PID, WINLOG_MAIL);
#else
#ifndef __ANDROID__
        ::openlog(LOGFILE, LOG_NDELAY, LOG_USER);
#endif
#endif /* _WIN32 */
    }

    virtual void consume(Logger::Msg& msg) override
    {
#ifdef __ANDROID__
        __android_log_print(msg.level_, APP_NAME, "%s%s", msg.header_.c_str(), msg.payload_.c_str());
#elifndef _WIN32
        ::syslog(msg.level_, "%.*s", (int) msg.payload_.size(), msg.payload_.data());
#endif
    }
};

void
Logger::setSysLog(bool en)
{
    SysLog::instance().enable(en);
}

class MonitorLog : public Logger::Handler
{
public:
    static MonitorLog& instance()
    {
        // Intentional memory leak
        // Some thread can still be logging even during static destructors.
        static MonitorLog* self = new MonitorLog();
        return *self;
    }

    virtual void consume(sip_core::Logger::Msg& msg) override
    {
        /*
         * TODO - Maybe change the MessageSend sigature to avoid copying
         * of message payload?
         */
        auto tmp = msg.header_ + msg.payload_;

        sip_core::emitSignal<libsip_core::ConfigurationSignal::MessageSend>(tmp);
    }
};

void
Logger::setMonitorLog(bool en)
{
    MonitorLog::instance().enable(en);
}

class FileLog : public Logger::Handler
{
public:
    static FileLog& instance()
    {
        // Intentional memory leak:
        // Some thread can still be logging even during static destructors.
        static FileLog* self = new FileLog();
        return *self;
    }

    void setFile(const std::string& path)
    {
        setFile(path, synchronous_.load(std::memory_order_relaxed));
    }

    void setFile(const std::string& path, bool synchronous)
    {
        stop();

        std::lock_guard lk(mtx_);
        synchronous_.store(synchronous, std::memory_order_relaxed);
        path_ = path;

        if (path_.empty()) {
            enable(false);
            return;
        }

        fileutils::openStream(file_,
                              path_,
                              std::ios_base::out | std::ios_base::app | std::ios_base::binary);
        if (not file_.is_open()) {
            enable(false);
            return;
        }

        const auto existing_size = fileutils::size(path_);
        current_size_ = existing_size > 0 ? static_cast<uint64_t>(existing_size) : 0;

        enable(true);

        if (not synchronous_.load(std::memory_order_relaxed)) {
            startThread();
        }
    }

    void setSynchronous(bool synchronous)
    {
        std::string path;
        {
            std::lock_guard lk(mtx_);
            path = path_;
        }
        setFile(path, synchronous);
    }

    void setRotationSize(uint64_t bytes)
    {
        if (bytes == 0) {
            bytes = DEFAULT_ROTATION_SIZE;
        }
        rotation_size_.store(bytes, std::memory_order_relaxed);
    }

    void setRotationCount(std::size_t files)
    {
        rotation_keep_count_.store(files, std::memory_order_relaxed);
    }

    void setCompressionLevel(int level)
    {
        if (level < Z_DEFAULT_COMPRESSION)
            level = Z_DEFAULT_COMPRESSION;
        if (level > 9)
            level = 9;
        compression_level_.store(level, std::memory_order_relaxed);
    }

    ~FileLog()
    {
        stop();
    }

    virtual void consume(Logger::Msg& msg) override
    {
        if (synchronous_.load(std::memory_order_relaxed)) {
            std::lock_guard lk(mtx_);
            if (not(isEnable() and file_.is_open())) {
                return;
            }
            writeMsg(msg);
            file_.flush();
            return;
        }

        notify([&, this] { currentQ_.emplace_back(std::move(msg)); });
    }

private:
    static constexpr uint64_t DEFAULT_ROTATION_SIZE = 100ull * 1024ull * 1024ull;
    static constexpr std::size_t DEFAULT_ROTATION_KEEP_COUNT = 5;

    template<typename T>
    void notify(T func)
    {
        std::lock_guard lk(mtx_);
        func();
        cv_.notify_one();
    }

    void stop()
    {
        std::thread threadToJoin;
        {
            std::lock_guard lk(mtx_);
            if (thread_.joinable()) {
                enable(false);
                cv_.notify_one();
                threadToJoin = std::move(thread_);
            } else {
                enable(false);
            }
        }

        if (threadToJoin.joinable()) {
            threadToJoin.join();
        }

        std::lock_guard lk(mtx_);
        if (file_.is_open()) {
            file_.flush();
            file_.close();
        }
        currentQ_.clear();
        current_size_ = 0;
    }

    static std::string rotationSuffix()
    {
        struct timeval tv;
        if (gettimeofday(&tv, NULL) != 0) {
            tv.tv_sec = time(NULL);
            tv.tv_usec = 0;
        }

        std::time_t t = tv.tv_sec;
        std::tm tm {};
#ifdef _WIN32
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif

        char buf[32] = {0};
        std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tm);

        char suffix[48] = {0};
        std::snprintf(suffix,
                      sizeof(suffix),
                      "%s.%03ld",
                      buf,
                      static_cast<long>(tv.tv_usec / 1000));
        return suffix;
    }

    static int renameFile(const std::string& from, const std::string& to)
    {
#ifdef _WIN32
        return _wrename(sip_core::to_wstring(from).c_str(), sip_core::to_wstring(to).c_str());
#else
        return std::rename(from.c_str(), to.c_str());
#endif
    }

    static bool compressFileGz(const std::string& srcPath, const std::string& dstPath, int level)
    {
        std::ifstream src;
        fileutils::openStream(src, srcPath, std::ios_base::in | std::ios_base::binary);
        if (not src.is_open()) {
            return false;
        }

        std::string mode = "wb";
        if (level >= 0 && level <= 9) {
            mode += std::to_string(level);
        }

#ifdef _WIN32
        auto wdst = sip_core::to_wstring(dstPath);
        auto wmode = sip_core::to_wstring(mode);
        gzFile dst = gzopen_w(wdst.c_str(), wmode.c_str());
#else
        gzFile dst = gzopen(dstPath.c_str(), mode.c_str());
#endif
        if (dst == nullptr) {
            return false;
        }

        std::array<char, 64 * 1024> buffer;
        while (src.good()) {
            src.read(buffer.data(), buffer.size());
            const auto got = src.gcount();
            if (got <= 0) {
                continue;
            }
            const auto toWrite = static_cast<unsigned int>(got);
            const int written = gzwrite(dst, buffer.data(), toWrite);
            if (written != static_cast<int>(toWrite)) {
                gzclose(dst);
                return false;
            }
        }

        const int closeResult = gzclose(dst);
        return closeResult == Z_OK;
    }

    static void splitDirAndFilename(const std::string& path, std::string& dir, std::string& filename)
    {
        const auto pos = path.find_last_of("/\\");
        if (pos == std::string::npos) {
            dir = ".";
            filename = path;
            return;
        }

        if (pos == 0) {
            dir = path.substr(0, 1);
        } else {
            dir = path.substr(0, pos);
        }
        filename = path.substr(pos + 1);

        if (dir.empty()) {
            dir = ".";
        }
    }

    static std::string joinPath(const std::string& dir, const std::string& filename)
    {
        if (dir.empty() or dir == ".") {
            return filename;
        }

        const char last = dir.back();
        if (last == '/' || last == '\\') {
            return dir + filename;
        }

        return dir + DIR_SEPARATOR_CH + filename;
    }

    static bool startsWith(const std::string& value, const std::string& prefix)
    {
        return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
    }

    void pruneRotatedFiles()
    {
        const auto keep = rotation_keep_count_.load(std::memory_order_relaxed);
        if (keep == 0) {
            return;
        }

        std::string dir;
        std::string filename;
        splitDirAndFilename(path_, dir, filename);

        const auto entries = fileutils::readDirectory(dir);
        const std::string prefix = filename + ".";
        const auto isRotatedLogName = [&](const std::string& name) -> bool {
            const auto start = prefix.size();
            if (name.size() <= start + 8) {
                return false;
            }
            for (std::size_t i = 0; i < 8; ++i) {
                const char c = name[start + i];
                if (c < '0' || c > '9') {
                    return false;
                }
            }
            return name[start + 8] == '-';
        };

        struct Candidate
        {
            std::string path;
            uint64_t lastWriteTime;
            std::string name;
        };

        std::vector<Candidate> candidates;
        candidates.reserve(entries.size());
        for (const auto& entry : entries) {
            if (not startsWith(entry, prefix)) {
                continue;
            }
            if (not isRotatedLogName(entry)) {
                continue;
            }

            auto fullPath = joinPath(dir, entry);
            const auto lastWriteTime = fileutils::lastWriteTime(fullPath);
            candidates.emplace_back(Candidate {std::move(fullPath), lastWriteTime, entry});
        }

        if (candidates.size() <= keep) {
            return;
        }

        std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
            if (a.lastWriteTime != b.lastWriteTime) {
                return a.lastWriteTime > b.lastWriteTime;
            }
            return a.name > b.name;
        });

        for (std::size_t i = keep; i < candidates.size(); ++i) {
            fileutils::remove(candidates[i].path);
        }
    }

    void rotateIfNeeded(uint64_t nextWriteSize)
    {
        const auto limit = rotation_size_.load(std::memory_order_relaxed);
        if (limit == 0) {
            return;
        }

        if (current_size_ == 0) {
            return;
        }

        if (current_size_ + nextWriteSize < limit) {
            return;
        }

        rotate();
    }

    void rotate()
    {
        if (path_.empty() or not file_.is_open()) {
            return;
        }

        file_.flush();
        file_.close();

        const auto suffix = rotationSuffix();

        std::string rotatedPath;
        bool renamed = false;
        for (int attempt = 0; attempt < 10; ++attempt) {
            rotatedPath = path_ + "." + suffix;
            if (attempt != 0) {
                rotatedPath += fmt::format(".{}", attempt);
            }
            if (renameFile(path_, rotatedPath) == 0) {
                renamed = true;
                break;
            }
        }

        if (not renamed) {
            fileutils::openStream(file_,
                                  path_,
                                  std::ios_base::out | std::ios_base::app | std::ios_base::binary);
            const auto existing_size = fileutils::size(path_);
            current_size_ = existing_size > 0 ? static_cast<uint64_t>(existing_size) : 0;
            return;
        }

        fileutils::openStream(file_, path_, std::ios_base::out | std::ios_base::trunc | std::ios_base::binary);
        if (not file_.is_open()) {
            // Fallback to append mode if we can't recreate the log file.
            fileutils::openStream(file_,
                                  path_,
                                  std::ios_base::out | std::ios_base::app | std::ios_base::binary);
        }
        current_size_ = 0;

        const auto level = compression_level_.load(std::memory_order_relaxed);
        const auto gzPath = rotatedPath + ".gz";
        if (compressFileGz(rotatedPath, gzPath, level)) {
            std::remove(rotatedPath.c_str());
        }

        pruneRotatedFiles();
    }

    static uint64_t estimateWriteSize(const Logger::Msg& msg, const std::string& level)
    {
        // header + "{" + level + "} " + payload + optional '\n'
        uint64_t size = msg.header_.size() + 1 + level.size() + 2 + msg.payload_.size();
        if (msg.linefeed_) {
            size += 1;
        }
        return size;
    }

    void writeMsg(const Logger::Msg& msg)
    {
        const auto level = Logger::logLevelToString(msg.level_);
        const auto writeSize = estimateWriteSize(msg, level);
        rotateIfNeeded(writeSize);

        file_.write(msg.header_.data(), static_cast<std::streamsize>(msg.header_.size()));
        file_.put('{');
        file_.write(level.data(), static_cast<std::streamsize>(level.size()));
        file_.write("} ", 2);
        file_.write(msg.payload_.data(), static_cast<std::streamsize>(msg.payload_.size()));
        if (msg.linefeed_) {
            file_.put(ENDL);
        }
        current_size_ += writeSize;
    }

    void do_consume(const std::vector<Logger::Msg>& messages)
    {
        for (const auto& msg : messages) {
            writeMsg(msg);
        }
        file_.flush();
    }

    void startThread()
    {
        thread_ = std::thread([this]() mutable {
            std::vector<Logger::Msg> pendingQ_;
            while (true) {
                {
                    std::unique_lock lk(mtx_);
                    cv_.wait(lk, [&] { return not isEnable() or not currentQ_.empty(); });
                    if (not isEnable() and currentQ_.empty())
                        break;

                    std::swap(currentQ_, pendingQ_);
                }

                do_consume(pendingQ_);
                pendingQ_.clear();
            }
        });
    }

    std::atomic_bool synchronous_ {false};
    std::atomic<uint64_t> rotation_size_ {DEFAULT_ROTATION_SIZE};
    std::atomic_size_t rotation_keep_count_ {DEFAULT_ROTATION_KEEP_COUNT};
    std::atomic_int compression_level_ {Z_DEFAULT_COMPRESSION};

    std::string path_;
    std::ofstream file_;
    uint64_t current_size_ {0};
    std::vector<Logger::Msg> currentQ_;
    std::mutex mtx_;
    std::condition_variable cv_;
    std::thread thread_;
};

void
Logger::setFileLog(const std::string& path)
{
    FileLog::instance().setFile(path);
}

void
Logger::setFileLog(const std::string& path, bool synchronous)
{
    FileLog::instance().setFile(path, synchronous);
}

void
Logger::setFileLogSync(bool enable)
{
    FileLog::instance().setSynchronous(enable);
}

void
Logger::setFileLogRotationSize(std::size_t bytes)
{
    FileLog::instance().setRotationSize(bytes);
}

void
Logger::setFileLogRotationCount(std::size_t files)
{
    FileLog::instance().setRotationCount(files);
}

void
Logger::setFileLogCompressionLevel(int level)
{
    FileLog::instance().setCompressionLevel(level);
}

LIBSIP_CORE_PUBLIC void
Logger::log(int level, const char* file, int line, bool linefeed, const char* fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);

    vlog(level, file, line, linefeed, fmt, ap);

    va_end(ap);
}

template<typename T>
void
log_to_if_enabled(T& handler, Logger::Msg& msg)
{
    if (handler.isEnable()) {
        handler.consume(msg);
    }
}

static std::atomic_bool debugEnabled_ {false};

void
Logger::setDebugMode(bool enable)
{
    debugEnabled_.store(enable, std::memory_order_relaxed);
}

bool
Logger::debugEnabled()
{
    return debugEnabled_.load(std::memory_order_relaxed);
}

void
Logger::vlog(int level, const char* file, int line, bool linefeed, const char* fmt, va_list ap)
{
    if (level < LOG_WARNING and not debugEnabled_.load(std::memory_order_relaxed)) {
        return;
    }

    if (not(ConsoleLog::instance().isEnable() or SysLog::instance().isEnable()
            or MonitorLog::instance().isEnable() or FileLog::instance().isEnable())) {
        return;
    }

    /* Timestamp is generated here. */
    Msg msg(level, file, line, linefeed, fmt, ap);

    log_to_if_enabled(ConsoleLog::instance(), msg);
    log_to_if_enabled(SysLog::instance(), msg);
    log_to_if_enabled(MonitorLog::instance(), msg);
    log_to_if_enabled(FileLog::instance(), msg); // Takes ownership of msg if enabled
}

void
Logger::write(int level, const char* file, int line, std::string&& message)
{
    /* Timestamp is generated here. */
    Msg msg(level, file, line, true, std::move(message));

    log_to_if_enabled(ConsoleLog::instance(), msg);
    log_to_if_enabled(SysLog::instance(), msg);
    log_to_if_enabled(MonitorLog::instance(), msg);
    log_to_if_enabled(FileLog::instance(), msg); // Takes ownership of msg if enabled
}

void
Logger::fini()
{
    // Force close on file and join thread
    FileLog::instance().setFile({});

#ifdef _WIN32
    Logger::setConsoleLog(false);
#endif /* _WIN32 */
}

std::string
Logger::logLevelToString(int level)
{
    if (level == LOG_ERR) {
        return "ERROR";
    } else if (level == LOG_WARNING) {
        return "WARNING";
    }
    else if (level == LOG_INFO)
    {
        return "INFO";
    }
    else if (level == LOG_DEBUG) {
        return "DEBUG";
    }
    return "UNKNOWN";
}

} // namespace sip_core
