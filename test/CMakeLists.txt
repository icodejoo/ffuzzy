cmake_minimum_required(VERSION 3.18)
project(ffuzzy_tests C)

set(CMAKE_C_STANDARD 11)

# ---- ffuzzy static library ----
add_library(ffuzzy_static STATIC
    ../src/ffuzzy.c
    ../src/scorer.c
    ../src/corpus.c
    ../src/thread_pool.c
)
target_include_directories(ffuzzy_static PUBLIC ../src)

if(WIN32)
    # No extra libs needed on Windows (uses Win32 threads)
elseif(APPLE)
    target_link_libraries(ffuzzy_static PRIVATE "-framework CoreFoundation")
else()
    target_link_libraries(ffuzzy_static PRIVATE pthread)
endif()

# ---- test executable ----
add_executable(ffuzzy_test test_main.c)
target_link_libraries(ffuzzy_test PRIVATE ffuzzy_static)
target_include_directories(ffuzzy_test PRIVATE ../src)

# Register with CTest
enable_testing()
add_test(NAME ffuzzy_unit_tests COMMAND ffuzzy_test)
