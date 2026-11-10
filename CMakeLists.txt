# Build Strata on Windows with Visual Studio 2022 (Desktop C++ workload):
#   cmake -S . -B build -A x64
#   cmake --build build --config Release
# Or cross-compile from Linux/macOS with ./build.sh (Zig).
cmake_minimum_required(VERSION 3.20)
project(Strata LANGUAGES CXX RC)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")  # static CRT, no redistributable needed

file(GLOB STRATA_SOURCES CONFIGURE_DEPENDS src/*.cpp)
add_executable(Strata WIN32 ${STRATA_SOURCES} res/strata.rc)
target_compile_definitions(Strata PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00 WINVER=0x0A00)
if(MSVC)
  target_compile_options(Strata PRIVATE /W4 /utf-8 /permissive-)
  target_link_options(Strata PRIVATE /MANIFEST:NO)   # manifest comes from strata.rc
endif()
target_link_libraries(Strata PRIVATE d2d1 dwrite ole32 oleaut32 uuid dwmapi shcore shell32 user32 gdi32 advapi32 shlwapi windowscodecs comctl32 mpr uxtheme)
set_target_properties(Strata PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/dist)
