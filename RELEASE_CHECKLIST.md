# GitHub release checklist

1. Choose and add a source-code license. Until then, the repository is visible
   source rather than open-source software.
2. Review names, comments and DSP constants for third-party provenance and obtain
   independent legal review if patent exposure is a release concern.
3. Build on a clean Windows 11 machine with WinLibs GCC 16.1, CMake and Ninja.
4. Run both CTest targets and perform live 7.1 endpoint, WAV-channel, 24 kHz,
   buffer-resync and stop/start smoke tests.
5. Scan the executable with Microsoft Defender and at least one multi-engine
   service; document likely false positives from a fully static unsigned binary.
6. Package only `stereo_surround.exe`, `README.md` and the chosen license. Include
   the compiler version, commit ID and SHA-256 checksum in the release notes.
7. Tag the reviewed commit as `v0.1.0`, create a GitHub release, attach the ZIP
   and checksum, and mark it as a prerelease while endpoint compatibility remains
   experimental.
