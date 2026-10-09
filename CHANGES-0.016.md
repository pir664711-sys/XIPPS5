# ps5emu 0.016

## What the 0.015 log showed
The game now runs real startup code: TLS patch applied to all 444 sites, 85 imports resolved, 515 stubbed.
It crashed at guest vaddr 0x7bfe63, a 32-byte AVX store `vmovups [rax+rdx-0x20], ymm0` with rax = 0 (a memory clear loop over a 1 MB block).
The last unresolved import called was `sceKernelReserveVirtualRange`: the stub returned 0 but never wrote the address
back through the `void** addr` out-parameter, so the game used a NULL base.
Also visible: `sceKernelCreateSema` (16 calls) returned without writing a semaphore id, and the mutex-attribute calls
(`scePthreadMutexattrInit/Settype/Destroy`, plus one unnamed NID `1FGvU0i9saQ`) ran 18 times each (these are harmless as stubs).

## Changes
* `sceKernelReserveVirtualRange`: reserves address space (no access until mapped), honours hint, MAP_FIXED and alignment.
* `sceKernelMapDirectMemory` / `MapNamedDirectMemory`: commit inside a reserved range, else allocate aligned.
* Semaphores (create/wait/poll/signal/delete) and event flags (create/set/wait/delete) with 32-bit ids.
* Threads: `scePthreadCreate`/`pthread_create`, join, exit, attr init/destroy/stacksize. Each guest thread gets its own TCB and
  fault capture. Guest threads are stopped when emulation stops.
* Condition variables (`pthread_cond_*`, `scePthreadCond*`), `scePthreadYield`.

## Still missing (next likely blockers)
Event queues (`sceKernelCreateEqueue`, `WaitEqueue`, ...), `scePthreadMutexattr*` semantics, file I/O (`sceKernelOpen/Read/...`),
`fopen`/`fwrite`, `qsort` (needs a host->guest callback), `setjmp/longjmp`, and everything AGC/GPU.
