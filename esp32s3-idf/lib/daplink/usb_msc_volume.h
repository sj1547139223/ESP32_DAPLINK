#pragma once

#include <string>

#include "esp_err.h"

namespace usb_msc_volume {

esp_err_t init();
esp_err_t reset_volume();
esp_err_t write_status_files(const std::string &status, const std::string &details, bool success);
void scan_and_process_once();
const char *base_path();

// Temporarily switch to APP mount, call func, switch back to USB.
// Used by wifi_debug to read config files at startup.
esp_err_t with_app_mount(void (*func)(void *ctx), void *ctx);

// Find the first .FLM file in /usb/algo/ directory.
// Must be called while APP mount is active (inside with_app_mount callback).
std::string find_flm_file();

} // namespace usb_msc_volume
