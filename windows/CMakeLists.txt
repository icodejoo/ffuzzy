cmake_minimum_required(VERSION 3.18)
project(ffuzzy C)

set(CMAKE_C_STANDARD 11)

add_library(ffuzzy SHARED
    ../src/ffuzzy.c
    ../src/scorer.c
    ../src/corpus.c
    ../src/thread_pool.c
)

target_include_directories(ffuzzy PRIVATE ../src)

# Windows uses Win32 CreateThread / CRITICAL_SECTION (no extra libs needed)
# Export symbols so dart:ffi can load them
set_target_properties(ffuzzy PROPERTIES
    WINDOWS_EXPORT_ALL_SYMBOLS ON
)
