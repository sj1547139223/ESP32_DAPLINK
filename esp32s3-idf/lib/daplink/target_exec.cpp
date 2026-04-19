#include "target_exec.h"

#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_task_wdt.h"
#include "swd.h"

namespace {

const char *kTag = "target_exec";

// Cortex-M debug registers
constexpr uint32_t kDHCSR = 0xE000EDF0;
constexpr uint32_t kDCRSR = 0xE000EDF4;
constexpr uint32_t kDCRDR = 0xE000EDF8;

constexpr uint32_t kDHCSR_KEY       = 0xA05F0000;
constexpr uint32_t kDHCSR_C_DEBUGEN = (1 << 0);
constexpr uint32_t kDHCSR_C_HALT    = (1 << 1);
constexpr uint32_t kDHCSR_S_HALT    = (1 << 17);
constexpr uint32_t kDHCSR_S_REGRDY  = (1 << 16);

// AP CSW for 32-bit word access without auto-increment
esp_err_t ap_word_access()
{
    auto r = swd::write_dp(0x08, 0x00000000);
    if (r.error != ESP_OK) return r.error;
    r = swd::write_ap(0x00, 0x23000002);
    return r.error;
}

esp_err_t mem_write32(uint32_t address, uint32_t value)
{
    auto r = swd::write_ap(0x04, address);
    if (r.error != ESP_OK) return r.error;
    r = swd::write_ap(0x0C, value);
    return r.error;
}

esp_err_t mem_read32(uint32_t address, uint32_t &value)
{
    auto r = swd::write_ap(0x04, address);
    if (r.error != ESP_OK) return r.error;
    r = swd::read_ap(0x0C);
    if (r.error != ESP_OK) return r.error;
    value = r.value;
    return ESP_OK;
}

// Write a core register (R0-R15, xPSR=16)
esp_err_t write_core_reg(uint8_t reg, uint32_t value)
{
    ESP_RETURN_ON_ERROR(mem_write32(kDCRDR, value), kTag, "DCRDR write failed");
    ESP_RETURN_ON_ERROR(mem_write32(kDCRSR, (1U << 16) | reg), kTag, "DCRSR write failed");
    // Wait for S_REGRDY
    for (int i = 0; i < 100; ++i) {
        uint32_t dhcsr = 0;
        ESP_RETURN_ON_ERROR(mem_read32(kDHCSR, dhcsr), kTag, "DHCSR poll failed");
        if (dhcsr & kDHCSR_S_REGRDY) return ESP_OK;
    }
    return ESP_ERR_TIMEOUT;
}

// Read a core register
esp_err_t read_core_reg(uint8_t reg, uint32_t &value)
{
    ESP_RETURN_ON_ERROR(mem_write32(kDCRSR, reg), kTag, "DCRSR read request failed");
    for (int i = 0; i < 100; ++i) {
        uint32_t dhcsr = 0;
        ESP_RETURN_ON_ERROR(mem_read32(kDHCSR, dhcsr), kTag, "DHCSR poll failed");
        if (dhcsr & kDHCSR_S_REGRDY) {
            return mem_read32(kDCRDR, value);
        }
    }
    return ESP_ERR_TIMEOUT;
}

// Halt the core
esp_err_t halt_core()
{
    ESP_RETURN_ON_ERROR(mem_write32(kDHCSR, kDHCSR_KEY | kDHCSR_C_DEBUGEN | kDHCSR_C_HALT), kTag, "halt failed");
    for (int i = 0; i < 100; ++i) {
        uint32_t val = 0;
        ESP_RETURN_ON_ERROR(mem_read32(kDHCSR, val), kTag, "halt poll failed");
        if (val & kDHCSR_S_HALT) return ESP_OK;
        esp_rom_delay_us(1000);
    }
    return ESP_ERR_TIMEOUT;
}

// Resume execution (clear halt)
esp_err_t resume()
{
    return mem_write32(kDHCSR, kDHCSR_KEY | kDHCSR_C_DEBUGEN);
}

// Wait for target to halt (e.g., hit BKPT)
esp_err_t wait_halt(uint32_t timeout_ms)
{
    uint32_t start = esp_log_timestamp();
    uint32_t last_wdt_feed = start;
    while (true) {
        uint32_t val = 0;
        ESP_RETURN_ON_ERROR(mem_read32(kDHCSR, val), kTag, "DHCSR read failed");
        if (val & kDHCSR_S_HALT) return ESP_OK;
        uint32_t now = esp_log_timestamp();
        if (now - start > timeout_ms) {
            ESP_LOGE(kTag, "Wait halt timeout (%lums)", (unsigned long)timeout_ms);
            // Force halt
            halt_core();
            return ESP_ERR_TIMEOUT;
        }
        // Feed watchdog every 100ms
        if (now - last_wdt_feed >= 100) {
            esp_task_wdt_reset();
            last_wdt_feed = now;
        }
        esp_rom_delay_us(100);
    }
}

} // namespace

namespace target_exec {

esp_err_t setup(const flm_parser::ParsedFlm &flm, uint32_t ram_base, uint32_t ram_size, RamLayout &layout)
{
    ESP_RETURN_ON_ERROR(ap_word_access(), kTag, "AP init failed");
    ESP_RETURN_ON_ERROR(halt_core(), kTag, "halt failed");

    // Calculate RAM layout
    layout.ram_base = ram_base;
    layout.ram_size = ram_size;
    layout.code_addr = ram_base;
    layout.code_size = flm.code_size;

    // BKPT instruction right after code (aligned to 4)
    layout.bkpt_addr = (ram_base + flm.code_size + 3) & ~3U;
    // Stack after BKPT (grows down, so stack_ptr is end of stack area)
    uint32_t stack_base = layout.bkpt_addr + 4;
    layout.stack_ptr = stack_base + flm.stack_size;
    // Data buffer after stack
    layout.buf_addr = layout.stack_ptr;
    uint32_t buf_end = ram_base + ram_size;
    if (layout.buf_addr >= buf_end) {
        ESP_LOGE(kTag, "Insufficient RAM: need %lu, have %lu",
                 (unsigned long)(layout.buf_addr - ram_base), (unsigned long)ram_size);
        return ESP_ERR_NO_MEM;
    }
    layout.buf_size = buf_end - layout.buf_addr;

    ESP_LOGI(kTag, "RAM layout: code=0x%08lx bkpt=0x%08lx sp=0x%08lx buf=0x%08lx(%lu)",
             (unsigned long)layout.code_addr,
             (unsigned long)layout.bkpt_addr,
             (unsigned long)layout.stack_ptr,
             (unsigned long)layout.buf_addr,
             (unsigned long)layout.buf_size);

    // Upload algorithm code to target RAM (word at a time)
    const uint8_t *code = flm.code.data();
    for (uint32_t i = 0; i < flm.code_size; i += 4) {
        uint32_t word = 0;
        for (uint32_t b = 0; b < 4 && (i + b) < flm.code_size; ++b) {
            word |= (uint32_t)code[i + b] << (b * 8);
        }
        ESP_RETURN_ON_ERROR(mem_write32(ram_base + i, word), kTag, "code upload failed at offset %lu", (unsigned long)i);

        if ((i & 0x3FF) == 0) {
            esp_task_wdt_reset();
        }
    }

    // Write BKPT instruction (0xBE00BE00 — two BKPT.W)
    ESP_RETURN_ON_ERROR(mem_write32(layout.bkpt_addr, 0xBE00BE00), kTag, "BKPT write failed");

    ESP_LOGI(kTag, "Algorithm uploaded: %lu bytes", (unsigned long)flm.code_size);
    return ESP_OK;
}

esp_err_t call_function(const RamLayout &layout, uint32_t func_addr,
                        uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3,
                        uint32_t &result, uint32_t timeout_ms)
{
    // Ensure halted
    ESP_RETURN_ON_ERROR(halt_core(), kTag, "pre-call halt failed");

    // The function address from FLM is relative to code_base (usually 0).
    // Adjust to the actual RAM load address. Set Thumb bit.
    uint32_t actual_pc = (func_addr - 0 /* code_base in FLM is 0 */) + layout.code_addr;
    actual_pc |= 1;  // Thumb mode

    uint32_t lr = layout.bkpt_addr | 1;  // Return to BKPT

    // Set registers
    ESP_RETURN_ON_ERROR(write_core_reg(0, r0), kTag, "R0 write failed");
    ESP_RETURN_ON_ERROR(write_core_reg(1, r1), kTag, "R1 write failed");
    ESP_RETURN_ON_ERROR(write_core_reg(2, r2), kTag, "R2 write failed");
    ESP_RETURN_ON_ERROR(write_core_reg(3, r3), kTag, "R3 write failed");
    ESP_RETURN_ON_ERROR(write_core_reg(13, layout.stack_ptr), kTag, "SP write failed");
    ESP_RETURN_ON_ERROR(write_core_reg(14, lr), kTag, "LR write failed");
    ESP_RETURN_ON_ERROR(write_core_reg(15, actual_pc), kTag, "PC write failed");
    // Set xPSR with Thumb bit
    ESP_RETURN_ON_ERROR(write_core_reg(16, 0x01000000), kTag, "xPSR write failed");

    // Resume execution
    ESP_RETURN_ON_ERROR(resume(), kTag, "resume failed");

    // Wait for BKPT halt
    ESP_RETURN_ON_ERROR(wait_halt(timeout_ms), kTag, "function execution timeout");

    // Read return value (R0)
    ESP_RETURN_ON_ERROR(read_core_reg(0, result), kTag, "R0 read failed");

    return ESP_OK;
}

esp_err_t upload_data(const RamLayout &layout, const uint8_t *data, uint32_t size)
{
    if (size > layout.buf_size) {
        ESP_LOGE(kTag, "Data too large: %lu > buf %lu", (unsigned long)size, (unsigned long)layout.buf_size);
        return ESP_ERR_NO_MEM;
    }

    for (uint32_t i = 0; i < size; i += 4) {
        uint32_t word = 0xFFFFFFFF;
        for (uint32_t b = 0; b < 4 && (i + b) < size; ++b) {
            word = (word & ~(0xFFU << (b * 8))) | ((uint32_t)data[i + b] << (b * 8));
        }
        ESP_RETURN_ON_ERROR(mem_write32(layout.buf_addr + i, word), kTag, "data upload failed");
    }
    return ESP_OK;
}

} // namespace target_exec
