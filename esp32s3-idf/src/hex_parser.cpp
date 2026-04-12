#include "hex_parser.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace {

bool parse_hex_byte(const char *text, uint8_t &value)
{
    auto nibble = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') {
            return ch - '0';
        }
        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        if (ch >= 'A' && ch <= 'F') {
            return 10 + (ch - 'A');
        }
        return -1;
    };

    const int hi = nibble(text[0]);
    const int lo = nibble(text[1]);
    if (hi < 0 || lo < 0) {
        return false;
    }
    value = static_cast<uint8_t>((hi << 4) | lo);
    return true;
}

} // namespace

namespace hex_parser {

esp_err_t parse_file(const char *path, ParsedHexImage &image, std::string &error_message)
{
    image = {};

    FILE *file = fopen(path, "r");
    if (file == nullptr) {
        error_message = "failed to open hex file";
        return ESP_ERR_NOT_FOUND;
    }

    char line[600];
    uint32_t base_address = 0;
    int line_number = 0;

    while (fgets(line, sizeof(line), file) != nullptr) {
        ++line_number;
        const size_t len = std::strlen(line);
        if (len < 11 || line[0] != ':') {
            error_message = "invalid Intel HEX line format at line " + std::to_string(line_number);
            fclose(file);
            return ESP_ERR_INVALID_ARG;
        }

        uint8_t byte_count = 0;
        uint8_t addr_hi = 0;
        uint8_t addr_lo = 0;
        uint8_t record_type = 0;
        if (!parse_hex_byte(line + 1, byte_count) || !parse_hex_byte(line + 3, addr_hi) ||
            !parse_hex_byte(line + 5, addr_lo) || !parse_hex_byte(line + 7, record_type)) {
            error_message = "invalid Intel HEX header at line " + std::to_string(line_number);
            fclose(file);
            return ESP_ERR_INVALID_ARG;
        }

        const uint16_t offset = static_cast<uint16_t>((addr_hi << 8) | addr_lo);
        const char *data_ptr = line + 9;

        if (record_type == 0x00) {
            Segment segment;
            segment.address = base_address + offset;
            segment.data.resize(byte_count);
            for (uint8_t i = 0; i < byte_count; ++i) {
                if (!parse_hex_byte(data_ptr + (i * 2), segment.data[i])) {
                    error_message = "invalid data byte at line " + std::to_string(line_number);
                    fclose(file);
                    return ESP_ERR_INVALID_ARG;
                }
            }
            image.lowest_address = std::min(image.lowest_address, segment.address);
            image.highest_address = std::max(image.highest_address, segment.address + static_cast<uint32_t>(segment.data.size()));
            image.segments.push_back(std::move(segment));
        } else if (record_type == 0x01) {
            break;
        } else if (record_type == 0x04) {
            uint8_t msb = 0;
            uint8_t lsb = 0;
            if (byte_count != 2 || !parse_hex_byte(data_ptr, msb) || !parse_hex_byte(data_ptr + 2, lsb)) {
                error_message = "invalid extended linear address at line " + std::to_string(line_number);
                fclose(file);
                return ESP_ERR_INVALID_ARG;
            }
            base_address = static_cast<uint32_t>(((msb << 8) | lsb) << 16);
        }
    }

    fclose(file);

    if (image.segments.empty()) {
        error_message = "hex file contains no data records";
        return ESP_ERR_INVALID_SIZE;
    }

    std::sort(image.segments.begin(), image.segments.end(), [](const Segment &lhs, const Segment &rhs) {
        return lhs.address < rhs.address;
    });

    // Merge contiguous segments
    std::vector<Segment> merged;
    for (auto &seg : image.segments) {
        if (!merged.empty() && seg.address == merged.back().address + merged.back().data.size()) {
            merged.back().data.insert(merged.back().data.end(), seg.data.begin(), seg.data.end());
        } else {
            merged.push_back(std::move(seg));
        }
    }
    image.segments = std::move(merged);

    return ESP_OK;
}

} // namespace hex_parser
