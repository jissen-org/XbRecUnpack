#include "unpack.hpp"
#include <algorithm>
#include <iostream>
#include <set>

namespace xbr {
namespace {

struct Manifest {
    std::string variant, language, action, source, destination;
    std::string directory() const { return variant + (language.empty() || language == "0000" ? "" : "." + language); }
};

bool fileAction(const std::string& action) {
    return action == "file" || action == "sharedfile" || action == "backupsharedfile" || action == "singlefile";
}

std::vector<std::vector<std::string>> csv(std::span<const uint8_t> bytes) {
    std::vector<std::vector<std::string>> rows;
    std::vector<std::string> row;
    std::string field;
    bool quoted{}, closed{};
    for (size_t position = 0; position <= bytes.size(); ++position) {
        const char c = position == bytes.size() ? '\n' : static_cast<char>(bytes[position]);
        if (!c) fail("NUL in manifest CSV");
        if (quoted) {
            if (c == '"') {
                if (position + 1 < bytes.size() && bytes[position + 1] == '"') { field += '"'; ++position; }
                else { quoted = false; closed = true; }
            } else if (position == bytes.size()) fail("Unterminated quoted manifest field");
            else field += c;
            continue;
        }
        if (c == '"') {
            if (!field.empty() || closed) fail("Unexpected quote in manifest CSV");
            quoted = true;
        } else if (c == ',' || c == '\n') {
            if (!field.empty() && field.back() == '\r') field.pop_back();
            row.push_back(std::move(field)); field.clear(); closed = false;
            if (c == ',' && row.size() == 3 && !fileAction(row[1]) && row[1] != "copy" &&
                !fileAction(row[2]) && row[2] != "copy") {
                // Setup directives contain command-line quotes rather than CSV escaping.
                while (position + 1 < bytes.size() && bytes[position + 1] != '\n') ++position;
            }
            if (c == '\n') { if (row.size() > 1 || !row[0].empty()) rows.push_back(std::move(row)); row.clear(); }
        } else if (closed && c != '\r') fail("Characters after quoted manifest field");
        else if (!closed) field += c;
    }
    return rows;
}

std::vector<Cabinet> cabinets(Input input) {
    std::vector<Cabinet> result;
    std::vector<uint8_t> buffer((1u << 20) + 7);
    uint64_t position{};
    while (position + 8 <= input.size) {
        const auto count = static_cast<size_t>(std::min<uint64_t>(buffer.size(), input.size - position));
        input.read(position, std::span(buffer).first(count));
        bool found{};
        for (size_t index = 0; index + 8 <= count; ++index) {
            if (buffer[index] != 'M' || buffer[index + 1] != 'S' || buffer[index + 2] != 'C' || buffer[index + 3] != 'F' ||
                buffer[index + 4] || buffer[index + 5] || buffer[index + 6] || buffer[index + 7]) continue;
            const uint64_t absolute = position + index;
            if (input.size - absolute < 36) fail("Truncated embedded CAB header");
            Reader header(input.slice(absolute, input.size - absolute)); header.seek(24);
            if (header.u8() != 3 || header.u8() != 1) continue;
            std::cout << "Validating CAB " << result.size() + 1 << " at offset " << absolute << "...\n";
            result.push_back(readCabinet(input.slice(absolute, input.size - absolute), true));
            if (result.size() > 4096) fail("Embedded CAB count exceeds limit");
            position = absolute + result.back().input.size; found = true; break;
        }
        if (!found) position += count - 7;
    }
    return result;
}
}

void processRemote(Input input, const fs::path& output, const Options& options) {
    Reader reader(input);
    if (reader.u16() != 0x5A4D) fail("SDK/recovery executable has no MZ header");
    auto archives = cabinets(input);
    if (archives.size() < 2) fail("SDK/recovery executable does not contain required CAB files");
    size_t metadata = archives.size();
    size_t manifestIndex{};
    for (size_t index = 0; index < archives.size(); ++index)
        for (size_t entry = 0; entry < archives[index].entries.size(); ++entry)
            if (lower(archives[index].entries[entry].name) == "manifest.csv") {
                if (metadata != archives.size()) fail("Ambiguous SDK manifest: multiple manifest.csv files");
                metadata = index; manifestIndex = entry;
            }
    if (metadata == archives.size()) fail("SDK/recovery executable contains no manifest.csv");
    const auto contents = readCabEntry(archives[metadata], manifestIndex, 16u << 20);
    std::vector<Manifest> entries;
    for (auto row : csv(contents)) {
        if (row.size() >= 2 && (fileAction(row[1]) || row[1] == "copy")) row.insert(row.begin() + 1, "");
        if (row.size() < 3 || (!fileAction(row[2]) && row[2] != "copy")) continue;
        if (row.size() < 6) fail("File action has incomplete manifest columns");
        entries.push_back({row[1].empty() ? "_All" : row[1], row[0], row[2], row[4], row[5]});
    }
    if (entries.empty()) fail("Manifest contains no supported file actions");
    std::vector<std::vector<fs::path>> targets(archives.size());
    for (size_t index = 0; index < archives.size(); ++index) targets[index].resize(archives[index].entries.size());
    std::vector<std::pair<fs::path, fs::path>> copies;
    std::set<fs::path, PathLess> outputs;
    size_t archive{}, file{};
    auto advance = [&] {
        while (archive < archives.size() && (archive == metadata || file == archives[archive].entries.size())) { ++archive; file = 0; }
    };
    for (size_t index = 0; index < entries.size(); ++index) {
        const auto& entry = entries[index];
        const auto source = safeOutput(output, entry.directory() + "/" + entry.source);
        fs::path destination = source;
        if (entry.action == "copy") {
            destination = safeOutput(output, entry.variant + "/" + entry.destination);
            if (!outputs.contains(source)) fail("Manifest copy source has not been declared earlier: " + utf8(source));
            copies.emplace_back(source, destination);
        } else {
            advance();
            if (archive == archives.size()) fail("Manifest has more file entries than the CAB payload");
            const auto& cabEntry = archives[archive].entries[file];
            std::string expected = lower(entry.source), actual = lower(cabEntry.name);
            std::replace(expected.begin(), expected.end(), '/', '\\');
            std::replace(actual.begin(), actual.end(), '/', '\\');
            if (expected != actual) fail("Manifest/CAB name mismatch: " + entry.source + " versus " + cabEntry.name);
            targets[archive][file++] = destination;
        }
        if (!outputs.insert(destination).second) fail("Duplicate manifest output: " + utf8(destination));
        if (!options.listOnly && fs::exists(destination) && !options.overwrite) fail("Output already exists: " + utf8(destination));
        std::cout << '(' << index + 1 << '/' << entries.size() << ") " << utf8(destination.lexically_relative(fs::absolute(output)))
            << (entry.action == "copy" ? " (copy)" : "") << '\n';
    }
    advance();
    validateOutputTree(outputs);
    if (archive != archives.size()) fail("CAB payload contains files absent from the manifest");
    if (options.listOnly) return;
    for (size_t index = 0; index < archives.size(); ++index) {
        if (index == metadata) continue;
        std::cout << "Extracting CAB " << index + 1 << "...\n";
        extractCabinet(archives[index], [&](const CabEntry&, size_t entry) { return targets[index][entry]; }, options.overwrite);
    }
    for (const auto& [source, destination] : copies) {
        WIN32_FILE_ATTRIBUTE_DATA attributes{};
        if (!GetFileAttributesExW(source.c_str(), GetFileExInfoStandard, &attributes)) fail("Cannot read manifest copy timestamp: " + utf8(source));
        const auto time = uint64_t(attributes.ftLastWriteTime.dwLowDateTime) | (uint64_t(attributes.ftLastWriteTime.dwHighDateTime) << 32);
        copyInput(Input::open(source), destination, options.overwrite, time);
    }
}
}