#include "swd.h"

#include <cstring>

#include "board_config.h"
#include "driver/gpio.h"
#include "hal/gpio_ll.h"
#include "soc/gpio_struct.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_cpu.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_attr.h"

namespace {

const char *kTag = "swd";
constexpr uint8_t kAckOk = 0x1;
constexpr uint8_t kAckWait = 0x2;
constexpr uint8_t kAckFault = 0x4;
constexpr uint32_t kDefaultClockHz = 1000000;

// Precomputed bitmasks for direct register access (all pins < 32)
static_assert(static_cast<int>(board_config::kSwclkPin) < 32, "SWCLK pin must be < 32");
static_assert(static_cast<int>(board_config::kSwdOutputPin) < 32, "SWD OUT pin must be < 32");
static_assert(static_cast<int>(board_config::kSwdInputPin) < 32, "SWD IN pin must be < 32");

constexpr uint32_t kSwclkMask = (1UL << static_cast<uint32_t>(board_config::kSwclkPin));
constexpr uint32_t kSwdOutMask = (1UL << static_cast<uint32_t>(board_config::kSwdOutputPin));
constexpr uint32_t kSwdInShift = static_cast<uint32_t>(board_config::kSwdInputPin);

uint32_t g_clock_hz = kDefaultClockHz;
uint32_t g_half_period_us = 0;
uint32_t g_delay_cycles = 0;  // Sub-microsecond delay in CPU cycles
bool g_connected = false;
uint32_t turnaround = 1;  // SWD turnaround period in clock cycles
bool g_data_phase = false; // Generate data phase on FAULT/WAIT
uint32_t g_idle_cycles = 8; // Idle cycles after each transfer
uint16_t g_wait_retry = 100; // WAIT retry count
SemaphoreHandle_t g_swd_mutex = nullptr;

IRAM_ATTR static inline void delay_half_period()
{
    if (g_half_period_us > 0) {
        esp_rom_delay_us(g_half_period_us);
    } else if (g_delay_cycles > 0) {
        // Sub-microsecond delay using CPU cycle counter
        uint32_t start = esp_cpu_get_cycle_count();
        while ((esp_cpu_get_cycle_count() - start) < g_delay_cycles) {}
    }
}

// Direct register GPIO access — bypasses gpio_set_level() overhead
inline void set_swclk(int level)
{
    if (level) {
        GPIO.out_w1ts = kSwclkMask;
    } else {
        GPIO.out_w1tc = kSwclkMask;
    }
}

inline void set_swd_out(int level)
{
    if (level) {
        GPIO.out_w1ts = kSwdOutMask;
    } else {
        GPIO.out_w1tc = kSwdOutMask;
    }
}

// Fast tristate control for SWDIO output.
inline void swd_out_enable()
{
    GPIO.enable_w1ts = (1UL << board_config::kSwdOutputPin);
}

inline void swd_out_disable()
{
    GPIO.enable_w1tc = (1UL << board_config::kSwdOutputPin);
}

inline int get_swd_in()
{
    return (GPIO.in >> kSwdInShift) & 0x1U;
}

IRAM_ATTR void clock_cycle()
{
    GPIO.out_w1tc = kSwclkMask;
    delay_half_period();
    GPIO.out_w1ts = kSwclkMask;
    delay_half_period();
}

IRAM_ATTR void write_bits(uint64_t value, size_t bit_count)
{
    for (size_t bit = 0; bit < bit_count; ++bit) {
        // Set data while clock is still high (setup time)
        if ((value >> bit) & 0x1U) {
            GPIO.out_w1ts = kSwdOutMask;
        } else {
            GPIO.out_w1tc = kSwdOutMask;
        }
        GPIO.out_w1tc = kSwclkMask;  // CLK LOW
        delay_half_period();
        GPIO.out_w1ts = kSwclkMask;  // CLK HIGH — target samples here
        delay_half_period();
    }
}

IRAM_ATTR uint32_t read_bits(size_t bit_count)
{
    uint32_t value = 0;
    for (size_t bit = 0; bit < bit_count; ++bit) {
        GPIO.out_w1tc = kSwclkMask;  // CLK LOW — target drives data
        delay_half_period();
        value |= (((GPIO.in >> kSwdInShift) & 0x1U) << bit);
        GPIO.out_w1ts = kSwclkMask;  // CLK HIGH
        delay_half_period();
    }
    return value;
}

IRAM_ATTR uint8_t parity32(uint32_t value)
{
    value ^= value >> 16;
    value ^= value >> 8;
    value ^= value >> 4;
    value ^= value >> 2;
    value ^= value >> 1;
    return value & 0x1U;
}

uint8_t make_request(bool ap, bool read, uint8_t addr)
{
    const uint8_t a2 = (addr >> 2) & 0x1U;
    const uint8_t a3 = (addr >> 3) & 0x1U;
    const uint8_t parity = (ap ^ read ^ a2 ^ a3) & 0x1U;
    return static_cast<uint8_t>(
        (1U << 0) |
        ((ap ? 1U : 0U) << 1) |
        ((read ? 1U : 0U) << 2) |
        (a2 << 3) |
        (a3 << 4) |
        (parity << 5) |
        (0U << 6) |
        (1U << 7));
}

IRAM_ATTR swd::TransferResult transfer(bool ap, bool read, uint8_t addr, uint32_t write_value)
{
    swd::TransferResult result{};
    const uint8_t request = make_request(ap, read, addr);
    uint16_t retries_left = g_wait_retry;
    uint8_t fault_retries = 1;  // Auto-retry once on FAULT (clear STICKYERR + retry)

    do {
        swd_out_enable();
        write_bits(request, 8);

        // Turnaround: host releases SWDIO, target takes over
        swd_out_disable();
        clock_cycle();
        result.ack = static_cast<uint8_t>(read_bits(3));

        if (result.ack == kAckOk) {
            break;  // Success — proceed to data phase
        }

        // FAULT or WAIT: if data_phase configured, complete dummy data phase
        // This keeps the protocol in sync (33 clocks = 32 data + 1 parity)
        if (g_data_phase) {
            for (uint32_t n = 0; n < 33; n++) {
                clock_cycle();
            }
        }

        // Turnaround with output disabled (DAPLink reference)
        for (uint32_t n = 0; n < turnaround; n++) {
            clock_cycle();
        }
        // Re-enable output and drive HIGH (idle)
        swd_out_enable();
        set_swd_out(1);

        // FAULT: clear STICKYERR via ABORT and retry
        if (result.ack == kAckFault && fault_retries > 0) {
            --fault_retries;
            // Write ABORT register (DP addr 0x00) to clear STICKYERR
            // Inline abort: request + turnaround + ACK + turnaround + data + parity
            {
                const uint8_t abort_req = make_request(false, false, 0x00);
                swd_out_enable();
                write_bits(abort_req, 8);
                swd_out_disable();
                clock_cycle();
                read_bits(3);  // ACK (ignore, ABORT always accepted)
                clock_cycle();  // Turnaround T→H
                swd_out_enable();
                write_bits(0x1E, 32);  // Clear all sticky flags
                write_bits(parity32(0x1E), 1);
                if (g_idle_cycles > 0) {
                    set_swd_out(0);
                    for (uint32_t i = 0; i < g_idle_cycles; i++) {
                        clock_cycle();
                    }
                }
                set_swd_out(1);
            }
            continue;  // Retry the original operation
        }

        if (result.ack != kAckWait || retries_left == 0) {
            if (result.ack == kAckFault) {
                ESP_LOGW(kTag, "SWD FAULT: %s %s addr=0x%02x val=0x%08lx retries=%d",
                         ap ? "AP" : "DP", read ? "R" : "W", addr,
                         (unsigned long)write_value, (int)(g_wait_retry - retries_left));
            } else {
                ESP_LOGW(kTag, "SWD ERR: ack=0x%02x %s %s addr=0x%02x retries=%d",
                         result.ack, ap ? "AP" : "DP", read ? "R" : "W", addr,
                         (int)(g_wait_retry - retries_left));
            }
            result.error = (result.ack == kAckWait) ? ESP_ERR_TIMEOUT : ESP_FAIL;
            return result;
        }
        --retries_left;
    } while (true);

    // ACK OK — handle data phase
    if (read) {
        // Target continues driving: read 32-bit data + parity
        result.value = read_bits(32);
        const uint8_t parity = static_cast<uint8_t>(read_bits(1));
        // Turnaround: target releases, host takes over
        // Keep output disabled during turnaround to avoid bus contention
        clock_cycle();
        swd_out_enable();
        // Idle cycles
        if (g_idle_cycles > 0) {
            set_swd_out(0);
            for (uint32_t i = 0; i < g_idle_cycles; i++) {
                clock_cycle();
            }
        }
        set_swd_out(1);
        if (parity != parity32(result.value)) {
            ESP_LOGW(kTag, "SWD PARITY: %s R addr=0x%02x val=0x%08lx parity=%d expected=%d",
                     ap ? "AP" : "DP", addr, (unsigned long)result.value,
                     parity, parity32(result.value));
            result.error = ESP_ERR_INVALID_CRC;
            return result;
        }
        result.error = ESP_OK;
        return result;
    }

    // Write: turnaround back to host
    // Keep output disabled during turnaround to avoid bus contention
    clock_cycle();
    swd_out_enable();
    write_bits(write_value, 32);
    write_bits(parity32(write_value), 1);
    // Idle cycles
    if (g_idle_cycles > 0) {
        set_swd_out(0);
        for (uint32_t i = 0; i < g_idle_cycles; i++) {
            clock_cycle();
        }
    }
    set_swd_out(1);
    result.value = write_value;
    result.error = ESP_OK;
    return result;
}

} // namespace

namespace swd {

esp_err_t init()
{
    if (!g_swd_mutex) {
        g_swd_mutex = xSemaphoreCreateMutex();
    }
    return set_clock_hz(kDefaultClockHz);
}

esp_err_t set_clock_hz(uint32_t hz)
{
    if (hz == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    g_clock_hz = hz;
    // Always use cycle-accurate delay for all frequencies.
    // esp_rom_delay_us() has unpredictable jitter that causes SWD FAULT at ≤500KHz.
    g_half_period_us = 0;
    const uint32_t cpu_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
    g_delay_cycles = (uint32_t)((uint64_t)cpu_mhz * 500000ULL / hz);
    // Minimum ~30 cycles (~125ns at 240MHz) to ensure SWD setup/hold times
    if (g_delay_cycles < 30) g_delay_cycles = 30;

    ESP_LOGI(kTag, "SWD clock set to %lu Hz (cycles=%lu)",
             static_cast<unsigned long>(g_clock_hz),
             static_cast<unsigned long>(g_delay_cycles));
    return ESP_OK;
}

uint32_t clock_hz()
{
    return g_clock_hz;
}

void line_reset()
{
    swd_out_enable();
    set_swd_out(1);
    for (int i = 0; i < 60; ++i) {
        clock_cycle();
    }
}

void jtag_to_swd()
{
    line_reset();
    write_bits(0xE79E, 16);
    line_reset();
    write_bits(0, 8);
}

esp_err_t connect()
{
    // Don't do jtag_to_swd() here; Keil sends SWJ_Sequence commands to handle it.
    // Just mark as connected.
    g_connected = true;
    return ESP_OK;
}

esp_err_t disconnect()
{
    g_connected = false;
    line_reset();
    return ESP_OK;
}

TransferResult read_dp(uint8_t addr)
{
    if (!g_connected) {
        connect();
    }
    return transfer(false, true, addr, 0);
}

TransferResult write_dp(uint8_t addr, uint32_t value)
{
    if (!g_connected) {
        connect();
    }
    return transfer(false, false, addr, value);
}

TransferResult read_ap(uint8_t addr)
{
    if (!g_connected) {
        connect();
    }
    TransferResult select = write_dp(0x08, addr & 0xF0);
    if (select.error != ESP_OK) {
        return select;
    }
    TransferResult ap_read = transfer(true, true, addr, 0);
    if (ap_read.error != ESP_OK) {
        return ap_read;
    }
    return read_dp(0x0C);
}

TransferResult write_ap(uint8_t addr, uint32_t value)
{
    if (!g_connected) {
        connect();
    }
    TransferResult select = write_dp(0x08, addr & 0xF0);
    if (select.error != ESP_OK) {
        return select;
    }
    return transfer(true, false, addr, value);
}

TransferResult raw_transfer(bool ap, bool read, uint8_t addr, uint32_t write_value)
{
    if (!g_connected) {
        connect();
    }
    return transfer(ap, read, addr, write_value);
}

TransferResult write_abort(uint32_t value)
{
    TransferResult result{};
    const uint8_t request = make_request(false, false, 0x00);  // DP write addr=0 (ABORT)
    uint16_t retries_left = g_wait_retry;

    do {
        swd_out_enable();
        write_bits(request, 8);

        // Turnaround: host releases, target drives ACK
        swd_out_disable();
        clock_cycle();
        result.ack = static_cast<uint8_t>(read_bits(3));

        if (result.ack == kAckOk || result.ack == kAckFault) {
            break;  // Proceed to data phase (always complete for ABORT)
        }

        // WAIT or other: turnaround with output disabled, then retry
        for (uint32_t n = 0; n < turnaround; n++) {
            clock_cycle();
        }
        swd_out_enable();
        set_swd_out(1);

        if (result.ack != kAckWait || retries_left == 0) {
            break;  // Can't retry, force data phase anyway
        }
        --retries_left;
    } while (true);

    // ALWAYS complete data phase for ABORT register.
    // Per ARM ADI spec, ABORT writes are accepted regardless of STICKYERR.
    // Turnaround with output disabled to avoid bus contention
    clock_cycle();
    swd_out_enable();
    write_bits(value, 32);
    write_bits(parity32(value), 1);
    // Idle cycles
    if (g_idle_cycles > 0) {
        set_swd_out(0);
        for (uint32_t i = 0; i < g_idle_cycles; i++) {
            clock_cycle();
        }
    }
    set_swd_out(1);

    result.value = value;
    result.error = (result.ack == kAckOk) ? ESP_OK : ESP_FAIL;
    ESP_LOGD(kTag, "write_abort(0x%08lx) ack=%d retries_used=%d",
             (unsigned long)value, result.ack, (int)(g_wait_retry - retries_left));
    return result;
}

void set_transfer_config(uint8_t idle_cycles, uint16_t wait_retry)
{
    g_idle_cycles = idle_cycles;
    g_wait_retry = wait_retry;
    ESP_LOGD(kTag, "transfer config: idle=%u wait_retry=%u", idle_cycles, wait_retry);
}

void set_swd_config(uint8_t turnaround_cycles, bool data_phase)
{
    turnaround = turnaround_cycles;
    g_data_phase = data_phase;
    ESP_LOGI(kTag, "SWD config: turnaround=%u data_phase=%d", turnaround_cycles, data_phase);
}

void lock()
{
    if (g_swd_mutex) {
        xSemaphoreTake(g_swd_mutex, portMAX_DELAY);
    }
}

void unlock()
{
    if (g_swd_mutex) {
        xSemaphoreGive(g_swd_mutex);
    }
}

} // namespace swd
