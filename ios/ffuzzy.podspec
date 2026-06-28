#
# Flutter iOS FFI-plugin: static-links the ffz C sources into the app so the
# symbols are reachable via DynamicLibrary.process(). (No Rust involved.)
#
Pod::Spec.new do |s|
  s.name             = 'ffuzzy'
  s.version          = '0.4.0'
  s.summary          = 'ffz C fuzzy matcher (nucleo-compatible).'
  s.description      = 'Fuzzy/substring/prefix/postfix/exact matching engine in C.'
  s.homepage         = 'https://github.com/icodejoo/ffuzzy'
  s.license          = { :file => '../LICENSE' }
  s.author           = { 'ffz' => 'ffz@example.com' }
  s.source           = { :path => '.' }
  # The C engine + FFI shim live two levels up (clang/src, clang/ffi, clang/include).
  # Only ffz_ffi.c is included — ffz_crash.c requires FFZ_HAVE_CRASH_HANDLER and
  # is compiled conditionally by CMake builds; Xcode/App Store apps use the OS
  # tombstone + dSYM for crash symbolication instead.
  s.source_files     = '../src/*.c', '../ffi/ffz_ffi.c', '../include/*.h'
  s.public_header_files = '../include/ffz.h', '../include/ffz_corpus.h'
  # No forced -O/-DNDEBUG: Xcode's per-config defaults are the automatic switch
  # (Debug -O0 -g = locatable; Release -Os + .dSYM = compressed, symbolized
  # offline). On Apple the app's .dSYM symbolizes crash addresses regardless of
  # symbol visibility, so hiding internals is safe.
  s.compiler_flags   = '-fvisibility=hidden -funwind-tables'
  s.platform         = :ios, '12.0'
  s.pod_target_xcconfig = { 'DEFINES_MODULE' => 'YES' }
end
