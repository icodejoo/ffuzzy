cmake_minimum_required(VERSION 3.16)
project(ffuzzy_test C)
set(CMAKE_C_STANDARD 11)

set(FFUZZY_SOURCES
    ../src/scorer.c
    ../src/corpus.c
    ../src/thread_pool.c
    ../src/ffuzzy.c
)

add_executable(ffuzzy_test test_main.c ${FFUZZY_SOURCES})
target_include_directories(ffuzzy_test PRIVATE ../src)
if(UNIX)
    target_link_libraries(ffuzzy_test pthread)
endif()

add_executable(ffuzzy_integration test_integration.c ${FFUZZY_SOURCES})
target_include_directories(ffuzzy_integration PRIVATE ../src)
if(UNIX)
    target_link_libraries(ffuzzy_integration pthread)
endif()

add_executable(ffuzzy_adversarial test_adversarial.c ${FFUZZY_SOURCES})
target_include_directories(ffuzzy_adversarial PRIVATE ../src)
if(UNIX)
    target_link_libraries(ffuzzy_adversarial pthread)
endif()

enable_testing()
add_test(NAME unit        COMMAND ffuzzy_test)
add_test(NAME integration COMMAND ffuzzy_integration)
add_test(NAME adversarial COMMAND ffuzzy_adversarial)
