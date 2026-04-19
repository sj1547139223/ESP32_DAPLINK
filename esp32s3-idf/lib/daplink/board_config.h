#pragma once

#include "driver/gpio.h"
#include "driver/uart.h"

namespace board_config {

constexpr gpio_num_t kSwclkPin = GPIO_NUM_7;
constexpr gpio_num_t kSwdInputPin = GPIO_NUM_5;
constexpr gpio_num_t kSwdOutputPin = GPIO_NUM_6;
constexpr gpio_num_t kNrstPin = GPIO_NUM_3;
constexpr gpio_num_t kLedRedPin = GPIO_NUM_38;
constexpr gpio_num_t kLedGreenPin = GPIO_NUM_21;
constexpr uart_port_t kBridgeUart = UART_NUM_1;
constexpr gpio_num_t kBridgeTxPin = GPIO_NUM_17;
constexpr gpio_num_t kBridgeRxPin = GPIO_NUM_18;
constexpr int kBridgeDefaultBaudRate = 115200;
constexpr const char *kUsbMountPath = "/usb";
constexpr const char *kUsbStatusFile = "/usb/DETAILS.TXT";
constexpr const char *kUsbErrorFile = "/usb/FAIL.TXT";
constexpr const char *kUsbIndexFile = "/usb/INDEX.HTM";
constexpr const char *kMscPartitionLabel = "mscfat";

constexpr uint16_t kCmsisDapReportSize = 64;
constexpr uint16_t kUsbVendorId = APP_USB_VID;
constexpr uint16_t kUsbProductId = APP_USB_PID;

void configure_swd_gpio();
void configure_led_gpio();
void set_led_red(bool on);
void set_led_green(bool on);

// Activity state for LED indication
enum class ActivityState {
    kIdle,        // No activity
    kProgramming, // Flash programming in progress
    kDebugging,   // CMSIS-DAP debug session active
};

void set_activity_state(ActivityState state);
ActivityState get_activity_state();

} // namespace board_config
