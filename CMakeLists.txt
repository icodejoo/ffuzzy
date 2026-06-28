# Builds the ffz shared library (matcher + FFI shim [+ crash handler]) for
# Flutter ffiPlugin use (Android / Windows / Linux desktop) or any CMake consumer.
#
# The debug/release split is AUTOMATIC, keyed off CMAKE_BUILD_TYPE (Flutter sets
# Debug / Profile / Release for `flutter run`/`--profile`/`--release`):
#   * debug/profile -> LOCATABLE: -O1 -g, symbols kept (no strip, no hidden
#     visibility), asserts on, the in-process crash handler compiled in. Native
#     errors AND hard crashes are pinpointed in-process (Windows: file:line via
#     PDB; POSIX: func+offset via dladdr).
#   * release       -> COMPRESSED: -Os/-O2, NDEBUG, hidden visibility, stripped
#     with a split .debug sidecar (GCC/Clang) / .pdb (MSVC). The crash handler
#     is NOT compiled here, so the release lib stays ~32 KB (arm64).
#
# Why the handler is release-OFF by default: it forces .eh_frame unwind tables
# across the whole library (so _Unwind_Backtrace can walk the stack), which
# inflates the stripped arm64 .so from ~32 KB to ~58 KB. Release crashes are
# instead diagnosed from the OS tombstone + the shipped .debug/.pdb sidecar.
# To put the in-process handler back into release (the ~58 KB path), configure
# with -DFFZ_CRASH_IN_RELEASE=ON and ship the sidecar.
cmake_minimum_required(VERSION 3.14)
project(ffz LANGUAGES C)

option(FFZ_CRASH_IN_RELEASE
  "Compile the in-process crash handler into release builds (+~26 KB unwind tables; ship the .debug/.pdb sidecar to symbolize)." OFF)

file(GLOB FFZ_CORE CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c")
set(FFZ_SRCS ${FFZ_CORE} "${CMAKE_CURRENT_SOURCE_DIR}/ffi/ffz_ffi.c")

# --- pick locatable (debug/profile) vs compressed (release) ----------------
string(TOUPPER "${CMAKE_BUILD_TYPE}" FFZ_BT)
if(FFZ_BT STREQUAL "DEBUG" OR FFZ_BT STREQUAL "PROFILE" OR
   FFZ_BT STREQUAL "RELWITHDEBINFO")
  set(FFZ_LOCATABLE ON)
else()
  set(FFZ_LOCATABLE OFF)   # Release / unspecified -> smallest, stripped
endif()

# Crash handler is compiled when locatable, or when forced into release.
set(FFZ_CRASH OFF)
if(FFZ_LOCATABLE OR FFZ_CRASH_IN_RELEASE)
  set(FFZ_CRASH ON)
  list(APPEND FFZ_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/ffi/ffz_crash.c")
endif()

add_library(ffz SHARED ${FFZ_SRCS})
target_include_directories(ffz PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(ffz PRIVATE c_std_11)

# ffz_corpus.c parallel scan uses pthreads on POSIX. glibc>=2.34 folds libpthread
# into libc, but musl / old glibc / non-glibc need it linked explicitly. No-op on
# Win32 (the corpus uses CreateThread there). Threads::Threads adds -pthread.
find_package(Threads REQUIRED)
target_link_libraries(ffz PRIVATE Threads::Threads)

if(NOT FFZ_LOCATABLE)
  target_compile_definitions(ffz PRIVATE NDEBUG)
  set_target_properties(ffz PROPERTIES C_VISIBILITY_PRESET hidden)
endif()

if(FFZ_CRASH)
  # ffz_ffi.c only exports ffz_ffi_install_crash_handler when the handler is in.
  target_compile_definitions(ffz PRIVATE FFZ_HAVE_CRASH_HANDLER)
  if(WIN32)
    target_link_libraries(ffz PRIVATE dbghelp)
  elseif(ANDROID)
    target_link_libraries(ffz PRIVATE log dl)   # logcat + dladdr symbol names
  endif()
  if(NOT MSVC)
    # unwind tables let _Unwind_Backtrace/backtrace walk the stack in-process.
    target_compile_options(ffz PRIVATE -funwind-tables -fno-omit-frame-pointer)
  endif()
endif()

# Optimization tier: Debug = low-opt for stepping; Profile (and RelWithDebInfo)
# = full -O2 AND symbols kept (optimized but still locatable — Flutter profile
# is a performance build); Release = size-optimized + stripped.
if(MSVC)
  target_compile_options(ffz PRIVATE /Zi)              # always emit a PDB sidecar
  target_link_options(ffz PRIVATE /DEBUG /OPT:REF /OPT:ICF)
  if(FFZ_BT STREQUAL "DEBUG")
    target_compile_options(ffz PRIVATE /Od)
  else()
    target_compile_options(ffz PRIVATE /O2)            # profile + release
  endif()
else()
  # -g always: in locatable builds it stays in the .so; in release it is split
  # to the .debug sidecar and then stripped (so the sidecar is non-empty).
  target_compile_options(ffz PRIVATE -g -ffunction-sections -fdata-sections)
  if(FFZ_BT STREQUAL "DEBUG")
    target_compile_options(ffz PRIVATE -O1)
  elseif(FFZ_LOCATABLE)
    target_compile_options(ffz PRIVATE -O2)            # profile: optimized + symbols
  else()
    target_compile_options(ffz PRIVATE -Os)            # release: smallest
  endif()
  if(NOT APPLE)
    target_link_options(ffz PRIVATE -Wl,--gc-sections)
  endif()
  # release (non-Apple): split debug info to a sidecar, then strip the shipped
  # library and link it back via gnu-debuglink. Apple keeps symbols in a .dSYM
  # produced by Xcode, so nothing to do there.
  if(NOT FFZ_LOCATABLE AND NOT APPLE AND CMAKE_OBJCOPY AND CMAKE_STRIP)
    add_custom_command(TARGET ffz POST_BUILD
      COMMAND "${CMAKE_OBJCOPY}" --only-keep-debug
              "$<TARGET_FILE:ffz>" "$<TARGET_FILE:ffz>.debug"
      COMMAND "${CMAKE_STRIP}" --strip-all "$<TARGET_FILE:ffz>"
      COMMAND "${CMAKE_OBJCOPY}"
              "--add-gnu-debuglink=$<TARGET_FILE:ffz>.debug" "$<TARGET_FILE:ffz>"
      VERBATIM)
  endif()
endif()

# Flutter ffiPlugin: expose the built artifact for bundling.
set(ffz_bundled_libraries "$<TARGET_FILE:ffz>" PARENT_SCOPE)
