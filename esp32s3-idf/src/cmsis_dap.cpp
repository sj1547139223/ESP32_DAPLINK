#include "cmsis_dap.h"

#include <array>
#include <cstring>

#include "board_config.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "swd.h"
#include "tusb.h"

namespace {
const char *kTag = "cmsis_dap";
constexpr size_t kMaxResponseSize = 1500;  // Support elaphureLink large packets (> 64 bytes)
std::array<uint8_t, kMaxResponseSize> g_response{};
bool g_connected = false;

// Transfer configuration (from DAP_TransferConfigure)
uint8_t g_idle_cycles = 0;
uint16_t g_wait_retry = 100;   // default reasonable retry count
uint16_t g_match_retry = 0;
uint32_t g_match_mask = 0xFFFFFFFF;  // Persistent across Transfer commands (per CMSIS-DAP spec)

// Session statistics
uint32_t g_stat_xfer = 0;
uint32_t g_stat_block = 0;
uint32_t g_stat_fault = 0;
uint32_t g_stat_mismatch = 0;
uint32_t g_stat_ap_read = 0;
uint32_t g_stat_ap_write = 0;
uint32_t g_last_select = 0xFFFFFFFF;  // Track last SELECT write

constexpr uint8_t kCmdInfo = 0x00;
constexpr uint8_t kCmdHostStatus = 0x01;
constexpr uint8_t kCmdConnect = 0x02;
constexpr uint8_t kCmdDisconnect = 0x03;
constexpr uint8_t kCmdTransferConfigure = 0x04;
constexpr uint8_t kCmdTransfer = 0x05;
constexpr uint8_t kCmdTransferBlock = 0x06;
constexpr uint8_t kCmdTransferAbort = 0x07;
constexpr uint8_t kCmdWriteAbort = 0x08;
constexpr uint8_t kCmdDelay = 0x09;
constexpr uint8_t kCmdResetTarget = 0x0A;
constexpr uint8_t kCmdSwjPins = 0x10;
constexpr uint8_t kCmdSwjClock = 0x11;
constexpr uint8_t kCmdSwjSequence = 0x12;
constexpr uint8_t kCmdSwdConfigure = 0x13;
constexpr uint8_t kCmdJtagSequence = 0x14;
constexpr uint8_t kCmdJtagConfigure = 0x15;
constexpr uint8_t kCmdJtagIdcode = 0x16;

constexpr uint8_t kDapPortDisabled = 0x00;
constexpr uint8_t kDapPortSwd = 0x01;
constexpr uint8_t kTransferApnDp = 1U << 0;
constexpr uint8_t kTransferRnW = 1U << 1;
constexpr uint8_t kTransferA2 = 1U << 2;
constexpr uint8_t kTransferA3 = 1U << 3;
constexpr uint8_t kTransferMatchValue = 1U << 4;
constexpr uint8_t kTransferMatchMask = 1U << 5;
constexpr uint8_t kTransferTimestamp = 1U << 7;

uint32_t g_tx_fail_count = 0;
bool g_suppress_usb_send = false;  // When true, send_response only caches, no USB write

void send_response(size_t length)
{
    if (length > g_response.size()) {
        length = g_response.size();
    }

    // Cache for later retrieval
    cmsis_dap::g_last_dap_response_length = length;
    std::memcpy(cmsis_dap::g_last_dap_response.data(), g_response.data(), length);

    if (g_suppress_usb_send) {
        return;  // WiFi path: caller reads from g_last_dap_response
    }

    // Send via vendor bulk IN endpoint
    uint32_t written = tud_vendor_write(g_response.data(), static_cast<uint32_t>(length));
    tud_vendor_flush();
    if (written != length) {
        g_tx_fail_count++;
        ESP_LOGW(kTag, "TX FAIL #%lu: tried=%d wrote=%lu", (unsigned long)g_tx_fail_count,
                 (int)length, (unsigned long)written);
    }
    ESP_LOGD(kTag, "send_response: len=%d wrote=%lu data=%02x%02x%02x%02x%02x%02x%02x%02x",
             (int)length, (unsigned long)written,
             g_response[0], g_response[1], g_response[2], g_response[3],
             g_response[4], g_response[5], g_response[6], g_response[7]);
}

void clear_response(uint8_t command)
{
    std::memset(g_response.data(), 0, g_response.size());
    g_response[0] = command;
}

void put_u32_le(size_t offset, uint32_t value)
{
    if (offset + 4 > g_response.size()) {
        return;
    }
    g_response[offset + 0] = static_cast<uint8_t>(value & 0xFFU);
    g_response[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFFU);
    g_response[offset + 2] = static_cast<uint8_t>((value >> 16) & 0xFFU);
    g_response[offset + 3] = static_cast<uint8_t>((value >> 24) & 0xFFU);
}

uint32_t get_u32_le(const uint8_t *buffer, size_t offset, size_t length)
{
    if (buffer == nullptr || offset + 4 > length) {
        return 0;
    }
    return static_cast<uint32_t>(buffer[offset]) |
           (static_cast<uint32_t>(buffer[offset + 1]) << 8) |
           (static_cast<uint32_t>(buffer[offset + 2]) << 16) |
           (static_cast<uint32_t>(buffer[offset + 3]) << 24);
}

void handle_dap_info(const uint8_t *buffer, size_t length)
{
    clear_response(kCmdInfo);
    if (length < 2) {
        g_response[1] = 0;
        send_response(2);
        return;
    }

    switch (buffer[1]) {
    case 0x01: {
        static constexpr char kVendor[] = "ESP DAPLink";
        g_response[1] = sizeof(kVendor) - 1;
        std::memcpy(&g_response[2], kVendor, sizeof(kVendor) - 1);
        send_response(2 + sizeof(kVendor) - 1);
        break;
    }
    case 0x02: {
        static constexpr char kProduct[] = "ESP32S3 CMSIS-DAP";
        g_response[1] = sizeof(kProduct) - 1;
        std::memcpy(&g_response[2], kProduct, sizeof(kProduct) - 1);
        send_response(2 + sizeof(kProduct) - 1);
        break;
    }
    case 0x03: {
        static constexpr char kSerial[] = "ESP32S3-0001";
        g_response[1] = sizeof(kSerial) - 1;
        std::memcpy(&g_response[2], kSerial, sizeof(kSerial) - 1);
        send_response(2 + sizeof(kSerial) - 1);
        break;
    }
    case 0x04: {  // CMSIS-DAP Protocol Version
        static constexpr char kFwVersion[] = "2.1.1";
        g_response[1] = sizeof(kFwVersion) - 1;
        std::memcpy(&g_response[2], kFwVersion, sizeof(kFwVersion) - 1);
        send_response(2 + sizeof(kFwVersion) - 1);
        break;
    }
    case 0x05: {
        static constexpr char kTargetVendor[] = "ARM";
        g_response[1] = sizeof(kTargetVendor) - 1;
        std::memcpy(&g_response[2], kTargetVendor, sizeof(kTargetVendor) - 1);
        send_response(2 + sizeof(kTargetVendor) - 1);
        break;
    }
    case 0x06: {
        static constexpr char kTargetName[] = "Cortex-M";
        g_response[1] = sizeof(kTargetName) - 1;
        std::memcpy(&g_response[2], kTargetName, sizeof(kTargetName) - 1);
        send_response(2 + sizeof(kTargetName) - 1);
        break;
    }
    case 0x07:  // Board Vendor (string, optional)
    case 0x08:  // Board Name (string, optional)
    case 0x09:  // Product Firmware Version (string, optional)
        g_response[1] = 0;
        send_response(2);
        break;
    case 0xF0:  // Capabilities
        g_response[1] = 1;
        g_response[2] = 0x01;  // Bit 0: SWD support
        send_response(3);
        break;
    case 0xF1:  // Test Domain Timer
        g_response[1] = 4;
        put_u32_le(2, 1000000);  // 1MHz timer
        send_response(6);
        break;
    case 0xFB:  // UART RX Buffer Size (not supported)
    case 0xFC:  // UART TX Buffer Size (not supported)
        g_response[1] = 0;
        send_response(2);
        break;
    case 0xFD:  // SWO Trace Buffer Size (not supported)
        g_response[1] = 0;
        send_response(2);
        break;
    case 0xFE:  // Packet Count
        g_response[1] = 1;
        g_response[2] = 1;  // Single packet buffering
        send_response(3);
        break;
    case 0xFF:  // Packet Size (uint16_t LE)
        g_response[1] = 2;
        g_response[2] = static_cast<uint8_t>(board_config::kCmsisDapReportSize & 0xFF);
        g_response[3] = static_cast<uint8_t>((board_config::kCmsisDapReportSize >> 8) & 0xFF);
        send_response(4);
        break;
    default:
        g_response[1] = 0;
        send_response(2);
        break;
    }
}

void handle_connect(const uint8_t *buffer, size_t length)
{
    clear_response(kCmdConnect);
    const uint8_t requested_port = (length >= 2) ? buffer[1] : kDapPortSwd;
    if (requested_port != kDapPortSwd && requested_port != 0x00) {
        // Only SWD is supported; JTAG (2) or unknown ports fail
        g_response[1] = kDapPortDisabled;
        send_response(2);
        return;
    }

    if (swd::connect() == ESP_OK) {
        g_connected = true;
        g_response[1] = kDapPortSwd;
        ESP_LOGW(kTag, "Connected [FW v13]");
    }
    send_response(2);
}

void handle_disconnect()
{
    ESP_LOGW(kTag, "=== SESSION SUMMARY v2: xfer=%lu block=%lu fault=%lu mismatch=%lu ap_r=%lu ap_w=%lu last_sel=0x%lx ===",
             (unsigned long)g_stat_xfer, (unsigned long)g_stat_block,
             (unsigned long)g_stat_fault, (unsigned long)g_stat_mismatch,
             (unsigned long)g_stat_ap_read, (unsigned long)g_stat_ap_write,
             (unsigned long)g_last_select);
    g_stat_xfer = g_stat_block = g_stat_fault = g_stat_mismatch = 0;
    g_stat_ap_read = g_stat_ap_write = 0;
    clear_response(kCmdDisconnect);
    swd::disconnect();
    g_connected = false;
    g_response[1] = 0;
    send_response(2);
}

void handle_delay(const uint8_t *buffer, size_t length)
{
    clear_response(kCmdDelay);
    const uint16_t delay_us = (length >= 3) ? static_cast<uint16_t>(buffer[1] | (buffer[2] << 8)) : 0;
    esp_rom_delay_us(delay_us);
    g_response[1] = 0;
    send_response(2);
}

void handle_swj_clock(const uint8_t *buffer, size_t length)
{
    clear_response(kCmdSwjClock);
    const uint32_t hz = get_u32_le(buffer, 1, length);
    g_response[1] = (swd::set_clock_hz(hz == 0 ? 1000000UL : hz) == ESP_OK) ? 0 : 0xFF;
    send_response(2);
}

void handle_swj_sequence(const uint8_t *buffer, size_t length)
{
    clear_response(kCmdSwjSequence);
    if (length >= 2) {
        // Ensure output is enabled for SWJ sequence
        gpio_set_direction(board_config::kSwdOutputPin, GPIO_MODE_OUTPUT);
        const uint8_t count = buffer[1] == 0 ? 256 : buffer[1];
        size_t bit_index = 0;
        for (size_t byte_index = 2; byte_index < length && bit_index < count; ++byte_index) {
            for (int bit = 0; bit < 8 && bit_index < count; ++bit, ++bit_index) {
                gpio_set_level(board_config::kSwdOutputPin, (buffer[byte_index] >> bit) & 0x1U);
                gpio_set_level(board_config::kSwclkPin, 0);
                esp_rom_delay_us(1);
                gpio_set_level(board_config::kSwclkPin, 1);
                esp_rom_delay_us(1);
            }
        }
    }
    ESP_LOGD(kTag, "SWJ_Seq bits=%d", (length >= 2) ? (buffer[1] == 0 ? 256 : buffer[1]) : 0);
    g_response[1] = 0;
    send_response(2);
}

void handle_simple_ok(uint8_t command)
{
    clear_response(command);
    g_response[1] = 0;
    send_response(2);
}

void handle_host_status(const uint8_t *buffer, size_t length)
{
    clear_response(kCmdHostStatus);
    if (length >= 3) {
        const uint8_t type = buffer[1];    // 0=Connect, 1=Running
        const uint8_t status = buffer[2];  // 0=off, 1=on
        ESP_LOGD(kTag, "Host status: type=%d status=%d", type, status);
    }
    g_response[1] = 0;
    send_response(2);
}

void handle_transfer_configure(const uint8_t *buffer, size_t length)
{
    clear_response(kCmdTransferConfigure);
    if (length >= 6) {
        g_idle_cycles = buffer[1];
        g_wait_retry = buffer[2] | (buffer[3] << 8);
        g_match_retry = buffer[4] | (buffer[5] << 8);
        // Apply to SWD layer
        swd::set_transfer_config(g_idle_cycles, g_wait_retry);
        ESP_LOGD(kTag, "Transfer config: idle=%d wait=%d match=%d", g_idle_cycles, g_wait_retry, g_match_retry);
    }
    g_response[1] = 0;
    send_response(2);
}

void handle_swd_configure(const uint8_t *buffer, size_t length)
{
    clear_response(kCmdSwdConfigure);
    if (length >= 2) {
        const uint8_t config = buffer[1];
        // Bits 1:0 = Turnaround clock period (0=1, 1=2, 2=3, 3=4 cycles)
        const uint8_t trn = (config & 0x03) + 1;
        // Bit 2 = DataPhase (0=no data phase on FAULT/WAIT, 1=always generate)
        const bool data_phase = (config & 0x04) != 0;
        swd::set_swd_config(trn, data_phase);
        ESP_LOGD(kTag, "SWD config: raw=0x%02x turnaround=%d data_phase=%d", config, trn, data_phase);
    }
    g_response[1] = 0;
    send_response(2);
}

void handle_swj_pins(const uint8_t *buffer, size_t length)
{
    clear_response(kCmdSwjPins);
    if (length >= 7) {
        const uint8_t pin_output = buffer[1];
        const uint8_t pin_select = buffer[2];
        const uint32_t wait_us = get_u32_le(buffer, 3, length);

        // Set selected pin outputs
        if (pin_select & (1 << 0)) {  // SWCLK
            gpio_set_level(board_config::kSwclkPin, (pin_output >> 0) & 1);
        }
        if (pin_select & (1 << 1)) {  // SWDIO
            gpio_set_level(board_config::kSwdOutputPin, (pin_output >> 1) & 1);
        }
        // nRESET (bit 7): hardware reset via GPIO3 + software reset via AIRCR
        if (pin_select & (1 << 7)) {
            if (!(pin_output & (1 << 7))) {
                // nRESET asserted LOW: drive hardware pin + software SYSRESETREQ
                gpio_set_level(board_config::kNrstPin, 0);
                // Also trigger SYSRESETREQ via AIRCR (for targets without nRST wired)
                swd::write_dp(0x08, 0x00000000);       // SELECT = 0
                swd::raw_transfer(true, false, 0x04, 0xE000ED0C);  // TAR = AIRCR
                swd::raw_transfer(true, false, 0x0C, 0x05FA0004);  // DRW = VECTKEY|SYSRESETREQ
                ESP_LOGW(kTag, "nRESET assert LOW (HW GPIO%d + SW AIRCR)", board_config::kNrstPin);
            } else {
                // nRESET deasserted HIGH
                gpio_set_level(board_config::kNrstPin, 1);
                ESP_LOGW(kTag, "nRESET deassert HIGH (GPIO%d)", board_config::kNrstPin);
            }
        }

        // Wait for pin state or timeout
        if (wait_us > 0) {
            esp_rom_delay_us(wait_us > 3000000 ? 3000000 : wait_us);
        }

        // Read current pin states
        uint8_t pin_input = 0;
        if (gpio_get_level(board_config::kSwclkPin)) {
            pin_input |= (1 << 0);  // SWCLK bit
        }
        if (gpio_get_level(board_config::kSwdInputPin)) {
            pin_input |= (1 << 1);  // SWDIO bit
        }
        if (gpio_get_level(board_config::kNrstPin)) {
            pin_input |= (1 << 7);  // nRESET bit (read actual GPIO)
        }

        g_response[1] = pin_input;
    } else {
        g_response[1] = 0;
    }
    send_response(2);
}

void handle_reset_target(const uint8_t *buffer, size_t length)
{
    clear_response(kCmdResetTarget);
    g_response[1] = 0;  // Execute status (0=OK)
    g_response[2] = 0;  // Response data
    send_response(3);
}


void handle_transfer(const uint8_t *buffer, size_t length)
{
    g_stat_xfer++;
    clear_response(kCmdTransfer);
    if (length < 3) {
        g_response[1] = 0;
        g_response[2] = 0xFF;
        send_response(3);
        return;
    }

    const uint8_t transfer_count = buffer[2];

    // Log Transfer command summary: count + first few request types
    {
        char summary[64] = {};
        size_t pos = 0;
        size_t scan = 3;
        for (uint8_t i = 0; i < transfer_count && scan < length && pos < sizeof(summary) - 8; ++i) {
            uint8_t req = buffer[scan++];
            if (req & kTransferMatchMask) { pos += snprintf(summary + pos, sizeof(summary) - pos, "MM "); scan += 4; continue; }
            const bool is_ap = req & kTransferApnDp;
            const bool is_rd = req & kTransferRnW;
            const uint8_t a = req & (kTransferA2 | kTransferA3);
            if (req & kTransferMatchValue) {
                pos += snprintf(summary + pos, sizeof(summary) - pos, "MV%s%02x ", is_ap?"A":"D", a);
                scan += 4;
            } else if (is_rd) {
                pos += snprintf(summary + pos, sizeof(summary) - pos, "R%s%02x ", is_ap?"A":"D", a);
            } else {
                pos += snprintf(summary + pos, sizeof(summary) - pos, "W%s%02x ", is_ap?"A":"D", a);
                scan += 4;
            }
        }
        ESP_LOGD(kTag, "Xfer[%d] %s", transfer_count, summary);
    }

    size_t request_offset = 3;
    size_t response_offset = 3;
    uint8_t completed = 0;
    uint8_t transfer_status = 1;
    bool post_read = false;
    bool check_write = false;

    for (uint8_t index = 0; index < transfer_count && request_offset < length; ++index) {
        const uint8_t request = buffer[request_offset++];

        // Handle Match Mask: store mask, no actual transfer (not counted)
        if (request & kTransferMatchMask) {
            if (request_offset + 4 > length) { transfer_status = 0x04; break; }
            g_match_mask = get_u32_le(buffer, request_offset, length);
            request_offset += 4;
            // MatchMask request is considered a completed request in Transfer Count.
            ++completed;
            continue;
        }

        const bool ap = (request & kTransferApnDp) != 0;
        const bool read = (request & kTransferRnW) != 0;
        const uint8_t addr = request & (kTransferA2 | kTransferA3);

        // Handle Match Value: read-and-compare loop
        if (request & kTransferMatchValue) {
            if (request_offset + 4 > length) { transfer_status = 0x04; break; }
            const uint32_t match_value = get_u32_le(buffer, request_offset, length);
            request_offset += 4;

            // Flush pending AP read if needed
            if (post_read) {
                auto rdbuf = swd::read_dp(0x0C);
                if (rdbuf.error != ESP_OK) {
                    post_read = false;
                    transfer_status = (rdbuf.ack == 0x2) ? 0x02 : 0x04;
                    break;
                }
                put_u32_le(response_offset, rdbuf.value);
                response_offset += 4;
                ++completed;
                post_read = false;
            }

            bool matched = false;
            uint32_t last_read_val = 0;
            for (uint16_t retry = 0; retry <= g_match_retry; ++retry) {
                auto result = swd::raw_transfer(ap, true, addr, 0);
                if (result.error != ESP_OK) {
                    transfer_status = (result.ack == 0x2) ? 0x02 : 0x04;
                    break;
                }
                // For AP reads, need RDBUFF to get actual value
                uint32_t read_val = result.value;
                if (ap) {
                    auto rdbuf = swd::read_dp(0x0C);
                    if (rdbuf.error != ESP_OK) {
                        transfer_status = (rdbuf.ack == 0x2) ? 0x02 : 0x04;
                        break;
                    }
                    read_val = rdbuf.value;
                }
                last_read_val = read_val;
                if ((read_val & g_match_mask) == (match_value & g_match_mask)) {
                    matched = true;
                    break;
                }
            }
            if (transfer_status != 1) { g_stat_fault++; check_write = false; break; }
            // Match always counts as completed (per CMSIS5 reference)
            ++completed;
            if (!matched) {
                g_stat_mismatch++;
                ESP_LOGW(kTag, "MatchValue MISMATCH got=0x%08lx mask=0x%08lx expect=0x%08lx retry=%d",
                         (unsigned long)last_read_val, (unsigned long)g_match_mask,
                         (unsigned long)match_value, (int)g_match_retry);
                transfer_status |= 0x10;  // Add Mismatch flag to response
                // Stop this Transfer on mismatch so host can retry with a new request.
                check_write = false;
                break;
            } else {
                ESP_LOGD(kTag, "MatchValue OK mask=0x%08lx expect=0x%08lx",
                         (unsigned long)g_match_mask, (unsigned long)match_value);
            }
            check_write = false;
            continue;
        }

        swd::TransferResult result{};

        if (read) {
            if (ap) {
                // AP Read — SWD pipeline: the SWD response contains data from
                // the PREVIOUS AP read. First AP read returns stale data.
                g_stat_ap_read++;
                result = swd::raw_transfer(true, true, addr, 0);
                ESP_LOGD(kTag, "Xfer[%d] R AP addr=0x%02x ack=%d val=0x%08lx err=%d",
                         index, addr, result.ack, (unsigned long)result.value, result.error);
                if (result.error != ESP_OK) {
                    post_read = false;
                    g_stat_fault++;
                    transfer_status = (result.ack == 0x2) ? 0x02 : 0x04;
                    break;
                }
                if (post_read) {
                    // result.value is the PREVIOUS AP read's data (SWD pipeline)
                    put_u32_le(response_offset, result.value);
                    response_offset += 4;
                    ++completed;
                }
                post_read = true;
                check_write = false;
            } else {
                // DP Read — flush pending AP read first via RDBUFF
                if (post_read) {
                    auto rdbuf = swd::read_dp(0x0C);
                    if (rdbuf.error != ESP_OK) {
                        post_read = false;
                        transfer_status = (rdbuf.ack == 0x2) ? 0x02 : 0x04;
                        break;
                    }
                    put_u32_le(response_offset, rdbuf.value);
                    response_offset += 4;
                    ++completed;
                    post_read = false;
                }
                result = swd::raw_transfer(false, true, addr, 0);
                ESP_LOGD(kTag, "Xfer[%d] R DP addr=0x%02x ack=%d val=0x%08lx err=%d",
                         index, addr, result.ack, (unsigned long)result.value, result.error);
                if (result.error != ESP_OK) {
                    g_stat_fault++;
                    transfer_status = (result.ack == 0x2) ? 0x02 : 0x04;
                    break;
                }
                put_u32_le(response_offset, result.value);
                response_offset += 4;
                ++completed;
                check_write = false;
            }
        } else {
            // Write — flush pending AP read first
            if (request_offset + 4 > length) {
                transfer_status = 0x04;
                break;
            }
            const uint32_t value = get_u32_le(buffer, request_offset, length);
            request_offset += 4;

            if (post_read) {
                auto rdbuf = swd::read_dp(0x0C);
                if (rdbuf.error != ESP_OK) {
                    post_read = false;
                    transfer_status = (rdbuf.ack == 0x2) ? 0x02 : 0x04;
                    break;
                }
                put_u32_le(response_offset, rdbuf.value);
                response_offset += 4;
                ++completed;
                post_read = false;
            }

            if (ap) g_stat_ap_write++;
            result = swd::raw_transfer(ap, false, addr, value);
            // Track SELECT writes and log important AP writes
            if (!ap && addr == 0x08) {
                g_last_select = value;
            }
            if (ap) {
                ESP_LOGD(kTag, "W AP[0x%02x]=0x%08lx sel=0x%lx", addr, (unsigned long)value, (unsigned long)g_last_select);
            }
            ESP_LOGD(kTag, "Xfer[%d] W %s addr=0x%02x ack=%d val=0x%08lx err=%d",
                     index, ap ? "AP" : "DP", addr, result.ack, (unsigned long)value, result.error);
            if (result.error != ESP_OK) {
                g_stat_fault++;
                transfer_status = (result.ack == 0x2) ? 0x02 : 0x04;
                break;
            }
            check_write = ap;  // Track if last was AP write
            ++completed;
        }
    }

    // Flush last pending AP read via RDBUFF
    if (post_read) {
        auto rdbuf = swd::read_dp(0x0C);
        if (rdbuf.error != ESP_OK) {
            ESP_LOGW(kTag, "RDBUFF flush FAIL ack=%d err=%d", rdbuf.ack, rdbuf.error);
            transfer_status = (rdbuf.ack == 0x2) ? 0x02 : 0x04;
        } else {
            ESP_LOGD(kTag, "RDBUFF=0x%08lx sel=0x%lx", (unsigned long)rdbuf.value, (unsigned long)g_last_select);
            put_u32_le(response_offset, rdbuf.value);
            response_offset += 4;
            ++completed;
        }
    } else if (check_write) {
        // Last transfer was AP write — verify completion via RDBUFF
        auto rdbuf = swd::read_dp(0x0C);
        if (rdbuf.error != ESP_OK) {
            ESP_LOGW(kTag, "Write check FAIL ack=%d err=%d", rdbuf.ack, rdbuf.error);
            transfer_status = (rdbuf.ack == 0x2) ? 0x02 : 0x04;
        }
    }

    g_response[1] = completed;
    g_response[2] = transfer_status;

    // Auto-clear STICKYERR when FAULT detected to prevent cascading FAULTs
    // on subsequent commands. The FAULT status is still reported to the host.
    if (transfer_status == 0x04 || transfer_status == 0x02) {
        auto abort_r = swd::write_abort(0x1E);
        // Verify STICKYERR was cleared by reading CTRL/STAT
        auto cs = swd::raw_transfer(false, true, 0x04, 0);  // DP Read CTRL/STAT
        ESP_LOGW(kTag, "Xfer auto-clear: abort_ack=%d CTRL/STAT ack=%d val=0x%08lx",
                 abort_r.ack, cs.ack, (unsigned long)cs.value);
    }

    send_response(response_offset);
}

void handle_transfer_block(const uint8_t *buffer, size_t length)
{
    g_stat_block++;
    clear_response(kCmdTransferBlock);
    if (length < 5) {
        g_response[1] = 0;
        g_response[2] = 0;
        g_response[3] = 0xFF;
        send_response(4);
        return;
    }

    const uint16_t transfer_count = static_cast<uint16_t>(buffer[2] | (buffer[3] << 8));
    const uint8_t request = buffer[4];

    if (request & (kTransferMatchValue | kTransferMatchMask | kTransferTimestamp)) {
        g_response[1] = 0;
        g_response[2] = 0;
        g_response[3] = 0x04;
        send_response(4);
        return;
    }

    const bool ap = (request & kTransferApnDp) != 0;
    const bool read = (request & kTransferRnW) != 0;
    const uint8_t addr = request & (kTransferA2 | kTransferA3);

    ESP_LOGD(kTag, "Blk %s %s[0x%02x] cnt=%d sel=0x%lx",
             read ? "R" : "W", ap ? "AP" : "DP", addr,
             transfer_count, (unsigned long)g_last_select);
    // Log first 2 data words for writes
    if (!read && length >= 13) {
        uint32_t w0 = get_u32_le(buffer, 5, length);
        uint32_t w1 = get_u32_le(buffer, 9, length);
        ESP_LOGD(kTag, "  data[0:1]=0x%08lx 0x%08lx", (unsigned long)w0, (unsigned long)w1);
    }

    size_t request_offset = 5;
    size_t response_offset = 4;
    uint16_t completed = 0;
    uint8_t transfer_status = 1;

    for (uint16_t index = 0; index < transfer_count; ++index) {
        swd::TransferResult result{};
        if (read) {
            if (ap) g_stat_ap_read++;
            result = swd::raw_transfer(ap, true, addr, 0);
            if (result.error != ESP_OK) {
                g_stat_fault++;
                transfer_status = (result.ack == 0x2) ? 0x02 : 0x04;
                ESP_LOGW(kTag, "Blk FAULT at R[%d/%d] ack=%d blk#%lu",
                         index, transfer_count, result.ack, (unsigned long)g_stat_block);
                break;
            }
            if (ap) {
                // AP Read pipeline: for consecutive AP reads, the first N-1
                // return data from the previous read. We need RDBUFF for the last.
                if (index > 0) {
                    // Store previous pipelined data
                    if (response_offset + 4 > g_response.size()) {
                        transfer_status = 0x04;
                        break;
                    }
                    put_u32_le(response_offset, result.value);
                    response_offset += 4;
                }
                ++completed;
            } else {
                // DP Read: data is immediately valid
                ++completed;
                if (response_offset + 4 > g_response.size()) {
                    transfer_status = 0x04;
                    break;
                }
                put_u32_le(response_offset, result.value);
                response_offset += 4;
            }
        } else {
            if (request_offset + 4 > length) {
                transfer_status = 0x04;
                break;
            }
            const uint32_t value = get_u32_le(buffer, request_offset, length);
            request_offset += 4;
            if (ap) g_stat_ap_write++;
            result = swd::raw_transfer(ap, false, addr, value);
            if (result.error != ESP_OK) {
                g_stat_fault++;
                transfer_status = (result.ack == 0x2) ? 0x02 : 0x04;
                ESP_LOGW(kTag, "Blk FAULT at W[%d/%d] ack=%d val=0x%08lx blk#%lu",
                         index, transfer_count, result.ack,
                         (unsigned long)value, (unsigned long)g_stat_block);
                break;
            }
            ++completed;
        }
    }

    // Flush last AP read via RDBUFF
    if (read && ap && completed > 0 && transfer_status == 1) {
        auto rdbuf = swd::read_dp(0x0C);
        if (rdbuf.error != ESP_OK) {
            transfer_status = (rdbuf.ack == 0x2) ? 0x02 : 0x04;
        } else {
            if (response_offset + 4 <= g_response.size()) {
                put_u32_le(response_offset, rdbuf.value);
                response_offset += 4;
            }
        }
    } else if (!read && ap && completed > 0 && transfer_status == 1) {
        // AP write block — verify completion via RDBUFF
        auto rdbuf = swd::read_dp(0x0C);
        if (rdbuf.error != ESP_OK) {
            transfer_status = (rdbuf.ack == 0x2) ? 0x02 : 0x04;
        }
    }

    g_response[1] = static_cast<uint8_t>(completed & 0xFF);
    g_response[2] = static_cast<uint8_t>((completed >> 8) & 0xFF);
    g_response[3] = transfer_status;
    // Log first 2 read words for verification
    if (read && response_offset >= 12) {
        uint32_t r0 = get_u32_le(g_response.data(), 4, response_offset);
        uint32_t r1 = get_u32_le(g_response.data(), 8, response_offset);
        ESP_LOGD(kTag, "  read[0:1]=0x%08lx 0x%08lx done=%d", (unsigned long)r0, (unsigned long)r1, completed);
    }

    // Auto-clear STICKYERR when FAULT detected to prevent cascading FAULTs
    if (transfer_status == 0x04 || transfer_status == 0x02) {
        auto abort_r = swd::write_abort(0x1E);
        auto cs = swd::raw_transfer(false, true, 0x04, 0);  // DP Read CTRL/STAT
        ESP_LOGW(kTag, "Blk auto-clear: abort_ack=%d CTRL/STAT ack=%d val=0x%08lx",
                 abort_r.ack, cs.ack, (unsigned long)cs.value);
    }

    send_response(response_offset);
}

} // namespace

namespace cmsis_dap {

bool is_connected() { return g_connected; }

// Track DAP activity: RTT pauses while DAP is busy
static uint32_t g_last_activity_tick = 0;

bool is_dap_busy()
{
    if (!g_connected) return false;
    uint32_t now = xTaskGetTickCount();
    // Busy if last DAP command was within 2 seconds
    return (now - g_last_activity_tick) < pdMS_TO_TICKS(2000);
}

std::array<uint8_t, kMaxResponseSize> g_last_dap_response{};
size_t g_last_dap_response_length = 0;

esp_err_t init()
{
    ESP_RETURN_ON_ERROR(swd::init(), kTag, "swd init failed");
    ESP_LOGI(kTag, "CMSIS-DAP ready on SWCLK=%d SWD_IN=%d SWD_OUT=%d",
             board_config::kSwclkPin,
             board_config::kSwdInputPin,
             board_config::kSwdOutputPin);
    return ESP_OK;
}

void poll()
{
    // Vendor data is handled synchronously in tud_vendor_rx_cb
}

void on_vendor_data(const uint8_t *buffer, size_t length)
{
    g_last_activity_tick = xTaskGetTickCount();
    if (length == 0 || buffer == nullptr) {
        return;
    }

    ESP_LOGD(kTag, "RX cmd=0x%02x len=%d", buffer[0], length);

    switch (buffer[0]) {
    case kCmdInfo:
        handle_dap_info(buffer, length);
        break;
    case kCmdHostStatus:
        handle_host_status(buffer, length);
        break;
    case kCmdTransferConfigure:
        handle_transfer_configure(buffer, length);
        break;
    case kCmdSwdConfigure:
        handle_swd_configure(buffer, length);
        break;
    case kCmdSwjPins:
        handle_swj_pins(buffer, length);
        break;
    case kCmdResetTarget:
        handle_reset_target(buffer, length);
        break;
    case kCmdTransferAbort:
        handle_simple_ok(buffer[0]);
        break;
    case kCmdWriteAbort: {
        // DAP_WriteABORT: actually write to ABORT register
        // Format: [cmd, dap_index, abort_word(4 bytes LE)]
        // Per ARM ADI spec, ABORT writes are ALWAYS accepted by the DP even when
        // STICKYERR is set (ACK=FAULT). The data phase clears the error flags.
        // Return OK status for both ACK=OK and ACK=FAULT.
        clear_response(kCmdWriteAbort);
        if (length >= 6) {
            const uint32_t abort_val = get_u32_le(buffer, 2, length);
            swd::TransferResult r = swd::write_abort(abort_val);
            g_response[1] = (r.ack == 0x1 || r.ack == 0x4) ? 0x00 : 0xFF;  // OK or FAULT both acceptable
            ESP_LOGW(kTag, "WriteAbort val=0x%08lx ack=%d resp=%s",
                     (unsigned long)abort_val, r.ack,
                     g_response[1] == 0 ? "OK" : "FAIL");
        } else {
            g_response[1] = 0xFF;
        }
        send_response(2);
        break;
    }
    case kCmdConnect:
        handle_connect(buffer, length);
        break;
    case kCmdDisconnect:
        handle_disconnect();
        break;
    case kCmdDelay:
        handle_delay(buffer, length);
        break;
    case kCmdSwjClock:
        handle_swj_clock(buffer, length);
        break;
    case kCmdSwjSequence:
        handle_swj_sequence(buffer, length);
        break;
    case kCmdTransfer:
        handle_transfer(buffer, length);
        break;
    case kCmdTransferBlock:
        handle_transfer_block(buffer, length);
        break;
    case kCmdJtagSequence:
    case kCmdJtagConfigure:
    case kCmdJtagIdcode:
        clear_response(buffer[0]);
        send_response(1);
        break;
    default:
        clear_response(buffer[0]);
        send_response(1);
        break;
    }
}

} // namespace cmsis_dap

size_t cmsis_dap::process_command(const uint8_t *input, size_t input_len,
                                   uint8_t *output, size_t output_max)
{
    swd::lock();
    g_suppress_usb_send = true;
    on_vendor_data(input, input_len);
    g_suppress_usb_send = false;
    swd::unlock();

    size_t len = g_last_dap_response_length;
    if (len > output_max) {
        len = output_max;
    }
    std::memcpy(output, g_last_dap_response.data(), len);
    return len;
}

// TinyUSB vendor callbacks
extern "C" void tud_vendor_rx_cb(uint8_t itf, uint8_t const* buffer, uint16_t bufsize)
{
    (void)itf;
    if (bufsize > 0 && buffer != nullptr) {
        swd::lock();
        cmsis_dap::on_vendor_data(buffer, bufsize);
        swd::unlock();
    }
    // Drain the internal stream FIFO so the endpoint can be re-armed for the next transfer.
    uint8_t drain[64];
    while (tud_vendor_available() > 0) {
        tud_vendor_read(drain, sizeof(drain));
    }
}

extern "C" void tud_vendor_tx_cb(uint8_t itf, uint32_t sent_bytes)
{
    (void)itf;
    (void)sent_bytes;
}
