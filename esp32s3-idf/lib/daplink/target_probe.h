#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "esp_err.h"

namespace target_probe {

enum class Family {
    kUnknown,
    kStm32,
    kNordic,
    kNxp,
    kGd32,    // GigaDevice GD32 series
    kCw32,    // Wuhan CW32 series
    kCh32,    // WCH CH32 Cortex-M series
    kApm32,   // Geehy APM32 series
    kAgm32,   // AGM AGM32 series
};

struct TargetInfo {
    Family family = Family::kUnknown;
    std::string name;
    uint32_t dpidr = 0;
    uint32_t idcode = 0;
    uint32_t dev_id = 0;   // STM32 DBGMCU DEV_ID (bits [11:0])
    bool attached = false;
};

esp_err_t probe(TargetInfo &info);
const char *family_name(Family family);

} // namespace target_probe
