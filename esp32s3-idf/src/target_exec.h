#pragma once

#include <cstdint>
#include <vector>

#include "esp_err.h"
#include "flm_parser.h"

namespace target_exec {

/// RAM layout for algorithm execution
struct RamLayout {
    uint32_t ram_base;      // Start of target SRAM
    uint32_t ram_size;      // Total SRAM size
    uint32_t code_addr;     // Where algorithm code starts (= ram_base)
    uint32_t code_size;     // Size of algorithm code
    uint32_t bkpt_addr;     // Address of BKPT instruction (after code)
    uint32_t stack_ptr;     // Stack pointer (grows down from here)
    uint32_t buf_addr;      // Programming data buffer address
    uint32_t buf_size;      // Buffer size available
};

/// Upload algorithm code to target RAM and prepare execution environment.
/// Places BKPT at the end of code for function return detection.
esp_err_t setup(const flm_parser::ParsedFlm &flm, uint32_t ram_base, uint32_t ram_size, RamLayout &layout);

/// Execute a function on the target.
/// Sets R0-R3, SP, LR=BKPT, PC=func_addr, then runs until halt.
/// Returns function result in R0.
esp_err_t call_function(const RamLayout &layout, uint32_t func_addr,
                        uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3,
                        uint32_t &result, uint32_t timeout_ms = 5000);

/// Upload data to the target RAM buffer.
esp_err_t upload_data(const RamLayout &layout, const uint8_t *data, uint32_t size);

} // namespace target_exec
