#pragma once

#include <string>

struct AiotLogProfile {
    std::string username;
    std::string password;
    std::string discovery_token;
    std::string host;
    int port = 1883;
    bool tls = false;
    bool has_password = false;
    bool has_discovery_token = false;
};

// Runtime configuration for the optional, independent AIoT MQTT client. LAN
// and remote settings are kept in separate profiles so switching modes never
// overwrites the other profile. The namespace remains isolated from Wi-Fi,
// official MQTT, OTA and WebSocket settings.
struct AiotLogConfig {
    bool enabled = false;
    bool remote_mode = false;
    AiotLogProfile lan;
    AiotLogProfile remote;

    const AiotLogProfile& ActiveProfile() const {
        return remote_mode ? remote : lan;
    }
};

struct AiotLogProfilePatch {
    std::string username;
    std::string host;
    int port = 1883;
    bool update_password = false;
    std::string password;
    bool clear_password = false;
    bool update_discovery_token = false;
    std::string discovery_token;
    bool clear_discovery_token = false;
};

struct AiotLogConfigPatch {
    bool enabled = false;
    bool remote_mode = false;
    AiotLogProfilePatch lan;
    AiotLogProfilePatch remote;
};

class AiotLogConfigStore {
public:
    static AiotLogConfig Load();
    // Accepts a full DNS hostname only. IP literals, URLs, ports and malformed
    // DNS labels are rejected before ESP-MQTT can attempt a remote connection.
    static bool IsValidRemoteHostname(const std::string& host);
    // Validates sizes and port range, writes an atomic NVS update, and never
    // logs secrets. Returns false and puts a user-safe message in error.
    static bool Save(const AiotLogConfigPatch& patch, std::string* error = nullptr);
};
