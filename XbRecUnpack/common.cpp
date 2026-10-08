#include "unpack.hpp"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace xbr {
namespace {

std::atomic<uint64_t> sequence{};

void winError(std::string_view operation, DWORD error = GetLastError()) {
    fail(std::string(operation) + " (Windows error " + std::to_string(error) + ")");
}

void checkParents(const fs::path& path) {
    const auto target = fs::absolute(path).lexically_normal();
    fs::path current;
    for (const auto& part : target) {
        current /= part;
        const DWORD attributes = GetFileAttributesW(current.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            const DWORD error = GetLastError();
            if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) continue;
            winError("Inspecting output path " + utf8(current));
        } else if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) {
            fail("Output path contains a reparse point: " + utf8(current));
        } else if (current != target && !(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
            fail("Output parent is a regular file: " + utf8(current));
        }
    }
}

std::wstring uniqueName() {
    return L"xbr-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
        std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(sequence++);
}
}

[[noreturn]] void fail(std::string_view message) { throw std::runtime_error(std::string(message)); }
std::string utf8(const fs::path& path) {
    const auto value = path.u8string();
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

fs::path textPath(std::string_view text, bool utf8Name) {
    if (text.empty()) return {};
    if (text.size() > static_cast<size_t>(INT_MAX)) fail("Path exceeds Windows string limit");
    const UINT page = utf8Name ? CP_UTF8 : CP_ACP;
    const DWORD flags = utf8Name ? MB_ERR_INVALID_CHARS : 0;
    const int length = MultiByteToWideChar(page, flags, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (!length) winError("Decoding archive path");
    std::wstring value(static_cast<size_t>(length), L'\0');
    if (!MultiByteToWideChar(page, flags, text.data(), static_cast<int>(text.size()), value.data(), length))
        winError("Decoding archive path");
    return fs::path(value);
}

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

bool PathLess::operator()(const fs::path& left, const fs::path& right) const {
    const auto a = fs::path(left).make_preferred().wstring();
    const auto b = fs::path(right).make_preferred().wstring();
    if (a.size() > INT_MAX || b.size() > INT_MAX) fail("Path exceeds Windows comparison limit");
    const int result = CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE);
    if (!result) winError("Comparing output paths");
    return result == CSTR_LESS_THAN;
}

void validateOutputTree(const std::set<fs::path, PathLess>& paths) {
    for (const auto& path : paths) {
        for (auto parent = path.parent_path(); !parent.empty();) {
            if (paths.contains(parent)) fail("Archive output is both a file and a parent directory: " + utf8(parent));
            const auto next = parent.parent_path();
            if (next == parent) break;
            parent = next;
        }
    }
}

File::File(const fs::path& path) : path_(fs::absolute(path)) {
    handle_ = CreateFileW(path_.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
    if (handle_ == INVALID_HANDLE_VALUE) winError("Opening " + utf8(path_));
    LARGE_INTEGER length{};
    if (!GetFileSizeEx(handle_, &length) || length.QuadPart < 0) {
        const DWORD error = GetLastError(); CloseHandle(handle_); handle_ = INVALID_HANDLE_VALUE;
        SetLastError(error); winError("Reading input length");
    }
    size_ = static_cast<uint64_t>(length.QuadPart);
}

File::~File() { if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_); }
void File::read(uint64_t offset, std::span<uint8_t> bytes) const {
    if (offset > size_ || bytes.size() > size_ - offset) fail("Read exceeds input extent: " + utf8(path_));
    if (bytes.empty()) return;
    auto readPhysical = [&](uint64_t start, std::span<uint8_t> target) {
        LARGE_INTEGER position{}; position.QuadPart = static_cast<LONGLONG>(start);
        if (!SetFilePointerEx(handle_, position, nullptr, FILE_BEGIN)) winError("Seeking input");
        while (!target.empty()) {
            const DWORD count = static_cast<DWORD>(std::min<size_t>(target.size(), 1u << 20));
            DWORD read{};
            if (!ReadFile(handle_, target.data(), count, &read, nullptr)) winError("Reading input");
            if (!read) fail("Unexpected end of input: " + utf8(path_));
            target = target.subspan(read);
        }
    };
    while (!bytes.empty()) {
        if (offset >= cacheOffset_ && offset - cacheOffset_ < cacheSize_) {
            const size_t position = static_cast<size_t>(offset - cacheOffset_);
            const size_t count = std::min(bytes.size(), cacheSize_ - position);
            std::memcpy(bytes.data(), cache_.data() + position, count);
            offset += count; bytes = bytes.subspan(count);
            continue;
        }
        if (bytes.size() >= cache_.size()) { readPhysical(offset, bytes); return; }
        const size_t count = static_cast<size_t>(std::min<uint64_t>(cache_.size(), size_ - offset));
        cacheSize_ = 0;
        readPhysical(offset, std::span(cache_).first(count));
        cacheOffset_ = offset; cacheSize_ = count;
    }
}

Input Input::open(const fs::path& path) {
    auto file = std::make_shared<File>(path); return {file, 0, file->size()};
}

Input Input::slice(uint64_t start, uint64_t length) const {
    if (start > size || length > size - start) fail("Input slice exceeds declared extent");
    return {file, offset + start, length};
}

void Input::read(uint64_t start, std::span<uint8_t> bytes) const {
    if (!file || start > size || bytes.size() > size - start) fail("Read exceeds declared input extent");
    file->read(offset + start, bytes);
}

void Reader::seek(uint64_t position) { if (position > input_.size) fail("Truncated input"); position_ = position; }
void Reader::skip(uint64_t count) { if (count > remaining()) fail("Truncated input"); position_ += count; }
std::vector<uint8_t> Reader::bytes(size_t count) {
    if (count > remaining()) fail("Truncated input");
    std::vector<uint8_t> result(count); input_.read(position_, result); position_ += count; return result;
}

uint8_t Reader::u8() { uint8_t value{}; input_.read(position_, {&value, 1}); ++position_; return value; }
uint16_t Reader::u16() {
    uint8_t bytes[2]; input_.read(position_, bytes); position_ += 2;
    return static_cast<uint16_t>(bytes[0] | (uint16_t(bytes[1]) << 8));
}

uint32_t Reader::u32() {
    uint8_t bytes[4]; input_.read(position_, bytes); position_ += 4;
    return uint32_t(bytes[0]) | (uint32_t(bytes[1]) << 8) | (uint32_t(bytes[2]) << 16) | (uint32_t(bytes[3]) << 24);
}

uint64_t Reader::u64() { const uint64_t low = u32(); return low | (uint64_t(u32()) << 32); }
std::string Reader::cstring(size_t limit) {
    std::string result;
    while (remaining() && result.size() < limit) { const char c = static_cast<char>(u8()); if (!c) return result; result += c; }
    fail("Unterminated or oversized archive string");
}

std::string Reader::msString() {
    const auto data = bytes(u16());
    if (tell() & 1) skip(1);
    if (std::find(data.begin(), data.end(), 0) != data.end()) fail("Embedded NUL in recovery string");
    return std::string(data.begin(), data.end());
}

fs::path safeOutput(const fs::path& root, std::string_view relative) {
    std::string normalized(relative);
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    if (normalized.empty() || normalized.front() == '/' || normalized.back() == '/') fail("Invalid archive output path: " + normalized);
    size_t start{};
    while (start < normalized.size()) {
        const size_t end = normalized.find('/', start);
        const std::string part = normalized.substr(start, end == std::string::npos ? end : end - start);
        if (part.empty() || part == "." || part == ".." || part.back() == '.' || part.back() == ' ')
            fail("Unsafe archive path component: " + normalized);
        for (unsigned char c : part) if (c < 32 || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
            fail("Unsafe archive path character: " + normalized);
        const auto base = lower(part.substr(0, part.find('.')));
        if (base == "con" || base == "prn" || base == "aux" || base == "nul" || base == "conin$" || base == "conout$" ||
            (base.size() == 4 && (base.starts_with("com") || base.starts_with("lpt")) && base[3] >= '0' && base[3] <= '9'))
            fail("Windows device name in archive path: " + normalized);
        if (end == std::string::npos) break;
        start = end + 1;
    }
    const fs::path result = fs::absolute(root).lexically_normal() / textPath(normalized);
    checkParents(result);
    const DWORD attributes = GetFileAttributesW(result.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY))
        fail("Output path is an existing directory: " + utf8(result));
    return result;
}

Output::Output(const fs::path& destination, bool overwrite)
    : destination_(fs::absolute(destination)), overwrite_(overwrite) {
    checkParents(destination_);
    const DWORD attributes = GetFileAttributesW(destination_.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (!overwrite || (attributes & FILE_ATTRIBUTE_DIRECTORY)))
        fail("Output already exists: " + utf8(destination_));
    if (!fs::exists(destination_.parent_path())) fs::create_directories(destination_.parent_path());
    checkParents(destination_.parent_path());
    bool recreatedParents{};
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
        temporary_ = destination_.parent_path() / (uniqueName() + L".tmp");
        handle_ = CreateFileW(temporary_.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle_ != INVALID_HANDLE_VALUE) return;
        const DWORD error = GetLastError();
        if (error == ERROR_PATH_NOT_FOUND && !recreatedParents) {
            checkParents(destination_);
            fs::create_directories(destination_.parent_path());
            checkParents(destination_.parent_path());
            recreatedParents = true;
            continue;
        }
        if (error != ERROR_FILE_EXISTS) winError("Creating temporary output " + utf8(temporary_), error);
    }
    fail("Unable to create unique output file");
}

Output::~Output() {
    if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
    if (!committed_ && !temporary_.empty()) DeleteFileW(temporary_.c_str());
}

void Output::write(std::span<const uint8_t> bytes) {
    if (bytes.size() > UINT64_MAX - written_) fail("Output length overflow");
    while (!bytes.empty()) {
        if (!buffered_ && bytes.size() >= buffer_.size()) {
            writePhysical(bytes); written_ += bytes.size(); return;
        }
        const size_t count = std::min(bytes.size(), buffer_.size() - buffered_);
        std::memcpy(buffer_.data() + buffered_, bytes.data(), count);
        buffered_ += count; written_ += count; bytes = bytes.subspan(count);
        if (buffered_ == buffer_.size()) flush();
    }
}

void Output::writePhysical(std::span<const uint8_t> bytes) {
    while (!bytes.empty()) {
        DWORD written{};
        const DWORD count = static_cast<DWORD>(std::min<size_t>(bytes.size(), 1u << 20));
        if (!WriteFile(handle_, bytes.data(), count, &written, nullptr)) winError("Writing output");
        if (!written) fail("Output write made no progress");
        bytes = bytes.subspan(written);
    }
}

void Output::flush() {
    writePhysical(std::span(buffer_).first(buffered_)); buffered_ = 0;
}

void Output::commit(uint64_t expected, uint64_t fileTime) {
    if (written_ != expected) fail("Decompressed file length mismatch: " + utf8(destination_));
    flush();
    if (fileTime) {
        FILETIME time{static_cast<DWORD>(fileTime), static_cast<DWORD>(fileTime >> 32)};
        if (!SetFileTime(handle_, nullptr, nullptr, &time)) winError("Setting file timestamp");
    }
    if (!CloseHandle(handle_)) winError("Closing output");
    handle_ = INVALID_HANDLE_VALUE;
    checkParents(destination_);
    if (!MoveFileExW(temporary_.c_str(), destination_.c_str(), overwrite_ ? MOVEFILE_REPLACE_EXISTING : 0))
        winError("Publishing " + utf8(destination_));
    committed_ = true;
}

void copyInput(Input input, const fs::path& destination, bool overwrite, uint64_t fileTime) {
    Output output(destination, overwrite);
    std::vector<uint8_t> buffer(1u << 20);
    for (uint64_t position = 0; position < input.size;) {
        const size_t count = static_cast<size_t>(std::min<uint64_t>(buffer.size(), input.size - position));
        auto chunk = std::span(buffer).first(count); input.read(position, chunk); output.write(chunk); position += count;
    }
    output.commit(input.size, fileTime);
}

TempDirectory::TempDirectory() {
    const auto root = fs::temp_directory_path();
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
        path_ = root / uniqueName();
        if (CreateDirectoryW(path_.c_str(), nullptr)) return;
        if (GetLastError() != ERROR_ALREADY_EXISTS) winError("Creating temporary directory");
    }
    fail("Unable to create temporary directory");
}

TempDirectory::~TempDirectory() { std::error_code ignored; fs::remove_all(path_, ignored); }
}