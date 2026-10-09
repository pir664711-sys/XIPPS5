# ps5emu 0.017 (source update)

## Changes
- Bumped CMake project and runtime version to 0.017.
- Vulkan backend now explicitly requires Vulkan 1.1 or newer from both the loader and selected physical device; it logs a useful reason and falls back through the existing caller path when unavailable.
- Added HLE handlers for `scePthreadMutexattrInit`, `scePthreadMutexattrSettype`, `scePthreadMutexattrDestroy`, and POSIX aliases.
- Added a basic `sceKernelVirtualQuery` implementation for host-backed virtual memory regions using a 72-byte Orbis-style query record. This is a compatibility approximation, not a complete PS5 kernel memory model.
- Added a guard against implausibly large direct-memory mapping sizes so corrupted/mismatched arguments fail clearly instead of reaching `VirtualAlloc`.

## Important limitations
- This is a source-level update; it has not been compiled with MSVC or tested against Peppa Pig Adventure in this environment.
- Vulkan 1.1 applies to the emulator's current presentation backend. It does not implement PS5 AGC/RDMA command buffers or translate PS5 shaders.
- Many imports remain unimplemented; unknown NIDs must be identified from `ps5emu.imports.txt` and implemented with their real signatures.
- Direct-memory emulation remains approximate. If the game still logs a huge `len`, inspect the caller's register values and import signature before treating it as a genuine mapping request.
