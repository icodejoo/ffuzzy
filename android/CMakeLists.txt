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

# Android NDK provides pthreads via libc; log is for ALog
find_library(log-lib log)
target_link_libraries(ffuzzy PRIVATE ${log-lib})
