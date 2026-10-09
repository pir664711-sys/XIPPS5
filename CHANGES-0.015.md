# ps5emu 0.015

## Why 0.014 stopped on a real game
The log showed `imports: 0 resolved to HLE, 600 unresolved`. Real console ELFs import functions by **NID**
(`gQX+4GDQjpM#j#j` = NID # library id # module id), and 0.014 only looked names up by plain text.
Every libc/libkernel call hit a stub that returned 0 (`memcpy` returning NULL, `strlen` returning 0, ...),
and the next dereference crashed. That crash was written to `ps5emu.log` by the old crash filter but never
reached the UI log panel, so it looked like the log just stopped.

Also wrong in 0.014: the entry point was called with `rdi = &HLE table`. Console `_start` expects
`rdi = { argc; argv...; NULL; envp...; NULL }` and `rsi = exit callback` (the entry bytes `mov r14d,[rdi]`,
`lea r15,[rdi+8]`, `mov rbx,rsi` show this).

## What changed
* **NID resolver** (`src/nid.cpp`): SHA-1 NID hashing. HLE names are hashed at start-up so one table serves both
  plain-name test guests and real NID imports. `src/nid_names.inc` holds 230 names verified against your log
  (e.g. `gQX+4GDQjpM` = `malloc`, `tsvEmnenz48` = `__cxa_atexit`, `UglJIZjGssM` = `sceAgcDriverSubmitDcb`).
  Put extra names in `nids.txt` next to the exe (one per line) to get readable logs.
* **ps5emu.imports.txt** written next to the exe on every boot: NID, lib, module, HLE/UNRESOLVED, name if known.
  `python tools/nid.py match ps5emu.imports.txt mynames.txt` shows which imports a name list explains.
* **Orbis entry ABI** for SCE ELF types (0xFE00/0xFE10/0xFE18). The old HLE-table entry is kept for the test guest.
* **TLS**: `mov r64, fs:[disp32]` (444 sites in your game) is rewritten to a jump to a trampoline that reads a
  per-thread TCB pointer from the TEB TLS slot. TCB has the self pointer at +0, the guard at +0x28 and the PT_TLS
  image just below it.
* **Fault capture on the guest thread** (vectored handler): registers, fault address, code bytes, guest vaddr,
  stack return-address candidates, and the **last unresolved import that was called**. It goes to the UI log too,
  and the guest thread then exits cleanly instead of killing the process.
* **Unresolved stubs** log the first 3 calls per import, then counts, plus a "most called" summary on exit/fault.
* **HLE batch 1**: malloc family (one aligned heap), mem*/str*, math, time, `__cxa_*` guards/atexit,
  `__stack_chk_guard` (data import) and `__stack_chk_fail`, direct-memory alloc/map (fake phys offsets, host memory),
  mutexes, TLS keys, `sceUserServiceGetInitialUser`, `sceVideoOutOpen`/`scePadOpen`/`sceAudioOutOpen` handles.
  Host thunks now carry 6 integer args. `printf`/`snprintf` only log/copy the raw format string (varargs not decoded).

## Known limits (next steps)
* ~400 of the 600 imports have no known name yet; send `ps5emu.imports.txt` and `ps5emu.log` after a run.
* Relocations of types 16/17/18 (TLS DTPMOD/DTPOFF/TPOFF) are counted and reported but not applied.
* `syscall` instructions are only counted (byte scan, may include false positives). Patching them safely needs a real
  disassembler (Zydis) rather than a byte scan.
* The game submits GPU work through AGC (`sceAgcDriverSubmitDcb`, `sceAgcDcbSetFlip`, ...). Nothing renders until the
  command buffers are parsed and the RDNA shaders are translated; the window stays black until then.
* Threads (`scePthreadCreate`), condition variables, semaphores, event queues and flexible memory are not implemented.
