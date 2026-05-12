// Minimal pthread.h shim for compiling Hamlib's prebuilt Windows headers
// under MSVC. Hamlib's rig.h references pthread_t and pthread_mutex_t in
// struct member declarations but the prebuilt libhamlib-4.dll provides
// the real pthread implementation internally (via libwinpthread-1.dll).
// We never call pthread functions from ProjectUltra code; we only need
// these types to be visible so rig.h compiles.
//
// Sizes match the MinGW-w64 winpthreads canonical layout
// (winpthreads-git): pthread_t is a 16-byte struct on x64,
// pthread_mutex_t is a pointer-sized opaque. Keeping these in sync with
// the libhamlib-4.dll we ship is required for the rig_state struct
// layout to round-trip correctly across the DLL boundary.

#ifndef PTHREAD_H
#define PTHREAD_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void* p;
    unsigned int x;
} pthread_t;

typedef void* pthread_mutex_t;

#ifdef __cplusplus
}
#endif

#endif /* PTHREAD_H */
