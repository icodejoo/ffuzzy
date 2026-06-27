# Flutter Linux FFI-plugin native build. Builds libffz.so and bundles it.
cmake_minimum_required(VERSION 3.14)
project(ffz_library LANGUAGES C)

add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/.." ffz_native)

set(ffuzzy_bundled_libraries "$<TARGET_FILE:ffz>" PARENT_SCOPE)
