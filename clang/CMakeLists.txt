# Builds the ffz shared library (matcher + FFI shim) for Flutter ffiPlugin use
# (Android / Windows / Linux desktop) or any CMake consumer. Locked config:
# exact mode, release optimization, FFI-only exports. MSVC and GCC/Clang both
# compile this (portable intrinsics + ctz).
cmake_minimum_required(VERSION 3.14)
project(ffz LANGUAGES C)

file(GLOB FFZ_CORE "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c")
add_library(ffz SHARED ${FFZ_CORE} "${CMAKE_CURRENT_SOURCE_DIR}/ffi/ffz_ffi.c")

target_include_directories(ffz PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(ffz PRIVATE c_std_11)
target_compile_definitions(ffz PRIVATE NDEBUG)

# Only the ffz_ffi_* shim symbols are exported (they carry the export attribute);
# hide everything else.
set_target_properties(ffz PROPERTIES C_VISIBILITY_PRESET hidden)

if(MSVC)
  target_compile_options(ffz PRIVATE /O2)
else()
  target_compile_options(ffz PRIVATE -O2 -ffunction-sections -fdata-sections)
  if(NOT APPLE)
    target_link_options(ffz PRIVATE -Wl,--gc-sections)
  endif()
endif()

# Flutter ffiPlugin: expose the built artifact for bundling.
set(ffz_bundled_libraries "$<TARGET_FILE:ffz>" PARENT_SCOPE)
