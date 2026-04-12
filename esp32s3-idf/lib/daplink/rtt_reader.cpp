#include "rtt_reader.h"

#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "swd.h"
#include "esp_log.h"

namespace {

const char *kTag = "rtt";

// SEGGER RTT Control Block signature
constexpr char kRttSignature[] = "SEGGER RTT";
constexpr size_t kSignatureLen = 16;  // padded to 16 bytes in CB

// Default search range (STM32 common RAM)
uint32_t g_search_start = 0x20000000;
uint32_t g_search_size = 0x20000;  // 128KB default

// RTT state
bool g_found = false;
bool g_enabled = false;
uint32_t g_cb_addr = 0;           // Control block address on target
uint32_t g_cached_cb_addr = 0;    // Cached from last successful find

// Cached control block header
uint32_t g_max_up = 0;
uint32_t g_max_down = 0;

// Up channel 0 descriptor (cached)
struct RttBufDesc {
    uint32_t name_ptr;
    uint32_t buf_ptr;
    uint32_t size;
    uint32_t wr_off;  // address in target RAM
    uint32_t rd_off;   // address in target RAM
    uint32_t flags;
};

// Addresses of up[0] and down[0] fields in target RAM
uint32_t g_up0_base = 0;   // start of aUp[0] in target
uint32_t g_down0_base = 0; // start of aDown[0] in target

// Cached read/write offsets
uint32_t g_up0_size = 0;
uint32_t g_up0_buf_ptr = 0;
uint32_t g_down0_size = 0;
uint32_t g_down0_buf_ptr = 0;

// ---- SWD Memory Access Helpers ----

// Read a 32-bit word from target memory via SWD AP
// Caller must hold SWD lock.
esp_err_t mem_read32(uint32_t addr, uint32_t *value)
{
    // Select AHB-AP bank 0 (CSW/TAR/DRW at 0x00/0x04/0x0C)
    auto sel = swd::write_dp(0x08, 0x00000000);  // SELECT = AP0, bank 0
    if (sel.error != ESP_OK) return sel.error;

    // Set TAR (Transfer Address Register)
    auto tar = swd::raw_transfer(true, false, 0x04, addr);  // AP write TAR
    if (tar.error != ESP_OK || tar.ack != 0x01) return ESP_FAIL;

    // Read DRW (Data Read/Write) — pipeline: first read is dummy
    auto drw = swd::raw_transfer(true, true, 0x0C, 0);  // AP read DRW
    if (drw.error != ESP_OK || drw.ack != 0x01) return ESP_FAIL;

    // Read RDBUFF to get actual data
    auto rb = swd::read_dp(0x0C);  // DP RDBUFF
    if (rb.error != ESP_OK) return rb.error;

    *value = rb.value;
    return ESP_OK;
}

// Write a 32-bit word to target memory via SWD AP
esp_err_t mem_write32(uint32_t addr, uint32_t value)
{
    auto sel = swd::write_dp(0x08, 0x00000000);
    if (sel.error != ESP_OK) return sel.error;

    auto tar = swd::raw_transfer(true, false, 0x04, addr);
    if (tar.error != ESP_OK || tar.ack != 0x01) return ESP_FAIL;

    auto drw = swd::raw_transfer(true, false, 0x0C, value);
    if (drw.error != ESP_OK || drw.ack != 0x01) return ESP_FAIL;

    return ESP_OK;
}

// Read a block of memory (must be 4-byte aligned, length multiple of 4)
esp_err_t mem_read_block(uint32_t addr, uint8_t *buf, size_t len)
{
    for (size_t i = 0; i < len; i += 4) {
        uint32_t val;
        esp_err_t err = mem_read32(addr + i, &val);
        if (err != ESP_OK) return err;
        memcpy(buf + i, &val, 4);
    }
    return ESP_OK;
}

// ---- RTT Control Block Search ----

// Verify RTT signature at a specific address. Caller must hold SWD lock.
bool verify_cb_at(uint32_t addr)
{
    uint8_t sig[kSignatureLen];
    if (mem_read_block(addr, sig, kSignatureLen) != ESP_OK) return false;
    return memcmp(sig, kRttSignature, 10) == 0;
}

// Search for RTT control block. Releases/re-acquires SWD lock between chunks
// to avoid blocking DAP for long periods. Returns with lock held if found.
bool search_control_block()
{
    // Try cached address first (fast path)
    if (g_cached_cb_addr != 0) {
        if (verify_cb_at(g_cached_cb_addr)) {
            g_cb_addr = g_cached_cb_addr;
            ESP_LOGI(kTag, "RTT CB verified at cached 0x%08lx",
                     (unsigned long)g_cb_addr);
            return true;
        }
        g_cached_cb_addr = 0;  // Cache invalid, do full search
    }

    // Full search: read in 256-byte chunks, release lock between chunks
    constexpr size_t kChunkSize = 256;
    uint8_t chunk[kChunkSize];

    ESP_LOGD(kTag, "Searching for RTT CB at 0x%08lx..0x%08lx",
             (unsigned long)g_search_start,
             (unsigned long)(g_search_start + g_search_size));

    for (uint32_t offset = 0; offset < g_search_size; offset += kChunkSize) {
        uint32_t addr = g_search_start + offset;

        // Release lock between chunks to let DAP commands through
        if (offset > 0) {
            swd::unlock();
            vTaskDelay(1);  // Yield to higher-priority tasks
            swd::lock();
        }

        if (mem_read_block(addr, chunk, kChunkSize) != ESP_OK) {
            continue;
        }

        for (size_t i = 0; i <= kChunkSize - kSignatureLen; i += 4) {
            if (memcmp(chunk + i, kRttSignature, 10) == 0) {
                g_cb_addr = addr + i;
                g_cached_cb_addr = g_cb_addr;
                ESP_LOGW(kTag, "RTT Control Block found at 0x%08lx",
                         (unsigned long)g_cb_addr);
                return true;
            }
        }
    }

    ESP_LOGD(kTag, "RTT Control Block not found");
    return false;
}

// Read and cache the RTT buffer descriptors for up[0] and down[0]
bool read_buffer_descriptors()
{
    // CB layout:
    // +0x00: acID[16]           (16 bytes, "SEGGER RTT\0...")
    // +0x10: MaxNumUpBuffers    (4 bytes)
    // +0x14: MaxNumDownBuffers  (4 bytes)
    // +0x18: aUp[0] starts      (24 bytes per entry)
    //   +0x00: sName (ptr)
    //   +0x04: pBuffer (ptr)
    //   +0x08: SizeOfBuffer
    //   +0x0C: WrOff
    //   +0x10: RdOff
    //   +0x14: Flags

    esp_err_t err;

    err = mem_read32(g_cb_addr + 0x10, &g_max_up);
    if (err != ESP_OK) return false;

    err = mem_read32(g_cb_addr + 0x14, &g_max_down);
    if (err != ESP_OK) return false;

    if (g_max_up == 0) {
        ESP_LOGW(kTag, "RTT: no up channels");
        return false;
    }

    g_up0_base = g_cb_addr + 0x18;
    g_down0_base = g_up0_base + g_max_up * 24;

    // Read up[0] buffer pointer and size
    err = mem_read32(g_up0_base + 0x04, &g_up0_buf_ptr);
    if (err != ESP_OK) return false;
    err = mem_read32(g_up0_base + 0x08, &g_up0_size);
    if (err != ESP_OK) return false;

    ESP_LOGI(kTag, "RTT Up[0]: buf=0x%08lx size=%lu",
             (unsigned long)g_up0_buf_ptr, (unsigned long)g_up0_size);

    // Read down[0] if available
    if (g_max_down > 0) {
        err = mem_read32(g_down0_base + 0x04, &g_down0_buf_ptr);
        if (err != ESP_OK) return false;
        err = mem_read32(g_down0_base + 0x08, &g_down0_size);
        if (err != ESP_OK) return false;

        ESP_LOGI(kTag, "RTT Down[0]: buf=0x%08lx size=%lu",
                 (unsigned long)g_down0_buf_ptr, (unsigned long)g_down0_size);
    }

    return g_up0_size > 0 && g_up0_buf_ptr != 0;
}

} // anonymous namespace

namespace rtt_reader {

esp_err_t init()
{
    g_found = false;
    g_enabled = false;
    g_cb_addr = 0;
    g_max_up = 0;
    g_max_down = 0;
    return ESP_OK;
}

void set_enabled(bool enabled)
{
    g_enabled = enabled;
    if (!enabled) {
        g_found = false;
        g_cb_addr = 0;
    }
    ESP_LOGI(kTag, "RTT %s", enabled ? "enabled" : "disabled");
}

bool is_enabled()
{
    return g_enabled;
}

void set_search_range(uint32_t start_addr, uint32_t size)
{
    g_search_start = start_addr;
    g_search_size = size;
}

void reset()
{
    g_found = false;
    g_cb_addr = 0;
    // Keep g_cached_cb_addr — retry cached address on next search
}

bool is_found()
{
    return g_found;
}

size_t poll_up(uint8_t *buffer, size_t buffer_size)
{
    swd::lock();

    // First time: search for control block
    if (!g_found) {
        if (!search_control_block() || !read_buffer_descriptors()) {
            swd::unlock();
            return 0;
        }
        g_found = true;
    }

    // Lightweight check: only read WrOff and RdOff (2 reads = ~8 SWD ops)
    uint32_t wr_off, rd_off;
    if (mem_read32(g_up0_base + 0x0C, &wr_off) != ESP_OK ||
        mem_read32(g_up0_base + 0x10, &rd_off) != ESP_OK) {
        swd::unlock();
        return 0;
    }

    if (wr_off == rd_off) {
        // No new data — release lock immediately
        swd::unlock();
        return 0;
    }

    // Calculate available data
    size_t available;
    if (wr_off > rd_off) {
        available = wr_off - rd_off;
    } else {
        available = g_up0_size - rd_off + wr_off;
    }

    if (available > buffer_size) {
        available = buffer_size;
    }

    // Read data from circular buffer
    size_t total_read = 0;
    uint32_t read_pos = rd_off;

    while (total_read < available) {
        // Read 4-byte aligned chunks
        uint32_t aligned_addr = g_up0_buf_ptr + (read_pos & ~3u);
        uint32_t word;
        if (mem_read32(aligned_addr, &word) != ESP_OK) {
            break;
        }

        // Extract bytes from the word
        uint8_t *word_bytes = (uint8_t *)&word;
        size_t start_in_word = read_pos & 3;
        for (size_t j = start_in_word; j < 4 && total_read < available; j++) {
            buffer[total_read++] = word_bytes[j];
            read_pos++;
            if (read_pos >= g_up0_size) {
                read_pos = 0;
            }
        }
    }

    // Update RdOff in target memory (host updates RdOff)
    if (total_read > 0) {
        mem_write32(g_up0_base + 0x10, read_pos);
    }

    swd::unlock();
    return total_read;
}

size_t write_down(const uint8_t *data, size_t length)
{
    if (!g_found || g_max_down == 0 || g_down0_size == 0) {
        return 0;
    }

    swd::lock();

    // Read current WrOff and RdOff for down[0]
    uint32_t wr_off, rd_off;
    if (mem_read32(g_down0_base + 0x0C, &wr_off) != ESP_OK ||
        mem_read32(g_down0_base + 0x10, &rd_off) != ESP_OK) {
        swd::unlock();
        return 0;
    }

    // Calculate free space
    size_t free_space;
    if (wr_off >= rd_off) {
        free_space = g_down0_size - wr_off + rd_off - 1;
    } else {
        free_space = rd_off - wr_off - 1;
    }

    if (length > free_space) {
        length = free_space;
    }

    if (length == 0) {
        swd::unlock();
        return 0;
    }

    // Write data to circular buffer word-by-word
    size_t written = 0;
    uint32_t write_pos = wr_off;

    while (written < length) {
        // Build a 4-byte word
        uint32_t aligned_addr = g_down0_buf_ptr + (write_pos & ~3u);
        uint32_t word;

        // Read existing word if partial write
        size_t start_in_word = write_pos & 3;
        size_t bytes_in_word = 4 - start_in_word;
        if (bytes_in_word > length - written) {
            bytes_in_word = length - written;
        }

        if (start_in_word != 0 || bytes_in_word != 4) {
            // Partial word: read-modify-write
            if (mem_read32(aligned_addr, &word) != ESP_OK) break;
        } else {
            word = 0;
        }

        uint8_t *word_bytes = (uint8_t *)&word;
        for (size_t j = 0; j < bytes_in_word; j++) {
            word_bytes[start_in_word + j] = data[written++];
            write_pos++;
            if (write_pos >= g_down0_size) {
                write_pos = 0;
            }
        }

        if (mem_write32(aligned_addr, word) != ESP_OK) break;
    }

    // Update WrOff (host updates WrOff for down channel)
    if (written > 0) {
        mem_write32(g_down0_base + 0x0C, write_pos);
    }

    swd::unlock();
    return written;
}

} // namespace rtt_reader
