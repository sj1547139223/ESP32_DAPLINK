#include "app_main.h"

#include "board_config.h"
#include "cmsis_dap.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tinyusb.h"
#include "tusb.h"
#include "usb_cdc_bridge.h"
#include "usb_descriptors.h"
#include "usb_msc_volume.h"
#include "wifi_debug.h"
#include "rtt_reader.h"

namespace {
const char *kTag = "app_main";
const char *kVersion = "1.1.0";

// RTT polling task
void rtt_task(void *arg)
{
    uint8_t rtt_buf[256];
    int search_backoff = 0;

    while (true) {
        // Auto-enable RTT when WiFi RTT is active
        bool rtt_wanted = wifi_debug::is_connected();
        if (rtt_wanted != rtt_reader::is_enabled()) {
            rtt_reader::set_enabled(rtt_wanted);
        }

        // Only poll RTT when enabled and SWD target is connected
        if (!rtt_reader::is_enabled() || !cmsis_dap::is_connected()) {
            if (rtt_reader::is_found()) {
                rtt_reader::reset();
            }
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        // Pause RTT search while DAP is busy, but allow lightweight polling
        // (Keil continuously sends status polls → is_dap_busy stays true during debug)
        if (!rtt_reader::is_found() && cmsis_dap::is_dap_busy()) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        // Backoff between RTT search attempts (avoid SWD bus spam)
        if (!rtt_reader::is_found()) {
            if (search_backoff > 0) {
                search_backoff--;
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            search_backoff = 200;  // ~2s between search attempts
        }

        // Poll RTT up channel: target → host
        size_t n = rtt_reader::poll_up(rtt_buf, sizeof(rtt_buf));
        if (n > 0) {
            // Send to USB CDC port 1
            usb_cdc_bridge::rtt_write(rtt_buf, n);
            // Send to WiFi TCP
            wifi_debug::rtt_send(rtt_buf, n);
        }

        // RTT down channel: host → target (from USB CDC)
        size_t usb_n = usb_cdc_bridge::rtt_read(rtt_buf, sizeof(rtt_buf));
        if (usb_n > 0) {
            rtt_reader::write_down(rtt_buf, usb_n);
        }

        // RTT down channel: host → target (from WiFi TCP)
        size_t wifi_n = wifi_debug::rtt_recv(rtt_buf, sizeof(rtt_buf));
        if (wifi_n > 0) {
            rtt_reader::write_down(rtt_buf, wifi_n);
        }

        vTaskDelay(pdMS_TO_TICKS(50));  // 50ms RTT polling interval (lightweight: only reads WrOff when idle)
    }
}

}

extern "C" void app_main(void)
{
    ESP_LOGI(kTag, "Starting ESP32-S3 DAPLink v%s", kVersion);

    board_config::configure_swd_gpio();
    board_config::configure_led_gpio();
    ESP_ERROR_CHECK(tinyusb_driver_install(usb_descriptors::config()));
    ESP_ERROR_CHECK(usb_cdc_bridge::init());
    ESP_ERROR_CHECK(usb_cdc_bridge::init_rtt_cdc());
    ESP_ERROR_CHECK(usb_msc_volume::init());
    ESP_ERROR_CHECK(cmsis_dap::init());
    ESP_ERROR_CHECK(rtt_reader::init());

    // Initialize WiFi debug (reads /usb/wifi.txt, starts WiFi if configured)
    // Must be after usb_msc_volume::init() so FAT filesystem is mounted
    ESP_ERROR_CHECK(wifi_debug::init());

    // RTT polling task (currently disabled — uncomment to enable)
    // xTaskCreatePinnedToCore(rtt_task, "rtt_poll", 4096, nullptr,
    //                         4, nullptr, 0);  // Core 0, priority 4

    // LED indicator state
    uint32_t led_tick = 0;
    bool led_blink_state = false;

    while (true) {
        usb_msc_volume::scan_and_process_once();
        cmsis_dap::poll();

        // LED indicator logic (every 100ms)
        // Red: always on (overridden during programming/debugging)
        // Green: slow blink = WiFi connected, off = WiFi disconnected
        // Both: fast alternating = programming or CMSIS-DAP debugging
        led_tick++;
        const auto activity = board_config::get_activity_state();
        if (activity != board_config::ActivityState::kIdle || cmsis_dap::is_connected()) {
            // Programming or CMSIS-DAP debug session active: fast alternating red/green blink (200ms)
            bool phase = (led_tick % 2) == 0;
            board_config::set_led_red(phase);
            board_config::set_led_green(!phase);
        } else {
            // Idle: red always on
            board_config::set_led_red(true);
            if (wifi_debug::is_connected()) {
                // WiFi connected: green slow blink (500ms)
                if (led_tick % 5 == 0) {
                    led_blink_state = !led_blink_state;
                    board_config::set_led_green(led_blink_state);
                }
            } else {
                // WiFi not connected: green off
                board_config::set_led_green(false);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
