#
# Flutter macOS FFI-plugin: static-links the ffz C sources. Symbols are then
# reachable via DynamicLibrary.process(). (No Rust involved.)
#
Pod::Spec.new do |s|
  s.name             = 'ffuzzy'
  s.version          = '0.3.1'
  s.summary          = 'ffz C fuzzy matcher (nucleo-compatible).'
  s.description      = 'Fuzzy/substring/prefix/postfix/exact matching engine in C.'
  s.homepage         = 'https://github.com/icodejoo/ffuzzy'
  s.license          = { :file => '../LICENSE' }
  s.author           = { 'ffz' => 'ffz@example.com' }
  s.source           = { :path => '.' }
  # ffi/*.c bundles the FFI shim + ffz_crash.c (native crash handler).
  s.source_files     = '../src/*.c', '../ffi/*.c', '../include/*.h'
  s.public_header_files = '../include/ffz.h', '../include/ffz_corpus.h'
  # No forced -O/-DNDEBUG: Xcode's per-config defaults are the automatic switch
  # (Debug -O0 -g = locatable; Release -Os + .dSYM = compressed, symbolized
  # offline). The app's .dSYM symbolizes crash addresses regardless of
  # symbol visibility, so hiding internals is safe.
  s.compiler_flags   = '-fvisibility=hidden -funwind-tables'
  s.platform         = :osx, '10.14'
  s.pod_target_xcconfig = { 'DEFINES_MODULE' => 'YES' }
end
