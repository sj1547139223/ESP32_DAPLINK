#pragma once

#include "esp_err.h"

namespace usb_cdc_bridge {

esp_err_t init();
void task();

// When true, the bridge task stops reading UART (WiFi serial takes over)
void set_uart_paused(bool paused);

// Initialize RTT CDC port (CDC_ACM_1). Call after init().
esp_err_t init_rtt_cdc();

// Write RTT data to USB CDC port 1.
void rtt_write(const uint8_t *data, size_t length);

// Read RTT data from USB CDC port 1 (host → target).
// Returns bytes read, 0 if none.
size_t rtt_read(uint8_t *buffer, size_t buffer_size);

// Write WiFi connection info (IP + code) to CDC if terminal is open.
void print_wifi_info();

} // namespace usb_cdc_bridge
