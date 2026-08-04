#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

// Owns only bounded, already-validated announcement work.  MQTT callbacks pass
// data here and return immediately; Application owns the eventual playback.
class AnnouncementManager {
public:
    static AnnouncementManager& GetInstance();

    static constexpr size_t kMaxFrames = 40;
    static constexpr size_t kMaxFrameBytes = 1500;
    static constexpr size_t kMaxTaskBytes = kMaxFrames * kMaxFrameBytes;

    struct Packet {
        std::vector<uint8_t> payload;
        bool last = false;
    };
    struct Task {
        std::string task_id;
        int priority = 0;
        int64_t expires_at = 0;
        std::vector<Packet> packets;
    };

    // Safe, protocol-level outcomes for a manifest command. These values are
    // suitable for diagnostics and ACK reasons; they never expose payload data.
    enum class ManifestResult {
        kAccepted,
        kDuplicate,
        kExpired,
        kInvalidManifest,
        kTooLarge,
        kIdConflict,
        kQueueFull,
    };

    struct ManifestOutcome {
        ManifestResult result = ManifestResult::kInvalidManifest;
        // Set after a command's expiresAt has been parsed, so diagnostics can
        // compare UTC Unix epochs without exposing request content or identity.
        int64_t now_epoch = 0;
        int64_t expires_epoch = 0;
    };

    using AckCallback = std::function<void(const std::string&, const std::string&, const std::string&)>;
    using ReadyCallback = std::function<void()>;
    void SetCallbacks(AckCallback ack, ReadyCallback ready);
    void SetDeviceCode(const std::string& device_code);
    // The manifest must be complete JSON. A malformed payload is reported as
    // kInvalidManifest; ACK is sent only when a safe task identifier exists.
    ManifestOutcome AcceptManifest(const char* json, size_t size);
    static const char* ManifestResultReason(ManifestResult result);
    // frame_index is derived from the strictly validated MQTT topic.
    bool AcceptFrame(const std::string& task_id, int frame_index, const uint8_t* data, size_t size);
    bool TakePending(Task* task);
    void Complete(const std::string& task_id, const std::string& status, const std::string& reason = "");
    void InterruptActive();

private:
    AnnouncementManager() = default;
    struct FrameSpec { size_t bytes = 0; uint32_t crc32 = 0; bool received = false; };
    struct Receiving { Task task; std::vector<FrameSpec> frames; std::string fingerprint; size_t bytes = 0; };
    struct Terminal { std::string fingerprint; std::string status; int64_t expires_at = 0; };
    bool IsExpired(int64_t expires_at) const;
    void Ack(const std::string& id, const std::string& status, const std::string& reason);
    std::mutex mutex_;
    std::string device_code_;
    Receiving receiving_;
    bool receiving_valid_ = false;
    Task pending_;
    bool pending_valid_ = false;
    std::map<std::string, Terminal> terminal_;
    AckCallback ack_;
    ReadyCallback ready_;
};
