#include "local_ai_discovery.h"

#include <cstdio>
#include <cerrno>
#include <cstring>

#include <cJSON.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <lwip/inet.h>
#include <lwip/sockets.h>

namespace {
constexpr const char* TAG = "LocalAiDiscovery";
constexpr const char* kProtocol = "xiaozhi-ai-discovery-v1";

bool IsPrivateIpv4(const char* ip) {
    in_addr address{};
    if (inet_pton(AF_INET, ip, &address) != 1) return false;
    const uint32_t value = ntohl(address.s_addr);
    const uint8_t first = value >> 24;
    const uint8_t second = (value >> 16) & 0xFF;
    return first == 10 || (first == 172 && second >= 16 && second <= 31) || (first == 192 && second == 168);
}

bool IsLanHttpUrl(const char* url) {
    if (url == nullptr || std::strncmp(url, "http://", 7) != 0) return false;
    const char* host_start = url + 7;
    const char* host_end = std::strchr(host_start, ':');
    if (host_end == nullptr) host_end = std::strchr(host_start, '/');
    if (host_end == nullptr) host_end = host_start + std::strlen(host_start);
    const size_t length = static_cast<size_t>(host_end - host_start);
    if (length == 0 || length >= INET_ADDRSTRLEN) return false;
    char host[INET_ADDRSTRLEN]{};
    std::memcpy(host, host_start, length);
    return IsPrivateIpv4(host);
}

bool IsLanWebsocketUrl(const char* url) {
    return url != nullptr && std::strncmp(url, "ws://", 5) == 0 && IsLanHttpUrl((std::string("http://") + (url + 5)).c_str());
}
}

bool LocalAiDiscovery::Discover(LocalAiEndpoint* endpoint) const {
#if !CONFIG_LOCAL_AI_DISCOVERY_ENABLED
    return false;
#else
    if (endpoint == nullptr) return false;
    for (int attempt = 0; attempt < CONFIG_LOCAL_AI_DISCOVERY_RETRIES; ++attempt) {
        char nonce[17];
        std::snprintf(nonce, sizeof(nonce), "%08lx%08lx", static_cast<unsigned long>(esp_random()), static_cast<unsigned long>(esp_random()));
        uint8_t mac[6]{};
        char mac_text[18] = "unknown";
        if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
            std::snprintf(mac_text, sizeof(mac_text), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        }
        cJSON* request = cJSON_CreateObject();
        cJSON_AddStringToObject(request, "protocol", kProtocol);
        cJSON_AddStringToObject(request, "mac", mac_text);
        cJSON_AddStringToObject(request, "nonce", nonce);
        cJSON_AddStringToObject(request, "token", CONFIG_LOCAL_AI_DISCOVERY_TOKEN);
        char* json = cJSON_PrintUnformatted(request);
        cJSON_Delete(request);
        if (json == nullptr) continue;
        const int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
        if (sock < 0) { cJSON_free(json); continue; }
        int broadcast = 1;
        setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));
        // Explicitly bind before broadcasting. Although lwIP normally assigns
        // an ephemeral port on sendto(), this activation-time path was sending
        // requests that reached the server while their unicast replies were
        // not delivered back to recvfrom(). A concrete local binding makes the
        // reply endpoint unambiguous and is still fully dynamic.
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = htonl(INADDR_ANY);
        local.sin_port = htons(0);
        if (bind(sock, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) {
            ESP_LOGW(TAG, "Local AI discovery bind failed: errno=%d", errno);
            close(sock);
            cJSON_free(json);
            continue;
        }
        sockaddr_in bound{};
        socklen_t bound_length = sizeof(bound);
        if (getsockname(sock, reinterpret_cast<sockaddr*>(&bound), &bound_length) == 0) {
            ESP_LOGI(TAG, "Local AI discovery using UDP port %u", ntohs(bound.sin_port));
        }
        sockaddr_in destination{};
        destination.sin_family = AF_INET;
        destination.sin_port = htons(CONFIG_LOCAL_AI_DISCOVERY_PORT);
        destination.sin_addr.s_addr = inet_addr("255.255.255.255");
        sendto(sock, json, std::strlen(json), 0, reinterpret_cast<sockaddr*>(&destination), sizeof(destination));
        cJSON_free(json);
        // A broadcast socket can receive unrelated packets before the local
        // responder's unicast reply. Keep reading until the whole configured
        // window expires instead of treating that first packet as the result.
        const int64_t deadline_ms = esp_timer_get_time() / 1000 + CONFIG_LOCAL_AI_DISCOVERY_TIMEOUT_MS;
        while (true) {
            const int64_t remaining_ms = deadline_ms - esp_timer_get_time() / 1000;
            if (remaining_ms <= 0) break;
            timeval timeout{
                .tv_sec = static_cast<long>(remaining_ms / 1000),
                .tv_usec = static_cast<long>((remaining_ms % 1000) * 1000),
            };
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            char response[512]{};
            sockaddr_in source{};
            socklen_t source_len = sizeof(source);
            const int received = recvfrom(sock, response, sizeof(response) - 1, 0,
                reinterpret_cast<sockaddr*>(&source), &source_len);
            if (received <= 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    ESP_LOGW(TAG, "Local AI discovery receive failed: errno=%d", errno);
                }
                break;
            }
            char source_ip[INET_ADDRSTRLEN]{};
            inet_ntop(AF_INET, &source.sin_addr, source_ip, sizeof(source_ip));
            if (!IsPrivateIpv4(source_ip)) {
                ESP_LOGW(TAG, "Ignoring local AI response from non-private source %s", source_ip);
                continue;
            }
            ESP_LOGI(TAG, "Received local AI response (%d bytes) from %s", received, source_ip);
            cJSON* root = cJSON_Parse(response);
            cJSON* protocol = root ? cJSON_GetObjectItem(root, "protocol") : nullptr;
            cJSON* response_nonce = root ? cJSON_GetObjectItem(root, "nonce") : nullptr;
            cJSON* ota_url = root ? cJSON_GetObjectItem(root, "otaUrl") : nullptr;
            cJSON* websocket_url = root ? cJSON_GetObjectItem(root, "websocketUrl") : nullptr;
            cJSON* token = root ? cJSON_GetObjectItem(root, "token") : nullptr;
            const bool protocol_valid = cJSON_IsString(protocol) && std::strcmp(protocol->valuestring, kProtocol) == 0;
            // `nonce` is a char array and decays to a pointer here.  Compare
            // its text rather than pointer addresses, otherwise a correctly
            // echoed response is always rejected.
            const bool nonce_valid = cJSON_IsString(response_nonce) && std::strcmp(nonce, response_nonce->valuestring) == 0;
            const bool ota_valid = cJSON_IsString(ota_url) && IsLanHttpUrl(ota_url->valuestring);
            const bool websocket_valid = cJSON_IsString(websocket_url) && IsLanWebsocketUrl(websocket_url->valuestring);
            const bool token_valid = std::strlen(CONFIG_LOCAL_AI_DISCOVERY_TOKEN) == 0 ||
                (cJSON_IsString(token) && std::strcmp(token->valuestring, CONFIG_LOCAL_AI_DISCOVERY_TOKEN) == 0);
            const bool valid = protocol_valid && nonce_valid && ota_valid && websocket_valid && token_valid;
            if (valid) {
                endpoint->ota_url = ota_url->valuestring;
                endpoint->websocket_url = websocket_url->valuestring;
                cJSON_Delete(root);
                close(sock);
                ESP_LOGI(TAG, "Discovered local AI OTA: %s", endpoint->ota_url.c_str());
                return true;
            }
            ESP_LOGW(TAG, "Rejected local AI response: protocol=%d nonce=%d ota=%d websocket=%d token=%d",
                protocol_valid, nonce_valid, ota_valid, websocket_valid, token_valid);
            if (root) cJSON_Delete(root);
        }
        close(sock);
    }
    // A log-broker address, an old NVS value, or a guessed same-host endpoint
    // is never evidence that the local AI server exists.  Only this boot's
    // nonce-validated UDP reply may return success from this function.
    ESP_LOGI(TAG, "No local AI server discovered; using official OTA");
    return false;
#endif
}
