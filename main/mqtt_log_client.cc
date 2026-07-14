#include "mqtt_log_client.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>

#include <cJSON.h>
#include <aiot_log_config.h>
#include <esp_crt_bundle.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <lwip/inet.h>
#include <lwip/sockets.h>

namespace {
constexpr const char* TAG = "MqttLog";
constexpr const char* kDiscoveryProtocol = "aiot-mqtt-discovery-v1";
constexpr std::time_t kValidUnixTime = 1700000000;

bool IsAcceptedStatus(const std::string& status) {
    return status == "NORMAL" || status == "ABNORMAL" ||
           status == "OFFLINE" || status == "MAINTENANCE";
}

bool IsAcceptedLevel(const std::string& level) {
    return level == "INFO" || level == "WARN" || level == "ERROR";
}

void CopyUtf8(char* destination, size_t destination_size, const std::string& source) {
    if (destination_size == 0) return;
    const size_t copy_length = std::min(source.size(), destination_size - 1);
    std::memcpy(destination, source.data(), copy_length);
    destination[copy_length] = '\0';
    // Never publish an incomplete UTF-8 sequence when the byte buffer ends mid-character.
    size_t continuation_start = copy_length;
    while (continuation_start > 0 &&
           (static_cast<unsigned char>(destination[continuation_start - 1]) & 0xC0) == 0x80) {
        --continuation_start;
    }
    if (continuation_start > 0 && continuation_start < copy_length) {
        const unsigned char lead = static_cast<unsigned char>(destination[continuation_start - 1]);
        const size_t expected = (lead & 0xF0) == 0xF0 ? 4 :
            (lead & 0xE0) == 0xE0 ? 3 : (lead & 0xC0) == 0xC0 ? 2 : 1;
        if (copy_length - (continuation_start - 1) < expected) destination[continuation_start - 1] = '\0';
    }
}
}

MqttLogClient& MqttLogClient::GetInstance() {
    static MqttLogClient instance;
    return instance;
}

void MqttLogClient::Start() {
#if !CONFIG_AIOT_MQTT_LOG_ENABLED
    return;
#else
    if (started_) return;
    startup_verification_succeeded_.store(false);
    startup_verification_failed_.store(false);
    {
        std::lock_guard<std::mutex> lock(startup_verification_mutex_);
        startup_verification_reason_.clear();
    }
    // This is an optional local-only feature. A compiled client is inert until
    // its captive-portal switch is enabled in the independent aiot_log NVS
    // namespace. It must not inherit any old build-time host or credentials.
    const AiotLogConfig runtime_config = AiotLogConfigStore::Load();
    if (!runtime_config.enabled) {
        ESP_LOGI(TAG, "Optional local IoT logging is disabled");
        return;
    }
    startup_verification_required_.store(true);
    if (runtime_config.username.empty() || runtime_config.password.empty()) {
        SetStartupVerificationFailed("MQTT username and password are required when local logging is enabled");
        ESP_LOGW(TAG, "Local IoT logging enabled without MQTT credentials");
        return;
    }
    fallback_broker_.host = runtime_config.manual_host;
    fallback_broker_.port = runtime_config.port;
    fallback_broker_.tls = false;
    active_broker_ = fallback_broker_;
    username_ = runtime_config.username;
    password_ = runtime_config.password;
    keepalive_seconds_ = CONFIG_AIOT_MQTT_LOG_KEEPALIVE;
    retry_min_ms_ = CONFIG_AIOT_MQTT_LOG_RETRY_MIN_SECONDS * 1000U;
    retry_delay_ms_ = retry_min_ms_;
    retry_max_ms_ = CONFIG_AIOT_MQTT_LOG_RETRY_MAX_SECONDS * 1000U;
    device_code_ = BuildDeviceCode();
    client_id_ = BuildClientId();
    report_topic_ = "aiot/device/" + device_code_ + "/report";
    log_topic_ = "aiot/device/" + device_code_ + "/log";
#if CONFIG_AIOT_MQTT_LOG_DISCOVERY_ENABLED
    discovery_port_ = CONFIG_AIOT_MQTT_LOG_DISCOVERY_PORT;
    discovery_timeout_ms_ = CONFIG_AIOT_MQTT_LOG_DISCOVERY_TIMEOUT_MS;
    discovery_retries_ = CONFIG_AIOT_MQTT_LOG_DISCOVERY_RETRIES;
    discovery_token_ = runtime_config.discovery_token;
#endif
    queue_ = xQueueCreate(kQueueDepth, sizeof(LogRecord));
    mqtt_events_ = xEventGroupCreate();
    if (queue_ == nullptr || mqtt_events_ == nullptr ||
        xTaskCreate(TaskEntry, "mqtt_log", 6144, this, 1, &task_handle_) != pdPASS) {
        ESP_LOGW(TAG, "MQTT log worker allocation failed; reporting disabled");
        if (queue_) vQueueDelete(queue_);
        if (mqtt_events_) vEventGroupDelete(mqtt_events_);
        queue_ = nullptr;
        mqtt_events_ = nullptr;
        return;
    }
    started_ = true;
#endif
}

void MqttLogClient::Report(const std::string& status, const std::string& message) {
    if (!started_ || queue_ == nullptr) return;
    LogRecord record{};
    std::snprintf(record.status, sizeof(record.status), "%s", IsAcceptedStatus(status) ? status.c_str() : "ABNORMAL");
    CopyUtf8(record.message, sizeof(record.message), message);
    std::snprintf(record.reported_at, sizeof(record.reported_at), "%s", GetReportedAt().c_str());
    if (xQueueSend(queue_, &record, 0) != pdPASS) {
        ESP_LOGW(TAG, "MQTT log queue full; dropping %s", record.status);
    }
}

void MqttLogClient::Log(const std::string& event_type, const std::string& level, const std::string& message) {
    if (!started_ || queue_ == nullptr) return;
    LogRecord record{};
    record.is_event_log = true;
    std::snprintf(record.event_type, sizeof(record.event_type), "%s", event_type.c_str());
    std::snprintf(record.level, sizeof(record.level), "%s", IsAcceptedLevel(level) ? level.c_str() : "ERROR");
    CopyUtf8(record.message, sizeof(record.message), message);
    std::snprintf(record.reported_at, sizeof(record.reported_at), "%s", GetReportedAt().c_str());
    if (xQueueSend(queue_, &record, 0) != pdPASS) {
        ESP_LOGW(TAG, "MQTT log queue full; dropping event %s", record.event_type);
    }
}

void MqttLogClient::RequestDiscovery() {
#if CONFIG_AIOT_MQTT_LOG_DISCOVERY_ENABLED
    if (started_) {
        discovery_requested_.store(true);
        if (task_handle_) xTaskNotifyGive(task_handle_);
    }
#endif
}

void MqttLogClient::NotifyNetworkDisconnected() {
    if (started_) {
        network_disconnected_.store(true);
        if (task_handle_) xTaskNotifyGive(task_handle_);
    }
}

bool MqttLogClient::WaitForStartupVerification(uint32_t timeout_ms, std::string* reason) {
    if (!startup_verification_required_.load()) return true;
    if (startup_verification_succeeded_.load()) return true;
    if (!mqtt_events_ || startup_verification_failed_.load()) {
        std::lock_guard<std::mutex> lock(startup_verification_mutex_);
        if (reason) *reason = startup_verification_reason_;
        return false;
    }
    const EventBits_t bits = xEventGroupWaitBits(
        mqtt_events_, kStartupVerificationSucceeded | kStartupVerificationFailed,
        pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    if ((bits & kStartupVerificationSucceeded) != 0 || startup_verification_succeeded_.load()) {
        return true;
    }
    if (!(bits & kStartupVerificationFailed)) {
        SetStartupVerificationFailed("MQTT credentials or broker connection verification timed out");
    }
    std::lock_guard<std::mutex> lock(startup_verification_mutex_);
    if (reason) *reason = startup_verification_reason_;
    return false;
}

std::string MqttLogClient::GetLastDiscoveredBrokerHost() const {
    std::lock_guard<std::mutex> lock(discovered_broker_mutex_);
    return last_discovered_broker_host_;
}

void MqttLogClient::TaskEntry(void* arg) {
    static_cast<MqttLogClient*>(arg)->TaskLoop();
}

void MqttLogClient::TaskLoop() {
    LogRecord pending{};
    bool has_pending = false;
    while (true) {
        if (network_disconnected_.exchange(false)) {
            network_ready_.store(false);
            ResetMqtt();
            active_broker_ = fallback_broker_;
            using_discovered_broker_ = false;
            {
                std::lock_guard<std::mutex> lock(discovered_broker_mutex_);
                last_discovered_broker_host_.clear();
            }
            next_retry_at_ms_ = 0;
        }
        if (discovery_requested_.exchange(false)) {
            DiscoverBroker();
            // Discovery is requested only from the Wi-Fi-connected callback. Whether it
            // finds a broker or falls back, lwIP is now safe for the background client.
            network_ready_.store(true);
            ResetMqtt();
            next_retry_at_ms_ = 0;
        }
        if (!has_pending && xQueueReceive(queue_, &pending, pdMS_TO_TICKS(250)) == pdPASS) {
            has_pending = true;
        }
        if (!has_pending) continue;
        if (!network_ready_.load()) {
            // Startup records wait here until the Wi-Fi callback marks lwIP ready.
            // Yielding is essential: this task shares CPU1 with the idle watchdog.
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        const int64_t now_ms = esp_timer_get_time() / 1000;
        if (now_ms < next_retry_at_ms_) {
            vTaskDelay(pdMS_TO_TICKS(std::min<int64_t>(next_retry_at_ms_ - now_ms, 250)));
            continue;
        }
        if (!EnsureConnected()) {
            mqtt_failure_pending_.store(true);
            if (startup_verification_required_.load() && !startup_verification_succeeded_.load()) {
                SetStartupVerificationFailed("MQTT username/password or broker connection verification failed");
            }
            // A syntactically valid discovery response can still name a stale or
            // temporarily unreachable broker. Do not let it block the configured
            // manual fallback indefinitely.
            if (using_discovered_broker_) {
                ESP_LOGW(TAG, "Discovered broker unreachable; falling back to manual broker %s:%d",
                    fallback_broker_.host.c_str(), fallback_broker_.port);
                ResetMqtt();
                active_broker_ = fallback_broker_;
                using_discovered_broker_ = false;
            }
            ScheduleRetry();
            continue;
        }
        if (mqtt_failure_pending_.exchange(false)) {
            Log("mqtt_failure", "ERROR", "Log MQTT connection failed and was retried");
            Log("mqtt_reconnected", "INFO", "Log MQTT connection recovered");
        } else if (!mqtt_connected_once_) {
            mqtt_connected_once_ = true;
            Log("mqtt_connected", "INFO", "Log MQTT connected");
        }
        SetStartupVerificationSucceeded();
        if (Publish(pending)) {
            has_pending = false;
            retry_delay_ms_ = retry_min_ms_;
        } else {
            mqtt_failure_pending_.store(true);
            ResetMqtt();
            ScheduleRetry();
        }
    }
}

void MqttLogClient::SetStartupVerificationFailed(const char* reason) {
    if (!startup_verification_required_.load() && started_) return;
    bool expected = false;
    if (!startup_verification_failed_.compare_exchange_strong(expected, true)) return;
    {
        std::lock_guard<std::mutex> lock(startup_verification_mutex_);
        startup_verification_reason_ = reason ? reason : "MQTT verification failed";
    }
    if (mqtt_events_) xEventGroupSetBits(mqtt_events_, kStartupVerificationFailed);
}

void MqttLogClient::SetStartupVerificationSucceeded() {
    if (!startup_verification_required_.load() || startup_verification_failed_.load()) return;
    bool expected = false;
    if (!startup_verification_succeeded_.compare_exchange_strong(expected, true)) return;
    if (mqtt_events_) xEventGroupSetBits(mqtt_events_, kStartupVerificationSucceeded);
}

bool MqttLogClient::DiscoverBroker() {
#if !CONFIG_AIOT_MQTT_LOG_DISCOVERY_ENABLED
    return false;
#else
    active_broker_ = fallback_broker_;
    using_discovered_broker_ = false;
    for (int attempt = 0; attempt < discovery_retries_; ++attempt) {
        char nonce[17];
        std::snprintf(nonce, sizeof(nonce), "%08lx%08lx", static_cast<unsigned long>(esp_random()), static_cast<unsigned long>(esp_random()));
        cJSON* request = cJSON_CreateObject();
        cJSON_AddStringToObject(request, "protocol", kDiscoveryProtocol);
        cJSON_AddStringToObject(request, "deviceCode", device_code_.c_str());
        uint8_t mac[6]{};
        char mac_text[18] = "unknown";
        if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
            std::snprintf(mac_text, sizeof(mac_text), "%02X:%02X:%02X:%02X:%02X:%02X",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        }
        cJSON_AddStringToObject(request, "mac", mac_text);
        cJSON_AddStringToObject(request, "nonce", nonce);
        cJSON_AddStringToObject(request, "token", discovery_token_.c_str());
        char* json = cJSON_PrintUnformatted(request);
        cJSON_Delete(request);
        if (json == nullptr) continue;

        int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
        if (sock < 0) { cJSON_free(json); continue; }
        int broadcast = 1;
        setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));
        sockaddr_in destination{};
        destination.sin_family = AF_INET;
        destination.sin_port = htons(discovery_port_);
        destination.sin_addr.s_addr = inet_addr("255.255.255.255");
        sendto(sock, json, std::strlen(json), 0, reinterpret_cast<sockaddr*>(&destination), sizeof(destination));
        cJSON_free(json);

        timeval timeout{};
        timeout.tv_sec = discovery_timeout_ms_ / 1000;
        timeout.tv_usec = (discovery_timeout_ms_ % 1000) * 1000;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        char response[512]{};
        sockaddr_in source{};
        socklen_t source_len = sizeof(source);
        int received = recvfrom(sock, response, sizeof(response) - 1, 0, reinterpret_cast<sockaddr*>(&source), &source_len);
        char source_ip[INET_ADDRSTRLEN]{};
        inet_ntop(AF_INET, &source.sin_addr, source_ip, sizeof(source_ip));
        close(sock);
        Broker discovered{};
        if (received > 0 && IsValidDiscoveryResponse(response, nonce, source_ip, &discovered)) {
            active_broker_ = discovered;
            using_discovered_broker_ = true;
            {
                std::lock_guard<std::mutex> lock(discovered_broker_mutex_);
                last_discovered_broker_host_ = discovered.host;
            }
            ESP_LOGI(TAG, "Discovered log broker %s:%d TLS=%d", active_broker_.host.c_str(), active_broker_.port, active_broker_.tls);
            return true;
        }
    }
    ESP_LOGI(TAG, "No LAN log broker discovered; using manual fallback");
    return false;
#endif
}

bool MqttLogClient::IsValidDiscoveryResponse(const char* payload, const std::string& nonce, const char* source_ip, Broker* broker) const {
    if (!IsPrivateIpv4(source_ip)) return false;
    cJSON* root = cJSON_Parse(payload);
    if (!root) return false;
    auto protocol = cJSON_GetObjectItem(root, "protocol");
    auto response_nonce = cJSON_GetObjectItem(root, "nonce");
    auto host = cJSON_GetObjectItem(root, "host");
    auto port = cJSON_GetObjectItem(root, "port");
    auto tls = cJSON_GetObjectItem(root, "tls");
    auto token = cJSON_GetObjectItem(root, "token");
    const bool valid = cJSON_IsString(protocol) && std::strcmp(protocol->valuestring, kDiscoveryProtocol) == 0 &&
        cJSON_IsString(response_nonce) && nonce == response_nonce->valuestring &&
        cJSON_IsString(host) && IsPrivateIpv4(host->valuestring) &&
        cJSON_IsNumber(port) && port->valueint > 0 && port->valueint <= 65535 && cJSON_IsBool(tls) &&
        (discovery_token_.empty() || (cJSON_IsString(token) && discovery_token_ == token->valuestring));
    if (valid) {
        broker->host = host->valuestring;
        broker->port = port->valueint;
        broker->tls = cJSON_IsTrue(tls);
    }
    cJSON_Delete(root);
    return valid;
}

bool MqttLogClient::EnsureConnected() {
    if (active_broker_.host.empty()) return false;
    if (mqtt_client_ && connected_.load()) return true;
    ResetMqtt();
    esp_mqtt_client_config_t config{};
    config.task.stack_size = 4096;
    config.broker.address.hostname = active_broker_.host.c_str();
    config.broker.address.port = active_broker_.port;
    config.broker.address.transport = active_broker_.tls ? MQTT_TRANSPORT_OVER_SSL : MQTT_TRANSPORT_OVER_TCP;
    if (active_broker_.tls) config.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
    config.credentials.client_id = client_id_.c_str();
    config.credentials.username = username_.c_str();
    config.credentials.authentication.password = password_.c_str();
    config.session.keepalive = keepalive_seconds_;
    mqtt_client_ = esp_mqtt_client_init(&config);
    if (!mqtt_client_) { mqtt_failure_pending_.store(true); return false; }
    xEventGroupClearBits(mqtt_events_, kMqttConnected | kMqttFailed);
    esp_mqtt_client_register_event(mqtt_client_, MQTT_EVENT_ANY, MqttEventHandler, this);
    if (esp_mqtt_client_start(mqtt_client_) != ESP_OK) { mqtt_failure_pending_.store(true); ResetMqtt(); return false; }
    EventBits_t bits = xEventGroupWaitBits(mqtt_events_, kMqttConnected | kMqttFailed, pdTRUE, pdFALSE, pdMS_TO_TICKS(10000));
    return (bits & kMqttConnected) != 0;
}

void MqttLogClient::ResetMqtt() {
    connected_.store(false);
    if (mqtt_client_) {
        esp_mqtt_client_stop(mqtt_client_);
        esp_mqtt_client_destroy(mqtt_client_);
        mqtt_client_ = nullptr;
    }
}

void MqttLogClient::MqttEventHandler(void* handler_args, esp_event_base_t, int32_t event_id, void*) {
    auto* self = static_cast<MqttLogClient*>(handler_args);
    if (event_id == MQTT_EVENT_CONNECTED) {
        self->connected_.store(true);
        xEventGroupSetBits(self->mqtt_events_, kMqttConnected);
    } else if (event_id == MQTT_EVENT_ERROR || event_id == MQTT_EVENT_DISCONNECTED) {
        self->connected_.store(false);
        self->mqtt_failure_pending_.store(true);
        xEventGroupSetBits(self->mqtt_events_, kMqttFailed);
    }
}

bool MqttLogClient::Publish(const LogRecord& record) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "deviceCode", device_code_.c_str());
    if (record.is_event_log) {
        cJSON_AddStringToObject(root, "eventType", record.event_type);
        cJSON_AddStringToObject(root, "level", record.level);
    } else {
        cJSON_AddStringToObject(root, "status", record.status);
    }
    cJSON_AddStringToObject(root, "message", record.message);
    // Before SNTP/OTA time synchronization, omit reportedAt so the IOT backend
    // can safely assign its receipt time instead of receiving a 1970 timestamp.
    if (record.reported_at[0] != '\0') {
        cJSON_AddStringToObject(root, "reportedAt", record.reported_at);
    }
    char* json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json || !mqtt_client_) { if (json) cJSON_free(json); return false; }
    const std::string& topic = record.is_event_log ? log_topic_ : report_topic_;
    int id = esp_mqtt_client_publish(mqtt_client_, topic.c_str(), json, 0, 1, 0);
    cJSON_free(json);
    if (id > 0) {
        ESP_LOGI(TAG, "Published %s to %s", record.is_event_log ? record.event_type : record.status, topic.c_str());
    } else {
        ESP_LOGW(TAG, "Failed to queue publish to %s", topic.c_str());
    }
    return id > 0;
}

void MqttLogClient::ScheduleRetry() {
    next_retry_at_ms_ = esp_timer_get_time() / 1000 + retry_delay_ms_;
    retry_delay_ms_ = std::min(retry_delay_ms_ * 2U, retry_max_ms_);
}

std::string MqttLogClient::BuildDeviceCode() const {
    std::string configured = CONFIG_AIOT_MQTT_LOG_DEVICE_CODE;
    if (!configured.empty()) return configured;
    uint8_t mac[6]{};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        char code[32];
        std::snprintf(code, sizeof(code), "xiaozhi-%02X%02X%02X", mac[3], mac[4], mac[5]);
        return code;
    }
    return "xiaozhi-unknown";
}

std::string MqttLogClient::BuildClientId() const {
#ifdef CONFIG_AIOT_MQTT_LOG_CLIENT_ID
    std::string configured = CONFIG_AIOT_MQTT_LOG_CLIENT_ID;
    if (!configured.empty()) return configured;
#endif
    return "xiaozhi-" + device_code_;
}

std::string MqttLogClient::GetReportedAt() const {
    const std::time_t now = std::time(nullptr);
    if (now >= kValidUnixTime) {
        std::tm utc{};
        gmtime_r(&now, &utc);
        char timestamp[20];
        if (std::strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%S", &utc)) return timestamp;
    }
    return "";
}

bool MqttLogClient::IsPrivateIpv4(const char* ip) {
    in_addr address{};
    if (inet_pton(AF_INET, ip, &address) != 1) return false;
    const uint32_t value = ntohl(address.s_addr);
    const uint8_t first = value >> 24;
    const uint8_t second = (value >> 16) & 0xFF;
    return first == 10 || (first == 172 && second >= 16 && second <= 31) || (first == 192 && second == 168);
}
