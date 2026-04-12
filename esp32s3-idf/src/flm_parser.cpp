#include "flm_parser.h"

#include <cstdio>
#include <cstring>

#include "esp_log.h"

namespace {

const char *kTag = "flm_parser";

// ============ ELF32 structures ============

struct Elf32_Ehdr {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint32_t e_entry;
    uint32_t e_phoff;
    uint32_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
};

struct Elf32_Phdr {
    uint32_t p_type;
    uint32_t p_offset;
    uint32_t p_vaddr;
    uint32_t p_paddr;
    uint32_t p_filesz;
    uint32_t p_memsz;
    uint32_t p_flags;
    uint32_t p_align;
};

struct Elf32_Shdr {
    uint32_t sh_name;
    uint32_t sh_type;
    uint32_t sh_flags;
    uint32_t sh_addr;
    uint32_t sh_offset;
    uint32_t sh_size;
    uint32_t sh_link;
    uint32_t sh_info;
    uint32_t sh_addralign;
    uint32_t sh_entsize;
};

struct Elf32_Sym {
    uint32_t st_name;
    uint32_t st_value;
    uint32_t st_size;
    uint8_t  st_info;
    uint8_t  st_other;
    uint16_t st_shndx;
};

constexpr uint32_t PT_LOAD = 1;
constexpr uint32_t SHT_SYMTAB = 2;
constexpr uint32_t SHT_STRTAB = 3;

// Read T bytes from file at specific offset
bool read_at(FILE *f, uint32_t offset, void *buf, size_t size)
{
    if (fseek(f, offset, SEEK_SET) != 0) return false;
    return fread(buf, 1, size, f) == size;
}

// Find a symbol by name in the symbol table and return its value
bool find_symbol(FILE *f, const Elf32_Shdr &symtab, const Elf32_Shdr &strtab,
                 const char *name, uint32_t &value)
{
    uint32_t num_syms = symtab.sh_size / sizeof(Elf32_Sym);
    for (uint32_t i = 0; i < num_syms; ++i) {
        Elf32_Sym sym;
        if (!read_at(f, symtab.sh_offset + i * sizeof(Elf32_Sym), &sym, sizeof(sym))) {
            continue;
        }
        if (sym.st_name == 0 || sym.st_name >= strtab.sh_size) {
            continue;
        }
        char sym_name[64] = {};
        uint32_t name_offset = strtab.sh_offset + sym.st_name;
        if (!read_at(f, name_offset, sym_name, sizeof(sym_name) - 1)) {
            continue;
        }
        sym_name[sizeof(sym_name) - 1] = '\0';
        if (strcmp(sym_name, name) == 0) {
            value = sym.st_value;
            return true;
        }
    }
    return false;
}

// Parse the FlashDevice structure from the code blob
bool parse_flash_device(const uint8_t *data, size_t data_size, uint32_t offset,
                        flm_parser::FlashDeviceInfo &device)
{
    if (offset + 160 > data_size) return false;

    const uint8_t *p = data + offset;
    // Skip Vers (2 bytes)
    p += 2;
    // DevName (128 bytes)
    memcpy(device.name, p, 128);
    device.name[127] = '\0';
    p += 128;
    // Skip DevType (2 bytes)
    p += 2;
    // DevAdr (4 bytes, may need alignment)
    memcpy(&device.dev_addr, p, 4);
    p += 4;
    memcpy(&device.dev_size, p, 4);
    p += 4;
    memcpy(&device.page_size, p, 4);
    p += 4;
    // Skip Res (4 bytes)
    p += 4;
    device.val_empty = *p;
    p += 1;
    // Padding (3 bytes for alignment)
    p += 3;
    memcpy(&device.timeout_prog, p, 4);
    p += 4;
    memcpy(&device.timeout_erase, p, 4);
    p += 4;

    // Parse sector table (terminated by entry with size=0xFFFFFFFF or size=0)
    device.sectors.clear();
    size_t remaining = data_size - (p - data);
    while (remaining >= 8) {
        uint32_t sz, addr;
        memcpy(&sz, p, 4);
        memcpy(&addr, p + 4, 4);
        if (sz == 0xFFFFFFFF || sz == 0) break;
        device.sectors.push_back({sz, addr});
        p += 8;
        remaining -= 8;
    }

    return true;
}

} // namespace

namespace flm_parser {

esp_err_t parse_file(const char *path, ParsedFlm &flm, std::string &error_message)
{
    flm = {};
    flm.valid = false;

    FILE *f = fopen(path, "rb");
    if (!f) {
        error_message = "Cannot open FLM file";
        return ESP_ERR_NOT_FOUND;
    }

    // Read ELF header
    Elf32_Ehdr ehdr;
    if (!read_at(f, 0, &ehdr, sizeof(ehdr))) {
        error_message = "Failed to read ELF header";
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }

    // Validate ELF magic
    if (ehdr.e_ident[0] != 0x7F || ehdr.e_ident[1] != 'E' ||
        ehdr.e_ident[2] != 'L' || ehdr.e_ident[3] != 'F') {
        error_message = "Not a valid ELF file";
        fclose(f);
        return ESP_ERR_INVALID_ARG;
    }

    // Must be 32-bit ARM
    if (ehdr.e_ident[4] != 1 || ehdr.e_machine != 40) {
        error_message = "Not a 32-bit ARM ELF";
        fclose(f);
        return ESP_ERR_INVALID_ARG;
    }

    // Find PT_LOAD segments and merge into code blob
    uint32_t code_min_addr = 0xFFFFFFFF;
    uint32_t code_max_addr = 0;

    for (uint16_t i = 0; i < ehdr.e_phnum; ++i) {
        Elf32_Phdr phdr;
        if (!read_at(f, ehdr.e_phoff + i * ehdr.e_phentsize, &phdr, sizeof(phdr))) {
            continue;
        }
        if (phdr.p_type != PT_LOAD || phdr.p_filesz == 0) continue;

        if (phdr.p_vaddr < code_min_addr) code_min_addr = phdr.p_vaddr;
        if (phdr.p_vaddr + phdr.p_memsz > code_max_addr) code_max_addr = phdr.p_vaddr + phdr.p_memsz;
    }

    if (code_min_addr >= code_max_addr) {
        error_message = "No loadable segments found";
        fclose(f);
        return ESP_ERR_INVALID_ARG;
    }

    flm.code_base = code_min_addr;
    flm.code_size = code_max_addr - code_min_addr;
    flm.code.resize(flm.code_size, 0xFF);

    // Load all PT_LOAD segments into the code blob
    for (uint16_t i = 0; i < ehdr.e_phnum; ++i) {
        Elf32_Phdr phdr;
        if (!read_at(f, ehdr.e_phoff + i * ehdr.e_phentsize, &phdr, sizeof(phdr))) {
            continue;
        }
        if (phdr.p_type != PT_LOAD || phdr.p_filesz == 0) continue;

        uint32_t offset_in_blob = phdr.p_vaddr - code_min_addr;
        if (!read_at(f, phdr.p_offset, flm.code.data() + offset_in_blob, phdr.p_filesz)) {
            error_message = "Failed to read load segment";
            fclose(f);
            return ESP_ERR_INVALID_SIZE;
        }
    }

    // Find section headers for symbol resolution
    Elf32_Shdr symtab_shdr = {};
    Elf32_Shdr strtab_shdr = {};
    bool found_symtab = false;

    for (uint16_t i = 0; i < ehdr.e_shnum; ++i) {
        Elf32_Shdr shdr;
        if (!read_at(f, ehdr.e_shoff + i * ehdr.e_shentsize, &shdr, sizeof(shdr))) {
            continue;
        }
        if (shdr.sh_type == SHT_SYMTAB) {
            symtab_shdr = shdr;
            // The linked string table
            Elf32_Shdr link_shdr;
            if (read_at(f, ehdr.e_shoff + shdr.sh_link * ehdr.e_shentsize, &link_shdr, sizeof(link_shdr))) {
                strtab_shdr = link_shdr;
            }
            found_symtab = true;
            break;
        }
    }

    // Resolve function entry points
    flm.func = {};
    if (found_symtab) {
        find_symbol(f, symtab_shdr, strtab_shdr, "Init", flm.func.init);
        find_symbol(f, symtab_shdr, strtab_shdr, "UnInit", flm.func.uninit);
        find_symbol(f, symtab_shdr, strtab_shdr, "EraseChip", flm.func.erase_chip);
        find_symbol(f, symtab_shdr, strtab_shdr, "EraseSector", flm.func.erase_sector);
        find_symbol(f, symtab_shdr, strtab_shdr, "ProgramPage", flm.func.program_page);
        find_symbol(f, symtab_shdr, strtab_shdr, "Verify", flm.func.verify);

        // Find FlashDevice descriptor
        uint32_t flash_device_addr = 0;
        if (find_symbol(f, symtab_shdr, strtab_shdr, "FlashDevice", flash_device_addr)) {
            uint32_t fd_offset = flash_device_addr - code_min_addr;
            parse_flash_device(flm.code.data(), flm.code.size(), fd_offset, flm.device);
        }
    }

    fclose(f);

    // Default stack size
    flm.stack_size = 512;

    // Validate required functions
    if (flm.func.init == 0 && flm.func.erase_sector == 0 && flm.func.program_page == 0) {
        error_message = "FLM missing required functions (Init/EraseSector/ProgramPage)";
        return ESP_ERR_INVALID_ARG;
    }

    flm.valid = true;

    ESP_LOGI(kTag, "FLM parsed: code_size=%lu entry: Init=0x%lx Erase=0x%lx Prog=0x%lx",
             (unsigned long)flm.code_size,
             (unsigned long)flm.func.init,
             (unsigned long)flm.func.erase_sector,
             (unsigned long)flm.func.program_page);
    ESP_LOGI(kTag, "  Device: %s base=0x%08lx size=0x%lx page=%lu",
             flm.device.name,
             (unsigned long)flm.device.dev_addr,
             (unsigned long)flm.device.dev_size,
             (unsigned long)flm.device.page_size);
    if (!flm.device.sectors.empty()) {
        ESP_LOGI(kTag, "  Sectors: %u entries, first size=%lu",
                 (unsigned)flm.device.sectors.size(),
                 (unsigned long)flm.device.sectors[0].size);
    }

    return ESP_OK;
}

} // namespace flm_parser
