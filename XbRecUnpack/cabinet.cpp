#include "unpack.hpp"
#include <fdi.h>
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <unordered_map>

namespace xbr {
namespace {

uint32_t checksum(std::span<const uint8_t> bytes) {
    uint32_t value{};
    size_t position{};
    for (; position + 4 <= bytes.size(); position += 4)
        value ^= uint32_t(bytes[position]) | (uint32_t(bytes[position + 1]) << 8) |
            (uint32_t(bytes[position + 2]) << 16) | (uint32_t(bytes[position + 3]) << 24);
    uint32_t tail{};
    for (; position < bytes.size(); ++position) tail = (tail << 8) | bytes[position];
    return value ^ tail;
}

std::string offsetText(uint64_t value) { std::ostringstream out; out << "0x" << std::hex << value; return out.str(); }
struct Folder { uint32_t offset{}; uint16_t blocks{}, compression{}; uint64_t decoded{}; };
struct Io {
    Input input;
    uint64_t position{}, expected{};
    std::unique_ptr<Output> output;
};

struct Decoder {
    const Cabinet& cabinet;
    const CabTarget& target;
    bool overwrite;
    size_t entry{};
    INT_PTR nextHandle{1};
    std::unordered_map<INT_PTR, std::unique_ptr<Io>> handles;
    std::string error;
    Io& get(INT_PTR handle) {
        const auto found = handles.find(handle);
        if (found == handles.end()) fail("Invalid CAB decoder handle");
        return *found->second;
    }
    void saveError() noexcept {
        try { throw; } catch (const std::exception& e) { try { error = e.what(); } catch (...) {} }
        catch (...) { try { error = "Unknown CAB callback failure"; } catch (...) {} }
    }
};

thread_local Decoder* active{};
void* DIAMONDAPI allocate(ULONG count) { return std::malloc(count); }
void DIAMONDAPI release(void* pointer) { std::free(pointer); }

INT_PTR DIAMONDAPI openCab(char* name, int, int) {
    try {
        if (std::string_view(name) != "xbr.cab") fail("CAB requested an unexpected external file");
        auto handle = std::make_unique<Io>(); handle->input = active->cabinet.input;
        const INT_PTR id = active->nextHandle++; active->handles.emplace(id, std::move(handle)); return id;
    } catch (...) { active->saveError(); return -1; }
}

UINT DIAMONDAPI readCab(INT_PTR handle, void* buffer, UINT count) {
    try {
        auto& io = active->get(handle);
        if (io.output) fail("CAB attempted to read output");
        const auto actual = static_cast<UINT>(std::min<uint64_t>(count, io.input.size - io.position));
        io.input.read(io.position, {static_cast<uint8_t*>(buffer), actual}); io.position += actual; return actual;
    } catch (...) { active->saveError(); return UINT_MAX; }
}

UINT DIAMONDAPI writeCab(INT_PTR handle, void* buffer, UINT count) {
    try {
        auto& io = active->get(handle);
        if (!io.output || count > io.expected - io.position) fail("CAB output exceeds declared file length");
        io.output->write({static_cast<const uint8_t*>(buffer), count}); io.position += count; return count;
    } catch (...) { active->saveError(); return UINT_MAX; }
}

int DIAMONDAPI closeCab(INT_PTR handle) {
    try { active->get(handle); active->handles.erase(handle); return 0; }
    catch (...) { active->saveError(); return -1; }
}

long DIAMONDAPI seekCab(INT_PTR handle, long offset, int origin) {
    try {
        auto& io = active->get(handle);
        if (io.output) fail("CAB attempted to seek output");
        int64_t base{};
        if (origin == SEEK_CUR) base = static_cast<int64_t>(io.position);
        else if (origin == SEEK_END) base = static_cast<int64_t>(io.input.size);
        else if (origin != SEEK_SET) fail("Invalid CAB seek origin");
        const int64_t position = base + offset;
        if (position < 0 || static_cast<uint64_t>(position) > io.input.size) fail("CAB seek exceeds cabinet extent");
        io.position = static_cast<uint64_t>(position); return static_cast<long>(position);
    } catch (...) { active->saveError(); return -1; }
}

uint64_t dosTime(uint16_t date, uint16_t time) {
    FILETIME local{}, utc{};
    SYSTEMTIME localTime{}, utcTime{};
    if (!date || !DosDateTimeToFileTime(date, time, &local) || !FileTimeToSystemTime(&local, &localTime) ||
        !TzSpecificLocalTimeToSystemTimeEx(nullptr, &localTime, &utcTime) || !SystemTimeToFileTime(&utcTime, &utc)) return 0;
    return uint64_t(utc.dwLowDateTime) | (uint64_t(utc.dwHighDateTime) << 32);
}

INT_PTR DIAMONDAPI notify(FDINOTIFICATIONTYPE type, PFDINOTIFICATION info) {
    try {
        if (type == fdintCOPY_FILE) {
            if (active->entry >= active->cabinet.entries.size()) fail("CAB returned an unexpected file");
            const auto& entry = active->cabinet.entries[active->entry];
            if (entry.name != info->psz1 || info->cb < 0 || entry.size != static_cast<uint32_t>(info->cb))
                fail("CAB decoder file metadata disagrees with validated table");
            const auto path = active->target(entry, active->entry++);
            if (path.empty()) return 0;
            auto io = std::make_unique<Io>(); io->expected = entry.size;
            io->output = std::make_unique<Output>(path, active->overwrite);
            const INT_PTR handle = active->nextHandle++; active->handles.emplace(handle, std::move(io)); return handle;
        }
        if (type == fdintCLOSE_FILE_INFO) {
            auto& io = active->get(info->hf);
            if (!io.output) fail("CAB closed an unexpected output handle");
            io.output->commit(io.expected, dosTime(info->date, info->time));
            active->handles.erase(info->hf); return TRUE;
        }
        if (type == fdintNEXT_CABINET || type == fdintPARTIAL_FILE)
            fail("Continued files across cabinets are unsupported");
        return 0;
    } catch (...) { active->saveError(); return -1; }
}
}

Cabinet readCabinet(Input input, bool validateData) {
    Reader reader(input);
    if (input.size < 36 || reader.u32() != 0x4643534D) fail("Invalid CAB signature/header");
    if (reader.u32()) fail("Invalid CAB reserved field");
    const uint32_t size = reader.u32();
    if (size < 36 || size > input.size || size > LONG_MAX) fail("CAB length exceeds input or native decoder seek limit");
    input = input.slice(0, size); reader = Reader(input);
    reader.seek(12);
    if (reader.u32()) fail("Invalid CAB reserved field");
    const uint32_t filesOffset = reader.u32();
    if (reader.u32()) fail("Invalid CAB reserved field");
    const uint8_t minor = reader.u8(), major = reader.u8();
    if (major != 1 || minor != 3) fail("Unsupported CAB format version");
    Cabinet cabinet{input};
    cabinet.folders = reader.u16();
    const uint16_t files = reader.u16();
    cabinet.flags = reader.u16(); cabinet.setId = reader.u16(); cabinet.index = reader.u16();
    if (!cabinet.folders || (cabinet.flags & ~7)) fail("Invalid CAB folder count/flags");
    uint8_t folderReserve{}, dataReserve{};
    if (cabinet.flags & 4) { const auto headerReserve = reader.u16(); folderReserve = reader.u8(); dataReserve = reader.u8(); reader.skip(headerReserve); }
    if (cabinet.flags & 1) { reader.cstring(); reader.cstring(); }
    if (cabinet.flags & 2) { reader.cstring(); reader.cstring(); }
    std::vector<Folder> folders;
    for (uint16_t index = 0; index < cabinet.folders; ++index) {
        Folder folder{reader.u32(), reader.u16(), reader.u16()}; reader.skip(folderReserve);
        const auto type = folder.compression & 15;
        if (type > 3 || (type == 3 && ((folder.compression >> 8) < 15 || (folder.compression >> 8) > 21)))
            fail("Unsupported or malformed CAB compression parameters");
        folders.push_back(folder);
    }
    if (filesOffset < reader.tell() || filesOffset > input.size) fail("Invalid CAB file table offset");
    reader.seek(filesOffset);
    for (uint16_t index = 0; index < files; ++index) {
        CabEntry entry;
        entry.size = reader.u32(); entry.folderOffset = reader.u32(); entry.folder = reader.u16();
        entry.date = reader.u16(); entry.time = reader.u16(); entry.attributes = reader.u16(); entry.name = reader.cstring();
        if (entry.name.size() >= CB_MAX_FILENAME) fail("CAB filename exceeds Windows decoder's 255-byte limit");
        if (entry.folder >= cabinet.folders) fail("Invalid CAB folder index or unsupported continued file");
        if (entry.size > LONG_MAX) fail("CAB file exceeds native decoder length limit");
        cabinet.entries.push_back(std::move(entry));
    }
    const uint64_t tableEnd = reader.tell();
    std::vector<Folder*> ordered;
    for (auto& folder : folders) ordered.push_back(&folder);
    std::sort(ordered.begin(), ordered.end(), [](const Folder* a, const Folder* b) { return a->offset < b->offset; });
    uint64_t previousEnd = tableEnd;
    for (auto* current : ordered) {
        auto& folder = *current;
        if (folder.offset < previousEnd) fail("Overlapping CAB folder data");
        if (folder.offset < tableEnd || folder.offset > input.size) fail("CAB folder data overlaps metadata or exceeds input");
        reader.seek(folder.offset);
        for (uint16_t block = 0; block < folder.blocks; ++block) {
            const auto position = reader.tell();
            const uint32_t stored = reader.u32();
            const uint16_t compressed = reader.u16(), decoded = reader.u16();
            if (!decoded || decoded > 32768 || !compressed) fail("Invalid CAB data block sizes at " + offsetText(input.offset + position));
            const auto reserve = reader.bytes(dataReserve);
            if (validateData) {
                const auto data = reader.bytes(compressed);
                std::vector<uint8_t> header{static_cast<uint8_t>(compressed), static_cast<uint8_t>(compressed >> 8),
                    static_cast<uint8_t>(decoded), static_cast<uint8_t>(decoded >> 8)};
                header.insert(header.end(), reserve.begin(), reserve.end());
                const auto computed = checksum(data) ^ checksum(header);
                if (stored && stored != computed) fail("CAB checksum mismatch at " + offsetText(input.offset + position) +
                    " (stored " + offsetText(stored) + ", calculated " + offsetText(computed) + ")");
            } else reader.skip(compressed);
            if ((folder.compression & 15) == 0 && compressed != decoded) fail("Uncompressed CAB block length mismatch");
            folder.decoded += decoded;
        }
        previousEnd = reader.tell();
    }
    for (const auto& entry : cabinet.entries)
        if (uint64_t(entry.folderOffset) + entry.size > folders[entry.folder].decoded) fail("CAB file exceeds decompressed folder extent");
    return cabinet;
}

void extractCabinet(const Cabinet& cabinet, const CabTarget& target, bool overwrite) {
    if (active) fail("Nested CAB decoder invocation");
    Decoder decoder{cabinet, target, overwrite}; active = &decoder;
    struct ActiveReset { ~ActiveReset() { active = nullptr; } } reset;
    ERF error{};
    HFDI context = FDICreate(allocate, release, openCab, readCab, writeCab, closeCab, seekCab, cpuUNKNOWN, &error);
    if (!context) fail("Creating Windows CAB decoder failed: " + std::to_string(error.erfOper));
    char name[] = "xbr.cab", path[] = "";
    const BOOL result = FDICopy(context, name, path, 0, notify, nullptr, &decoder);
    FDIDestroy(context);
    if (!decoder.error.empty()) fail(decoder.error);
    if (!result) fail("Windows CAB decompression failed (FDI error " + std::to_string(error.erfOper) + ")");
    if (decoder.entry != cabinet.entries.size()) fail("CAB decoder did not visit all declared files");
}

std::vector<uint8_t> readCabEntry(const Cabinet& cabinet, size_t index, size_t limit) {
    if (index >= cabinet.entries.size() || cabinet.entries[index].size > limit) fail("CAB metadata file exceeds limit");
    TempDirectory temporary; const auto path = temporary.path() / L"metadata.bin";
    extractCabinet(cabinet, [&](const CabEntry&, size_t current) { return current == index ? path : fs::path{}; }, false);
    const auto input = Input::open(path); std::vector<uint8_t> bytes(static_cast<size_t>(input.size)); input.read(0, bytes); return bytes;
}
}