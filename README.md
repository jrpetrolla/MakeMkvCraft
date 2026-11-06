# MakeMkvCraft

An independent, open-source desktop application for inspecting DVD/Blu-ray
media and remuxing selected titles to Matroska without re-encoding.

> [!IMPORTANT]
> This is a clean-room project and is not affiliated with, endorsed by, or
> derived from MakeMKV or GuinpinSoft. MakeMKV is a trademark of its respective
> owner. No proprietary MakeMKV binaries, source code, resources, disc images,
> or reverse-engineering fixtures are included in this repository.

## Project status

This project is an early preview. DVD-Video ISO remuxing is the most exercised
path; Blu-ray support remains experimental and needs a broader regression set.

Implemented:

- Qt 5 desktop interface for opening folders, optical drives, and ISO images;
- DVD title playback through `libdvdnav`, preserving logical cells and angle;
- DVD and Blu-ray title/track discovery through `libdvdread` and `libbluray`;
- selectable video, audio, VobSub/PGS subtitle tracks, and chapters;
- ISO 639 language metadata and Matroska chapter output;
- stream-copy remuxing through FFmpeg libraries, without transcoding;
- adaptive, global DVD SCR correction across active video/audio streams;
- bounded timestamp lookahead and interleaving;
- real extraction/remux progress on a worker thread;
- cleanup of partial MKV and temporary VOB files after failure.

Known limitations:

- cancellation and persistent settings are not implemented;
- Blu-ray ISO/folder remuxing has not received the same real-media coverage as
  DVD ISO remuxing;
- optical media vary significantly, so a successful build is not proof that a
  title will remux correctly;
- there is currently no registered CTest suite; validation uses focused local
  fixtures and real-media regression tests kept outside this repository.

## Dependencies

- C++20 compiler and CMake 3.21+
- Qt 5 Widgets
- `libdvdread` and `libdvdnav`
- `libbluray`
- FFmpeg libraries: `libavformat`, `libavcodec`, `libavutil`,
  `libswresample`, and `libswscale`

### Debian/Ubuntu

```bash
sudo apt install \
  build-essential cmake pkg-config qtbase5-dev \
  libbluray-dev libdvdread-dev libdvdnav-dev \
  libavformat-dev libavcodec-dev libavutil-dev \
  libswresample-dev libswscale-dev
```

## Build on Linux

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
```

The executable is generated as `build/MakeMkvCraft`.

## Cross-compile for Windows x64 with MXE

Prepare an MXE tree containing the shared x86-64 variants of Qt 5, FFmpeg,
`libbluray`, `libdvdread`, and `libdvdnav`. Then run:

```bash
cmake -S . -B build-windows \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/mxe-toolchain.cmake" \
  -DMXE_ROOT=/absolute/path/to/mxe

cmake --build build-windows -j"$(nproc)"
```

The resulting `.exe` still needs the corresponding shared MXE DLLs beside it.
Those redistributable packages are intentionally not tracked in Git.

## Repository boundaries

Only the independent implementation and its build documentation belong here.
Keep the following outside the Git working tree:

- commercial application installers or extracted binaries;
- DVD/Blu-ray ISO images and copied disc structures;
- test VOB/M2TS files and other copyrighted media;
- local MXE trees, build directories, packaged DLLs, logs, and backups.

## Architecture

```text
src/main.cpp                 Qt application entry point and FFmpeg logging
src/MainWindow.*             UI, track selection, progress, and worker thread
src/DiscInspector.*          DVD/Blu-ray structure and metadata inspection
src/MkvRemuxer.*             logical extraction, timestamp repair, and MKV mux
```

## Legal use

Use this software only with media you are legally allowed to access and copy.
Laws concerning encryption and copy protection differ by jurisdiction. The
project does not include media, keys, or proprietary vendor components.

## License

Copyright (c) 2026 João Petrolla and contributors.

Licensed under the GNU General Public License v3.0 or later. See
[`LICENSE`](LICENSE).
