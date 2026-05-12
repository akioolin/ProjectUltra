# ProjectUltra Licensing

ProjectUltra source code is distributed under the MIT License. See `LICENSE`.

Bundled release libraries:

- SDL2 runtime library: zlib license. Bundled in macOS, Linux, and Windows operator bundles when the package script can locate the built binary's SDL2 runtime dependency.
- PocketFFT: BSD-3-Clause license. Vendored header-only source is in `thirdparty/pocketfft/`; see `thirdparty/pocketfft/LICENSE.md`.
- Dear ImGui: MIT license. Vendored source is in `thirdparty/imgui/`.
- miniz: public domain/MIT; vendored source is in `thirdparty/miniz/`.
- Hamlib (libhamlib): LGPL 2.1+ (or later). Dynamically linked into ultra_gui and ultra_tnc when built with `ULTRA_USE_LIBHAMLIB=ON`. The Windows release bundle ships the prebuilt 4.7.1 runtime DLLs (libhamlib-4.dll plus libusb-1.0.dll, libgcc_s_seh-1.dll, libwinpthread-1.dll) alongside the binary; the full LGPL text is included as `COPYING.LIB.txt` in the same directory. Upstream source: https://github.com/Hamlib/Hamlib. To exercise the LGPL substitution right, replace the shipped libhamlib-4.dll with your own build of the same major version. The Hamlib utility binaries (rigctld, rigctl) are GPL 2+ and are NOT redistributed by this project; operators install them separately if they want the rigctld TCP backend.
