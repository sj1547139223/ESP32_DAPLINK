#pragma once

#include <string>
#include <vector>

#include "esp_err.h"

namespace hex_parser {

struct Segment {
    uint32_t address;
    std::vector<uint8_t> data;
};

struct ParsedHexImage {
    std::vector<Segment> segments;
    uint32_t lowest_address = 0xFFFFFFFFu;
    uint32_t highest_address = 0;
};

esp_err_t parse_file(const char *path, ParsedHexImage &image, std::string &error_message);

} // namespace hex_parser
