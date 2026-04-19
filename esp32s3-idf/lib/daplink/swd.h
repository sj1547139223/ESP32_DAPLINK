#pragma once

#include <cstdint>

#include "esp_err.h"

namespace swd {

struct TransferResult {
    esp_err_t error = ESP_OK;
    uint8_t ack = 0;
    uint32_t value = 0;
};

esp_err_t init();
esp_err_t set_clock_hz(uint32_t hz);
uint32_t clock_hz();
void line_reset();
void jtag_to_swd();
esp_err_t connect();
esp_err_t disconnect();
TransferResult read_dp(uint8_t addr);
TransferResult write_dp(uint8_t addr, uint32_t value);
TransferResult read_ap(uint8_t addr);
TransferResult write_ap(uint8_t addr, uint32_t value);

// Raw single SWD transfer (no SELECT management, no RDBUFF pipeline handling).
// Used by CMSIS-DAP Transfer command which manages pipelining at protocol level.
TransferResult raw_transfer(bool ap, bool read, uint8_t addr, uint32_t write_value);

// Write ABORT register. Always completes data phase regardless of ACK,
// because ABORT writes are accepted even when STICKYERR is set.
TransferResult write_abort(uint32_t value);

// Set transfer configuration (idle cycles, wait retry count)
void set_transfer_config(uint8_t idle_cycles, uint16_t wait_retry);

// Set SWD configuration (turnaround period, data phase)
void set_swd_config(uint8_t turnaround_cycles, bool data_phase);

// Mutex for exclusive SWD bus access (needed for RTT concurrent polling)
void lock();
void unlock();

} // namespace swd
