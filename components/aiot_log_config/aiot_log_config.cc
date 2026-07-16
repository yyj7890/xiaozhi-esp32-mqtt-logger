#include "aiot_log_config.h"

#include <cctype>
#include <nvs.h>

namespace {
constexpr const char* kNamespace = "aiot_log";
constexpr const char* kEnabled = "enabled";
constexpr const char* kMode = "mode";
constexpr const char* kLanUsername = "lan_user";
constexpr const char* kLanPassword = "lan_pass";
constexpr const char* kLanToken = "lan_token";
constexpr const char* kLanHost = "lan_host";
constexpr const char* kLanPort = "lan_port";
constexpr const char* kRemoteUsername = "rem_user";
constexpr const char* kRemotePassword = "rem_pass";
constexpr const char* kRemoteHost = "rem_host";
constexpr const char* kRemotePort = "rem_port";

// Legacy keys are mirrored for downgrade compatibility with the existing LAN
// firmware and are used as a read-only migration source on first boot.
constexpr const char* kLegacyUsername = "username";
constexpr const char* kLegacyPassword = "password";
constexpr const char* kLegacyToken = "disc_token";
constexpr const char* kLegacyHost = "manual_host";
constexpr const char* kLegacyPort = "port";
constexpr const char* kLegacyRemoteMode = "remote_mode";
constexpr const char* kLegacyTls = "tls";
constexpr size_t kUsernameMax = 128;
constexpr size_t kPasswordMax = 256;
constexpr size_t kTokenMax = 128;
constexpr size_t kHostMax = 253;

std::string ReadString(nvs_handle_t handle, const char* key) {
    size_t length = 0;
    if (nvs_get_str(handle, key, nullptr, &length) != ESP_OK || length == 0) return {};
    std::string value(length - 1, '\0');
    if (nvs_get_str(handle, key, value.data(), &length) != ESP_OK) return {};
    return value;
}

bool IsTooLong(const std::string& value, size_t maximum) {
    return value.size() > maximum;
}

bool IsValidRemoteHostnameValue(const std::string& host) {
    if (host.empty() || host.size() > kHostMax || host.front() == '.' ||
        host.back() == '.') {
        return false;
    }
    bool has_dot = false;
    bool has_alpha = false;
    size_t label_length = 0;
    bool previous_was_hyphen = false;
    for (const unsigned char value : host) {
        if (value == '.') {
            if (label_length == 0 || label_length > 63 || previous_was_hyphen) return false;
            has_dot = true;
            label_length = 0;
            previous_was_hyphen = false;
            continue;
        }
        const bool alpha = std::isalpha(value) != 0;
        const bool digit = std::isdigit(value) != 0;
        if (!alpha && !digit && value != '-') return false;
        if (label_length == 0 && value == '-') return false;
        has_alpha = has_alpha || alpha;
        previous_was_hyphen = value == '-';
        ++label_length;
    }
    return has_dot && has_alpha && label_length > 0 && label_length <= 63 &&
        !previous_was_hyphen;
}

esp_err_t EraseOptional(nvs_handle_t handle, const char* key) {
    const esp_err_t result = nvs_erase_key(handle, key);
    return result == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : result;
}

bool HasString(nvs_handle_t handle, const char* key) {
    size_t length = 0;
    return nvs_get_str(handle, key, nullptr, &length) == ESP_OK;
}

bool HasProfile(nvs_handle_t handle, const char* username_key, const char* password_key,
                const char* host_key, const char* port_key) {
    uint16_t port = 0;
    return HasString(handle, username_key) || HasString(handle, password_key) ||
        HasString(handle, host_key) || nvs_get_u16(handle, port_key, &port) == ESP_OK;
}

void LoadProfile(nvs_handle_t handle, AiotLogProfile* profile,
                 const char* username_key, const char* password_key,
                 const char* host_key, const char* port_key,
                 const char* token_key = nullptr) {
    profile->username = ReadString(handle, username_key);
    profile->password = ReadString(handle, password_key);
    profile->host = ReadString(handle, host_key);
    profile->has_password = !profile->password.empty();
    if (token_key != nullptr) {
        profile->discovery_token = ReadString(handle, token_key);
        profile->has_discovery_token = !profile->discovery_token.empty();
    }
    uint16_t port = static_cast<uint16_t>(profile->port);
    if (nvs_get_u16(handle, port_key, &port) == ESP_OK && port > 0) {
        profile->port = port;
    }
}

esp_err_t ApplySecret(nvs_handle_t handle, const char* key,
                      const AiotLogProfilePatch& patch) {
    if (patch.clear_password) return EraseOptional(handle, key);
    if (patch.update_password) return nvs_set_str(handle, key, patch.password.c_str());
    return ESP_OK;
}

esp_err_t ApplyToken(nvs_handle_t handle, const char* key,
                     const AiotLogProfilePatch& patch) {
    if (patch.clear_discovery_token) return EraseOptional(handle, key);
    if (patch.update_discovery_token) {
        return nvs_set_str(handle, key, patch.discovery_token.c_str());
    }
    return ESP_OK;
}

bool HasEffectivePassword(const AiotLogProfilePatch& patch,
                          const AiotLogProfile& current) {
    if (patch.clear_password) return false;
    if (patch.update_password) return !patch.password.empty();
    return current.has_password;
}

void SetError(std::string* error, const char* message) {
    if (error) *error = message;
}
}  // namespace

AiotLogConfig AiotLogConfigStore::Load() {
    AiotLogConfig config;
    config.lan.port = 1883;
    config.lan.tls = false;
    config.remote.port = 8883;
    config.remote.tls = true;
    nvs_handle_t handle;
    if (nvs_open(kNamespace, NVS_READONLY, &handle) != ESP_OK) return config;

    uint8_t enabled = 0;
    uint8_t mode = 0;
    uint8_t legacy_remote_mode = 0;
    nvs_get_u8(handle, kEnabled, &enabled);
    config.enabled = enabled != 0;
    if (nvs_get_u8(handle, kMode, &mode) == ESP_OK) {
        config.remote_mode = mode == 1;
    } else {
        nvs_get_u8(handle, kLegacyRemoteMode, &legacy_remote_mode);
        config.remote_mode = legacy_remote_mode != 0;
    }

    const bool has_lan_profile = HasProfile(
        handle, kLanUsername, kLanPassword, kLanHost, kLanPort);
    const bool has_remote_profile = HasProfile(
        handle, kRemoteUsername, kRemotePassword, kRemoteHost, kRemotePort);
    LoadProfile(handle, &config.lan, kLanUsername, kLanPassword,
                kLanHost, kLanPort, kLanToken);
    LoadProfile(handle, &config.remote, kRemoteUsername, kRemotePassword,
                kRemoteHost, kRemotePort);
    config.remote.port = 8883;

    // Existing devices have one shared profile. Treat it as the currently
    // selected profile until the user saves the new two-profile form.
    if ((!config.remote_mode && !has_lan_profile) ||
        (config.remote_mode && !has_remote_profile)) {
        AiotLogProfile* target = config.remote_mode ? &config.remote : &config.lan;
        target->username = ReadString(handle, kLegacyUsername);
        target->password = ReadString(handle, kLegacyPassword);
        target->host = ReadString(handle, kLegacyHost);
        target->has_password = !target->password.empty();
        uint16_t legacy_port = static_cast<uint16_t>(target->port);
        if (nvs_get_u16(handle, kLegacyPort, &legacy_port) == ESP_OK && legacy_port > 0) {
            target->port = legacy_port;
        }
        if (!config.remote_mode) {
            target->discovery_token = ReadString(handle, kLegacyToken);
            target->has_discovery_token = !target->discovery_token.empty();
        } else {
            target->port = 8883;
        }
    }
    nvs_close(handle);
    return config;
}

bool AiotLogConfigStore::IsValidRemoteHostname(const std::string& host) {
    return IsValidRemoteHostnameValue(host);
}

bool AiotLogConfigStore::Save(const AiotLogConfigPatch& patch, std::string* error) {
    if (patch.lan.port < 1 || patch.lan.port > 65535 || patch.remote.port != 8883) {
        SetError(error, "LAN port must be valid and remote MQTT must use port 8883");
        return false;
    }
    if (IsTooLong(patch.lan.username, kUsernameMax) ||
        IsTooLong(patch.remote.username, kUsernameMax) ||
        IsTooLong(patch.lan.host, kHostMax) || IsTooLong(patch.remote.host, kHostMax) ||
        (patch.lan.update_password && IsTooLong(patch.lan.password, kPasswordMax)) ||
        (patch.remote.update_password && IsTooLong(patch.remote.password, kPasswordMax)) ||
        (patch.lan.update_discovery_token &&
            IsTooLong(patch.lan.discovery_token, kTokenMax))) {
        SetError(error, "One or more values are too long");
        return false;
    }
    const AiotLogConfig current = Load();
    const AiotLogProfilePatch& active_patch = patch.remote_mode ? patch.remote : patch.lan;
    const AiotLogProfile& current_profile = patch.remote_mode ? current.remote : current.lan;
    if (patch.enabled && active_patch.username.empty()) {
        SetError(error, "MQTT username is required when AIoT logging is enabled");
        return false;
    }
    if (patch.enabled && !HasEffectivePassword(active_patch, current_profile)) {
        SetError(error, "MQTT password is required when AIoT logging is enabled");
        return false;
    }
    if (patch.enabled && patch.remote_mode && patch.remote.host.empty()) {
        SetError(error, "Remote MQTT requires a hostname and TLS port 8883");
        return false;
    }
    if (!patch.remote.host.empty() && !IsValidRemoteHostnameValue(patch.remote.host)) {
        SetError(error, "Remote MQTT host must be a full DNS hostname, not an IP address or URL");
        return false;
    }

    nvs_handle_t handle;
    if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) {
        SetError(error, "Unable to save local IoT configuration");
        return false;
    }
    esp_err_t result = nvs_set_u8(handle, kEnabled, patch.enabled ? 1 : 0);
    if (result == ESP_OK) result = nvs_set_u8(handle, kMode, patch.remote_mode ? 1 : 0);
    if (result == ESP_OK) result = nvs_set_str(handle, kLanUsername, patch.lan.username.c_str());
    if (result == ESP_OK) result = nvs_set_str(handle, kLanHost, patch.lan.host.c_str());
    if (result == ESP_OK) result = nvs_set_u16(handle, kLanPort, static_cast<uint16_t>(patch.lan.port));
    if (result == ESP_OK) result = ApplySecret(handle, kLanPassword, patch.lan);
    if (result == ESP_OK) result = ApplyToken(handle, kLanToken, patch.lan);
    if (result == ESP_OK) result = nvs_set_str(handle, kRemoteUsername, patch.remote.username.c_str());
    if (result == ESP_OK) result = nvs_set_str(handle, kRemoteHost, patch.remote.host.c_str());
    if (result == ESP_OK) result = nvs_set_u16(handle, kRemotePort, 8883);
    if (result == ESP_OK) result = ApplySecret(handle, kRemotePassword, patch.remote);

    // Mirror the selected profile to legacy keys so an older LAN firmware can
    // still read a coherent configuration after a downgrade.
    const AiotLogProfilePatch& active = patch.remote_mode ? patch.remote : patch.lan;
    const AiotLogProfile& old_active = patch.remote_mode ? current.remote : current.lan;
    const std::string effective_password = active.update_password ? active.password : old_active.password;
    if (result == ESP_OK) result = nvs_set_u8(handle, kLegacyRemoteMode, patch.remote_mode ? 1 : 0);
    if (result == ESP_OK) result = nvs_set_u8(handle, kLegacyTls, patch.remote_mode ? 1 : 0);
    if (result == ESP_OK) result = nvs_set_str(handle, kLegacyUsername, active.username.c_str());
    if (result == ESP_OK) result = nvs_set_str(handle, kLegacyHost, active.host.c_str());
    if (result == ESP_OK) result = nvs_set_u16(handle, kLegacyPort, static_cast<uint16_t>(active.port));
    if (result == ESP_OK) {
        result = active.clear_password || effective_password.empty()
            ? EraseOptional(handle, kLegacyPassword)
            : nvs_set_str(handle, kLegacyPassword, effective_password.c_str());
    }
    if (result == ESP_OK && !patch.remote_mode) {
        const std::string effective_token = active.update_discovery_token
            ? active.discovery_token : old_active.discovery_token;
        result = active.clear_discovery_token || effective_token.empty()
            ? EraseOptional(handle, kLegacyToken)
            : nvs_set_str(handle, kLegacyToken, effective_token.c_str());
    }
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    if (result != ESP_OK) {
        SetError(error, "Unable to save local IoT configuration");
        return false;
    }
    return true;
}
