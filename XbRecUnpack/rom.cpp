#include "unpack.hpp"

#include <array>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <sstream>

namespace xbr {
namespace {

uint16_t be16(std::span<const uint8_t> bytes, size_t offset) {
    return static_cast<uint16_t>((uint16_t{bytes[offset]} << 8) | bytes[offset + 1]);
}

uint32_t be32(std::span<const uint8_t> bytes, size_t offset) {
    return (uint32_t{be16(bytes, offset)} << 16) | be16(bytes, offset + 2);
}

struct RomInfo {
    uint16_t build{}, smcBuild{};
    std::string motherboard{"SMC unavailable"};
};

RomInfo readRom(const fs::path& path) {
    const Input input = Input::open(path);
    std::array<uint8_t, 144> header{};
    input.read(0, header);
    RomInfo info;
    info.build = be16(header, 2);
    if (info.build == 0) info.build = be16(header, 130);
    const uint32_t smcSize = be32(header, 120);
    const uint32_t smcAddress = be32(header, 124);
    if (smcAddress == 0) return info;
    if (smcSize < 0x103 || smcAddress > input.size || smcSize > input.size - smcAddress)
        fail("ROM SMC extent is invalid: " + utf8(path));
    std::array<uint8_t, 0x103> smc{};
    input.read(smcAddress, smc);
    std::array<uint8_t, 4> key{0x42, 0x75, 0x4E, 0x79};
    for (size_t index = 0; index < smc.size(); ++index) {
        const uint8_t cipher = smc[index];
        const unsigned modifier = static_cast<unsigned>(cipher) * 0xFB;
        smc[index] ^= key[index & 3];
        key[(index + 1) & 3] = static_cast<uint8_t>(key[(index + 1) & 3] + modifier);
        key[(index + 2) & 3] = static_cast<uint8_t>(key[(index + 2) & 3] + (modifier >> 8));
    }
    constexpr std::array<std::string_view, 10> names{
        "none/unk", "xenon", "zephyr", "falcon", "jasper", "trinity", "corona",
        "winchester", "unknown0x8", "ridgeway"};
    const unsigned type = smc[0x100] >> 4;
    const unsigned revision = smc[0x100] & 15;
    std::ostringstream text;
    text << "0x" << std::hex << std::uppercase << static_cast<unsigned>(smc[0x100]) << ": ";
    if (type < names.size()) text << names[type];
    else text << "unknown0x" << type;
    text << "-r" << std::dec << revision;
    info.motherboard = text.str();
    info.smcBuild = be16(smc, 0x101);
    return info;
}
}

void printRomSummary(const fs::path& root, bool detailed) {
    if (!fs::exists(root)) return;
    std::set<uint16_t> builds;
    std::map<std::string, std::set<uint16_t>> motherboards;
    std::set<std::string> originalMotherboards;
    size_t count = 0;
    std::vector<fs::path> pending{root};
    while (!pending.empty()) {
        auto directory = std::move(pending.back()); pending.pop_back();
        const DWORD attributes = GetFileAttributesW(directory.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) fail("Cannot inspect extracted path: " + utf8(directory));
        if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        WIN32_FIND_DATAW data{};
        const HANDLE handle = FindFirstFileExW((directory / L"*").c_str(), FindExInfoBasic, &data,
            FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (handle == INVALID_HANDLE_VALUE) {
            const DWORD error = GetLastError();
            if (error == ERROR_FILE_NOT_FOUND) continue;
            fail("Cannot enumerate extracted directory: " + utf8(directory) + " (Windows error " + std::to_string(error) + ")");
        }
        struct Search { HANDLE handle; ~Search() { FindClose(handle); } } search{handle};
        do {
            const std::wstring_view filename(data.cFileName);
            if (filename == L"." || filename == L"..") continue;
            if (++count > 1000000) fail("ROM summary directory contains too many entries");
            if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
            const auto path = directory / data.cFileName;
            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { pending.push_back(path); continue; }
            const auto name = lower(utf8(fs::path(filename)));
            if (name == "xboxrom_dvt.bin") originalMotherboards.insert("dvt1");
            else if (name == "xboxrom_xblade.bin") originalMotherboards.insert("xblade");
            else if (name.size() == 16 && name.starts_with("xboxrom_dvt") && name.ends_with(".bin") &&
                     name[11] >= '1' && name[11] <= '7')
                originalMotherboards.insert("dvt" + std::string(1, name[11]));
            else if (name == "xboxrom_update.bin") {
                try {
                    const auto info = readRom(path);
                    builds.insert(info.build);
                    motherboards[info.motherboard].insert(info.build);
                    if (detailed)
                        std::cout << utf8(path.lexically_relative(root)) << ": "
                                  << info.motherboard << " v" << info.build << " (SMC v" << info.smcBuild << ")\n";
                } catch (const std::exception& error) {
                    std::cerr << "ROM inspection failed: " << error.what() << '\n';
                }
            }
        } while (FindNextFileW(handle, &data));
        const DWORD error = GetLastError();
        if (error != ERROR_NO_MORE_FILES)
            fail("Cannot enumerate extracted directory: " + utf8(directory) + " (Windows error " + std::to_string(error) + ")");
    }
    if (builds.empty() && originalMotherboards.empty()) return;
    std::cout << "\nXbox ROM summary:\n";
    for (const auto build : builds) std::cout << "  Kernel " << build << '\n';
    for (const auto& [motherboard, versions] : motherboards) {
        std::cout << "  " << motherboard << " (kernels:";
        for (const auto build : versions) std::cout << ' ' << build;
        std::cout << ")\n";
    }
    for (const auto& motherboard : originalMotherboards) std::cout << "  " << motherboard << '\n';
}
}