#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <mqtt_client.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

class MqttLogClient {
public:
    static MqttLogClient& GetInstance();

    void Start();
    // Never blocks callers such as audio, wake-word, UI, or Wi-Fi event tasks.
    void Report(const std::string& status, const std::string& message);
    // Publishes a low-frequency operational event to aiot/device/{deviceCode}/log.
    void Log(const std::string& event_type, const std::string& level, const std::string& message);
    // Called after Wi-Fi connects or reconnects. Discovery runs only in the worker task.
    void RequestDiscovery();
    void NotifyNetworkDisconnected();
    // LAN logging preserves the existing explicit startup gate. Remote logging
    // is always best-effort and returns true here so it cannot block XiaoZhi.
    bool WaitForStartupVerification(uint32_t timeout_ms, std::string* reason = nullptr);
    // Returns only the last validated, dynamically discovered local broker
    // host. It is empty when logging is disabled or the current network has
    // not produced a valid discovery response.
    std::string GetLastDiscoveredBrokerHost() const;

private:
    static constexpr size_t kStatusSize = 16;
    static constexpr size_t kEventTypeSize = 48;
    static constexpr size_t kLevelSize = 8;
    // UTF-8 uses at most three bytes for the Chinese text expected here: 500 characters + terminator.
    static constexpr size_t kMessageSize = 1501;
    static constexpr size_t kQueueDepth = 16;
    static constexpr uint32_t kDeferredRetryMs = 1000;
    static constexpr uint32_t kRemoteFailureAttemptThreshold = 3;
    static constexpr int64_t kRemoteFailureGraceMs = 30000;
    static constexpr EventBits_t kMqttConnected = BIT0;
    static constexpr EventBits_t kMqttFailed = BIT1;
    static constexpr EventBits_t kStartupVerificationSucceeded = BIT2;
    static constexpr EventBits_t kStartupVerificationFailed = BIT3;

    struct LogRecord {
        bool is_event_log = false;
        char status[kStatusSize];
        char event_type[kEventTypeSize];
        char level[kLevelSize];
        char message[kMessageSize];
        char reported_at[20];
    };

    struct Broker {
        std::string host;
        int port = 1883;
        bool tls = false;
    };

    enum class ConnectionAttemptResult {
        Connected,
        Deferred,
        Failed,
    };

    MqttLogClient() = default;
    static void TaskEntry(void* arg);
    static void MqttEventHandler(void* handler_args, esp_event_base_t base, int32_t event_id, void* event_data);
    void TaskLoop();
    bool DiscoverBroker();
    bool IsValidDiscoveryResponse(const char* payload, const std::string& nonce, const char* source_ip, Broker* broker) const;
    ConnectionAttemptResult EnsureConnected();
    void ResetMqtt();
    bool Publish(const LogRecord& record);
    void PublishAnnouncementAck(const std::string& task_id, const std::string& status, const std::string& reason);
    void HandleAnnouncementData(esp_mqtt_event_handle_t event);
    void ProcessAnnouncementMessage(const std::string& topic, const std::vector<uint8_t>& payload);
    void ScheduleRetry();
    void ScheduleDeferredRetry();
    void RecordConnectionFailure(int64_t now_ms);
    void ResetConnectionFailureTracking();
    std::string BuildDeviceCode() const;
    std::string BuildClientId() const;
    std::string GetReportedAt() const;
    void RestoreCachedTrustedTime();
    void SaveTrustedTime();
    static bool IsPrivateIpv4(const char* ip);

    QueueHandle_t queue_ = nullptr;
    EventGroupHandle_t mqtt_events_ = nullptr;
    TaskHandle_t task_handle_ = nullptr;
    esp_mqtt_client_handle_t mqtt_client_ = nullptr;
    std::atomic<bool> connected_{false};
    // esp_mqtt_client_stop() emits a disconnect event too. Ignore that expected
    // event so reconnects and profile/network transitions are not reported as failures.
    std::atomic<bool> mqtt_stop_expected_{false};
    // The ESP-IDF TCP/IP mailbox does not exist until the Wi-Fi-connected callback.
    // Keep every MQTT operation behind this gate so startup logging cannot touch lwIP early.
    std::atomic<bool> network_ready_{false};
    // Set synchronously from Wi-Fi callbacks before the worker resets MQTT.
    // MQTT disconnect/error callbacks during this transition are expected and
    // must not become remote broker incidents.
    std::atomic<bool> network_transition_{false};
    std::atomic<bool> discovery_requested_{false};
    std::atomic<bool> network_disconnected_{false};
    bool started_ = false;
    Broker fallback_broker_;
    Broker active_broker_;
    bool remote_mode_ = false;
    // The captive portal selects whether LAN service discovery is desired.
    bool local_service_mode_ = false;
    bool waiting_for_trusted_time_logged_ = false;
    bool trusted_time_persisted_ = false;
    bool using_discovered_broker_ = false;
    std::string username_;
    std::string password_;
    std::string device_code_;
    std::string client_id_;
    std::string report_topic_;
    std::string log_topic_;
    std::string announcement_command_topic_;
    std::string announcement_audio_prefix_;
    std::string announcement_ack_topic_;
    std::string incoming_topic_;
    std::vector<uint8_t> incoming_payload_;
    int incoming_total_len_ = 0;
    std::mutex incoming_mutex_;
    std::string discovery_token_;
    mutable std::mutex discovered_broker_mutex_;
    std::string last_discovered_broker_host_;
    int keepalive_seconds_ = 60;
    int discovery_port_ = 19830;
    int discovery_timeout_ms_ = 1500;
    int discovery_retries_ = 3;
    uint32_t retry_delay_ms_ = 5000;
    uint32_t retry_min_ms_ = 5000;
    uint32_t retry_max_ms_ = 300000;
    int64_t next_retry_at_ms_ = 0;
    // mqtt_failure_pending_ requests a reconnect. mqtt_failure_reportable_
    // becomes true only after a sustained remote failure; keeping them separate
    // prevents brief Wi-Fi/DNS/TLS transitions from producing failure/recovery noise.
    std::atomic<bool> mqtt_failure_pending_{false};
    std::atomic<bool> mqtt_failure_reportable_{false};
    uint32_t consecutive_connection_failures_ = 0;
    int64_t first_connection_failure_at_ms_ = 0;
    bool mqtt_connected_once_ = false;
    std::atomic<bool> startup_verification_required_{false};
    std::atomic<bool> startup_verification_succeeded_{false};
    std::atomic<bool> startup_verification_failed_{false};
    mutable std::mutex startup_verification_mutex_;
    std::string startup_verification_reason_;

    void SetStartupVerificationFailed(const char* reason);
    void SetStartupVerificationSucceeded();
};
