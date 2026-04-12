#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "board_config.h"
#include "esp_err.h"

namespace cmsis_dap {

esp_err_t init();
void poll();
void on_vendor_data(const uint8_t *buffer, size_t length);
bool is_connected();
bool is_dap_busy();  // True during active DAP session (programming, etc.)

// Transport-independent command processing (for WiFi/TCP path)
// Returns response length. Thread-safe: must be called from same task as on_vendor_data,
// or with external serialization.
size_t process_command(const uint8_t *input, size_t input_len,
                       uint8_t *output, size_t output_max);

constexpr size_t kMaxResponseSize = 1500;

// Exported for response caching
extern std::array<uint8_t, kMaxResponseSize> g_last_dap_response;
extern size_t g_last_dap_response_length;

} // namespace cmsis_dap
