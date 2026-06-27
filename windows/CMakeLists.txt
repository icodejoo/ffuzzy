# Flutter Windows FFI-plugin native build: builds the ffz shared library from
# the C sources (../../CMakeLists.txt) and tells Flutter to bundle it next to
# the runner .exe (so DynamicLibrary.open('ffz.dll') resolves). MSVC-compatible.
cmake_minimum_required(VERSION 3.14)
project(ffz_library LANGUAGES C)

# clang/CMakeLists.txt defines the `ffz` target (src/*.c + ffi/ffz_ffi.c).
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/.." ffz_native)

set(ffuzzy_bundled_libraries "$<TARGET_FILE:ffz>" PARENT_SCOPE)
