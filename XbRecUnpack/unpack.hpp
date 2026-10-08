#pragma once

#include <windows.h>
#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace xbr {
namespace fs = std::filesystem;

struct Options { bool listOnly{}, romInfo{}, overwrite{}; };
[[noreturn]] void fail(std::string_view message);
std::string utf8(const fs::path& path);
fs::path textPath(std::string_view text, bool utf8Name = true);
std::string lower(std::string text);
struct PathLess { bool operator()(const fs::path& left, const fs::path& right) const; };
void validateOutputTree(const std::set<fs::path, PathLess>& paths);

class File {

public:
    explicit File(const fs::path& path);
    ~File();
    File(const File&) = delete;
    File& operator=(const File&) = delete;
    void read(uint64_t offset, std::span<uint8_t> bytes) const;
    uint64_t size() const noexcept { return size_; }
    const fs::path& path() const noexcept { return path_; }
private:
    fs::path path_;
    HANDLE handle_{INVALID_HANDLE_VALUE};
    uint64_t size_{};
    mutable std::array<uint8_t, 1u << 20> cache_{};
    mutable uint64_t cacheOffset_{};
    mutable size_t cacheSize_{};
};

struct Input {
    std::shared_ptr<File> file;
    uint64_t offset{}, size{};
    static Input open(const fs::path& path);
    Input slice(uint64_t start, uint64_t length) const;
    void read(uint64_t start, std::span<uint8_t> bytes) const;
};

class Reader {

public:
    explicit Reader(Input input) : input_(std::move(input)) {}
    uint64_t tell() const noexcept { return position_; }
    uint64_t remaining() const noexcept { return input_.size - position_; }
    void seek(uint64_t position);
    void skip(uint64_t count);
    std::vector<uint8_t> bytes(size_t count);
    uint8_t u8();
    uint16_t u16();
    uint32_t u32();
    uint64_t u64();
    std::string cstring(size_t limit = 32768);
    std::string msString();
private:
    Input input_;
    uint64_t position_{};
};

fs::path safeOutput(const fs::path& root, std::string_view relative);

class Output {

public:
    Output(const fs::path& destination, bool overwrite);
    ~Output();
    Output(const Output&) = delete;
    Output& operator=(const Output&) = delete;
    void write(std::span<const uint8_t> bytes);
    void commit(uint64_t expected, uint64_t fileTime = 0);
private:
    void writePhysical(std::span<const uint8_t> bytes);
    void flush();
    fs::path destination_, temporary_;
    HANDLE handle_{INVALID_HANDLE_VALUE};
    uint64_t written_{};
    std::array<uint8_t, 1u << 18> buffer_;
    size_t buffered_{};
    bool overwrite_{}, committed_{};
};

void copyInput(Input input, const fs::path& destination, bool overwrite, uint64_t fileTime = 0);

class TempDirectory {

public:
    TempDirectory();
    ~TempDirectory();
    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;
    const fs::path& path() const noexcept { return path_; }
private:
    fs::path path_;
};

struct CabEntry {
    std::string name;
    uint32_t size{}, folderOffset{};
    uint16_t folder{}, date{}, time{}, attributes{};
};

struct Cabinet {
    Input input;
    std::vector<CabEntry> entries;
    uint16_t folders{}, flags{}, setId{}, index{};
};

Cabinet readCabinet(Input input, bool validateData = false);
using CabTarget = std::function<fs::path(const CabEntry&, size_t)>;
void extractCabinet(const Cabinet& cabinet, const CabTarget& target, bool overwrite);
std::vector<uint8_t> readCabEntry(const Cabinet& cabinet, size_t index, size_t limit);

void processRemote(Input input, const fs::path& output, const Options& options);
void processRecovery(Input control, const Input* data, const fs::path& output, const Options& options);
void processImage(Input image, const fs::path& output, const Options& options);
void processZip(Input zip, const fs::path& output, const Options& options);
void printRomSummary(const fs::path& root, bool detailed);
}