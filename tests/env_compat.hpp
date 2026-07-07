#pragma once
// setenv/unsetenv portability for MSVC (error C3861: the POSIX pair does not
// exist in the Windows CRT). _putenv_s covers both directions — an empty value
// REMOVES the variable (documented CRT behavior) — and getenv() reflects it
// within the same CRT. Knob-pinning tests (ULTRA_* env latches) include this
// so they RUN on Windows instead of being #ifndef'd out (the older
// test_diagnostics-style skip loses exactly the coverage the knob-graduation
// audit needs).
#include <cstdlib>

#ifdef _WIN32
inline int setenv(const char* name, const char* value, int /*overwrite*/) {
    return _putenv_s(name, value);
}
inline int unsetenv(const char* name) {
    return _putenv_s(name, "");
}
#endif
