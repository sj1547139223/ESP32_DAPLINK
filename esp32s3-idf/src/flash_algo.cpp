#include "flash_algo.h"

#include <cstring>
#include <set>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "flm_parser.h"
#include "swd.h"
#include "target_exec.h"

namespace {
const char *kTag = "flash_algo";

// ---- Common SWD memory access helpers ----

// Set AP CSW for word access with auto-increment disabled
esp_err_t ap_init_word_access()
{
    // SELECT = 0 (AP bank 0)
    auto r = swd::write_dp(0x08, 0x00000000);
    if (r.error != ESP_OK) return r.error;
    // CSW = 0x23000002 (word, no auto-increment, DbgSwEnable)
    r = swd::write_ap(0x00, 0x23000002);
    return r.error;
}

esp_err_t mem_write32(uint32_t address, uint32_t value)
{
    auto r = swd::write_ap(0x04, address);  // TAR
    if (r.error != ESP_OK) return r.error;
    r = swd::write_ap(0x0C, value);  // DRW
    return r.error;
}

esp_err_t mem_read32(uint32_t address, uint32_t &value)
{
    auto r = swd::write_ap(0x04, address);  // TAR
    if (r.error != ESP_OK) return r.error;
    r = swd::read_ap(0x0C);  // DRW (pipeline: initiates read)
    if (r.error != ESP_OK) return r.error;
    value = r.value;
    return ESP_OK;
}

// ---- Cortex-M Debug Halt ----

constexpr uint32_t kDHCSR = 0xE000EDF0;
constexpr uint32_t kDHCSR_KEY = 0xA05F0000;
constexpr uint32_t kDHCSR_C_DEBUGEN = (1 << 0);
constexpr uint32_t kDHCSR_C_HALT = (1 << 1);
constexpr uint32_t kDHCSR_S_HALT = (1 << 17);

esp_err_t halt_core()
{
    ESP_RETURN_ON_ERROR(mem_write32(kDHCSR, kDHCSR_KEY | kDHCSR_C_DEBUGEN | kDHCSR_C_HALT), kTag, "halt write failed");
    // Wait for halt
    for (int i = 0; i < 100; ++i) {
        uint32_t val = 0;
        ESP_RETURN_ON_ERROR(mem_read32(kDHCSR, val), kTag, "halt poll read failed");
        if (val & kDHCSR_S_HALT) return ESP_OK;
        esp_rom_delay_us(1000);
    }
    return ESP_ERR_TIMEOUT;
}

esp_err_t reset_and_run()
{
    constexpr uint32_t kAIRCR = 0xE000ED0C;
    // Clear halt, keep debug enabled
    mem_write32(kDHCSR, kDHCSR_KEY | kDHCSR_C_DEBUGEN);
    esp_rom_delay_us(1000);
    // System reset
    mem_write32(kAIRCR, 0x05FA0004);
    esp_rom_delay_us(50000);  // Wait for reset
    return ESP_OK;
}

// ==========================================================================
// STM32G0 Flash Controller (RM0454)
// ==========================================================================

namespace stm32g0 {

constexpr uint32_t kFlashBase = 0x40022000;
constexpr uint32_t kFlashKeyr = kFlashBase + 0x08;
constexpr uint32_t kFlashSr   = kFlashBase + 0x10;
constexpr uint32_t kFlashCr   = kFlashBase + 0x14;

constexpr uint32_t kKey1 = 0x45670123;
constexpr uint32_t kKey2 = 0xCDEF89AB;

// CR bits
constexpr uint32_t kCR_PG   = (1 << 0);
constexpr uint32_t kCR_PER  = (1 << 1);
constexpr uint32_t kCR_STRT = (1 << 16);
constexpr uint32_t kCR_LOCK = (1UL << 31);
// PNB: bits [8:3] = page number

// SR bits
constexpr uint32_t kSR_BSY1 = (1 << 16);
constexpr uint32_t kSR_EOP  = (1 << 0);

constexpr uint32_t kPageSize = 2048;
constexpr uint32_t kFlashStart = 0x08000000;

esp_err_t wait_not_busy(uint32_t timeout_ms = 2000)
{
    uint32_t start = esp_log_timestamp();
    while (true) {
        uint32_t sr = 0;
        ESP_RETURN_ON_ERROR(mem_read32(kFlashSr, sr), kTag, "SR read failed");
        if (!(sr & kSR_BSY1)) {
            // Clear EOP by writing 1 to it
            if (sr & kSR_EOP) {
                mem_write32(kFlashSr, kSR_EOP);
            }
            return ESP_OK;
        }
        if (esp_log_timestamp() - start > timeout_ms) {
            ESP_LOGE(kTag, "Flash busy timeout SR=0x%08lx", (unsigned long)sr);
            return ESP_ERR_TIMEOUT;
        }
        esp_rom_delay_us(100);
    }
}

esp_err_t unlock()
{
    uint32_t cr = 0;
    ESP_RETURN_ON_ERROR(mem_read32(kFlashCr, cr), kTag, "CR read failed");
    if (!(cr & kCR_LOCK)) return ESP_OK;  // Already unlocked

    ESP_RETURN_ON_ERROR(mem_write32(kFlashKeyr, kKey1), kTag, "key1 failed");
    ESP_RETURN_ON_ERROR(mem_write32(kFlashKeyr, kKey2), kTag, "key2 failed");

    ESP_RETURN_ON_ERROR(mem_read32(kFlashCr, cr), kTag, "CR re-read failed");
    if (cr & kCR_LOCK) {
        ESP_LOGE(kTag, "Flash unlock failed CR=0x%08lx", (unsigned long)cr);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t lock()
{
    uint32_t cr = 0;
    ESP_RETURN_ON_ERROR(mem_read32(kFlashCr, cr), kTag, "CR read failed");
    cr |= kCR_LOCK;
    return mem_write32(kFlashCr, cr);
}

esp_err_t erase_page(uint32_t page_number)
{
    ESP_RETURN_ON_ERROR(wait_not_busy(), kTag, "busy before erase");

    uint32_t cr = kCR_PER | ((page_number & 0xFF) << 3) | kCR_STRT;
    ESP_RETURN_ON_ERROR(mem_write32(kFlashCr, cr), kTag, "erase CR write failed");

    ESP_RETURN_ON_ERROR(wait_not_busy(5000), kTag, "erase timeout");

    // Clear PER
    ESP_RETURN_ON_ERROR(mem_write32(kFlashCr, 0), kTag, "CR clear failed");
    return ESP_OK;
}

// Program a double-word (8 bytes) at the given address
esp_err_t program_dword(uint32_t address, uint32_t word_lo, uint32_t word_hi)
{
    ESP_RETURN_ON_ERROR(wait_not_busy(), kTag, "busy before program");

    // Set PG bit
    ESP_RETURN_ON_ERROR(mem_write32(kFlashCr, kCR_PG), kTag, "PG set failed");

    // Write low word first, then high word (must be two consecutive 32-bit writes)
    ESP_RETURN_ON_ERROR(mem_write32(address, word_lo), kTag, "write lo failed");
    ESP_RETURN_ON_ERROR(mem_write32(address + 4, word_hi), kTag, "write hi failed");

    ESP_RETURN_ON_ERROR(wait_not_busy(), kTag, "program timeout");

    // Clear PG
    ESP_RETURN_ON_ERROR(mem_write32(kFlashCr, 0), kTag, "PG clear failed");
    return ESP_OK;
}

} // namespace stm32g0

// ==========================================================================
// STM32F1/F2 Flash Controller (legacy, kept for compatibility)
// ==========================================================================

namespace stm32f1 {

constexpr uint32_t kFlashBase = 0x40022000;
constexpr uint32_t kFlashKeyr = kFlashBase + 0x04;
constexpr uint32_t kFlashSr   = kFlashBase + 0x0C;
constexpr uint32_t kFlashCr   = kFlashBase + 0x10;
constexpr uint32_t kFlashAr   = kFlashBase + 0x14;

constexpr uint32_t kKey1 = 0x45670123;
constexpr uint32_t kKey2 = 0xCDEF89AB;

constexpr uint32_t kCR_PG   = (1 << 0);
constexpr uint32_t kCR_PER  = (1 << 1);
constexpr uint32_t kCR_STRT = (1 << 6);
constexpr uint32_t kCR_LOCK = (1 << 7);

constexpr uint32_t kSR_BSY = (1 << 0);
constexpr uint32_t kSR_EOP = (1 << 5);

constexpr uint32_t kPageSize = 1024;  // Varies by device
constexpr uint32_t kFlashStart = 0x08000000;

esp_err_t wait_not_busy(uint32_t timeout_ms = 2000)
{
    uint32_t start = esp_log_timestamp();
    while (true) {
        uint32_t sr = 0;
        ESP_RETURN_ON_ERROR(mem_read32(kFlashSr, sr), kTag, "SR read failed");
        if (!(sr & kSR_BSY)) return ESP_OK;
        if (esp_log_timestamp() - start > timeout_ms) return ESP_ERR_TIMEOUT;
        esp_rom_delay_us(100);
    }
}

esp_err_t unlock()
{
    ESP_RETURN_ON_ERROR(mem_write32(kFlashKeyr, kKey1), kTag, "key1 failed");
    ESP_RETURN_ON_ERROR(mem_write32(kFlashKeyr, kKey2), kTag, "key2 failed");
    return ESP_OK;
}

esp_err_t lock()
{
    uint32_t cr = 0;
    mem_read32(kFlashCr, cr);
    return mem_write32(kFlashCr, cr | kCR_LOCK);
}

esp_err_t erase_page(uint32_t address)
{
    ESP_RETURN_ON_ERROR(wait_not_busy(), kTag, "busy before erase");
    ESP_RETURN_ON_ERROR(mem_write32(kFlashCr, kCR_PER), kTag, "PER set failed");
    ESP_RETURN_ON_ERROR(mem_write32(kFlashAr, address), kTag, "AR write failed");
    ESP_RETURN_ON_ERROR(mem_write32(kFlashCr, kCR_PER | kCR_STRT), kTag, "STRT failed");
    ESP_RETURN_ON_ERROR(wait_not_busy(5000), kTag, "erase timeout");
    ESP_RETURN_ON_ERROR(mem_write32(kFlashCr, 0), kTag, "CR clear failed");
    return ESP_OK;
}

} // namespace stm32f1

// ==========================================================================
// Programming entry points
// ==========================================================================

esp_err_t program_stm32_g0(const target_probe::TargetInfo &target, const std::vector<hex_parser::Segment> &segments)
{
    ESP_LOGI(kTag, "STM32G0 flash programming: %u segments", (unsigned)segments.size());

    ESP_RETURN_ON_ERROR(ap_init_word_access(), kTag, "AP init failed");
    ESP_RETURN_ON_ERROR(halt_core(), kTag, "halt failed");
    ESP_RETURN_ON_ERROR(stm32g0::unlock(), kTag, "flash unlock failed");

    // Collect unique pages that need erasing
    std::set<uint32_t> pages_to_erase;
    for (const auto &seg : segments) {
        uint32_t page_start = (seg.address - stm32g0::kFlashStart) / stm32g0::kPageSize;
        uint32_t page_end = (seg.address + seg.data.size() - 1 - stm32g0::kFlashStart) / stm32g0::kPageSize;
        for (uint32_t p = page_start; p <= page_end; ++p) {
            pages_to_erase.insert(p);
        }
    }

    ESP_LOGI(kTag, "  Erasing %u pages", (unsigned)pages_to_erase.size());
    for (uint32_t p : pages_to_erase) {
        ESP_LOGI(kTag, "  Erasing page %lu (0x%08lx)", (unsigned long)p,
                 (unsigned long)(stm32g0::kFlashStart + p * stm32g0::kPageSize));
        ESP_RETURN_ON_ERROR(stm32g0::erase_page(p), kTag, "erase page %lu failed", (unsigned long)p);
        vTaskDelay(1);  // Yield to let IDLE task feed WDT
    }

    // Program double-words
    for (const auto &seg : segments) {
        ESP_LOGI(kTag, "  Programming 0x%08lx (%u bytes)",
                 (unsigned long)seg.address, (unsigned)seg.data.size());

        // Align to 8-byte boundary
        for (size_t i = 0; i < seg.data.size(); i += 8) {
            uint32_t lo = 0xFFFFFFFF, hi = 0xFFFFFFFF;
            for (size_t b = 0; b < 4 && (i + b) < seg.data.size(); ++b)
                lo = (lo & ~(0xFFU << (b * 8))) | (seg.data[i + b] << (b * 8));
            for (size_t b = 0; b < 4 && (i + 4 + b) < seg.data.size(); ++b)
                hi = (hi & ~(0xFFU << (b * 8))) | (seg.data[i + 4 + b] << (b * 8));

            ESP_RETURN_ON_ERROR(stm32g0::program_dword(seg.address + i, lo, hi),
                                kTag, "program failed at 0x%08lx", (unsigned long)(seg.address + i));

            // Feed watchdog every 256 double-words (~2KB)
            if ((i & 0x7FF) == 0) {
                vTaskDelay(1);
            }
        }
    }

    ESP_RETURN_ON_ERROR(stm32g0::lock(), kTag, "flash lock failed");
    ESP_RETURN_ON_ERROR(reset_and_run(), kTag, "reset failed");

    ESP_LOGI(kTag, "STM32G0 programming complete");
    return ESP_OK;
}

esp_err_t program_stm32_f1(const target_probe::TargetInfo &target, const std::vector<hex_parser::Segment> &segments)
{
    ESP_LOGI(kTag, "STM32F1 flash programming: %u segments", (unsigned)segments.size());

    ESP_RETURN_ON_ERROR(ap_init_word_access(), kTag, "AP init failed");
    ESP_RETURN_ON_ERROR(halt_core(), kTag, "halt failed");
    ESP_RETURN_ON_ERROR(stm32f1::unlock(), kTag, "flash unlock failed");

    // Erase pages
    for (const auto &seg : segments) {
        uint32_t page_addr = seg.address & ~(stm32f1::kPageSize - 1);
        uint32_t end_addr = seg.address + seg.data.size();
        for (; page_addr < end_addr; page_addr += stm32f1::kPageSize) {
            ESP_LOGI(kTag, "  Erasing page at 0x%08lx", (unsigned long)page_addr);
            ESP_RETURN_ON_ERROR(stm32f1::erase_page(page_addr), kTag, "erase failed");
            vTaskDelay(1);  // Yield to let IDLE task feed WDT
        }
    }

    // Program half-words
    for (const auto &seg : segments) {
        ESP_RETURN_ON_ERROR(stm32f1::wait_not_busy(), kTag, "busy before program");
        ESP_RETURN_ON_ERROR(mem_write32(stm32f1::kFlashCr, stm32f1::kCR_PG), kTag, "PG set failed");

        for (size_t i = 0; i < seg.data.size(); i += 2) {
            uint16_t hw = seg.data[i];
            if (i + 1 < seg.data.size()) hw |= (uint16_t)seg.data[i + 1] << 8;
            // Write as 32-bit with upper half 0xFFFF (only lower 16 bits matter)
            ESP_RETURN_ON_ERROR(mem_write32(seg.address + i, hw), kTag, "write failed");
            ESP_RETURN_ON_ERROR(stm32f1::wait_not_busy(), kTag, "program timeout");

            // Feed watchdog every 1KB
            if ((i & 0x3FF) == 0) {
                vTaskDelay(1);
            }
        }

        ESP_RETURN_ON_ERROR(mem_write32(stm32f1::kFlashCr, 0), kTag, "PG clear failed");
    }

    ESP_RETURN_ON_ERROR(stm32f1::lock(), kTag, "flash lock failed");
    ESP_RETURN_ON_ERROR(reset_and_run(), kTag, "reset failed");

    ESP_LOGI(kTag, "STM32F1 programming complete");
    return ESP_OK;
}

} // namespace

namespace flash_algo {

SelectionResult select_algorithm(const hex_parser::ParsedHexImage &image, const target_probe::TargetInfo &target)
{
    SelectionResult result{};
    result.family = target.family;

    if (target.family == target_probe::Family::kStm32) {
        // Use DEV_ID (from DBGMCU_IDCODE) for reliable sub-family detection
        uint32_t dev = target.dev_id;

        // STM32G0 series DEV_IDs
        if (dev == 0x466 || dev == 0x460 || dev == 0x456 || dev == 0x467) {
            result.algorithm_name = "stm32g0_direct";
            return result;
        }
        // STM32G4 series DEV_IDs (same flash controller as G0)
        if (dev == 0x468 || dev == 0x469 || dev == 0x479) {
            result.algorithm_name = "stm32g4_direct";
            return result;
        }
        // STM32F1 series DEV_IDs
        if (dev == 0x410 || dev == 0x414 || dev == 0x412 || dev == 0x418 || dev == 0x420) {
            result.algorithm_name = "stm32f1_direct";
            return result;
        }

        // Fallback: use DPIDR to guess
        uint32_t dpidr_masked = target.dpidr & 0x0FFFFFFF;
        if (dpidr_masked == 0x0BC11477 || dpidr_masked == 0x0BC01477) {
            result.algorithm_name = "stm32g0_direct";  // M0/M0+ → G0 flash controller
        } else {
            result.algorithm_name = "stm32f1_direct";  // M3/M4 → F1 flash controller (default)
        }
        return result;
    }

    if (target.family != target_probe::Family::kUnknown) {
        switch (target.family) {
        case target_probe::Family::kNordic:
            result.algorithm_name = "nrf_generic";
            break;
        default:
            result.algorithm_name = "unsupported";
            break;
        }
        return result;
    }

    // Fallback: guess by address range
    if (image.lowest_address >= 0x08000000 && image.lowest_address < 0x08100000) {
        result.family = target_probe::Family::kStm32;
        result.algorithm_name = "stm32g0_address_guess";
    }

    return result;
}

esp_err_t program_target(const SelectionResult &selection,
                         const target_probe::TargetInfo &target,
                         const std::vector<hex_parser::Segment> &segments)
{
    ESP_LOGI(kTag, "Programming: family=%s algo=%s segments=%u",
             target_probe::family_name(selection.family),
             selection.algorithm_name,
             (unsigned)segments.size());

    if (selection.family == target_probe::Family::kUnknown) {
        ESP_LOGE(kTag, "Cannot program unknown target family");
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (selection.family == target_probe::Family::kStm32) {
        if (strstr(selection.algorithm_name, "g0") != nullptr ||
            strstr(selection.algorithm_name, "g4") != nullptr) {
            return program_stm32_g0(target, segments);
        } else {
            return program_stm32_f1(target, segments);
        }
    }

    ESP_LOGE(kTag, "Unsupported target family for drag-drop");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t program_with_flm(const char *flm_path,
                           const target_probe::TargetInfo &target,
                           const std::vector<hex_parser::Segment> &segments,
                           uint32_t ram_base, uint32_t ram_size)
{
    ESP_LOGI(kTag, "FLM programming: %s segments=%u", flm_path, (unsigned)segments.size());

    // Parse FLM file
    flm_parser::ParsedFlm flm;
    std::string parse_error;
    ESP_RETURN_ON_ERROR(flm_parser::parse_file(flm_path, flm, parse_error), kTag, "FLM parse: %s", parse_error.c_str());

    // Set up AP access and upload algorithm
    target_exec::RamLayout layout;
    ESP_RETURN_ON_ERROR(target_exec::setup(flm, ram_base, ram_size, layout), kTag, "setup failed");

    // Call Init(flash_base, 0=erase, SystemCoreClock)
    uint32_t result = 0;
    ESP_LOGI(kTag, "  Calling Init(0x%08lx)", (unsigned long)flm.device.dev_addr);
    ESP_RETURN_ON_ERROR(
        target_exec::call_function(layout, flm.func.init, flm.device.dev_addr, 0, 1, 0, result, 5000),
        kTag, "Init failed");
    if (result != 0) {
        ESP_LOGE(kTag, "Init returned error: %lu", (unsigned long)result);
        return ESP_FAIL;
    }

    // Determine sector sizes for erase
    uint32_t default_sector_size = flm.device.sectors.empty() ? flm.device.page_size : flm.device.sectors[0].size;

    // Erase needed sectors
    std::set<uint32_t> sectors_to_erase;
    for (const auto &seg : segments) {
        uint32_t addr = seg.address;
        uint32_t end = addr + seg.data.size();
        while (addr < end) {
            // Find sector size for this address
            uint32_t sector_size = default_sector_size;
            uint32_t sector_base = flm.device.dev_addr;
            for (const auto &s : flm.device.sectors) {
                if (addr >= flm.device.dev_addr + s.address) {
                    sector_size = s.size;
                    sector_base = flm.device.dev_addr + s.address;
                }
            }
            uint32_t sector_addr = addr - ((addr - sector_base) % sector_size);
            sectors_to_erase.insert(sector_addr);
            addr = sector_addr + sector_size;
        }
    }

    ESP_LOGI(kTag, "  Erasing %u sectors", (unsigned)sectors_to_erase.size());
    for (uint32_t sector_addr : sectors_to_erase) {
        ESP_LOGI(kTag, "    Erase sector 0x%08lx", (unsigned long)sector_addr);
        ESP_RETURN_ON_ERROR(
            target_exec::call_function(layout, flm.func.erase_sector, sector_addr, 0, 0, 0, result,
                                       flm.device.timeout_erase > 0 ? flm.device.timeout_erase : 10000),
            kTag, "EraseSector failed");
        if (result != 0) {
            ESP_LOGE(kTag, "EraseSector(0x%08lx) returned %lu", (unsigned long)sector_addr, (unsigned long)result);
            return ESP_FAIL;
        }
        vTaskDelay(1);  // Yield to let IDLE task feed WDT
    }

    // Program pages
    uint32_t page_size = flm.device.page_size;
    if (page_size == 0) page_size = 512;
    // Clamp to available buffer size
    if (page_size > layout.buf_size) {
        page_size = layout.buf_size & ~3U;  // Align to 4
    }

    for (const auto &seg : segments) {
        ESP_LOGI(kTag, "  Programming 0x%08lx (%u bytes)",
                 (unsigned long)seg.address, (unsigned)seg.data.size());

        for (size_t offset = 0; offset < seg.data.size(); offset += page_size) {
            uint32_t chunk = seg.data.size() - offset;
            if (chunk > page_size) chunk = page_size;

            // Upload data to target RAM buffer
            ESP_RETURN_ON_ERROR(
                target_exec::upload_data(layout, seg.data.data() + offset, chunk),
                kTag, "data upload failed");

            // Call ProgramPage(addr, size, buf_addr)
            ESP_RETURN_ON_ERROR(
                target_exec::call_function(layout, flm.func.program_page,
                                           seg.address + offset, chunk, layout.buf_addr, 0, result,
                                           flm.device.timeout_prog > 0 ? flm.device.timeout_prog : 5000),
                kTag, "ProgramPage failed");
            if (result != 0) {
                ESP_LOGE(kTag, "ProgramPage(0x%08lx, %lu) returned %lu",
                         (unsigned long)(seg.address + offset), (unsigned long)chunk, (unsigned long)result);
                return ESP_FAIL;
            }
            vTaskDelay(1);  // Yield to let IDLE task feed WDT
        }
    }

    // Call UnInit
    if (flm.func.uninit != 0) {
        ESP_LOGI(kTag, "  Calling UnInit");
        target_exec::call_function(layout, flm.func.uninit, 1, 0, 0, 0, result, 5000);
    }

    // Reset and run
    reset_and_run();

    ESP_LOGI(kTag, "FLM programming complete");
    return ESP_OK;
}

} // namespace flash_algo
