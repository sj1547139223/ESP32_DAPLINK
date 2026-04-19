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
    bool is_cortex_m = false;
    switch (idcode.value & 0x0FFFFFFF) {
    case 0x0BA01477:  // Cortex-M3/M4
    case 0x0BA02477:  // Cortex-M7
    case 0x0BA04477:  // Cortex-M4 (some variants)
        is_cortex_m = true;
        break;
    case 0x0BC11477:  // Cortex-M0+
    case 0x0BC01477:  // Cortex-M0
        is_cortex_m = true;
        is_cortex_m0 = true;
        break;
    default:
        info.family = Family::kUnknown;
        info.name = "Unknown ARM target";
        break;
    }

    if (is_cortex_m) {
        // Set AP CSW for 32-bit word access
        swd::write_ap(0x00, 0x23000002);

        if (is_cortex_m0) {
            // ---------- M0/M0+ identification ----------
            // Try STM32-style DBGMCU_IDCODE at 0x40015800 first
            swd::write_ap(0x04, 0x40015800);
            auto dbg = swd::read_ap(0x0C);  // DRW

            if (dbg.error == ESP_OK && dbg.value != 0 && dbg.value != 0xFFFFFFFF) {
                // Has valid DBGMCU_IDCODE → likely STM32 M0/M0+
                info.dev_id = dbg.value & 0xFFF;
                ESP_LOGI(kTag, "DBGMCU_IDCODE=0x%08lx DEV_ID=0x%03lx",
                         (unsigned long)dbg.value, (unsigned long)info.dev_id);
                info.family = Family::kStm32;
                switch (info.dev_id) {
                case 0x466: info.name = "STM32G030/G031"; break;
                case 0x460: info.name = "STM32G070/G071"; break;
                case 0x456: info.name = "STM32G051/G061"; break;
                case 0x467: info.name = "STM32G0B0/G0B1/G0C1"; break;
                case 0x440: info.name = "STM32F030/F051"; break;
                case 0x444: info.name = "STM32F030x4/x6"; break;
                case 0x445: info.name = "STM32F042"; break;
                case 0x448: info.name = "STM32F070/F072"; break;
                default:    info.name = "STM32 Cortex-M0/M0+"; break;
                }
            } else {
                // No STM32 DBGMCU → M0/M0+ with DBGMCU_IDCODE=0
                // STM32F0/G0 always have valid DBGMCU_IDCODE, so this is NOT STM32.
                // CW32 series (F030/F031/L011/L017/L083) and similar chips fall here.
                // Try to identify specific CW32 variant via its DBG base registers.

                // CW32: try reading DBGMCU at various addresses
                // CW32F030/F031/F036: DBG control at 0x40010000
                swd::write_ap(0x04, 0x40010000);
                auto cw_dbg = swd::read_ap(0x0C);

                bool cw32_found = false;
                if (cw_dbg.error == ESP_OK && cw_dbg.value != 0 && cw_dbg.value != 0xFFFFFFFF) {
                    uint16_t part_id = cw_dbg.value & 0xFFFF;
                    info.family = Family::kCw32;
                    info.dev_id = cw_dbg.value;
                    cw32_found = true;
                    ESP_LOGI(kTag, "CW32 DBG@0x40010000=0x%08lx part=0x%04x",
                             (unsigned long)cw_dbg.value, part_id);
                    switch (part_id) {
                    case 0x0030: info.name = "CW32F030"; break;
                    case 0x0031: info.name = "CW32F031"; break;
                    case 0x0036: info.name = "CW32F036"; break;
                    default:     info.name = "CW32 (unknown F-series)"; break;
                    }
                }

                if (!cw32_found) {
                    // CW32L series: DBG at 0x40015800 (same address but returns distinct values)
                    // or just accept as CW32 family based on DBGMCU_IDCODE=0 heuristic
                    // All common Cortex-M0+ chips without DBGMCU_IDCODE are CW32/similar
                    info.family = Family::kCw32;
                    info.dev_id = 0;
                    info.name = "CW32 (variant auto-detected)";
                    ESP_LOGW(kTag, "CW32-like M0/M0+ (DBGMCU_IDCODE=0), using CW32 Flash driver");
                }
            }
        } else {
            // ---------- M3/M4/M7 identification ----------
            // All known families (STM32/GD32/CH32/APM32/AGM32) have DBGMCU_IDCODE at 0xE0042000
            swd::write_ap(0x04, 0xE0042000);
            auto dbg = swd::read_ap(0x0C);  // DRW

            if (dbg.error != ESP_OK || dbg.value == 0 || dbg.value == 0xFFFFFFFF) {
                info.family = Family::kUnknown;
                info.name = "Unknown Cortex-M3/M4/M7";
                ESP_LOGW(kTag, "DBGMCU_IDCODE read failed or zero: 0x%08lx",
                         dbg.error == ESP_OK ? (unsigned long)dbg.value : 0UL);
            } else {
                info.dev_id = dbg.value & 0xFFF;
                uint32_t rev_id = (dbg.value >> 16) & 0xFFFF;
                ESP_LOGI(kTag, "DBGMCU_IDCODE=0x%08lx DEV_ID=0x%03lx REV=0x%04lx",
                         (unsigned long)dbg.value, (unsigned long)info.dev_id, (unsigned long)rev_id);

                // Classify by DEV_ID
                switch (info.dev_id) {
                // --- STM32F1 ---
                case 0x410: info.family = Family::kStm32; info.name = "STM32F103 (medium)"; break;
                case 0x414: info.family = Family::kStm32; info.name = "STM32F103 (high)"; break;
                case 0x412: info.family = Family::kStm32; info.name = "STM32F103 (low)"; break;
                case 0x418: info.family = Family::kStm32; info.name = "STM32F105/F107"; break;
                case 0x420: info.family = Family::kStm32; info.name = "STM32F100 (med/low)"; break;
                // --- STM32F2 ---
                case 0x411: info.family = Family::kStm32; info.name = "STM32F2xx"; break;
                // --- STM32F4 ---
                case 0x413: info.family = Family::kStm32; info.name = "STM32F405/F407/F415/F417"; break;
                case 0x419: info.family = Family::kStm32; info.name = "STM32F42x/F43x"; break;
                case 0x423: info.family = Family::kStm32; info.name = "STM32F401xB/C"; break;
                case 0x433: info.family = Family::kStm32; info.name = "STM32F401xD/E"; break;
                case 0x431: info.family = Family::kStm32; info.name = "STM32F411"; break;
                case 0x441: info.family = Family::kStm32; info.name = "STM32F412"; break;
                case 0x421: info.family = Family::kStm32; info.name = "STM32F446"; break;
                case 0x434: info.family = Family::kStm32; info.name = "STM32F469/F479"; break;
                // --- STM32F7 ---
                case 0x449: info.family = Family::kStm32; info.name = "STM32F74x/F75x"; break;
                case 0x451: info.family = Family::kStm32; info.name = "STM32F76x/F77x"; break;
                // --- STM32G4 ---
                case 0x468: info.family = Family::kStm32; info.name = "STM32G431/G441"; break;
                case 0x469: info.family = Family::kStm32; info.name = "STM32G471/G473/G474/G483/G484"; break;
                case 0x479: info.family = Family::kStm32; info.name = "STM32G491/G4A1"; break;
                // --- STM32H7 ---
                case 0x450: info.family = Family::kStm32; info.name = "STM32H74x/H75x"; break;
                case 0x480: info.family = Family::kStm32; info.name = "STM32H7A3/H7B3"; break;

                // --- GD32F1 (compatible with STM32F1 DEV_IDs but different REV_ID) ---
                // GD32F103 uses same DEV_ID 0x410 but REV_ID=0x1303
                // Handled above (STM32F103), GD32 uses same Flash controller → OK
                // But some GD32 have unique DEV_IDs:
                case 0x430: info.family = Family::kGd32; info.name = "GD32F303 (high)"; break;
                // GD32F303XL uses same DEV_ID 0x414 as STM32F103 high, distinguished by REV_ID below

                // --- CH32F103 uses DEV_ID 0x410 same as STM32F103, Flash compatible ---
                // Distinguished by REV_ID: CH32F103 REV=0x2000
                // We handle this below with REV_ID check

                // --- APM32F1 uses DEV_ID 0x410 same as STM32F103, Flash compatible ---
                // APM32F4 uses DEV_ID 0x413/0x419, Flash compatible with STM32F4

                // --- AGM32PF100 (M4F) ---
                case 0x601: info.family = Family::kAgm32; info.name = "AGM32PF100"; break;
                case 0x602: info.family = Family::kAgm32; info.name = "AGM32PF200"; break;

                default:
                    // Unknown DEV_ID: use REV_ID to try to distinguish
                    info.family = Family::kUnknown;
                    info.name = "Unknown Cortex-M (DBGMCU DEV_ID unknown)";
                    ESP_LOGW(kTag, "Unknown DEV_ID=0x%03lx, cannot identify chip",
                             (unsigned long)info.dev_id);
                    break;
                }

                // Refine CH32/APM32 by REV_ID when DEV_ID is shared with STM32
                // CH32F103: DEV_ID=0x410, REV_ID=0x2000
                // APM32F103: DEV_ID=0x410, REV_ID=0x3000 or 0x1000
                if (info.dev_id == 0x410) {
                    if (rev_id == 0x2000) {
                        info.family = Family::kCh32;
                        info.name = "CH32F103";
                    } else if (rev_id == 0x3000 || rev_id == 0x1000) {
                        info.family = Family::kApm32;
                        info.name = "APM32F103";
                    } else if (rev_id == 0x1303 || rev_id == 0x2303) {
                        // GD32F103 uses DEV_ID=0x410, REV_ID=0x1303/0x2303
                        info.family = Family::kGd32;
                        info.name = "GD32F103";
                    }
                    // else keep as STM32F103 (default)
                }
                // APM32F405/F407: DEV_ID=0x413, REV_ID may differ
                if (info.dev_id == 0x413 && rev_id == 0x1000) {
                    info.family = Family::kApm32;
                    info.name = "APM32F405/F407";
                }
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
    case Family::kCw32: return "CW32";
    case Family::kCh32: return "CH32";
    case Family::kApm32: return "APM32";
    case Family::kAgm32: return "AGM32";
    default: return "Unknown";
    }
}

} // namespace target_probe
