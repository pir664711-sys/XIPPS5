# ps5emu 0.018 (diagnostic update)

## Changes
- Unresolved-import thunks now pass the guest return address to the logger. The log reports the first guest call site for each unresolved NID, including repeated-call counters.
- Improved the no-frame heartbeat message so it no longer incorrectly implies the guest never calls any HLE imports.
- Version bumped to 0.018.

## Why this update
The supplied Peppa Pig Adventure log showed repeated calls to unresolved imports (`BHouLQzh0X0#k#N`, `1FGvU0i9saQ#k#N`, and `KuOuD58hqn4#j#j`) and zero submitted frames. Their actual API names/signatures are not established by the current NID table, so implementing guessed handlers would risk corrupting guest state. This version records the guest caller addresses so the next patch can identify the call sites and choose a correctly typed HLE implementation.

## Limitations
- This is a source-level diagnostic patch and has not been compiled or run here.
- It does not claim to make Peppa Pig Adventure playable. The emulator still lacks a PS5 AGC graphics implementation and many system-library APIs.
