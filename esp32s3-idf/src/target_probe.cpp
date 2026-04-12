#include "target_probe.h"

#include "esp_check.h"
#include "esp_log.h"
#include "swd.h"

namespace {
const char *kTag = "target_probe";
}

namespace target_probe {

esp_err_t probe(TargetInfo &info)
{
    info = {};

    // Full SWD initialization sequence for standalone probe
    swd::jtag_to_swd();
    ESP_RETURN_ON_ERROR(swd::connect(), kTag, "swd connect failed");

    const swd::TransferResult idcode = swd::read_dp(0x00);
    if (idcode.error != ESP_OK) {
        ESP_LOGW(kTag, "DPIDR read failed ack=0x%02x err=%s", idcode.ack, esp_err_to_name(idcode.error));
        return idcode.error;
    }

    info.attached = true;
    info.dpidr = idcode.value;
    info.idcode = idcode.value;

    // Clear sticky errors via ABORT
    swd::write_abort(0x0000001E);

    // Power up debug port: CSYSPWRUPREQ | CDBGPWRUPREQ
    swd::write_dp(0x04, 0x50000000);

    // Wait for power-up acknowledgment
    for (int i = 0; i < 100; ++i) {
        auto stat = swd::read_dp(0x04);
        if (stat.error == ESP_OK && (stat.value & 0xA0000000) == 0xA0000000) {
            break;
        }
        esp_rom_delay_us(1000);
    }

    // Identify target by DPIDR (mask out REVISION [31:28])
    bool is_cortex_m0 = false;
    switch (idcode.value & 0x0FFFFFFF) {
    case 0x0BA01477:  // Cortex-M3/M4 (STM32F1, STM32F4, STM32G4, etc.)
    case 0x0BA02477:  // Cortex-M7
    case 0x0BA04477:  // Cortex-M4 (some newer STM32)
        info.family = Family::kStm32;
        info.name = "STM32 Cortex-M3/M4/M7";
        break;
    case 0x0BC11477:  // Cortex-M0+ (STM32G0, STM32L0, etc.)
    case 0x0BC01477:  // Cortex-M0 (STM32F0)
        info.family = Family::kStm32;
        info.name = "STM32 Cortex-M0/M0+";
        is_cortex_m0 = true;
        break;
    default:
        info.family = Family::kUnknown;
        info.name = "Unknown ARM target";
        break;
    }

    // Read DBGMCU_IDCODE for specific STM32 device identification
    if (info.family == Family::kStm32) {
        // Set AP CSW for 32-bit word access
        swd::write_ap(0x00, 0x23000002);

        // DBGMCU_IDCODE address depends on core type
        // M0/M0+ (G0, L0, F0): 0x40015800
        // M3/M4/M7 (F1, F4, G4, etc.): 0xE0042000
        uint32_t dbgmcu_addr = is_cortex_m0 ? 0x40015800 : 0xE0042000;
        swd::write_ap(0x04, dbgmcu_addr);  // TAR
        auto dbg = swd::read_ap(0x0C);     // DRW
        if (dbg.error == ESP_OK) {
            info.dev_id = dbg.value & 0xFFF;  // DEV_ID is bits [11:0]
            ESP_LOGI(kTag, "DBGMCU_IDCODE=0x%08lx DEV_ID=0x%03lx",
                     (unsigned long)dbg.value, (unsigned long)info.dev_id);

            // Refine name based on DEV_ID
            switch (info.dev_id) {
            // STM32G0 series
            case 0x466: info.name = "STM32G030/G031"; break;
            case 0x460: info.name = "STM32G070/G071"; break;
            case 0x456: info.name = "STM32G051/G061"; break;
            case 0x467: info.name = "STM32G0B0/G0B1/G0C1"; break;
            // STM32G4 series
            case 0x468: info.name = "STM32G431/G441"; break;
            case 0x469: info.name = "STM32G471/G473/G474/G483/G484"; break;
            case 0x479: info.name = "STM32G491/G4A1"; break;
            // STM32F1 series
            case 0x410: info.name = "STM32F103 (medium)"; break;
            case 0x414: info.name = "STM32F103 (high)"; break;
            case 0x412: info.name = "STM32F103 (low)"; break;
            case 0x418: info.name = "STM32F105/F107"; break;
            // STM32F4 series
            case 0x413: info.name = "STM32F405/F407/F415/F417"; break;
            case 0x431: info.name = "STM32F411"; break;
            case 0x441: info.name = "STM32F412"; break;
            case 0x421: info.name = "STM32F446"; break;
            case 0x434: info.name = "STM32F469/F479"; break;
            default: break;  // Keep original name
            }
        }
    }

    ESP_LOGI(kTag, "Target probe DPIDR=0x%08lx family=%s name=%s",
             static_cast<unsigned long>(info.dpidr), family_name(info.family), info.name.c_str());
    return ESP_OK;
}

const char *family_name(Family family)
{
    switch (family) {
    case Family::kStm32: return "STM32";
    case Family::kNordic: return "Nordic nRF";
    case Family::kNxp: return "NXP LPC";
    case Family::kGd32: return "GD32";
    default: return "Unknown";
    }
}

} // namespace target_probe
