#include "announcement_manager.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <ctime>

#include <cJSON.h>
#include <esp_rom_crc.h>

namespace {
constexpr const char* kProtocol = "aiot-announcement-v1";
constexpr size_t kTerminalLimit = 16;
bool ReadFixedDigits(const char** cursor, size_t count, int* value) {
    if (!cursor || !*cursor || !value) return false;
    int parsed = 0;
    for (size_t i = 0; i < count; ++i) {
        const unsigned char c = static_cast<unsigned char>((*cursor)[i]);
        if (!std::isdigit(c)) return false;
        parsed = parsed * 10 + (c - '0');
    }
    *cursor += count;
    *value = parsed;
    return true;
}

bool Consume(const char** cursor, char expected) {
    if (!cursor || !*cursor || **cursor != expected) return false;
    ++*cursor;
    return true;
}

// Gregorian civil date to days since 1970-01-01. This is deliberately UTC-only
// and does not call mktime(), so a device timezone can never affect expiry.
int64_t DaysSinceUnixEpoch(int year, unsigned month, unsigned day) {
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned year_of_era = static_cast<unsigned>(year - era * 400);
    const unsigned adjusted_month = month > 2 ? month - 3 : month + 9;
    const unsigned day_of_year = (153 * adjusted_month + 2) / 5 + day - 1;
    const unsigned day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    return static_cast<int64_t>(era) * 146097 + day_of_era - 719468;
}

bool ParseUtc(const char* value, int64_t* out) {
    if (!value || !out) return false;
    const char* cursor = value;
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    if (!ReadFixedDigits(&cursor, 4, &year) || !Consume(&cursor, '-') ||
        !ReadFixedDigits(&cursor, 2, &month) || !Consume(&cursor, '-') ||
        !ReadFixedDigits(&cursor, 2, &day) || !Consume(&cursor, 'T') ||
        !ReadFixedDigits(&cursor, 2, &hour) || !Consume(&cursor, ':') ||
        !ReadFixedDigits(&cursor, 2, &minute) || !Consume(&cursor, ':') ||
        !ReadFixedDigits(&cursor, 2, &second)) return false;
    bool has_fraction = false;
    bool fraction_nonzero = false;
    if (*cursor == '.') {
        ++cursor;
        size_t fraction_digits = 0;
        while (std::isdigit(static_cast<unsigned char>(*cursor))) {
            has_fraction = true;
            fraction_nonzero = fraction_nonzero || *cursor != '0';
            ++cursor;
            if (++fraction_digits > 9) return false;
        }
        if (!has_fraction) return false;
    }
    if (*cursor != 'Z' || cursor[1] != '\0') return false;
    if (year < 2024 || month < 1 || month > 12 || day < 1 || hour > 23 || minute > 59 || second > 59) return false;
    const bool leap = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
    const int days_in_month = (month == 2) ? (leap ? 29 : 28) : ((month == 4 || month == 6 || month == 9 || month == 11) ? 30 : 31);
    if (day > days_in_month) return false;
    // time_t has second resolution. Round a non-zero fractional expiry up so
    // a valid task is never rejected before its declared UTC instant.
    *out = DaysSinceUnixEpoch(year, static_cast<unsigned>(month), static_cast<unsigned>(day)) * 86400 +
        hour * 3600 + minute * 60 + second + (fraction_nonzero ? 1 : 0);
    return true;
}

bool VerifyUtcParser() {
    struct TestCase { const char* text; int64_t epoch; };
    // Epoch constants were independently produced from the UTC Unix epoch
    // definition, not by DaysSinceUnixEpoch(), so this detects regressions in
    // the production civil-date conversion itself.
    static constexpr TestCase kCases[] = {
        {"2026-07-28T00:00:00Z", 1785196800},       // whole second
        {"2026-07-28T00:00:00.001Z", 1785196801},   // ceiling fractional seconds
        {"2026-07-28T00:00:00.999Z", 1785196801},
        {"2026-07-28T00:01:00Z", 1785196860},       // minute boundary
        {"2026-07-29T00:00:00Z", 1785283200},       // day boundary
        {"2026-07-28T00:05:00.001Z", 1785197101},   // IoT .sssZ + five minutes
    };
    int64_t parsed = 0;
    for (const auto& test : kCases) {
        if (!ParseUtc(test.text, &parsed) || parsed != test.epoch) return false;
    }
    // The fractional future sample is rounded up, so its delta is 301 seconds.
    return kCases[5].epoch - kCases[0].epoch == 301 && kCases[5].epoch > kCases[0].epoch;
}
bool HexCrc(const char* text, uint32_t* crc) {
    if (!text || !crc || std::strlen(text) != 8) return false;
    char* end = nullptr; const unsigned long value = std::strtoul(text, &end, 16);
    if (!end || *end != '\0') return false;
    *crc = static_cast<uint32_t>(value);
    return true;
}
bool IsSafeTaskId(const char* value) {
    if (!value || *value == '\0' || std::strlen(value) > 64) return false;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(value); *p; ++p) {
        if (!std::isalnum(*p) && *p != '-' && *p != '_' && *p != '.') return false;
    }
    return true;
}
}

AnnouncementManager& AnnouncementManager::GetInstance() { static AnnouncementManager instance; return instance; }
void AnnouncementManager::SetCallbacks(AckCallback ack, ReadyCallback ready) { std::lock_guard<std::mutex> lock(mutex_); if (ack) ack_ = std::move(ack); if (ready) ready_ = std::move(ready); }
void AnnouncementManager::SetDeviceCode(const std::string& value) { std::lock_guard<std::mutex> lock(mutex_); device_code_ = value; }
bool AnnouncementManager::IsExpired(int64_t expires_at) const { return std::time(nullptr) >= expires_at; }
void AnnouncementManager::Ack(const std::string& id, const std::string& status, const std::string& reason) { if (ack_) ack_(id, status, reason); }

const char* AnnouncementManager::ManifestResultReason(ManifestResult result) {
    switch (result) {
        case ManifestResult::kExpired: return "expired";
        case ManifestResult::kTooLarge: return "too_large";
        case ManifestResult::kIdConflict: return "id_conflict";
        case ManifestResult::kQueueFull: return "queue_full";
        case ManifestResult::kInvalidManifest: return "invalid_manifest";
        case ManifestResult::kAccepted:
        case ManifestResult::kDuplicate:
            return "";
    }
    return "invalid_manifest";
}

AnnouncementManager::ManifestOutcome AnnouncementManager::AcceptManifest(const char* json, size_t size) {
    static const bool utc_parser_verified = VerifyUtcParser();
    if (!utc_parser_verified) return {ManifestResult::kInvalidManifest};
    cJSON* root = cJSON_ParseWithLength(json, size);
    if (!root) return {ManifestResult::kInvalidManifest};
    auto protocol = cJSON_GetObjectItem(root, "protocol"); auto id = cJSON_GetObjectItem(root, "taskId");
    auto code = cJSON_GetObjectItem(root, "deviceCode"); auto priority = cJSON_GetObjectItem(root, "priority");
    auto expires = cJSON_GetObjectItem(root, "expiresAt"); auto audio = cJSON_GetObjectItem(root, "audio");
    const bool can_ack = cJSON_IsString(id) && IsSafeTaskId(id->valuestring) && cJSON_IsString(code) &&
        code->valuestring[0] != '\0' && std::strlen(code->valuestring) <= 64;
    const bool basic = cJSON_IsString(protocol) && std::strcmp(protocol->valuestring, kProtocol) == 0 && can_ack &&
        cJSON_IsNumber(priority) && cJSON_IsString(expires) && cJSON_IsObject(audio);
    int64_t expiry = 0;
    if (!basic || !ParseUtc(expires->valuestring, &expiry)) {
        cJSON_Delete(root);
        if (can_ack) Ack(id->valuestring, "failed", "invalid_manifest");
        return {ManifestResult::kInvalidManifest};
    }
    auto codec = cJSON_GetObjectItem(audio,"codec"); auto rate = cJSON_GetObjectItem(audio,"sampleRate"); auto channels = cJSON_GetObjectItem(audio,"channels"); auto duration = cJSON_GetObjectItem(audio,"frameDurationMs"); auto frame_count = cJSON_GetObjectItem(audio,"frameCount"); auto frames = cJSON_GetObjectItem(audio,"frames");
    const std::string task_id = id->valuestring;
    const int64_t now_epoch = std::time(nullptr);
    const bool expired = now_epoch >= expiry;
    if (device_code_ != code->valuestring || expired || !cJSON_IsString(codec) || std::strcmp(codec->valuestring,"opus") || !cJSON_IsNumber(rate) || rate->valueint != 16000 || !cJSON_IsNumber(channels) || channels->valueint != 1 || !cJSON_IsNumber(duration) || duration->valueint != 60 || !cJSON_IsNumber(frame_count) || !cJSON_IsArray(frames) || frame_count->valueint != cJSON_GetArraySize(frames) || cJSON_GetArraySize(frames) < 1 || cJSON_GetArraySize(frames) > static_cast<int>(kMaxFrames)) {
        const ManifestResult result = expired ? ManifestResult::kExpired : ManifestResult::kInvalidManifest;
        cJSON_Delete(root); Ack(task_id, "failed", ManifestResultReason(result));
        return {result, now_epoch, expiry};
    }
    Receiving candidate{}; candidate.task.task_id = id->valuestring; candidate.task.priority = priority->valueint; candidate.task.expires_at = expiry; candidate.fingerprint.assign(json, size); candidate.frames.resize(cJSON_GetArraySize(frames)); candidate.task.packets.resize(candidate.frames.size());
    for (int i=0;i<cJSON_GetArraySize(frames);++i) { auto f=cJSON_GetArrayItem(frames,i); auto index=cJSON_GetObjectItem(f,"index"); auto bytes=cJSON_GetObjectItem(f,"bytes"); auto crc=cJSON_GetObjectItem(f,"crc32"); if (!cJSON_IsObject(f)||!cJSON_IsNumber(index)||index->valueint!=i||!cJSON_IsNumber(bytes)||bytes->valueint<1||bytes->valueint>static_cast<int>(kMaxFrameBytes)||!cJSON_IsString(crc)||!HexCrc(crc->valuestring,&candidate.frames[i].crc32)) { cJSON_Delete(root); Ack(candidate.task.task_id,"failed","invalid_manifest"); return {ManifestResult::kInvalidManifest}; } candidate.frames[i].bytes=bytes->valueint; candidate.bytes+=bytes->valueint; }
    cJSON_Delete(root); if (candidate.bytes > kMaxTaskBytes) { Ack(candidate.task.task_id,"failed","too_large"); return {ManifestResult::kTooLarge}; }
    std::string ack_status;
    ManifestResult result = ManifestResult::kAccepted;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (device_code_.empty()) {
            result = ManifestResult::kInvalidManifest;
        } else {
            auto known = terminal_.find(task_id);
            if (known != terminal_.end() && known->second.expires_at > std::time(nullptr)) {
                result = known->second.fingerprint == candidate.fingerprint ? ManifestResult::kDuplicate : ManifestResult::kIdConflict;
                ack_status = known->second.fingerprint == candidate.fingerprint ? known->second.status : "failed";
            } else if (receiving_valid_ && receiving_.task.task_id == task_id) {
                result = receiving_.fingerprint == candidate.fingerprint ? ManifestResult::kDuplicate : ManifestResult::kIdConflict;
                ack_status = receiving_.fingerprint == candidate.fingerprint ? "received" : "failed";
            } else if (pending_valid_ && pending_.task_id == task_id) {
                result = ManifestResult::kIdConflict;
                ack_status = "failed";
            } else {
                receiving_ = std::move(candidate);
                receiving_valid_ = true;
            }
        }
    }
    if (result != ManifestResult::kAccepted) {
        Ack(task_id, ack_status.empty() ? "failed" : ack_status, ManifestResultReason(result));
    }
    return {result, now_epoch, expiry};
}

bool AnnouncementManager::AcceptFrame(const std::string& id, int index, const uint8_t* data, size_t size) {
    ReadyCallback ready; std::string reason;
    { std::lock_guard<std::mutex> lock(mutex_);
      if (!receiving_valid_ || receiving_.task.task_id != id || index < 0 || index >= static_cast<int>(receiving_.frames.size())) return false;
      auto& spec=receiving_.frames[index]; if (spec.received) return true;
      if (size != spec.bytes || esp_rom_crc32_le(0, data, size) != spec.crc32) { receiving_valid_=false; reason="invalid_frame"; }
      else { receiving_.task.packets[index].payload.assign(data,data+size); receiving_.task.packets[index].last=index+1==static_cast<int>(receiving_.frames.size()); spec.received=true;
        if (std::all_of(receiving_.frames.begin(), receiving_.frames.end(), [](const FrameSpec& f){return f.received;})) {
          if (pending_valid_ && pending_.priority >= receiving_.task.priority) reason="queue_full";
          else { if (pending_valid_) Ack(pending_.task_id,"failed","superseded"); pending_=std::move(receiving_.task); pending_valid_=true; while (terminal_.size() >= kTerminalLimit) terminal_.erase(terminal_.begin()); terminal_[id] = {receiving_.fingerprint, "received", pending_.expires_at}; ready=ready_; }
          receiving_valid_=false;
        }
      }
    }
    if (!reason.empty()) Ack(id,"failed",reason); else if (ready) { Ack(id,"received",""); ready(); } return true;
}
bool AnnouncementManager::TakePending(Task* task) { std::string expired; { std::lock_guard<std::mutex> lock(mutex_); if (!task||!pending_valid_) return false; if (IsExpired(pending_.expires_at)) { expired=pending_.task_id; pending_valid_=false; } else { *task=std::move(pending_); pending_valid_=false; return true; } } Ack(expired,"failed","expired"); return false; }
void AnnouncementManager::Complete(const std::string& id,const std::string& status,const std::string& reason){ std::lock_guard<std::mutex> lock(mutex_); auto& terminal = terminal_[id]; terminal.status=status; terminal.expires_at=std::max<int64_t>(terminal.expires_at, std::time(nullptr) + 3600); Ack(id,status,reason); }
void AnnouncementManager::InterruptActive() {}
