@echo off
rem Run from any VS developer prompt (x64 Native / x86 Native / x64_x86 Cross). Builds both archs.
cmake -S . -B build\x64 -A x64 || exit /b 1
cmake --build build\x64 --config Release || exit /b 1
cmake -S . -B build\x86 -A Win32 || exit /b 1
cmake --build build\x86 --config Release || exit /b 1
echo.
echo x64: build\x64\Release\ps5emu.exe
echo x86: build\x86\Release\ps5emu.exe  (UI only, cannot run x86-64 guests)
