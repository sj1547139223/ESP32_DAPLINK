#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "esp_err.h"

namespace flm_parser {

struct FlashSector {
    uint32_t size;
    uint32_t address;
};

struct FlashDeviceInfo {
    char name[128];
    uint32_t dev_addr;     // Flash base address
    uint32_t dev_size;     // Total flash size
    uint32_t page_size;    // Programming page size
    uint8_t  val_empty;    // Erased memory content (usually 0xFF)
    uint32_t timeout_prog; // Program timeout (ms)
    uint32_t timeout_erase;// Erase timeout (ms)
    std::vector<FlashSector> sectors;
};

struct FuncEntry {
    uint32_t init;
    uint32_t uninit;
    uint32_t erase_chip;
    uint32_t erase_sector;
    uint32_t program_page;
    uint32_t verify;       // 0 if not available
};

struct ParsedFlm {
    std::vector<uint8_t> code;  // Algorithm code blob (to upload to target RAM)
    uint32_t code_base;         // Virtual address the code is loaded at (usually 0)
    uint32_t code_size;         // Size of code
    FuncEntry func;             // Function entry points (offsets from code_base)
    FlashDeviceInfo device;     // Flash device descriptor
    uint32_t static_base;       // Static data base (for algorithm-local variables)
    uint32_t stack_size;        // Recommended stack size
    bool valid;
};

esp_err_t parse_file(const char *path, ParsedFlm &flm, std::string &error_message);

} // namespace flm_parser
