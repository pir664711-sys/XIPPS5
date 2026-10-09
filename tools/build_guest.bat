@echo off
rem needs clang+lld (LLVM). Output: games\TEST00001\eboot.bin (PIE ELF64 x86-64)
clang -target x86_64-linux-gnu -O2 -ffreestanding -fno-builtin -fno-stack-protector -fPIE -nostdlib -static-pie -fuse-ld=lld -Wl,-e,_start -Wl,-z,noseparate-code tools\guest.c -o games\TEST00001\eboot.bin
