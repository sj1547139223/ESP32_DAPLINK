#pragma once

#include "esp_err.h"

namespace wifi_debug {

// Initialize WiFi and start debug services.
// Reads wifi.txt from USB drive for SSID/password/relay config.
// Returns ESP_OK even if WiFi is not configured (feature disabled).
esp_err_t init();

// Poll WiFi debug connections (call from main loop).
void poll();

// Check if WiFi is connected.
bool is_connected();

// Get device pairing code (6-char string). Empty if not generated yet.
const char *get_device_code();

// Get device IP address string. Empty if not connected.
const char *get_ip_str();

// Check if WiFi is configured (wifi.txt found).
bool is_configured();

// Send RTT data to WiFi TCP clients (if any connected on RTT port).
void rtt_send(const uint8_t *data, size_t length);

// Receive RTT data from WiFi TCP (host → target).
size_t rtt_recv(uint8_t *buffer, size_t buffer_size);

} // namespace wifi_debug
