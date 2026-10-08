#include "unpack.hpp"

#include <array>
#include <iostream>
#include <limits>
#include <set>

namespace xbr {
namespace {

constexpr uint32_t frameSize = 0x8000;
constexpr size_t maximumEntries = 1000000;
constexpr std::string_view memberName = "payload.bin";
constexpr uint32_t cabinetFileTable = 44;
constexpr uint32_t cabinetDataStart = cabinetFileTable + 16 + static_cast<uint32_t>(memberName.size()) + 1;

struct RecoveryEntry {
    std::string path;
    fs::path destination;
    uint32_t size{};
    uint64_t fileTime{}, dataOffset{}, compressedSize{};
    std::vector<uint16_t> blocks;
    uint16_t finalSize{}, finalCompressed{};
};

std::string stripLeadingSeparators(std::string name) {
    size_t start = 0;
    while (start < name.size() && (name[start] == '\\' || name[start] == '/')) ++start;
    return name.substr(start);
}

void append16(std::vector<uint8_t>& bytes, uint16_t value) {
    bytes.push_back(static_cast<uint8_t>(value));
    bytes.push_back(static_cast<uint8_t>(value >> 8));
}

void append32(std::vector<uint8_t>& bytes, uint32_t value) {
    append16(bytes, static_cast<uint16_t>(value));
    append16(bytes, static_cast<uint16_t>(value >> 16));
}

void writeFrame(Output& cabinet, const Input& data, uint64_t& position,
                uint16_t compressed, uint16_t uncompressed) {
    std::vector<uint8_t> header;
    append32(header, 0); /* recovery frames have no stored CAB checksum */
    append16(header, compressed);
    append16(header, uncompressed);
    cabinet.write(header);
    std::vector<uint8_t> bytes(compressed);
    data.read(position, bytes);
    cabinet.write(bytes);
    position += compressed;
}

void setTimestamp(const fs::path& path, uint64_t value) {
    if (value == 0) return;
    HANDLE handle = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) fail("Cannot set recovery file timestamp: " + utf8(path));
    BY_HANDLE_FILE_INFORMATION information{};
    FILETIME time{static_cast<DWORD>(value), static_cast<DWORD>(value >> 32)};
    const bool ok = GetFileInformationByHandle(handle, &information) &&
        (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0 &&
        SetFileTime(handle, nullptr, nullptr, &time);
    CloseHandle(handle);
    if (!ok) fail("Cannot set recovery file timestamp: " + utf8(path));
}

void extractEntry(const RecoveryEntry& entry, const Input& data,
                  uint16_t compression, bool overwrite) {
    if (entry.size == 0 && entry.compressedSize == 0) {
        Output file(entry.destination, overwrite);
        file.commit(0, entry.fileTime);
        return;
    }

    const uint64_t frames = entry.blocks.size() + (entry.finalCompressed != 0 ? 1u : 0u);
    const uint64_t cabinetSize = cabinetDataStart + frames * 8 + entry.compressedSize;
    if (frames == 0 || frames > std::numeric_limits<uint16_t>::max() ||
        cabinetSize > static_cast<uint64_t>(std::numeric_limits<int32_t>::max())) {
        fail("Recovery entry exceeds native CAB adapter limits: " + entry.path);
    }

    TempDirectory temporary;
    const fs::path path = temporary.path() / L"recovery.cab";
    {
        Output file(path, false);
        std::vector<uint8_t> header;
        append32(header, 0x4643534D);
        append32(header, 0);
        append32(header, static_cast<uint32_t>(cabinetSize));
        append32(header, 0);
        append32(header, cabinetFileTable);
        append32(header, 0);
        header.push_back(3);
        header.push_back(1);
        append16(header, 1);
        append16(header, 1);
        append16(header, 0);
        append16(header, 0);
        append16(header, 0);
        append32(header, cabinetDataStart);
        append16(header, static_cast<uint16_t>(frames));
        append16(header, compression);
        append32(header, entry.size);
        append32(header, 0);
        append16(header, 0);
        append16(header, 0x21); /* DOS 1980-01-01, full FILETIME is applied below */
        append16(header, 0);
        append16(header, 0x20);
        header.insert(header.end(), memberName.begin(), memberName.end());
        header.push_back(0);
        file.write(header);
        uint64_t position = entry.dataOffset;
        for (const auto block : entry.blocks)
            writeFrame(file, data, position, block, static_cast<uint16_t>(frameSize));
        if (entry.finalCompressed != 0)
            writeFrame(file, data, position, entry.finalCompressed, entry.finalSize);
        file.commit(cabinetSize);
    }
    const Cabinet cabinet = readCabinet(Input::open(path), true);
    extractCabinet(cabinet, [&](const CabEntry&, size_t) { return entry.destination; }, overwrite);
    setTimestamp(entry.destination, entry.fileTime);
}
}

void processRecovery(Input control, const Input* data, const fs::path& output,
                     const Options& options) {
    if (!options.listOnly && data == nullptr) fail("Recovery extraction requires recdata.bin");
    if (control.size > 64u * 1024 * 1024) fail("Recovery control exceeds the 64 MiB metadata limit");
    Reader reader(std::move(control));
    const uint16_t versionCount = reader.u16();
    if (versionCount == 0) fail("Recovery control declares no version slots");
    std::vector<std::string> versions{"_All"};
    for (uint32_t index = 1; index < versionCount; ++index) versions.push_back(reader.msString());
    const uint16_t deviceCount = reader.u16();
    if (deviceCount == 0) fail("Recovery control declares no devices");
    std::vector<std::string> devices;
    for (uint32_t index = 0; index < deviceCount; ++index) {
        auto name = stripLeadingSeparators(reader.msString());
        const auto devicePath = reader.msString();
        if (name.empty() || devicePath.empty()) fail("Recovery control contains an empty device name/path");
        devices.push_back(std::move(name));
    }
    const uint32_t windowSize = reader.u32();
    uint16_t windowBits = 15;
    while (windowBits <= 21 && (uint32_t{1} << windowBits) != windowSize) ++windowBits;
    if (windowBits > 21) fail("Invalid recovery LZX window size (expected a power of two from 32768 to 2097152)");

    std::vector<RecoveryEntry> entries;
    std::set<fs::path, PathLess> destinations;
    uint64_t dataOffset = 0;
    while (reader.remaining() != 0) {
        if (entries.size() == maximumEntries) fail("Recovery control has too many entries");
        const uint16_t version = reader.u16();
        const uint16_t device = reader.u16();
        if (version >= versions.size() || device >= devices.size())
            fail("Recovery control entry has an invalid version/device index");
        RecoveryEntry entry;
        entry.size = reader.u32();
        entry.fileTime = reader.u64();
        const auto filePath = reader.msString();
        if (filePath.empty()) fail("Recovery control entry has an empty path");
        entry.path = versions[version] + "\\" + devices[device] + "\\" + filePath;
        entry.destination = safeOutput(output, entry.path);
        if (!destinations.insert(entry.destination).second) fail("Recovery control contains a duplicate output path: " + entry.path);
        entry.dataOffset = dataOffset;
        for (uint16_t compressed = reader.u16(); compressed != 0; compressed = reader.u16()) {
            if (entry.blocks.size() == std::numeric_limits<uint16_t>::max())
                fail("Recovery entry has too many LZX frames: " + entry.path);
            entry.blocks.push_back(compressed);
            entry.compressedSize += compressed;
        }
        entry.finalSize = reader.u16();
        entry.finalCompressed = reader.u16();
        if (entry.finalSize > frameSize || (entry.finalSize == 0) != (entry.finalCompressed == 0))
            fail("Recovery entry has invalid final LZX frame sizes: " + entry.path);
        entry.compressedSize += entry.finalCompressed;
        const uint64_t decompressed = entry.blocks.size() * uint64_t{frameSize} + entry.finalSize;
        if (entry.size > decompressed || decompressed - entry.size >= frameSize ||
            (entry.size == 0 && decompressed != 0))
            fail("Recovery file size does not match its LZX frame extent: " + entry.path);
        const uint64_t frames = entry.blocks.size() + (entry.finalCompressed != 0 ? 1u : 0u);
        const uint64_t cabinetSize = cabinetDataStart + frames * 8 + entry.compressedSize;
        if (!options.listOnly && (frames > std::numeric_limits<uint16_t>::max() ||
            cabinetSize > static_cast<uint64_t>(std::numeric_limits<int32_t>::max()) ||
            entry.size > static_cast<uint32_t>(std::numeric_limits<int32_t>::max())))
            fail("Recovery entry exceeds native CAB adapter limits: " + entry.path);
        if (entry.compressedSize > std::numeric_limits<uint64_t>::max() - dataOffset)
            fail("Recovery data offsets overflow");
        dataOffset += entry.compressedSize;
        if (data != nullptr && dataOffset > data->size)
            fail("Recovery compressed frame extends beyond recdata.bin: " + entry.path);
        if (!options.listOnly && !options.overwrite && fs::exists(entry.destination))
            fail("Output already exists (use --overwrite to replace it): " + utf8(entry.destination));
        entries.push_back(std::move(entry));
    }

    validateOutputTree(destinations);
    std::cout << "Recovery control: " << entries.size() << " files, " << versions.size()
              << " variants, " << devices.size() << " devices\n";
    for (size_t index = 0; index < entries.size(); ++index) {
        const auto& entry = entries[index];
        std::cout << '(' << index + 1 << '/' << entries.size() << ") " << entry.path
                  << " (" << entry.size << " bytes)\n";
        if (!options.listOnly)
            extractEntry(entry, *data, static_cast<uint16_t>((windowBits << 8) | 3), options.overwrite);
    }
}
}