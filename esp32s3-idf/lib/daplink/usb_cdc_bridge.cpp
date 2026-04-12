#include "usb_cdc_bridge.h"

#include <cstring>
#include <cstdio>

#include "board_config.h"
#include "driver/uart.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tinyusb_cdc_acm.h"
#include "wifi_debug.h"

namespace {

constexpr size_t kUsbRxBufferSize = 128;
constexpr size_t kUartRxBufferSize = 256;
constexpr size_t kTaskDelayMs = 10;

const char *kTag = "usb_cdc_bridge";
TaskHandle_t g_task_handle = nullptr;
volatile bool g_uart_paused = false;
volatile bool g_info_printed = false;
volatile bool g_print_info_pending = false;

void apply_line_coding(const cdc_line_coding_t *coding)
{
    if (coding == nullptr) {
        return;
    }

    uart_config_t uart_cfg = {
        .baud_rate = static_cast<int>(coding->bit_rate),
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 0,
        .source_clk = UART_SCLK_DEFAULT,
        .flags = {},
    };

    switch (coding->data_bits) {
    case 5: uart_cfg.data_bits = UART_DATA_5_BITS; break;
    case 6: uart_cfg.data_bits = UART_DATA_6_BITS; break;
    case 7: uart_cfg.data_bits = UART_DATA_7_BITS; break;
    default: uart_cfg.data_bits = UART_DATA_8_BITS; break;
    }

    switch (coding->parity) {
    case 1: uart_cfg.parity = UART_PARITY_ODD; break;
    case 2: uart_cfg.parity = UART_PARITY_EVEN; break;
    default: uart_cfg.parity = UART_PARITY_DISABLE; break;
    }

    switch (coding->stop_bits) {
    case 2: uart_cfg.stop_bits = UART_STOP_BITS_2; break;
    default: uart_cfg.stop_bits = UART_STOP_BITS_1; break;
    }

    ESP_ERROR_CHECK(uart_param_config(board_config::kBridgeUart, &uart_cfg));
}

void cdc_rx_callback(int itf, cdcacm_event_t *event)
{
    (void)event;
    uint8_t buffer[kUsbRxBufferSize];
    size_t rx_size = 0;
    const tinyusb_cdcacm_itf_t port = static_cast<tinyusb_cdcacm_itf_t>(itf);
    if (tinyusb_cdcacm_read(port, buffer, sizeof(buffer), &rx_size) == ESP_OK && rx_size > 0) {
        uart_write_bytes(board_config::kBridgeUart, buffer, rx_size);
    }
}

void cdc_line_coding_changed_callback(int itf, cdcacm_event_t *event)
{
    (void)itf;
    apply_line_coding(event->line_coding_changed_data.p_line_coding);
}

void cdc_line_state_changed_callback(int itf, cdcacm_event_t *event)
{
    (void)itf;
    bool dtr = event->line_state_changed_data.dtr;
    if (dtr) {
        g_print_info_pending = true;
    }
}

void task_entry(void *)
{
    uint8_t uart_buffer[kUartRxBufferSize];
    while (true) {
        // Deferred print of WiFi info (triggered by DTR or WiFi connect)
        if (g_print_info_pending) {
            g_print_info_pending = false;
            // Small delay to let USB host start polling
            vTaskDelay(pdMS_TO_TICKS(100));
            if (wifi_debug::is_configured()) {
                const char *code = wifi_debug::get_device_code();
                const char *ip = wifi_debug::get_ip_str();
                char msg[128];
                int len;
                if (ip[0] != '\0') {
                    len = snprintf(msg, sizeof(msg),
                                   "\r\n[DAPLink] IP: %s  Code: %s\r\n", ip, code);
                } else {
                    len = snprintf(msg, sizeof(msg),
                                   "\r\n[DAPLink] WiFi connecting...  Code: %s\r\n", code);
                }
                if (len > 0) {
                    tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0,
                                               (const uint8_t *)msg, (size_t)len);
                    tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, 0);
                }
            }
        }

        if (!g_uart_paused) {
            const int read = uart_read_bytes(board_config::kBridgeUart, uart_buffer, sizeof(uart_buffer), pdMS_TO_TICKS(kTaskDelayMs));
            if (read > 0) {
                tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, uart_buffer, static_cast<size_t>(read));
                tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, 0);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(kTaskDelayMs));
    }
}

} // namespace

namespace usb_cdc_bridge {

esp_err_t init()
{
    const uart_config_t uart_cfg = {
        .baud_rate = board_config::kBridgeDefaultBaudRate,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 0,
        .source_clk = UART_SCLK_DEFAULT,
        .flags = {},
    };

    ESP_RETURN_ON_ERROR(uart_driver_install(board_config::kBridgeUart, 2048, 0, 0, nullptr, 0), kTag, "uart install failed");
    ESP_RETURN_ON_ERROR(uart_param_config(board_config::kBridgeUart, &uart_cfg), kTag, "uart config failed");
    ESP_RETURN_ON_ERROR(uart_set_pin(board_config::kBridgeUart, board_config::kBridgeTxPin, board_config::kBridgeRxPin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE), kTag, "uart pin config failed");

    const tinyusb_config_cdcacm_t acm_cfg = {
        .cdc_port = TINYUSB_CDC_ACM_0,
        .callback_rx = &cdc_rx_callback,
        .callback_rx_wanted_char = nullptr,
        .callback_line_state_changed = &cdc_line_state_changed_callback,
        .callback_line_coding_changed = &cdc_line_coding_changed_callback,
    };
    ESP_RETURN_ON_ERROR(tinyusb_cdcacm_init(&acm_cfg), kTag, "cdc init failed");

    if (g_task_handle == nullptr) {
        xTaskCreate(task_entry, "usb_cdc_bridge", 4096, nullptr, 5, &g_task_handle);
    }

    ESP_LOGI(kTag, "CDC bridge ready on UART1 TX=%d RX=%d", board_config::kBridgeTxPin, board_config::kBridgeRxPin);
    return ESP_OK;
}

void task()
{
}

void set_uart_paused(bool paused)
{
    g_uart_paused = paused;
}

void print_wifi_info()
{
    g_print_info_pending = true;
}

esp_err_t init_rtt_cdc()
{
    // RTT CDC removed: ESP32-S3 only supports 5 IN endpoints (EP0-EP4)
    // RTT is available via WiFi TCP instead
    ESP_LOGI(kTag, "RTT CDC disabled (endpoint limit), use WiFi RTT");
    return ESP_OK;
}

void rtt_write(const uint8_t *data, size_t length)
{
    // RTT CDC removed (endpoint limit), WiFi RTT handles this
    (void)data;
    (void)length;
}

size_t rtt_read(uint8_t *buffer, size_t buffer_size)
{
    // RTT CDC removed (endpoint limit), WiFi RTT handles this
    (void)buffer;
    (void)buffer_size;
    return 0;
}

} // namespace usb_cdc_bridge
