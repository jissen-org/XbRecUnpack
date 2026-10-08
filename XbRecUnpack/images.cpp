#include "unpack.hpp"

#include <zlib.h>

#include <algorithm>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <utility>

namespace xbr {
namespace {

constexpr uint64_t sectorSize = 2048;
constexpr size_t metadataLimit = 64 * 1024 * 1024;
constexpr size_t transferSize = 1024 * 1024;
constexpr uint64_t imageLimit = 64ull * 1024 * 1024 * 1024;

uint16_t le16(std::span<const uint8_t> bytes, size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < 2) fail("Truncated image field");
    return static_cast<uint16_t>(bytes[offset] | (static_cast<uint16_t>(bytes[offset + 1]) << 8));
}

uint32_t le32(std::span<const uint8_t> bytes, size_t offset) {
    return static_cast<uint32_t>(le16(bytes, offset)) | (static_cast<uint32_t>(le16(bytes, offset + 2)) << 16);
}

uint64_t le64(std::span<const uint8_t> bytes, size_t offset) {
    return static_cast<uint64_t>(le32(bytes, offset)) | (static_cast<uint64_t>(le32(bytes, offset + 4)) << 32);
}

uint32_t both32(std::span<const uint8_t> bytes, size_t offset) {
    const uint32_t value = le32(bytes, offset);
    const uint32_t reverse = (static_cast<uint32_t>(le16(bytes, offset + 4) & 0xff) << 24) |
        (static_cast<uint32_t>(le16(bytes, offset + 4) >> 8) << 16) |
        (static_cast<uint32_t>(le16(bytes, offset + 6) & 0xff) << 8) | (le16(bytes, offset + 6) >> 8);
    if (value != reverse) fail("ISO9660 little/big endian fields disagree");
    return value;
}

uint16_t both16(std::span<const uint8_t> bytes, size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < 4) fail("Truncated image field");
    const uint16_t value = le16(bytes, offset);
    const uint16_t reverse = static_cast<uint16_t>((bytes[offset + 2] << 8) | bytes[offset + 3]);
    if (value != reverse) fail("ISO9660 little/big endian fields disagree");
    return value;
}

std::vector<uint8_t> readBytes(const Input& input, uint64_t offset, size_t size) {
    std::vector<uint8_t> bytes(size);
    input.read(offset, bytes);
    return bytes;
}

bool equals(std::span<const uint8_t> bytes, std::string_view text) {
    return bytes.size() == text.size() && std::equal(bytes.begin(), bytes.end(), text.begin());
}

struct ImageFile {
    std::vector<Input> parts;
    std::vector<uint8_t> inlineBytes;
    uint64_t size{};
};

struct ImageFiles {
    std::optional<ImageFile> control, data;
};

void addFile(ImageFiles& files, std::string name, ImageFile file) {
    name = lower(std::move(name));
    auto* destination = name == "recctrl.bin" ? &files.control : name == "recdata.bin" ? &files.data : nullptr;
    if (!destination) return;
    if (*destination) fail("Duplicate recovery file in image root: " + name);
    *destination = std::move(file);
}

std::vector<uint8_t> metadata(const ImageFile& file) {
    if (file.size > metadataLimit) fail("Image directory exceeds the 64 MiB metadata limit");
    if (!file.inlineBytes.empty()) return file.inlineBytes;
    std::vector<uint8_t> bytes(static_cast<size_t>(file.size));
    size_t offset = 0;
    for (const Input& part : file.parts) {
        if (part.size > bytes.size() - offset) fail("Image extent lengths exceed file length");
        part.read(0, std::span(bytes).subspan(offset, static_cast<size_t>(part.size)));
        offset += static_cast<size_t>(part.size);
    }
    if (offset != bytes.size()) fail("Image extents do not cover the file");
    return bytes;
}

Input materialize(const ImageFile& file, const fs::path& path) {
    if (file.inlineBytes.empty() && file.parts.size() == 1 && file.parts[0].size == file.size) return file.parts[0];
    Output output(path, false);
    if (!file.inlineBytes.empty()) {
        output.write(file.inlineBytes);
    } else {
        std::vector<uint8_t> buffer(transferSize);
        for (const Input& part : file.parts) {
            for (uint64_t offset = 0; offset < part.size;) {
                const size_t count = static_cast<size_t>(std::min<uint64_t>(buffer.size(), part.size - offset));
                part.read(offset, std::span(buffer).first(count));
                output.write(std::span(buffer).first(count));
                offset += count;
            }
        }
    }
    output.commit(file.size);
    return Input::open(path);
}

std::optional<ImageFiles> gdfFiles(const Input& image) {
    std::vector<uint8_t> descriptor;
    for (const uint64_t offset : {0x8000ull, 0x10000ull}) {
        if (offset > image.size || image.size - offset < 40) continue;
        auto candidate = readBytes(image, offset, 40);
        if (equals(std::span(candidate).first(20), "MICROSOFT*XBOX*MEDIA")) {
            descriptor = std::move(candidate);
            break;
        }
    }
    if (descriptor.empty()) return std::nullopt;
    const Input directory = image.slice(static_cast<uint64_t>(le32(descriptor, 20)) * sectorSize, le32(descriptor, 24));
    if (directory.size > metadataLimit) fail("GDF root directory exceeds the 64 MiB metadata limit");
    ImageFiles result;
    if (!directory.size) return result;
    const auto bytes = readBytes(directory, 0, static_cast<size_t>(directory.size));
    std::vector<uint32_t> pending{0};
    std::set<uint32_t> visited;
    std::map<uint32_t, uint32_t> ranges;
    while (!pending.empty()) {
        const uint32_t offset = pending.back();
        pending.pop_back();
        if (!visited.insert(offset).second) fail("GDF directory tree has a cycle or reused node");
        if (offset > bytes.size() || bytes.size() - offset < 14) fail("GDF directory node is outside the root extent");
        const auto node = std::span(bytes).subspan(offset);
        if (le16(node, 0) == 0xffff && le16(node, 2) == 0xffff) continue;
        const uint32_t end = offset + 14u + node[13];
        if (end > bytes.size() || !node[13]) fail("Invalid GDF file name extent");
        const auto next = ranges.lower_bound(offset);
        if ((next != ranges.end() && next->first < end) ||
            (next != ranges.begin() && std::prev(next)->second > offset)) fail("GDF directory nodes overlap");
        ranges.emplace(offset, end);
        const std::string name(reinterpret_cast<const char*>(node.data() + 14), node[13]);
        if (!(node[12] & 0x10)) {
            const uint32_t size = le32(node, 8);
            addFile(result, name, {{image.slice(static_cast<uint64_t>(le32(node, 4)) * sectorSize, size)}, {}, size});
        }
        for (const size_t edge : {0u, 2u}) {
            const uint16_t child = le16(node, edge);
            if (child == 0xffff) fail("Invalid GDF child index");
            if (child) pending.push_back(static_cast<uint32_t>(child) * 4);
        }
    }
    return result;
}

std::string isoName(std::span<const uint8_t> bytes, bool joliet) {
    std::string name;
    if (joliet) {
        if (bytes.size() % 2) fail("Odd length Joliet file identifier");
        for (size_t offset = 0; offset < bytes.size(); offset += 2) {
            if (bytes[offset]) return {};
            name += static_cast<char>(bytes[offset + 1]);
        }
    } else {
        name.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }
    if (const auto version = name.find(';'); version != std::string::npos) name.resize(version);
    if (!name.empty() && name.back() == '.') name.pop_back();
    return lower(std::move(name));
}

ImageFiles isoDirectory(const Input& image, std::span<const uint8_t> volume, bool joliet) {
    const uint16_t blockSize = both16(volume, 128);
    if (blockSize < 512 || blockSize > 8192 || (blockSize & (blockSize - 1))) fail("Unsupported ISO9660 logical block size");
    const uint64_t volumeSize = static_cast<uint64_t>(both32(volume, 80)) * blockSize;
    if (volumeSize > image.size) fail("ISO9660 volume extends outside image");
    const auto root = volume.subspan(156, volume[156]);
    if (root.size() < 34 || !(root[25] & 2)) fail("Invalid ISO9660 root directory record");
    const uint32_t rootSize = both32(root, 10);
    if (rootSize > metadataLimit) fail("ISO9660 root directory exceeds the 64 MiB metadata limit");
    const uint64_t rootOffset = (static_cast<uint64_t>(both32(root, 2)) + root[1]) * blockSize;
    if (rootOffset > volumeSize || rootSize > volumeSize - rootOffset) fail("ISO9660 root directory extends outside volume");
    const auto bytes = readBytes(image, rootOffset, rootSize);
    ImageFiles result;
    std::optional<ImageFile> continued;
    std::string continuedName;
    for (size_t offset = 0; offset < bytes.size();) {
        const uint8_t recordSize = bytes[offset];
        if (!recordSize) {
            offset = std::min(bytes.size(), ((offset / blockSize) + 1) * blockSize);
            continue;
        }
        if (recordSize < 34 || recordSize > bytes.size() - offset || recordSize > blockSize - offset % blockSize)
            fail("Truncated ISO9660 directory record");
        const auto record = std::span(bytes).subspan(offset, recordSize);
        if (!record[32] || record[32] > record.size() - 33) fail("Invalid ISO9660 file identifier length");
        const std::string name = isoName(record.subspan(33, record[32]), joliet);
        if (continued && name != continuedName) fail("ISO9660 multi extent file records are not consecutive");
        if (name == "recctrl.bin" || name == "recdata.bin") {
            if (record[25] & 2) fail("Recovery image root entry is a directory: " + name);
            if (record[26] || record[27]) fail("Interleaved ISO9660 recovery files are unsupported");
            if (both16(record, 28) != 1) fail("Multi volume ISO9660 recovery files are unsupported");
            const uint32_t size = both32(record, 10);
            const uint64_t start = (static_cast<uint64_t>(both32(record, 2)) + record[1]) * blockSize;
            if (start > volumeSize || size > volumeSize - start) fail("ISO9660 recovery file extends outside volume");
            if (!continued) { continued = ImageFile{}; continuedName = name; }
            continued->parts.push_back(image.slice(start, size));
            continued->size += size;
            if (continued->size > imageLimit) fail("Recovery image file exceeds the 64 GiB limit");
            if (!(record[25] & 0x80)) {
                addFile(result, name, std::move(*continued));
                continued.reset();
            }
        }
        offset += recordSize;
    }
    if (continued) fail("Incomplete ISO9660 multi extent file");
    return result;
}

std::optional<ImageFiles> isoFiles(const Input& image) {
    std::optional<ImageFiles> primary;
    for (uint64_t sector = 16; sector < 16 + 256 && (sector + 1) * sectorSize <= image.size; ++sector) {
        const auto volume = readBytes(image, sector * sectorSize, static_cast<size_t>(sectorSize));
        if (!equals(std::span(volume).subspan(1, 5), "CD001")) break;
        if (volume[6] != 1) fail("Unsupported ISO9660 volume descriptor version");
        if (volume[0] == 255) break;
        if (volume[0] == 1) {
            auto files = isoDirectory(image, volume, false);
            if (files.control) return files;
            primary = std::move(files);
        } else if (volume[0] == 2 && volume[88] == '%' && volume[89] == '/' &&
                   (volume[90] == '@' || volume[90] == 'C' || volume[90] == 'E')) {
            auto files = isoDirectory(image, volume, true);
            if (files.control) return files;
            if (!primary) primary = std::move(files);
        }
    }
    return primary;
}

uint16_t crc16(std::span<const uint8_t> bytes) {
    uint16_t crc = 0;
    for (const uint8_t value : bytes) {
        crc ^= static_cast<uint16_t>(value) << 8;
        for (int bit = 0; bit < 8; ++bit)
            crc = static_cast<uint16_t>((crc << 1) ^ ((crc & 0x8000) ? 0x1021 : 0));
    }
    return crc;
}

void validateTag(std::span<const uint8_t> bytes, std::optional<uint32_t> location, size_t required) {
    if (bytes.size() < required || required < 16) fail("Truncated UDF descriptor");
    const uint16_t version = le16(bytes, 2);
    if (version != 2 && version != 3) fail("Unsupported UDF descriptor version");
    uint8_t checksum = 0;
    for (size_t i = 0; i < 16; ++i) if (i != 4) checksum = static_cast<uint8_t>(checksum + bytes[i]);
    if (checksum != bytes[4]) fail("UDF descriptor tag checksum mismatch");
    const uint16_t length = le16(bytes, 10);
    if (length > bytes.size() - 16 || length < required - 16) fail("UDF descriptor CRC length does not cover its fields");
    if (crc16(bytes.subspan(16, length)) != le16(bytes, 8)) fail("UDF descriptor CRC mismatch");
    if (location && le32(bytes, 12) != *location) fail("UDF descriptor tag location mismatch");
}

struct UdfAddress { uint32_t size{}, block{}; uint16_t partition{}; };

UdfAddress longAddress(std::span<const uint8_t> bytes, size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < 16) fail("Truncated UDF long allocation descriptor");
    const uint32_t size = le32(bytes, offset);
    if (size >> 30) fail("Unsupported UDF unrecorded or continuation ICB extent");
    return {size, le32(bytes, offset + 4), le16(bytes, offset + 8)};
}

struct Partition { uint64_t start{}, size{}; uint32_t sequence{}; };

class Udf {

public:
    explicit Udf(Input image) : image_(std::move(image)) {}
    std::optional<ImageFiles> files() {
        std::optional<uint32_t> anchorBlock;
        const uint64_t sectors = image_.size / sectorSize;
        std::vector<uint64_t> candidates{256};
        if (sectors) candidates.push_back(sectors - 1);
        if (sectors > 256) candidates.push_back(sectors - 257);
        std::vector<uint8_t> anchor;
        for (const uint64_t block : candidates) {
            if (block >= sectors || block > std::numeric_limits<uint32_t>::max()) continue;
            auto bytes = readBytes(image_, block * sectorSize, static_cast<size_t>(sectorSize));
            if (le16(bytes, 0) == 2) { anchor = std::move(bytes); anchorBlock = static_cast<uint32_t>(block); break; }
        }
        if (!anchorBlock) return std::nullopt;
        validateTag(anchor, anchorBlock, 512);
        const uint32_t length = le32(anchor, 16), first = le32(anchor, 20);
        if (!length || length > metadataLimit || length % sectorSize) fail("Invalid UDF volume descriptor sequence length");
        image_.slice(static_cast<uint64_t>(first) * sectorSize, length);
        std::vector<uint8_t> logical;
        uint32_t logicalSequence = 0;
        for (uint32_t index = 0; index < length / sectorSize; ++index) {
            const uint64_t position = static_cast<uint64_t>(first) + index;
            if (position > std::numeric_limits<uint32_t>::max()) fail("UDF volume descriptor location overflow");
            auto descriptor = readBytes(image_, position * sectorSize, static_cast<size_t>(sectorSize));
            const uint16_t tag = le16(descriptor, 0);
            if (!tag) break;
            validateTag(descriptor, static_cast<uint32_t>(position), tag == 6 ? 440 : 512);
            if (tag == 8) break;
            if (tag == 3) fail("Chained UDF volume descriptor sequences are unsupported");
            if (tag == 5) {
                if (!(le16(descriptor, 20) & 1)) fail("Unallocated UDF partition");
                const uint16_t number = le16(descriptor, 22);
                const Partition partition{static_cast<uint64_t>(le32(descriptor, 188)) * sectorSize,
                    static_cast<uint64_t>(le32(descriptor, 192)) * sectorSize, le32(descriptor, 16)};
                image_.slice(partition.start, partition.size);
                const auto existing = partitions_.find(number);
                if (existing == partitions_.end() || partition.sequence > existing->second.sequence) partitions_[number] = partition;
            } else if (tag == 6 && (logical.empty() || le32(descriptor, 16) > logicalSequence)) {
                logicalSequence = le32(descriptor, 16);
                logical = std::move(descriptor);
            }
        }
        if (logical.empty()) fail("UDF logical volume descriptor is missing");
        blockSize_ = le32(logical, 212);
        if (blockSize_ < 512 || blockSize_ > 8192 || (blockSize_ & (blockSize_ - 1))) fail("Unsupported UDF logical block size");
        const uint32_t mapsSize = le32(logical, 264), mapCount = le32(logical, 268);
        if (!mapCount || mapCount > 256 || mapsSize > logical.size() - 440) fail("Invalid UDF partition map extent");
        validateTag(logical, std::nullopt, 440 + mapsSize);
        size_t offset = 440;
        for (uint32_t index = 0; index < mapCount; ++index) {
            if (offset + 2 > 440 + mapsSize) fail("Truncated UDF partition map");
            const uint8_t type = logical[offset], size = logical[offset + 1];
            if (type != 1) fail("UDF virtual, sparable, and metadata partition maps are unsupported");
            if (size != 6 || size > 440 + mapsSize - offset) fail("Invalid UDF physical partition map");
            if (le16(logical, offset + 2) != 1) fail("Multi volume UDF images are unsupported");
            const auto partition = partitions_.find(le16(logical, offset + 4));
            if (partition == partitions_.end()) fail("UDF partition map references an unknown partition");
            maps_.push_back(partition->second);
            offset += size;
        }
        if (offset != 440 + mapsSize) fail("UDF partition map count and length disagree");
        const UdfAddress fileSet = longAddress(logical, 248);
        if (fileSet.size < 512) fail("UDF file set descriptor extent is too short");
        const auto set = readBytes(extent(fileSet.partition, fileSet.block, fileSet.size), 0, std::min<size_t>(fileSet.size, blockSize_));
        if (le16(set, 0) != 256) fail("UDF file set descriptor is missing");
        validateTag(set, fileSet.block, 512);
        if (le32(set, 448)) fail("Chained UDF file set descriptors are unsupported");
        const auto root = metadata(file(longAddress(set, 400), true));
        ImageFiles result;
        for (size_t position = 0; position < root.size();) {
            if (root.size() - position < 38) fail("Truncated UDF file identifier descriptor");
            const auto descriptor = std::span(root).subspan(position);
            if (le16(descriptor, 0) != 257) fail("Invalid UDF file identifier descriptor tag");
            const size_t nameOffset = 38u + le16(descriptor, 36);
            const size_t size = nameOffset + descriptor[19];
            const size_t aligned = (size + 3) & ~size_t(3);
            if (aligned > descriptor.size()) fail("Truncated UDF file identifier");
            validateTag(descriptor.first(aligned), std::nullopt, size);
            if (!(descriptor[18] & (4 | 8))) {
                const auto encoded = descriptor.subspan(nameOffset, descriptor[19]);
                std::string name;
                if (!encoded.empty()) {
                    if (encoded[0] == 8) name.assign(reinterpret_cast<const char*>(encoded.data() + 1), encoded.size() - 1);
                    else if (encoded[0] == 16) name = isoName(encoded.subspan(1), true);
                    else fail("Unsupported UDF file identifier compression identifier");
                }
                name = lower(std::move(name));
                if (name == "recctrl.bin" || name == "recdata.bin") {
                    if (descriptor[18] & 2) fail("Recovery image root entry is a directory: " + name);
                    addFile(result, name, file(longAddress(descriptor, 20), false));
                }
            }
            position += aligned;
        }
        return result;
    }
    
private:
    Input extent(uint16_t partition, uint32_t block, uint64_t size) const {
        if (partition >= maps_.size()) fail("UDF extent references an unknown partition map");
        const Partition& map = maps_[partition];
        const uint64_t offset = static_cast<uint64_t>(block) * blockSize_;
        if (offset > map.size || size > map.size - offset) fail("UDF extent extends outside partition");
        return image_.slice(map.start + offset, size);
    }
    
    ImageFile file(UdfAddress address, bool directory) const {
        if (address.size < 176) fail("UDF file entry extent is too short");
        const auto bytes = readBytes(extent(address.partition, address.block, address.size), 0, std::min(address.size, blockSize_));
        const uint16_t tag = le16(bytes, 0);
        const size_t base = tag == 261 ? 176 : tag == 266 ? 216 : 0;
        if (!base) fail("Unsupported UDF ICB: expected a file entry");
        validateTag(bytes, address.block, base);
        if (le16(bytes, 20) != 4) fail("Unsupported UDF ICB strategy");
        if (bytes[27] != (directory ? 4 : 5)) fail("UDF recovery entry has an unexpected file type");
        const uint32_t extended = le32(bytes, base - 8), allocations = le32(bytes, base - 4);
        if (extended > bytes.size() - base || allocations > bytes.size() - base - extended) fail("UDF allocation descriptors exceed file entry");
        validateTag(bytes, address.block, base + extended + allocations);
        ImageFile result;
        result.size = le64(bytes, 56);
        if (result.size > (directory ? metadataLimit : imageLimit)) fail("UDF recovery file exceeds its size limit");
        const auto descriptors = std::span(bytes).subspan(base + extended, allocations);
        const uint16_t format = le16(bytes, 34) & 7;
        if (format == 3) {
            if (result.size > descriptors.size()) fail("UDF embedded file exceeds its allocation area");
            result.inlineBytes.assign(descriptors.begin(), descriptors.begin() + static_cast<size_t>(result.size));
            return result;
        }
        if (format > 1) fail("UDF extended allocation descriptors are unsupported");
        const size_t stride = format ? 16 : 8;
        if (descriptors.size() % stride) fail("Truncated UDF allocation descriptor");
        uint64_t remaining = result.size;
        for (size_t offset = 0; offset < descriptors.size(); offset += stride) {
            const uint32_t encodedLength = le32(descriptors, offset);
            const uint32_t size = encodedLength & 0x3fffffff;
            if (encodedLength >> 30) fail("UDF sparse and continuation allocation extents are unsupported");
            const uint16_t partition = format ? le16(descriptors, offset + 8) : address.partition;
            const Input part = extent(partition, le32(descriptors, offset + 4), size);
            const uint64_t count = std::min<uint64_t>(remaining, size);
            if (count) result.parts.push_back(part.slice(0, count));
            remaining -= count;
        }
        if (remaining) fail("UDF allocation descriptors do not cover file length");
        return result;
    }
    Input image_;
    uint32_t blockSize_{};
    std::map<uint16_t, Partition> partitions_;
    std::vector<Partition> maps_;
};

struct ZipEntry {
    std::string name;
    uint64_t compressed{}, size{}, offset{};
    uint32_t crc{};
    uint16_t flags{}, method{};
};

ZipEntry isoEntry(const Input& zip) {
    if (zip.size < 22) fail("ZIP archive is too short");
    const size_t tailSize = static_cast<size_t>(std::min<uint64_t>(zip.size, 22 + 65535));
    const auto tail = readBytes(zip, zip.size - tailSize, tailSize);
    std::optional<size_t> end;
    for (size_t offset = tail.size() - 22;; --offset) {
        if (le32(tail, offset) == 0x06054b50 && le16(tail, offset + 20) == tail.size() - offset - 22) { end = offset; break; }
        if (!offset) break;
    }
    if (!end) fail("ZIP end of central directory record was not found");
    const auto footer = std::span(tail).subspan(*end);
    if (le16(footer, 4) || le16(footer, 6)) fail("Multi disk ZIP archives are unsupported");
    uint64_t count = le16(footer, 10), centralSize = le32(footer, 12), centralOffset = le32(footer, 16);
    const uint64_t footerOffset = zip.size - tailSize + *end;
    if (count == 0xffff || centralSize == 0xffffffff || centralOffset == 0xffffffff) {
        if (footerOffset < 20) fail("ZIP64 locator is missing");
        const auto locator = readBytes(zip, footerOffset - 20, 20);
        if (le32(locator, 0) != 0x07064b50) fail("ZIP64 locator is missing");
        if (le32(locator, 4) || le32(locator, 16) != 1) fail("Multi disk ZIP64 archives are unsupported");
        const uint64_t recordOffset = le64(locator, 8);
        const auto record = readBytes(zip, recordOffset, 56);
        const uint64_t recordSize = le64(record, 4);
        if (le32(record, 0) != 0x06064b50 || recordSize < 44 || recordSize > metadataLimit ||
            recordOffset > footerOffset - 20 || recordSize + 12 > footerOffset - 20 - recordOffset)
            fail("Invalid ZIP64 end of central directory record");
        if (le32(record, 16) || le32(record, 20) || le64(record, 24) != le64(record, 32))
            fail("Multi disk ZIP64 archives are unsupported");
        count = le64(record, 32); centralSize = le64(record, 40); centralOffset = le64(record, 48);
    } else if (le16(footer, 8) != count) fail("ZIP disk entry counts disagree");
    if (count > 1000000) fail("ZIP central directory exceeds the one million entry limit");
    if (centralOffset > footerOffset || centralSize > footerOffset - centralOffset) fail("ZIP central directory extends outside archive");
    Reader central(zip.slice(centralOffset, centralSize));
    std::optional<ZipEntry> selected;
    for (uint64_t index = 0; index < count; ++index) {
        const auto header = central.bytes(46);
        if (le32(header, 0) != 0x02014b50) fail("Invalid ZIP central directory entry");
        const auto nameBytes = central.bytes(le16(header, 28));
        const auto extra = central.bytes(le16(header, 30));
        central.skip(le16(header, 32));
        ZipEntry entry{std::string(reinterpret_cast<const char*>(nameBytes.data()), nameBytes.size()),
            le32(header, 20), le32(header, 24), le32(header, 42), le32(header, 16), le16(header, 8), le16(header, 10)};
        uint32_t disk = le16(header, 34);
        const bool zip64 = entry.size == 0xffffffff || entry.compressed == 0xffffffff || entry.offset == 0xffffffff || disk == 0xffff;
        bool found64 = false;
        for (size_t offset = 0; offset < extra.size();) {
            if (extra.size() - offset < 4) fail("Truncated ZIP extra field");
            const uint16_t type = le16(extra, offset), size = le16(extra, offset + 2);
            offset += 4;
            if (size > extra.size() - offset) fail("Truncated ZIP extra field data");
            if (type == 1 && zip64) {
                if (found64) fail("Duplicate ZIP64 extra field");
                found64 = true;
                const auto field = std::span(extra).subspan(offset, size);
                size_t cursor = 0;
                auto next = [&]() { const uint64_t value = le64(field, cursor); cursor += 8; return value; };
                if (entry.size == 0xffffffff) entry.size = next();
                if (entry.compressed == 0xffffffff) entry.compressed = next();
                if (entry.offset == 0xffffffff) entry.offset = next();
                if (disk == 0xffff) disk = le32(field, cursor);
            }
            offset += size;
        }
        if (zip64 && !found64) fail("ZIP64 entry sizes are missing");
        if (disk) fail("Multi disk ZIP entries are unsupported");
        const std::string name = lower(entry.name);
        if (name.size() >= 4 && name.substr(name.size() - 4) == ".iso") {
            if (selected) fail("ZIP contains multiple ISO images, extract the intended image first");
            if (entry.flags & (1 | 0x40 | 0x2000)) fail("Encrypted ZIP entries are unsupported");
            if (entry.flags & ~uint16_t(0x080e)) fail("Unsupported ZIP ISO feature flags");
            if (entry.method != 0 && entry.method != 8) fail("ZIP ISO must use stored or deflate compression");
            if (entry.size > imageLimit) fail("ZIP ISO exceeds the 64 GiB image limit");
            const auto local = readBytes(zip, entry.offset, 30);
            if (le32(local, 0) != 0x04034b50 || le16(local, 6) != entry.flags || le16(local, 8) != entry.method)
                fail("ZIP local header disagrees with central directory");
            const uint16_t nameSize = le16(local, 26), extraSize = le16(local, 28);
            if (entry.offset > centralOffset || 30u + nameSize + extraSize > centralOffset - entry.offset)
                fail("ZIP ISO local header extends into central directory");
            const auto localName = readBytes(zip, entry.offset + 30, nameSize);
            if (localName != nameBytes) fail("ZIP local and central file names disagree");
            const uint64_t start = entry.offset + 30 + nameSize + extraSize;
            if (start > centralOffset || entry.compressed > centralOffset - start) fail("ZIP ISO data extends into central directory");
            if (!(entry.flags & 8)) {
                uint64_t localCompressed = le32(local, 18), localSize = le32(local, 22);
                if (localCompressed == 0xffffffff || localSize == 0xffffffff) {
                    const auto localExtra = readBytes(zip, entry.offset + 30 + nameSize, extraSize);
                    bool found = false;
                    for (size_t offset = 0; offset < localExtra.size();) {
                        if (localExtra.size() - offset < 4) fail("Truncated ZIP local extra field");
                        const uint16_t type = le16(localExtra, offset), size = le16(localExtra, offset + 2);
                        offset += 4;
                        if (size > localExtra.size() - offset) fail("Truncated ZIP local extra field data");
                        if (type == 1) {
                            if (found) fail("Duplicate ZIP64 local extra field");
                            found = true;
                            const auto field = std::span(localExtra).subspan(offset, size);
                            size_t cursor = 0;
                            if (localSize == 0xffffffff) { localSize = le64(field, cursor); cursor += 8; }
                            if (localCompressed == 0xffffffff) localCompressed = le64(field, cursor);
                        }
                        offset += size;
                    }
                    if (!found) fail("ZIP64 local sizes are missing");
                }
                if (le32(local, 14) != entry.crc || localCompressed != entry.compressed || localSize != entry.size)
                    fail("ZIP local CRC or sizes disagree with central directory");
            } else {
                const uint64_t endOffset = start + entry.compressed;
                const bool wide = le32(local, 18) == 0xffffffff || le32(local, 22) == 0xffffffff ||
                    entry.size >= 0xffffffff || entry.compressed >= 0xffffffff;
                const size_t descriptorSize = wide ? 20 : 12;
                if (endOffset > centralOffset || descriptorSize > centralOffset - endOffset)
                    fail("ZIP data descriptor extends into central directory");
                const auto prefix = readBytes(zip, endOffset, 4);
                const bool signature = le32(prefix, 0) == 0x08074b50;
                if (signature && descriptorSize + 4 > centralOffset - endOffset)
                    fail("Truncated ZIP data descriptor");
                const auto descriptor = readBytes(zip, endOffset + (signature ? 4 : 0), descriptorSize);
                if (le32(descriptor, 0) != entry.crc ||
                    (wide ? le64(descriptor, 4) : le32(descriptor, 4)) != entry.compressed ||
                    (wide ? le64(descriptor, 12) : le32(descriptor, 8)) != entry.size)
                    fail("ZIP data descriptor CRC or sizes disagree with central directory");
            }
            entry.offset = start;
            selected = std::move(entry);
        }
    }
    if (central.remaining()) fail("ZIP central directory size and entry count disagree");
    if (!selected) fail("No ISO image was found in ZIP archive");
    return *selected;
}

class Inflate {

public:
    Inflate() { if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) fail("Failed to initialize ZIP deflate decoder"); }
    ~Inflate() { inflateEnd(&stream); }
    z_stream stream{};
};

void extractIso(const Input& zip, const ZipEntry& entry, const fs::path& destination) {
    Output output(destination, false);
    std::vector<uint8_t> input(transferSize), decoded(transferSize);
    uLong crc = crc32(0, Z_NULL, 0);
    uint64_t written = 0;
    auto write = [&](std::span<const uint8_t> bytes) {
        if (bytes.size() > entry.size - written) fail("ZIP ISO decompressed size exceeds its declared size");
        output.write(bytes);
        crc = crc32(crc, bytes.data(), static_cast<uInt>(bytes.size()));
        written += bytes.size();
    };
    if (!entry.method) {
        if (entry.size != entry.compressed) fail("Stored ZIP ISO sizes disagree");
        for (uint64_t offset = 0; offset < entry.compressed;) {
            const size_t count = static_cast<size_t>(std::min<uint64_t>(input.size(), entry.compressed - offset));
            zip.read(entry.offset + offset, std::span(input).first(count));
            write(std::span(input).first(count));
            offset += count;
        }
    } else {
        Inflate decoder;
        uint64_t supplied = 0;
        for (;;) {
            if (!decoder.stream.avail_in && supplied < entry.compressed) {
                const size_t count = static_cast<size_t>(std::min<uint64_t>(input.size(), entry.compressed - supplied));
                zip.read(entry.offset + supplied, std::span(input).first(count));
                supplied += count;
                decoder.stream.next_in = input.data();
                decoder.stream.avail_in = static_cast<uInt>(count);
            }
            decoder.stream.next_out = decoded.data();
            decoder.stream.avail_out = static_cast<uInt>(decoded.size());
            const uInt previousInput = decoder.stream.avail_in;
            const int result = inflate(&decoder.stream, Z_NO_FLUSH);
            const size_t count = decoded.size() - decoder.stream.avail_out;
            write(std::span(decoded).first(count));
            if (result == Z_STREAM_END) {
                if (supplied != entry.compressed || decoder.stream.avail_in) fail("Trailing data inside ZIP ISO deflate stream");
                break;
            }
            if (result != Z_OK || (!count && previousInput == decoder.stream.avail_in))
                fail("Invalid or truncated ZIP ISO deflate stream");
        }
    }
    if (written != entry.size) fail("ZIP ISO decompressed size does not match central directory");
    if (static_cast<uint32_t>(crc) != entry.crc) fail("ZIP ISO CRC mismatch");
    output.commit(entry.size);
}
}

void processImage(Input image, const fs::path& output, const Options& options) {
    std::optional<ImageFiles> files = gdfFiles(image);
    if (!files || !files->control) files = isoFiles(image);
    if (!files || !files->control) files = Udf(image).files();
    if (!files || !files->control) fail("No recctrl.bin was found in the image root (GDF, ISO9660, or physical-partition UDF)");
    if (!options.listOnly && !files->data) fail("No recdata.bin was found in the image root");
    TempDirectory temporary;
    const Input control = materialize(*files->control, temporary.path() / L"recctrl.bin");
    std::optional<Input> data;
    if (!options.listOnly) data = materialize(*files->data, temporary.path() / L"recdata.bin");
    processRecovery(control, data ? &*data : nullptr, output, options);
}

void processZip(Input zip, const fs::path& output, const Options& options) {
    const ZipEntry entry = isoEntry(zip);
    std::cout << "Reading ISO from ZIP: " << entry.name << " (" << entry.size << " bytes)\n";
    TempDirectory temporary;
    const fs::path path = temporary.path() / L"recovery.iso";
    extractIso(zip, entry, path);
    processImage(Input::open(path), output, options);
}
}