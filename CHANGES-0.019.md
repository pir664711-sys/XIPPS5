# ps5emu 0.019

Diagnostic update based on the v0.018 log.

- For each unresolved import's first call, logs guest machine-code bytes from 16 bytes before through 11 bytes after the guest return address.
- This helps inspect the call-site around the hot unresolved imports without guessing NID meanings or changing their stub behavior.
- No new PlayStation APIs are implemented in this version.
- Source archive only; not compiled or tested in this environment.
