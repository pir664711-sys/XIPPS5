cmake_minimum_required(VERSION 3.20)
project(ps5emu VERSION 0.0.20 LANGUAGES C CXX)

if(NOT WIN32)
  message(FATAL_ERROR "ps5emu is Windows only")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# static CRT: no vcredist needed
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_executable(ps5emu WIN32 src/main.cpp src/emu.cpp src/nid.cpp src/perf.cpp src/overlay.cpp src/vk_backend.cpp)

target_compile_definitions(ps5emu PRIVATE
  PS5EMU_VERSION="0.020" UNICODE _UNICODE NOMINMAX _CRT_SECURE_NO_WARNINGS)
# Vulkan headers are vendored (third_party/vulkan); vulkan-1.dll is loaded at runtime, no SDK needed
target_include_directories(ps5emu PRIVATE ${CMAKE_SOURCE_DIR}/third_party/vulkan)
target_link_libraries(ps5emu PRIVATE user32 gdi32 comctl32 comdlg32 shell32 ole32 pdh psapi)

if(MSVC)
  target_compile_options(ps5emu PRIVATE /W3 /utf-8 /EHsc)
endif()

# copy sample game library next to the exe
add_custom_command(TARGET ps5emu POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          ${CMAKE_SOURCE_DIR}/games $<TARGET_FILE_DIR:ps5emu>/games)
