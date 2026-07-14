#pragma once

#include <string>

struct LocalAiEndpoint {
    std::string ota_url;
    std::string websocket_url;
};

// Synchronous by design: call only from the activation worker, never from the
// audio, wake-word, UI, or Wi-Fi event tasks.
class LocalAiDiscovery {
public:
    bool Discover(LocalAiEndpoint* endpoint) const;
};
