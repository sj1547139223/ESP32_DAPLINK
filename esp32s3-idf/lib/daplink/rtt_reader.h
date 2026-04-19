#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

namespace rtt_reader {

// Initialize RTT reader. Call after swd::init().
esp_err_t init();

// Poll RTT up channel (target → host). Reads new data into buffer.
// Returns number of bytes read, 0 if no data or not found.
// Acquires SWD mutex internally.
size_t poll_up(uint8_t *buffer, size_t buffer_size);

// Write data to RTT down channel (host → target).
// Acquires SWD mutex internally.
size_t write_down(const uint8_t *data, size_t length);

// Returns true if RTT control block has been found on target.
bool is_found();

// Reset state (call when SWD disconnects or target resets).
void reset();

// Set RAM search range for RTT control block.
void set_search_range(uint32_t start_addr, uint32_t size);

// Enable/disable RTT polling. Disabled by default.
void set_enabled(bool enabled);
bool is_enabled();

} // namespace rtt_reader
