// Portable setenv/unsetenv for tests.
//
// setenv/unsetenv are POSIX and absent from MSVC, so every test that toggles an
// ULTRA_* knob failed to COMPILE on the Windows CI leg while passing on Linux and
// macOS. That left Windows with no test coverage at all rather than a red test --
// a silent gap, which is why this is a shared header instead of a #ifdef repeated
// at each call site (12 test files were affected; scattered guards is how the
// first two got missed).
//
// Include this header and keep calling setenv()/unsetenv() normally.
#pragma once

#ifdef _WIN32
#include <cstdlib>

// MSVC declares neither name, so defining them at global scope cannot collide.
// _putenv_s(name, "") is the documented way to REMOVE a variable on Windows.
inline int setenv(const char* name, const char* value, int /*overwrite*/) {
    return _putenv_s(name, value);
}

inline int unsetenv(const char* name) {
    return _putenv_s(name, "");
}
#endif  // _WIN32
