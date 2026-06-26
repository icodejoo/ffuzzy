Pod::Spec.new do |s|
  s.name             = 'ffuzzy'
  s.version          = '0.1.0'
  s.summary          = 'C fuzzy-string-matching Flutter plugin (Smith-Waterman + bitmap prefilter)'
  s.description      = <<-DESC
    A Flutter FFI plugin that exposes a C implementation of fuzzy string
    matching using the Smith-Waterman DP algorithm with camelCase / boundary
    bonuses, a 64-bit bitmap prefilter, and a parallel thread-pool search.
  DESC
  s.homepage         = 'https://github.com/placeholder/ffuzzy'
  s.license          = { :type => 'MIT', :file => '../LICENSE' }
  s.author           = { 'ffuzzy' => 'ffuzzy@example.com' }

  s.source           = { :path => '.' }

  s.source_files     = '../ios/Classes/**/*',
                       '../src/ffuzzy.c',
                       '../src/scorer.c',
                       '../src/corpus.c',
                       '../src/thread_pool.c'

  s.public_header_files = '../src/ffuzzy.h'

  s.dependency 'FlutterMacOS'
  s.platform = :osx, '10.14'

  s.pod_target_xcconfig = {
    'DEFINES_MODULE' => 'YES',
    'HEADER_SEARCH_PATHS' => '$(PODS_TARGET_SRCROOT)/../src',
  }
end
