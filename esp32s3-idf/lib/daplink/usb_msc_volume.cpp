#include "usb_msc_volume.h"

#include <dirent.h>
#include <sys/stat.h>
#include <sys/unistd.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "board_config.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_vfs_fat.h"
#include "flash_algo.h"
#include "hex_parser.h"
#include "target_probe.h"
#include "tinyusb_msc.h"
#include "wear_levelling.h"

namespace {

const char *kTag = "usb_msc_volume";
wl_handle_t g_wl_handle = WL_INVALID_HANDLE;
tinyusb_msc_storage_handle_t g_storage_handle = nullptr;
bool g_storage_ready = false;
bool g_driver_ready = false;

// Write-idle detection
uint32_t g_last_write_seen = 0;      // Last write timestamp we've seen
uint32_t g_write_idle_start = 0;     // When write activity stopped
constexpr uint32_t kWriteIdleMs = 2000;  // 2 seconds of no writes → trigger programming

bool has_hex_extension(const char *name)
{
    const char *ext = std::strrchr(name, '.');
    return ext != nullptr && strcasecmp(ext, ".hex") == 0;
}

bool has_flm_extension(const char *name)
{
    const char *ext = std::strrchr(name, '.');
    return ext != nullptr && (strcasecmp(ext, ".flm") == 0 || strcasecmp(ext, ".FLM") == 0);
}

constexpr const char *kAlgoDir = "/usb/algo";

// Default SRAM parameters for common MCU families
constexpr uint32_t kDefaultRamBase = 0x20000000;
constexpr uint32_t kDefaultRamSize = 8 * 1024;

std::string join_path(const char *base, const char *name)
{
    std::string result(base);
    result += "/";
    result += name;
    return result;
}

} // namespace

esp_err_t ensure_msc_driver()
{
    if (g_driver_ready) {
        return ESP_OK;
    }

    const tinyusb_msc_driver_config_t driver_config = {
        .user_flags = {
            .val = 0,
        },
        .callback = nullptr,
        .callback_arg = nullptr,
    };
    ESP_RETURN_ON_ERROR(tinyusb_msc_install_driver(&driver_config), kTag, "msc driver install failed");
    g_driver_ready = true;
    return ESP_OK;
}

esp_err_t mount_partition()
{
    if (g_storage_ready) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(ensure_msc_driver(), kTag, "msc driver setup failed");

    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA,
        static_cast<esp_partition_subtype_t>(0x81),
        board_config::kMscPartitionLabel);

    if (partition == nullptr) {
        ESP_LOGE(kTag, "Partition '%s' not found", board_config::kMscPartitionLabel);
        return ESP_ERR_NOT_FOUND;
    }

    // Initialize wear levelling
    esp_err_t wl_err = wl_mount(partition, &g_wl_handle);
    if (wl_err != ESP_OK) {
        ESP_LOGE(kTag, "wl_mount failed: %s (0x%x)", esp_err_to_name(wl_err), wl_err);
        return wl_err;
    }
    ESP_LOGI(kTag, "Wear levelling initialized, wl_handle=%lu", static_cast<unsigned long>(g_wl_handle));

    tinyusb_msc_storage_config_t storage_config = {
        .medium = {
            .wl_handle = g_wl_handle,
        },
        .fat_fs = {
            .base_path = const_cast<char *>(board_config::kUsbMountPath),
            .config = {
                .max_files = 8,
            },
            .do_not_format = false,
            .format_flags = FM_ANY,
        },
        .mount_point = TINYUSB_MSC_STORAGE_MOUNT_USB,  // Start with USB ownership
    };

    esp_err_t storage_err = tinyusb_msc_new_storage_spiflash(&storage_config, &g_storage_handle);
    if (storage_err != ESP_OK) {
        ESP_LOGE(kTag, "tinyusb_msc_new_storage_spiflash failed: %s (0x%x)", esp_err_to_name(storage_err), storage_err);
        wl_unmount(g_wl_handle);
        return storage_err;
    }
    ESP_LOGI(kTag, "MSC storage created successfully");
    g_storage_ready = true;
    return ESP_OK;
}

// storage_available_to_app() removed — write-idle detection now handles ownership switching

void write_text_file(const char *path, const std::string &content)
{
    FILE *file = fopen(path, "w");
    if (file == nullptr) {
        ESP_LOGE(kTag, "failed to open %s", path);
        return;
    }
    fwrite(content.data(), 1, content.size(), file);
    fclose(file);
}

void delete_hex_files()
{
    DIR *dir = opendir(board_config::kUsbMountPath);
    if (dir == nullptr) {
        return;
    }

    while (dirent *entry = readdir(dir)) {
        if (entry->d_type == DT_DIR) {
            continue;
        }
        if (has_hex_extension(entry->d_name)) {
            unlink(join_path(board_config::kUsbMountPath, entry->d_name).c_str());
        }
    }
    closedir(dir);
}

namespace usb_msc_volume {

std::string find_flm_file()
{
    DIR *dir = opendir(kAlgoDir);
    if (dir == nullptr) return {};

    std::string result;
    while (dirent *entry = readdir(dir)) {
        if (entry->d_type == DT_DIR) continue;
        if (has_flm_extension(entry->d_name)) {
            result = join_path(kAlgoDir, entry->d_name);
            break;
        }
    }
    closedir(dir);
    return result;
}

esp_err_t init()
{
    ESP_RETURN_ON_ERROR(mount_partition(), kTag, "mount partition failed");
    ESP_LOGI(kTag, "MSC volume initialized successfully");
    return ESP_OK;
}

esp_err_t reset_volume()
{
    delete_hex_files();
    unlink(board_config::kUsbErrorFile);
    return write_status_files("READY", "Volume reset after programming.\n", true);
}

esp_err_t with_app_mount(void (*func)(void *ctx), void *ctx)
{
    if (!g_storage_ready || g_storage_handle == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t ret = tinyusb_msc_set_storage_mount_point(g_storage_handle,
                                                         TINYUSB_MSC_STORAGE_MOUNT_APP);
    if (ret != ESP_OK) {
        return ret;
    }
    func(ctx);
    tinyusb_msc_set_storage_mount_point(g_storage_handle, TINYUSB_MSC_STORAGE_MOUNT_USB);
    return ESP_OK;
}

esp_err_t write_status_files(const std::string &status, const std::string &details, bool success)
{
    const std::string html = "<html><body><h1>" + status + "</h1><pre>" + details + "</pre></body></html>\n";
    write_text_file(board_config::kUsbIndexFile, html);
    if (success) {
        write_text_file(board_config::kUsbStatusFile, details);
        unlink(board_config::kUsbErrorFile);
    } else {
        write_text_file(board_config::kUsbErrorFile, details);
    }
    return ESP_OK;
}

void scan_and_process_once()
{
    if (!g_storage_ready || g_storage_handle == nullptr) {
        return;
    }

    // Check if we're in USB mode and detect write-idle
    tinyusb_msc_mount_point_t mp = TINYUSB_MSC_STORAGE_MOUNT_USB;
    tinyusb_msc_get_storage_mount_point(g_storage_handle, &mp);

    if (mp == TINYUSB_MSC_STORAGE_MOUNT_USB) {
        // Track write activity for auto-trigger
        uint32_t last_write = tinyusb_msc_last_write_ms();
        if (last_write > 0 && last_write != g_last_write_seen) {
            // New write activity detected
            g_last_write_seen = last_write;
            g_write_idle_start = esp_log_timestamp();
        }
        if (g_write_idle_start > 0 && (esp_log_timestamp() - g_write_idle_start) > kWriteIdleMs) {
            // No writes for kWriteIdleMs — switch to APP to scan for hex files
            ESP_LOGI(kTag, "Write idle detected, switching to APP to scan");
            g_write_idle_start = 0;
            if (tinyusb_msc_set_storage_mount_point(g_storage_handle, TINYUSB_MSC_STORAGE_MOUNT_APP) != ESP_OK) {
                ESP_LOGW(kTag, "Failed to switch to APP mount");
                return;
            }
            // Fall through to scan
        } else {
            return;  // Still in USB mode, wait
        }
    }

    // APP mode: scan for hex files
    DIR *dir = opendir(board_config::kUsbMountPath);
    if (dir == nullptr) {
        // No hex files or can't open dir — switch back to USB
        tinyusb_msc_set_storage_mount_point(g_storage_handle, TINYUSB_MSC_STORAGE_MOUNT_USB);
        return;
    }

    bool found_hex = false;
    while (dirent *entry = readdir(dir)) {
        if (entry->d_type == DT_DIR || !has_hex_extension(entry->d_name)) {
            continue;
        }

        found_hex = true;
        ESP_LOGI(kTag, "Found hex file: %s", entry->d_name);

        const std::string hex_path = join_path(board_config::kUsbMountPath, entry->d_name);
        hex_parser::ParsedHexImage image;
        std::string parse_error;
        if (parse_file(hex_path.c_str(), image, parse_error) != ESP_OK) {
            ESP_LOGE(kTag, "Hex parse failed: %s", parse_error.c_str());
            write_status_files("PARSE FAILED", parse_error + "\n", false);
            continue;
        }

        target_probe::TargetInfo target;
        if (target_probe::probe(target) != ESP_OK) {
            ESP_LOGE(kTag, "Target probe failed");
            write_status_files("PROBE FAILED", "Unable to probe target over SWD.\n", false);
            continue;
        }

        const flash_algo::SelectionResult selection = flash_algo::select_algorithm(image, target);
        board_config::set_activity_state(board_config::ActivityState::kProgramming);
        esp_err_t prog_err = flash_algo::program_target(selection, target, image.segments);

        // If built-in algorithm failed or unsupported, try FLM file
        if (prog_err != ESP_OK) {
            std::string flm_path = find_flm_file();
            if (!flm_path.empty()) {
                ESP_LOGI(kTag, "Built-in algorithm failed, trying FLM: %s", flm_path.c_str());
                // Determine RAM size based on known devices
                uint32_t ram_size = kDefaultRamSize;
                uint32_t dev = target.dev_id;
                // STM32G4: 32-128KB SRAM
                if (dev == 0x468 || dev == 0x469 || dev == 0x479) ram_size = 32 * 1024;
                // STM32F1: 20-96KB SRAM
                else if (dev == 0x410 || dev == 0x414) ram_size = 20 * 1024;
                // STM32F4: 128-256KB SRAM
                else if (dev == 0x413 || dev == 0x431 || dev == 0x441) ram_size = 64 * 1024;

                prog_err = flash_algo::program_with_flm(flm_path.c_str(), target,
                                                         image.segments, kDefaultRamBase, ram_size);
            }
        }

        board_config::set_activity_state(board_config::ActivityState::kIdle);
        if (prog_err != ESP_OK) {
            ESP_LOGE(kTag, "Programming failed: %s", esp_err_to_name(prog_err));
            write_status_files("PROGRAM FAILED",
                               std::string("Algorithm: ") + selection.algorithm_name + "\nTarget: " + target.name + "\n",
                               false);
            continue;
        }

        ESP_LOGI(kTag, "Programming succeeded! Deleting hex files.");
        delete_hex_files();
        write_status_files("PROGRAM OK",
                           std::string("Target family: ") + target_probe::family_name(selection.family) +
                               "\nAlgorithm: " + selection.algorithm_name + "\nFile: " + entry->d_name + "\n",
                           true);
        break;
    }

    closedir(dir);

    if (!found_hex) {
        ESP_LOGD(kTag, "No hex files found on volume");
    }

    // Switch back to USB so the drive reappears on the host
    tinyusb_msc_set_storage_mount_point(g_storage_handle, TINYUSB_MSC_STORAGE_MOUNT_USB);
}

const char *base_path()
{
    return board_config::kUsbMountPath;
}

} // namespace usb_msc_volume
