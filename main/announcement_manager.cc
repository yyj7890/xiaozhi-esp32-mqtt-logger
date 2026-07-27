#include "announcement_manager.h"

#include <algorithm>
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
    int year, month, day, hour, minute, second;
    if (std::sscanf(value, "%d-%d-%dT%d:%d:%dZ", &year, &month, &day, &hour, &minute, &second) != 6) return false;
    if (year < 2024 || month < 1 || month > 12 || day < 1 || hour > 23 || minute > 59 || second > 59) return false;
    static constexpr int kDaysBeforeMonth[] = {0,31,59,90,120,151,181,212,243,273,304,334};
    const bool leap = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
    const int days_in_month = (month == 2) ? (leap ? 29 : 28) : ((month == 4 || month == 6 || month == 9 || month == 11) ? 30 : 31);
    if (day > days_in_month) return false;
    int64_t days = 0;
    for (int y = 1970; y < year; ++y) days += (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) ? 366 : 365;
    days += kDaysBeforeMonth[month - 1] + day - 1 + (leap && month > 2 ? 1 : 0);
    *out = days * 86400 + hour * 3600 + minute * 60 + second;
    return true;
}
bool HexCrc(const char* text, uint32_t* crc) {
    if (!text || !crc || std::strlen(text) != 8) return false;
    char* end = nullptr; const unsigned long value = std::strtoul(text, &end, 16);
    if (!end || *end != '\0') return false;
    *crc = static_cast<uint32_t>(value);
    return true;
}
}

AnnouncementManager& AnnouncementManager::GetInstance() { static AnnouncementManager instance; return instance; }
void AnnouncementManager::SetCallbacks(AckCallback ack, ReadyCallback ready) { std::lock_guard<std::mutex> lock(mutex_); if (ack) ack_ = std::move(ack); if (ready) ready_ = std::move(ready); }
void AnnouncementManager::SetDeviceCode(const std::string& value) { std::lock_guard<std::mutex> lock(mutex_); device_code_ = value; }
bool AnnouncementManager::IsExpired(int64_t expires_at) const { return std::time(nullptr) >= expires_at; }
void AnnouncementManager::Ack(const std::string& id, const std::string& status, const std::string& reason) { if (ack_) ack_(id, status, reason); }

bool AnnouncementManager::AcceptManifest(const char* json, size_t size) {
    cJSON* root = cJSON_ParseWithLength(json, size); if (!root) return false;
    auto protocol = cJSON_GetObjectItem(root, "protocol"); auto id = cJSON_GetObjectItem(root, "taskId");
    auto code = cJSON_GetObjectItem(root, "deviceCode"); auto priority = cJSON_GetObjectItem(root, "priority");
    auto expires = cJSON_GetObjectItem(root, "expiresAt"); auto audio = cJSON_GetObjectItem(root, "audio");
    const bool basic = cJSON_IsString(protocol) && std::strcmp(protocol->valuestring, kProtocol) == 0 && cJSON_IsString(id) &&
        std::strlen(id->valuestring) > 0 && std::strlen(id->valuestring) <= 64 && cJSON_IsString(code) && cJSON_IsNumber(priority) && cJSON_IsString(expires) && cJSON_IsObject(audio);
    int64_t expiry = 0; if (!basic || !ParseUtc(expires->valuestring, &expiry)) { cJSON_Delete(root); return false; }
    auto codec = cJSON_GetObjectItem(audio,"codec"); auto rate = cJSON_GetObjectItem(audio,"sampleRate"); auto channels = cJSON_GetObjectItem(audio,"channels"); auto duration = cJSON_GetObjectItem(audio,"frameDurationMs"); auto frame_count = cJSON_GetObjectItem(audio,"frameCount"); auto frames = cJSON_GetObjectItem(audio,"frames");
    const std::string task_id = id->valuestring;
    if (device_code_ != code->valuestring || IsExpired(expiry) || !cJSON_IsString(codec) || std::strcmp(codec->valuestring,"opus") || !cJSON_IsNumber(rate) || rate->valueint != 16000 || !cJSON_IsNumber(channels) || channels->valueint != 1 || !cJSON_IsNumber(duration) || duration->valueint != 60 || !cJSON_IsNumber(frame_count) || !cJSON_IsArray(frames) || frame_count->valueint != cJSON_GetArraySize(frames) || cJSON_GetArraySize(frames) < 1 || cJSON_GetArraySize(frames) > static_cast<int>(kMaxFrames)) { cJSON_Delete(root); Ack(task_id, "failed", IsExpired(expiry) ? "expired" : "invalid_manifest"); return true; }
    Receiving candidate{}; candidate.task.task_id = id->valuestring; candidate.task.priority = priority->valueint; candidate.task.expires_at = expiry; candidate.fingerprint.assign(json, size); candidate.frames.resize(cJSON_GetArraySize(frames)); candidate.task.packets.resize(candidate.frames.size());
    for (int i=0;i<cJSON_GetArraySize(frames);++i) { auto f=cJSON_GetArrayItem(frames,i); auto index=cJSON_GetObjectItem(f,"index"); auto bytes=cJSON_GetObjectItem(f,"bytes"); auto crc=cJSON_GetObjectItem(f,"crc32"); if (!cJSON_IsObject(f)||!cJSON_IsNumber(index)||index->valueint!=i||!cJSON_IsNumber(bytes)||bytes->valueint<1||bytes->valueint>static_cast<int>(kMaxFrameBytes)||!cJSON_IsString(crc)||!HexCrc(crc->valuestring,&candidate.frames[i].crc32)) { cJSON_Delete(root); Ack(candidate.task.task_id,"failed","invalid_manifest"); return true; } candidate.frames[i].bytes=bytes->valueint; candidate.bytes+=bytes->valueint; }
    cJSON_Delete(root); if (candidate.bytes > kMaxTaskBytes) { Ack(candidate.task.task_id,"failed","too_large"); return true; }
    std::lock_guard<std::mutex> lock(mutex_); if (device_code_.empty()) return false;
    auto known = terminal_.find(task_id); if (known != terminal_.end() && known->second.expires_at > std::time(nullptr)) { Ack(task_id, known->second.fingerprint == candidate.fingerprint ? known->second.status : "failed", known->second.fingerprint == candidate.fingerprint ? "" : "id_conflict"); return true; }
    if (receiving_valid_ && receiving_.task.task_id == task_id) { Ack(task_id, receiving_.fingerprint == candidate.fingerprint ? "received" : "failed", receiving_.fingerprint == candidate.fingerprint ? "" : "id_conflict"); return true; }
    if (pending_valid_ && pending_.task_id == task_id) { Ack(task_id, "failed", "id_conflict"); return true; }
    receiving_=std::move(candidate); receiving_valid_=true; return true;
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
