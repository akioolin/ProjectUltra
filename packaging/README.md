# ProjectUltra Packaging Guide

## Quick Package Commands

The alpha release download is the operator bundle: `projectultra-<platform>.zip`.
It contains `ultra_tnc`, `ultra_gui`, `ultra`, `tools/ultra_tnc.conf.example`,
and operator docs. Simulator and bench binaries are built into a separate
`dev-tools-<platform>.zip` artifact.

### macOS
```bash
cd packaging
./package_macos.sh
```
Output: `dist/macos/projectultra-macos.zip` and `dist/macos/dev-tools-macos.zip`

### Windows
```batch
cd packaging
package_windows.bat
```
Output: `dist/windows/projectultra-windows.zip` and `dist/windows/dev-tools-windows.zip`

### Linux
```bash
cd packaging
./package_linux.sh
```
Output: `dist/linux/projectultra-linux.zip` and `dist/linux/dev-tools-linux.zip`

---

## Prerequisites

### macOS
- Xcode Command Line Tools
- Homebrew
- SDL2: `brew install sdl2`

### Windows
- Visual Studio 2019+
- CMake
- SDL2 (via vcpkg or manual install)
  ```
  vcpkg install sdl2:x64-windows
  ```

### Linux
- GCC/G++
- CMake
- SDL2: `sudo apt install libsdl2-dev`
- zip

---

## Distribution Checklist

### Before Release
- [ ] Update version in `CMakeLists.txt`
- [ ] Update version in packaging scripts
- [ ] Test on clean machines
- [ ] Run full simulation test suite

### macOS
- [ ] Test on macOS 10.13+ (High Sierra)
- [ ] Consider code signing for Gatekeeper
- [ ] Consider notarization for macOS 10.15+

### Windows
- [ ] Test on Windows 7, 10, 11
- [ ] Include VC++ Redistributable or link statically
- [ ] Test with Windows Defender

### Linux
- [ ] Test operator bundle on Ubuntu, Fedora, Debian
- [ ] Verify audio device detection
- [ ] Check OpenGL compatibility

---

## GitHub Release

1. Create a new release tag:
   ```bash
   git tag -a v0.3.1-alpha -m "Release 0.3.1 alpha"
   git push origin v0.3.1-alpha
   ```

2. Build packages on each platform

3. Upload to GitHub Releases:
   - projectultra-macos.zip
   - projectultra-windows.zip
   - projectultra-linux.zip

---

## File Sizes (Approximate)

| Platform | Size |
|----------|------|
| macOS operator zip | TBD |
| Windows operator zip | TBD |
| Linux operator zip | TBD |
