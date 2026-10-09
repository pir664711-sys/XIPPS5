# ps5emu 0.020

- Adds live process RAM usage to the main status bar: Working Set (resident RAM) and Private Bytes (private committed memory), updated with the existing 500 ms performance refresh.
- Adds guest call-site byte dumps when an unresolved import is first invoked, so the instructions around the repeated unresolved calls can be identified before implementing any HLE.
- Does not guess implementations for unknown NIDs. The log so far proves the guest repeatedly calls `BHouLQzh0X0#k#N` and receives the unresolved stub's zero return; that may explain the loop, but the correct API/signature must be identified before a safe implementation can be added.
- Source-only diagnostic update; not compiled or run in this environment.
