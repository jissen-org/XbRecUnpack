#include "unpack.hpp"
#include <iostream>
#include <set>

int wmain(int argc, wchar_t** argv) {
    using namespace xbr;
    try {
        SetConsoleOutputCP(CP_UTF8);
        Options options;
        std::vector<fs::path> paths;
        bool positional{};
        for (int index = 1; index < argc; ++index) {
            const std::string argument = lower(utf8(fs::path(argv[index])));
            if (!positional && argument == "--") { positional = true; continue; }
            if (!positional && (argument == "-l" || argument == "--list")) options.listOnly = true;
            else if (!positional && (argument == "-r" || argument == "--rom-info")) options.romInfo = true;
            else if (!positional && argument == "--overwrite") options.overwrite = true;
            else if (!positional && (argument == "--help" || argument == "-h")) {
                std::cout << "Usage: XbRecUnpack.exe [-L] [-R] [--overwrite] <SDK.exe|recovery.iso|recovery.zip|recctrl.bin|archive.cab> [output-folder]\n"; return 0;
            } else if (!positional && argument.starts_with('-')) fail("Unknown option: " + argument);
            else paths.emplace_back(argv[index]);
        }
        if (paths.empty() || paths.size() > 2) fail("Expected input path and optional output folder, use --help for usage");
        const auto output = paths.size() == 2 ? paths[1] : fs::path(paths[0].wstring() + L"_ext");
        const auto input = Input::open(paths[0]);
        const auto extension = lower(utf8(paths[0].extension()));
        if (extension == ".exe") processRemote(input, output, options);
        else if (extension == ".iso") processImage(input, output, options);
        else if (extension == ".zip") processZip(input, output, options);
        else if (extension == ".cab") {
            const auto cabinet = readCabinet(input, true);
            std::vector<fs::path> targets;
            std::set<fs::path, PathLess> seen;
            for (const auto& entry : cabinet.entries) {
                const auto name = textPath(entry.name, (entry.attributes & 0x80) != 0);
                const auto target = safeOutput(output, utf8(name));
                if (!seen.insert(target).second) fail("Duplicate CAB output path: " + utf8(target));
                if (!options.listOnly && fs::exists(target) && !options.overwrite) fail("Output already exists: " + utf8(target));
                targets.push_back(target); std::cout << entry.name << " (" << entry.size << " bytes)\n";
            }
            validateOutputTree(seen);
            if (!options.listOnly) extractCabinet(cabinet, [&](const CabEntry&, size_t index) { return targets[index]; }, options.overwrite);
        } else {
            const auto dataPath = paths[0].parent_path() / L"recdata.bin";
            if (options.listOnly) processRecovery(input, nullptr, output, options);
            else { const auto data = Input::open(dataPath); processRecovery(input, &data, output, options); }
        }
        if (!options.listOnly) printRomSummary(output, options.romInfo);
        std::cout << (options.listOnly ? "Validation/listing complete.\n" : "Extraction complete.\n");
        return 0;
    } catch (const std::exception& error) { std::cerr << "Error: " << error.what() << '\n'; return 1; }
    catch (...) { std::cerr << "Error: unknown failure\n"; return 1; }
}
