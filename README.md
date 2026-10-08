# XbRecUnpack (C++)

Extract Xbox/Xbox 360 SDK and recovery files on Windows. This C++20 rewrite preserves emoose's original C# sources and CLI
layout.

## Build

Install Visual Studio 2022 with Desktop development with C++ and CMake 3.24+
The initial configuration downloads SHA-256 pinned zlib 1.3.2 for ZIP deflate CAB/LZX decompression uses Windows Cabinet API
Both the MSVC runtime and zlib are linked statically

```powershell
cmake -S . -B build/cpp-release -A x64
cmake --build build/cpp-release --config Release --target XbRecUnpack
ctest --test-dir build/cpp-release -C Release --output-on-failure
```

## Usage

```text
XbRecUnpack.exe [-L] [-R] [--overwrite] <input> [output-folder]
```

Inputs: SDK/remote-recovery `.exe`, recovery `.iso`, recovery `.zip`,
`recctrl.bin` (with sibling `recdata.bin`), or standalone `.cab`.
The default output is `<input>_ext`. `-L` validates and lists without extracting user files.
`-R` prints detailed ROM information after extraction, otherwise
ROM information is summarized. Existing files are preserved unless
`--overwrite` is supplied. Exit codes are 0 for success and 1 for failure.

Recovery images support GDF, ISO9660/Joliet and physical partition UDF.
ZIP/ZIP64 archives must contain one ISO using stored or deflate compression.
ISO data is streamed to temporary disk and checked before processing. Recovery
LZX frames also use temporary disk rather than retaining entire folders in RAM.

## Changes

- Validates CAB headers, all folders, block extents and present CFDATA checksums
  before extraction. Native FDI handles CAB compression, including LZX.
- Scans embedded CABs by declared extent so nested signatures are ignored,
  metadata lookup terminates when the manifest is absent or ambiguous.
- Requires the manifest and CAB file sequence to agree, supports quoted CSV,
  checks copy dependencies, and preserves copy timestamps and variant routing.
- Uses a bounded 1 MiB input cache and 256 KiB output buffer, decoded folders
  remain streamed rather than retained in RAM.
- Avoids repeated directory creation and uses directory enumeration attributes
  for ROM scanning. Retries a disappeared output parent once after revalidation.
- Rejects traversal, alternate streams, Windows device names, reparse points,
  case insensitive duplicate outputs and file/directory collisions.
- Checks every read, write and decoded size. Publishes each file through a
  temporary sibling only after its full contents have been written.
- Validates ZIP CRC/lengths and image/recovery metadata before extracting files.

CABs spanning multiple cabinets, individual CAB files or compressed cabinets
larger than the native signed 32-bit interface, CAB member names longer than
255 bytes, encrypted/multi disk ZIP, and UDF virtual/sparable/metadata maps or
sparse/continuation allocation extents are rejected explicitly. Image/recovery
metadata is limited to 64 MiB and ZIP ISO images to 64 GiB.