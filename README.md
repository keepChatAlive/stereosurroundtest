# Stereo Surround

Stereo Surround is a native Windows 11 application that captures a 7.1 WASAPI
endpoint, renders its channels for a fixed listener and two physical speakers,
and sends the resulting stereo stream to a selected output endpoint.

The repository contains only the independent speaker-virtualization backend.
It does not contain the earlier arcade-spatialization implementation or its
source data.

## Features

- Live 7.1-to-stereo WASAPI bridge with selectable endpoints
- Fixed-listener speaker virtualization and deliberately exaggerated expansion
- Reference/wild A/B controls and naive-downmix comparison
- Adjustable speaker span, distance, head width, buffer target and rear profile
- Optional 24 kHz stream and equal per-channel input headroom
- WAV channel-injection test mode and live queue/level monitoring

The renderer assumes the listener remains centered. It uses a generic acoustic
model rather than measurements of the listener, speakers or room, so placement
and listening position materially affect the result.

## Requirements

- Windows 11
- CMake 3.22 or newer
- Ninja
- A C++20 MinGW-w64 compiler; WinLibs GCC 16.1 is the reference toolchain

Put `cmake`, `ninja`, `g++` and `ctest` on `PATH`, then run:

```powershell
.\build.ps1
```

The release executable is `build\stereo_surround.exe`. To configure manually:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Use `-SkipTests` with `build.ps1` when only the executable is needed.

## Safety and limitations

Start at low speaker volume. This is experimental audio software, not a driver;
it requires an existing multichannel capture endpoint. The final stereo path
includes a linked limiter, but source and system volume still matter.

No patent clearance or legal opinion is provided. Anyone distributing binaries
should perform their own review of the implemented signal-processing methods and
choose an appropriate source-code license before publishing a release.
