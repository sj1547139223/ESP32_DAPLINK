#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "esp_err.h"
#include "hex_parser.h"
#include "target_probe.h"

namespace flash_algo {

struct SelectionResult {
    const char *algorithm_name = "unknown";
    target_probe::Family family = target_probe::Family::kUnknown;
};

SelectionResult select_algorithm(const hex_parser::ParsedHexImage &image, const target_probe::TargetInfo &target);
esp_err_t program_target(const SelectionResult &selection,
                         const target_probe::TargetInfo &target,
                         const std::vector<hex_parser::Segment> &segments);

/// Program target using an FLM flash algorithm file.
/// @param flm_path  Path to the .FLM file on the filesystem
/// @param target    Target info (must be probed first)
/// @param segments  Data segments from hex parser
/// @param ram_base  Target SRAM base address (e.g., 0x20000000)
/// @param ram_size  Target SRAM size in bytes
esp_err_t program_with_flm(const char *flm_path,
                           const target_probe::TargetInfo &target,
                           const std::vector<hex_parser::Segment> &segments,
                           uint32_t ram_base, uint32_t ram_size);

} // namespace flash_algo
