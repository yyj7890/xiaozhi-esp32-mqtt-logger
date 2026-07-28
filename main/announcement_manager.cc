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
bool ParseUtc(const char* value, int64_t* out) {
    if (!value || !out) return false;
    int year, month, day, hour, minute, second, consumed = 0;
    if (std::sscanf(value, "%d-%d-%dT%d:%d:%d%n", &year, &month, &day, &hour, &minute, &second, &consumed) != 6) return false;
    const char* suffix = value + consumed;
    bool has_fraction = false;
    bool fraction_nonzero = false;
    if (*suffix == '.') {
        ++suffix;
        while (std::isdigit(static_cast<unsigned char>(*suffix))) {
            has_fraction = true;
            fraction_nonzero = fraction_nonzero || *suffix != '0';
            ++suffix;
        }
        if (!has_fraction) return false;
    }
    if (*suffix != 'Z' || suffix[1] != '\0') return false;
    if (year < 2024 || month < 1 || month > 12 || day < 1 || hour > 23 || minute > 59 || second > 59) return false;
    static constexpr int kDaysBeforeMonth[] = {0,31,59,90,120,151,181,212,243,273,304,334};
    const bool leap = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
    const int days_in_month = (month == 2) ? (leap ? 29 : 28) : ((month == 4 || month == 6 || month == 9 || month == 11) ? 30 : 31);
    if (day > days_in_month) return false;
    int64_t days = 0;
    for (int y = 1970; y < year; ++y) days += (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) ? 366 : 365;
    days += kDaysBeforeMonth[month - 1] + day - 1 + (leap && month > 2 ? 1 : 0);
    // time_t has second resolution. Round a non-zero fractional expiry up so
    // a valid task is never rejected before its declared UTC instant.
    *out = days * 86400 + hour * 3600 + minute * 60 + second + (fraction_nonzero ? 1 : 0);
    return true;
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

AnnouncementManager::ManifestResult AnnouncementManager::AcceptManifest(const char* json, size_t size) {
    cJSON* root = cJSON_ParseWithLength(json, size);
    if (!root) return ManifestResult::kInvalidManifest;
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
        return ManifestResult::kInvalidManifest;
    }
    auto codec = cJSON_GetObjectItem(audio,"codec"); auto rate = cJSON_GetObjectItem(audio,"sampleRate"); auto channels = cJSON_GetObjectItem(audio,"channels"); auto duration = cJSON_GetObjectItem(audio,"frameDurationMs"); auto frame_count = cJSON_GetObjectItem(audio,"frameCount"); auto frames = cJSON_GetObjectItem(audio,"frames");
    const std::string task_id = id->valuestring;
    if (device_code_ != code->valuestring || IsExpired(expiry) || !cJSON_IsString(codec) || std::strcmp(codec->valuestring,"opus") || !cJSON_IsNumber(rate) || rate->valueint != 16000 || !cJSON_IsNumber(channels) || channels->valueint != 1 || !cJSON_IsNumber(duration) || duration->valueint != 60 || !cJSON_IsNumber(frame_count) || !cJSON_IsArray(frames) || frame_count->valueint != cJSON_GetArraySize(frames) || cJSON_GetArraySize(frames) < 1 || cJSON_GetArraySize(frames) > static_cast<int>(kMaxFrames)) {
        const ManifestResult result = IsExpired(expiry) ? ManifestResult::kExpired : ManifestResult::kInvalidManifest;
        cJSON_Delete(root); Ack(task_id, "failed", ManifestResultReason(result)); return result;
    }
    Receiving candidate{}; candidate.task.task_id = id->valuestring; candidate.task.priority = priority->valueint; candidate.task.expires_at = expiry; candidate.fingerprint.assign(json, size); candidate.frames.resize(cJSON_GetArraySize(frames)); candidate.task.packets.resize(candidate.frames.size());
    for (int i=0;i<cJSON_GetArraySize(frames);++i) { auto f=cJSON_GetArrayItem(frames,i); auto index=cJSON_GetObjectItem(f,"index"); auto bytes=cJSON_GetObjectItem(f,"bytes"); auto crc=cJSON_GetObjectItem(f,"crc32"); if (!cJSON_IsObject(f)||!cJSON_IsNumber(index)||index->valueint!=i||!cJSON_IsNumber(bytes)||bytes->valueint<1||bytes->valueint>static_cast<int>(kMaxFrameBytes)||!cJSON_IsString(crc)||!HexCrc(crc->valuestring,&candidate.frames[i].crc32)) { cJSON_Delete(root); Ack(candidate.task.task_id,"failed","invalid_manifest"); return ManifestResult::kInvalidManifest; } candidate.frames[i].bytes=bytes->valueint; candidate.bytes+=bytes->valueint; }
    cJSON_Delete(root); if (candidate.bytes > kMaxTaskBytes) { Ack(candidate.task.task_id,"failed","too_large"); return ManifestResult::kTooLarge; }
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
    return result;
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
