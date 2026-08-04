#include "mqtt_log_client.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

#include <cJSON.h>
#include <aiot_log_config.h>
#include <esp_crt_bundle.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <lwip/inet.h>
#include <lwip/sockets.h>
#include <mbedtls/private_access.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509.h>
#include <mbedtls/x509_crt.h>

#include "application.h"
#include "board.h"

namespace {
constexpr const char* TAG = "MqttLog";
constexpr const char* kDiscoveryProtocol = "aiot-mqtt-discovery-v1";
constexpr std::time_t kValidUnixTime = 1700000000;

using CertificateVerifyCallback = int (*)(void*, mbedtls_x509_crt*, int, uint32_t*);

struct BundleVerifyDelegate {
    CertificateVerifyCallback callback = nullptr;
    void* context = nullptr;
};

BundleVerifyDelegate remote_bundle_verify_delegate;

bool GetTrustedUtcTime(mbedtls_x509_time* current) {
    if (current == nullptr) return false;
    const std::time_t now = std::time(nullptr);
    if (now < kValidUnixTime) return false;
    std::tm utc{};
    if (gmtime_r(&now, &utc) == nullptr) return false;
    current->year = utc.tm_year + 1900;
    current->mon = utc.tm_mon + 1;
    current->day = utc.tm_mday;
    current->hour = utc.tm_hour;
    current->min = utc.tm_min;
    current->sec = utc.tm_sec;
    return true;
}

int RemoteCertificateVerify(void*, mbedtls_x509_crt* certificate, int depth, uint32_t* flags) {
    if (remote_bundle_verify_delegate.callback == nullptr || certificate == nullptr || flags == nullptr) {
        return MBEDTLS_ERR_X509_FATAL_ERROR;
    }
    const int result = remote_bundle_verify_delegate.callback(
        remote_bundle_verify_delegate.context, certificate, depth, flags);
    if (result != 0) return result;
    mbedtls_x509_time current{};
    if (!GetTrustedUtcTime(&current)) {
        *flags |= MBEDTLS_X509_BADCERT_FUTURE;
        return 0;
    }
    if (mbedtls_x509_time_cmp(&current, &certificate->valid_from) < 0) {
        *flags |= MBEDTLS_X509_BADCERT_FUTURE;
    }
    if (mbedtls_x509_time_cmp(&current, &certificate->valid_to) > 0) {
        *flags |= MBEDTLS_X509_BADCERT_EXPIRED;
    }
    return 0;
}

esp_err_t AttachRemoteCertificateBundle(void* configuration) {
    if (configuration == nullptr) return ESP_ERR_INVALID_ARG;
    const esp_err_t result = esp_crt_bundle_attach(configuration);
    if (result != ESP_OK) return result;
    auto* ssl_configuration = static_cast<mbedtls_ssl_config*>(configuration);
    remote_bundle_verify_delegate.callback = ssl_configuration->MBEDTLS_PRIVATE(f_vrfy);
    remote_bundle_verify_delegate.context = ssl_configuration->MBEDTLS_PRIVATE(p_vrfy);
    if (remote_bundle_verify_delegate.callback == nullptr) return ESP_FAIL;
    mbedtls_ssl_conf_verify(ssl_configuration, RemoteCertificateVerify, nullptr);
    return ESP_OK;
}

bool IsAcceptedStatus(const std::string& status) {
    return status == "NORMAL" || status == "ABNORMAL" ||
           status == "OFFLINE" || status == "MAINTENANCE";
}

bool IsAcceptedLevel(const std::string& level) {
    return level == "INFO" || level == "WARNING" || level == "ERROR";
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
    startup_verification_required_.store(false);
    startup_verification_succeeded_.store(false);
    startup_verification_failed_.store(false);
    {
        std::lock_guard<std::mutex> lock(startup_verification_mutex_);
        startup_verification_reason_.clear();
    }
    // This is an optional, independent logging feature. A compiled client is inert until
    // its captive-portal switch is enabled in the independent aiot_log NVS
    // namespace. It must not inherit any old build-time host or credentials.
    const AiotLogConfig runtime_config = AiotLogConfigStore::Load();
    if (!runtime_config.enabled) {
        ESP_LOGI(TAG, "Optional AIoT logging is disabled");
        return;
    }
    remote_mode_ = runtime_config.remote_mode;
    // Preserve the existing LAN startup gate. The new remote profile is
    // deliberately best-effort: DNS, TLS or credential failures must never
    // prevent the official XiaoZhi services from starting.
    startup_verification_required_.store(!remote_mode_);
    const AiotLogProfile& active_profile = runtime_config.ActiveProfile();
    if (active_profile.username.empty() || active_profile.password.empty()) {
        if (!remote_mode_) {
            SetStartupVerificationFailed(
                "MQTT username and password are required when local logging is enabled");
        }
        ESP_LOGW(TAG, "%s AIoT logging enabled without MQTT credentials; reporting disabled",
            remote_mode_ ? "Remote" : "LAN");
        return;
    }
    if (remote_mode_ && !AiotLogConfigStore::IsValidRemoteHostname(active_profile.host)) {
        ESP_LOGW(TAG, "Remote AIoT hostname is invalid; reporting disabled");
        return;
    }
    fallback_broker_.host = active_profile.host;
    fallback_broker_.port = active_profile.port;
    fallback_broker_.tls = active_profile.tls;
    active_broker_ = fallback_broker_;
    username_ = active_profile.username;
    password_ = active_profile.password;
    keepalive_seconds_ = CONFIG_AIOT_MQTT_LOG_KEEPALIVE;
    retry_min_ms_ = CONFIG_AIOT_MQTT_LOG_RETRY_MIN_SECONDS * 1000U;
    retry_delay_ms_ = retry_min_ms_;
    retry_max_ms_ = CONFIG_AIOT_MQTT_LOG_RETRY_MAX_SECONDS * 1000U;
    device_code_ = BuildDeviceCode();
    client_id_ = BuildClientId();
    report_topic_ = "aiot/device/" + device_code_ + "/report";
    log_topic_ = "aiot/device/" + device_code_ + "/log";
    environment_sensor_device_code_ = runtime_config.environment_sensor_device_code;
    if (!environment_sensor_device_code_.empty()) {
        environment_report_topic_ = "aiot/device/" + environment_sensor_device_code_ + "/report";
    }
#if CONFIG_AIOT_MQTT_LOG_DISCOVERY_ENABLED
    discovery_port_ = CONFIG_AIOT_MQTT_LOG_DISCOVERY_PORT;
    discovery_timeout_ms_ = CONFIG_AIOT_MQTT_LOG_DISCOVERY_TIMEOUT_MS;
    discovery_retries_ = CONFIG_AIOT_MQTT_LOG_DISCOVERY_RETRIES;
    discovery_token_ = runtime_config.lan.discovery_token;
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
    // Keep the wire format aligned with the IoT API enum. Accept legacy internal
    // callers using WARN, but always publish the canonical WARNING value.
    const std::string normalized_level = level == "WARN" ? "WARNING" : level;
    std::snprintf(record.level, sizeof(record.level), "%s",
        IsAcceptedLevel(normalized_level) ? normalized_level.c_str() : "ERROR");
    CopyUtf8(record.message, sizeof(record.message), message);
    std::snprintf(record.reported_at, sizeof(record.reported_at), "%s", GetReportedAt().c_str());
    if (xQueueSend(queue_, &record, 0) != pdPASS) {
        ESP_LOGW(TAG, "MQTT log queue full; dropping event %s", record.event_type);
    }
}

void MqttLogClient::RequestDiscovery() {
    if (started_) {
        network_transition_.store(true);
        discovery_requested_.store(true);
        if (task_handle_) xTaskNotifyGive(task_handle_);
    }
}

void MqttLogClient::NotifyNetworkDisconnected() {
    if (started_) {
        // Set this in the Wi-Fi callback instead of waiting for the worker.
        // ESP-MQTT may emit DISCONNECTED first; it is expected during a network
        // switch and must not be counted as a broker failure.
        network_transition_.store(true);
        network_ready_.store(false);
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
            mqtt_failure_pending_.store(false);
            mqtt_failure_reportable_.store(false);
            ResetConnectionFailureTracking();
            ResetMqtt();
            active_broker_ = fallback_broker_;
            using_discovered_broker_ = false;
            {
                std::lock_guard<std::mutex> lock(discovered_broker_mutex_);
                last_discovered_broker_host_.clear();
            }
            next_retry_at_ms_ = 0;
            retry_delay_ms_ = retry_min_ms_;
        }
        if (discovery_requested_.exchange(false)) {
            if (remote_mode_) {
                active_broker_ = fallback_broker_;
                using_discovered_broker_ = false;
                std::lock_guard<std::mutex> lock(discovered_broker_mutex_);
                last_discovered_broker_host_.clear();
            } else {
                DiscoverBroker();
            }
            // This request comes from the Wi-Fi-connected callback. In remote mode it
            // intentionally skips UDP discovery; either way lwIP is now safe for MQTT.
            ResetMqtt();
            mqtt_failure_pending_.store(false);
            mqtt_failure_reportable_.store(false);
            ResetConnectionFailureTracking();
            retry_delay_ms_ = retry_min_ms_;
            next_retry_at_ms_ = 0;
            network_ready_.store(true);
            network_transition_.store(false);
        }
        if (!has_pending && xQueueReceive(queue_, &pending, pdMS_TO_TICKS(250)) == pdPASS) {
            has_pending = true;
        }
        if (!network_ready_.load()) {
            // Startup records wait here until the Wi-Fi callback marks lwIP ready.
            // Yielding is essential: this task shares CPU1 with the idle watchdog.
            if (has_pending) vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        // An unexpected broker disconnect must reconnect even when no application
        // record is waiting. The recovered/failure events are queued after the
        // connection is restored and are published by the same low-priority task.
        if (!has_pending && !mqtt_failure_pending_.load()) continue;
        const int64_t now_ms = esp_timer_get_time() / 1000;
        if (now_ms < next_retry_at_ms_) {
            vTaskDelay(pdMS_TO_TICKS(std::min<int64_t>(next_retry_at_ms_ - now_ms, 250)));
            continue;
        }
        const ConnectionAttemptResult connection_result = EnsureConnected();
        if (connection_result != ConnectionAttemptResult::Connected) {
            if (connection_result == ConnectionAttemptResult::Failed) {
                mqtt_failure_pending_.store(true);
                RecordConnectionFailure(now_ms);
            }
            if (startup_verification_required_.load() && !startup_verification_succeeded_.load()) {
                if (connection_result == ConnectionAttemptResult::Failed) {
                    SetStartupVerificationFailed("MQTT username/password or broker connection verification failed");
                }
            }
            // A syntactically valid discovery response can still name a stale or
            // temporarily unreachable broker. Do not let it block the configured
            // manual fallback indefinitely.
            if (connection_result == ConnectionAttemptResult::Failed && using_discovered_broker_) {
                ESP_LOGW(TAG, "Discovered broker unreachable; falling back to manual broker %s:%d",
                    fallback_broker_.host.c_str(), fallback_broker_.port);
                ResetMqtt();
                active_broker_ = fallback_broker_;
                using_discovered_broker_ = false;
            }
            if (connection_result == ConnectionAttemptResult::Deferred) {
                ScheduleDeferredRetry();
            } else {
                ScheduleRetry();
            }
            continue;
        }
        mqtt_failure_pending_.store(false);
        if (mqtt_failure_reportable_.exchange(false)) {
            mqtt_connected_once_ = true;
            Log("mqtt_connection_failed", "ERROR", remote_mode_
                ? "Remote log MQTT TLS connection failed and was retried"
                : "Log MQTT connection failed and was retried");
            Log("mqtt_reconnected", "INFO", remote_mode_
                ? "Remote log MQTT TLS connection recovered"
                : "Log MQTT connection recovered");
        } else if (!mqtt_connected_once_) {
            mqtt_connected_once_ = true;
            Log("mqtt_connected", "INFO", remote_mode_
                ? "Remote log MQTT TLS connected"
                : "Log MQTT connected");
        }
        ResetConnectionFailureTracking();
        SetStartupVerificationSucceeded();
        retry_delay_ms_ = retry_min_ms_;
        if (!has_pending) continue;
        if (Publish(pending)) {
            has_pending = false;
        } else {
            mqtt_failure_pending_.store(true);
            RecordConnectionFailure(esp_timer_get_time() / 1000);
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
    if (remote_mode_) return false;
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

MqttLogClient::ConnectionAttemptResult MqttLogClient::EnsureConnected() {
    if (active_broker_.host.empty()) return ConnectionAttemptResult::Failed;
    if (mqtt_client_ && connected_.load()) return ConnectionAttemptResult::Connected;
    ResetMqtt();
    if (remote_mode_) {
        mbedtls_x509_time current{};
        if (!GetTrustedUtcTime(&current)) {
            if (!waiting_for_trusted_time_logged_) {
                ESP_LOGW(TAG, "Remote AIoT TLS is waiting for trusted system time");
                waiting_for_trusted_time_logged_ = true;
            }
            return ConnectionAttemptResult::Deferred;
        }
        if (waiting_for_trusted_time_logged_) {
            ESP_LOGI(TAG, "Trusted system time is available for remote AIoT TLS");
            waiting_for_trusted_time_logged_ = false;
        }
    }
    esp_mqtt_client_config_t config{};
    config.task.stack_size = 4096;
    config.broker.address.hostname = active_broker_.host.c_str();
    config.broker.address.port = active_broker_.port;
    config.broker.address.transport = active_broker_.tls ? MQTT_TRANSPORT_OVER_SSL : MQTT_TRANSPORT_OVER_TCP;
    if (remote_mode_) {
        if (!active_broker_.tls) return ConnectionAttemptResult::Failed;
        config.broker.verification.crt_bundle_attach = AttachRemoteCertificateBundle;
    } else if (active_broker_.tls) {
        config.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
    }
    if (active_broker_.tls) {
        config.broker.verification.skip_cert_common_name_check = false;
        // Keep the hostname as the verification target so ESP-IDF supplies the
        // correct TLS SNI name for a HiveMQ Cloud multi-tenant endpoint.
        config.broker.verification.common_name = active_broker_.host.c_str();
    }
    config.credentials.client_id = client_id_.c_str();
    config.credentials.username = username_.c_str();
    config.credentials.authentication.password = password_.c_str();
    config.session.keepalive = keepalive_seconds_;
    mqtt_client_ = esp_mqtt_client_init(&config);
    if (!mqtt_client_) return ConnectionAttemptResult::Failed;
    xEventGroupClearBits(mqtt_events_, kMqttConnected | kMqttFailed);
    esp_mqtt_client_register_event(mqtt_client_, MQTT_EVENT_ANY, MqttEventHandler, this);
    if (esp_mqtt_client_start(mqtt_client_) != ESP_OK) {
        ResetMqtt();
        return ConnectionAttemptResult::Failed;
    }
    EventBits_t bits = xEventGroupWaitBits(mqtt_events_, kMqttConnected | kMqttFailed, pdTRUE, pdFALSE, pdMS_TO_TICKS(10000));
    return (bits & kMqttConnected) != 0
        ? ConnectionAttemptResult::Connected
        : ConnectionAttemptResult::Failed;
}

void MqttLogClient::ResetMqtt() {
    connected_.store(false);
    if (mqtt_client_) {
        mqtt_stop_expected_.store(true);
        esp_mqtt_client_stop(mqtt_client_);
        esp_mqtt_client_destroy(mqtt_client_);
        mqtt_client_ = nullptr;
        mqtt_stop_expected_.store(false);
    }
}

void MqttLogClient::MqttEventHandler(void* handler_args, esp_event_base_t, int32_t event_id, void* event_data) {
    auto* self = static_cast<MqttLogClient*>(handler_args);
    auto* event = static_cast<esp_mqtt_event_handle_t>(event_data);
    if (event != nullptr && self->mqtt_client_ != nullptr && event->client != self->mqtt_client_) {
        return;
    }
    if (event_id == MQTT_EVENT_CONNECTED) {
        self->connected_.store(true);
        if (!self->environment_report_topic_.empty()) {
            const int message_id = esp_mqtt_client_subscribe(event->client, self->environment_report_topic_.c_str(), 1);
            if (message_id < 0) ESP_LOGW(TAG, "Environment display subscription could not be queued");
            else ESP_LOGI(TAG, "Environment display subscription queued");
        }
        xEventGroupSetBits(self->mqtt_events_, kMqttConnected);
    } else if (event_id == MQTT_EVENT_DATA) {
        if (event->current_data_offset == 0 && event->data_len == event->total_data_len) {
            self->HandleEnvironmentReport(event->topic, event->topic_len, event->data, event->data_len);
        }
    } else if (event_id == MQTT_EVENT_ERROR || event_id == MQTT_EVENT_DISCONNECTED) {
        self->connected_.store(false);
        if (self->mqtt_stop_expected_.load() ||
            self->network_transition_.load() ||
            !self->network_ready_.load()) {
            return;
        }
        self->mqtt_failure_pending_.store(true);
        if (!self->remote_mode_) {
            self->mqtt_failure_reportable_.store(true);
        }
        xEventGroupSetBits(self->mqtt_events_, kMqttFailed);
    }
}

void MqttLogClient::HandleEnvironmentReport(const char* topic, int topic_length, const char* payload, int payload_length) {
    if (environment_report_topic_.empty() || topic == nullptr || payload == nullptr || payload_length <= 0 ||
        environment_report_topic_ != std::string(topic, topic_length)) return;
    cJSON* root = cJSON_ParseWithLength(payload, payload_length);
    const cJSON* code = root == nullptr ? nullptr : cJSON_GetObjectItemCaseSensitive(root, "deviceCode");
    const cJSON* temperature = root == nullptr ? nullptr : cJSON_GetObjectItemCaseSensitive(root, "temperature");
    const cJSON* humidity = root == nullptr ? nullptr : cJSON_GetObjectItemCaseSensitive(root, "humidity");
    const cJSON* pressure = root == nullptr ? nullptr : cJSON_GetObjectItemCaseSensitive(root, "pressure");
    const cJSON* illuminance = root == nullptr ? nullptr : cJSON_GetObjectItemCaseSensitive(root, "illuminance");
    const bool valid = cJSON_IsString(code) && environment_sensor_device_code_ == code->valuestring &&
        (cJSON_IsNumber(temperature) || cJSON_IsNumber(humidity) || cJSON_IsNumber(pressure) || cJSON_IsNumber(illuminance));
    if (!valid) {
        if (!environment_parse_failure_logged_) {
            ESP_LOGW(TAG, "Ignored invalid environment report for standby display");
            environment_parse_failure_logged_ = true;
        }
        cJSON_Delete(root); return;
    }
    environment_parse_failure_logged_ = false;
    char temperature_text[20] = "--", humidity_text[20] = "--", pressure_text[20] = "--", illuminance_text[20] = "--";
    if (cJSON_IsNumber(temperature)) std::snprintf(temperature_text, sizeof(temperature_text), "%.1f°C", temperature->valuedouble);
    if (cJSON_IsNumber(humidity)) std::snprintf(humidity_text, sizeof(humidity_text), "%.1f%%", humidity->valuedouble);
    if (cJSON_IsNumber(pressure)) std::snprintf(pressure_text, sizeof(pressure_text), "%.1fhPa", pressure->valuedouble);
    if (cJSON_IsNumber(illuminance)) std::snprintf(illuminance_text, sizeof(illuminance_text), "%.0flux", illuminance->valuedouble);
    char status[128]{};
    std::snprintf(status, sizeof(status), "室内 %s  %s\n%s  %s", temperature_text, humidity_text, pressure_text, illuminance_text);
    {
        std::lock_guard<std::mutex> lock(environment_mutex_);
        environment_status_ = status;
        environment_received_at_us_ = esp_timer_get_time();
    }
    cJSON_Delete(root);
    Application::GetInstance().Schedule([]() { MqttLogClient::GetInstance().RefreshIdleEnvironmentDisplay(); });
}

void MqttLogClient::RefreshIdleEnvironmentDisplay() {
    if (Application::GetInstance().GetDeviceState() != kDeviceStateIdle) return;
    std::string status;
    const int64_t now = esp_timer_get_time();
    {
        std::lock_guard<std::mutex> lock(environment_mutex_);
        if (environment_status_.empty()) return;
        status = environment_status_;
        if (now - environment_received_at_us_ > 180LL * 1000 * 1000) status += "\n室内数据已过期";
        if (now - environment_last_rendered_at_us_ < 10LL * 1000 * 1000) return;
        environment_last_rendered_at_us_ = now;
    }
    Board::GetInstance().GetDisplay()->SetStatus(status.c_str());
}

bool MqttLogClient::Publish(const LogRecord& record) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "deviceCode", device_code_.c_str());
    if (record.is_event_log) {
        cJSON_AddStringToObject(root, "eventType", record.event_type);
        cJSON_AddStringToObject(root, "level", record.level);
        // The backend accepts exactly RUNNING, ERROR, MAINTENANCE or
        // INSPECTION. Operational firmware events map to the first two.
        cJSON_AddStringToObject(root, "logType",
            std::strcmp(record.level, "ERROR") == 0 ? "ERROR" : "RUNNING");
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

void MqttLogClient::ScheduleDeferredRetry() {
    // Waiting for SNTP/OTA time is a prerequisite, not a connection failure.
    // Poll it at a short fixed cadence so a time update is used promptly instead
    // of being delayed by the normal 5/10/20/40 second failure backoff.
    next_retry_at_ms_ = esp_timer_get_time() / 1000 + kDeferredRetryMs;
}

void MqttLogClient::RecordConnectionFailure(int64_t now_ms) {
    if (first_connection_failure_at_ms_ == 0) {
        first_connection_failure_at_ms_ = now_ms;
    }
    ++consecutive_connection_failures_;
    if (!remote_mode_ ||
        consecutive_connection_failures_ >= kRemoteFailureAttemptThreshold ||
        now_ms - first_connection_failure_at_ms_ >= kRemoteFailureGraceMs) {
        mqtt_failure_reportable_.store(true);
    }
}

void MqttLogClient::ResetConnectionFailureTracking() {
    consecutive_connection_failures_ = 0;
    first_connection_failure_at_ms_ = 0;
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
