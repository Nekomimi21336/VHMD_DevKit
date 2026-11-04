#include "state/recorder.h"

#include <cstdio>
#include <cstdlib>
#include <ctime>

#include "util/log.h"

namespace vhmd {

Recorder::Recorder(std::filesystem::path dir) : dir_(dir.empty() ? DefaultDir() : std::move(dir)) {}

std::filesystem::path Recorder::DefaultDir() {
#if defined(_WIN32)
    wchar_t* base = nullptr;
    size_t len = 0;
    std::filesystem::path dir;
    if (_wdupenv_s(&base, &len, L"LOCALAPPDATA") == 0 && base && *base) dir = std::filesystem::path(base) / L"VHMD" / L"recordings";
    std::free(base);
    return dir.empty() ? std::filesystem::temp_directory_path() / "VHMD" / "recordings" : dir;
#else
    if (const char* state = std::getenv("XDG_STATE_HOME"); state && *state)
        return std::filesystem::path(state) / "vhmd" / "recordings";
    if (const char* home = std::getenv("HOME"); home && *home)
        return std::filesystem::path(home) / ".local" / "state" / "vhmd" / "recordings";
    return std::filesystem::temp_directory_path() / "vhmd" / "recordings";
#endif
}

std::string Recorder::SafeName(const std::string& name) {
    if (name.empty() || name.size() > 64 || name[0] == '.') return {};
    for (char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
                        c == '_' || c == '.';
        if (!ok) return {};
    }
    return name;
}

std::string Recorder::Start(const std::string& name, Clock::time_point now) {
    std::string file;
    if (name.empty()) {
        char stamp[32];
        const std::time_t t = std::time(nullptr);
        std::tm tm{};
#if defined(_WIN32)
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        std::strftime(stamp, sizeof stamp, "vhmd_%Y%m%d_%H%M%S", &tm);
        file = stamp;
    } else {
        file = SafeName(name);
        if (file.empty()) return "name: letters, digits, '-', '_' and '.' only (at most 64)";
    }
    if (file.size() < 6 || file.compare(file.size() - 6, 6, ".jsonl") != 0) file += ".jsonl";

    std::lock_guard<std::mutex> lock(mu_);
    if (recording_) return "already recording";
    std::error_code ec;
    std::filesystem::create_directories(dir_, ec);
    const auto path = dir_ / file;
    if (std::filesystem::exists(path, ec)) return "a recording with that name exists";
    out_.open(path, std::ios::binary | std::ios::trunc);
    if (!out_) return "cannot write " + path.u8string();
    path_ = path;
    started_ = now;
    seconds_ = 0;
    lines_ = bytes_ = 0;
    recording_ = true;
    Log(LogLevel::Info, "recording to %s", path_.u8string().c_str());
    return {};
}

void Recorder::Stop() {
    std::lock_guard<std::mutex> lock(mu_);
    if (!recording_) return;
    out_.close();
    recording_ = false;
    Log(LogLevel::Info, "recording stopped: %llu lines, %.1f s", static_cast<unsigned long long>(lines_), seconds_);
}

bool Recorder::Recording() const {
    std::lock_guard<std::mutex> lock(mu_);
    return recording_;
}

void Recorder::Add(std::string_view datagram, Clock::time_point now) {
    if (datagram.find("\"type\":\"mic\"") != std::string_view::npos) return;   // sound, not motion
    std::lock_guard<std::mutex> lock(mu_);
    if (!recording_) return;
    seconds_ = std::chrono::duration<double>(now - started_).count();
    char head[48];
    std::snprintf(head, sizeof head, "{\"t\":%.6f,\"d\":", seconds_);
    std::string line = head;
    line.reserve(line.size() + datagram.size() + 2);
    for (char c : datagram) line += (c == '\n' || c == '\r') ? ' ' : c;   // one line per datagram
    line += "}\n";
    out_.write(line.data(), static_cast<std::streamsize>(line.size()));
    ++lines_;
    bytes_ += line.size();
    if (!out_ || bytes_ >= kMaxBytes) {
        out_.close();
        recording_ = false;
        Log(LogLevel::Warning, "recording stopped at %llu bytes (%s)", static_cast<unsigned long long>(bytes_),
            bytes_ >= kMaxBytes ? "size limit" : "write failed");
    }
}

nlohmann::json Recorder::Status(Clock::time_point now) const {
    std::lock_guard<std::mutex> lock(mu_);
    nlohmann::json j = {{"recording", recording_}, {"lines", lines_}, {"bytes", bytes_}};
    j["seconds"] = recording_ ? std::chrono::duration<double>(now - started_).count() : seconds_;
    if (!path_.empty()) j["path"] = path_.u8string();
    j["folder"] = dir_.u8string();
    return j;
}

} // namespace vhmd
