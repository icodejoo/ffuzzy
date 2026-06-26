Pod::Spec.new do |s|
  s.name             = 'ffuzzy'
  s.version          = '0.0.1'
  s.summary          = 'Fast fuzzy-matching Flutter plugin backed by a Rust native library.'
  s.description      = <<-DESC
    ffuzzy provides fast fuzzy string matching via a Rust native library.
    Import package:ffuzzy/ffuzzy.dart for the full variant (~531 KB),
    or package:ffuzzy/lite.dart for the lite variant (~254 KB, no parallel
    multi-core or advanced match modes).  No build flags needed — the correct
    native binary is selected automatically based on your import.
  DESC
  s.homepage         = 'http://example.com'
  s.license          = { :file => '../LICENSE' }
  s.author           = { 'Your Company' => 'email@example.com' }

  s.source           = { :path => '.' }
  s.source_files     = 'Classes/**/*'
  s.dependency 'Flutter'
  s.platform = :ios, '11.0'

  s.pod_target_xcconfig = {
    'DEFINES_MODULE'                    => 'YES',
    'EXCLUDED_ARCHS[sdk=iphonesimulator*]' => 'i386',
    'OTHER_LDFLAGS'                     => '-force_load ${BUILT_PRODUCTS_DIR}/librust_lib_ffuzzy.a',
  }
  s.swift_version = '5.0'

  # ── Auto-detect lite vs full from Dart imports ──────────────────────────────
  # Users simply import the variant they need; no build flags required:
  #   import 'package:ffuzzy/lite.dart'    →  lite  ~254 KB
  #   import 'package:ffuzzy/ffuzzy.dart'  →  full  ~531 KB
  #
  # The script walks up from the plugin dir to find the Flutter project root
  # (first parent directory containing pubspec.yaml), then greps lib/ for the
  # import paths and calls build_pod.sh with the appropriate manifest dir.
  # A clean temp manifest dir is built for lite (identical file set to CI),
  # so cargokit computes the same crate-hash and finds the precompiled binary.
  s.script_phase = {
    :name => 'Build Rust library (ffuzzy)',
    :script => <<~'SHELL',
      set -e

      PLUGIN_DIR="$PODS_TARGET_SRCROOT"

      # Walk up directory tree to find the Flutter project root (pubspec.yaml)
      FLUTTER_ROOT=""
      dir="$PLUGIN_DIR"
      for i in 1 2 3 4 5 6 7 8; do
        dir=$(dirname "$dir")
        if [ -f "$dir/pubspec.yaml" ]; then
          FLUTTER_ROOT="$dir"
          break
        fi
      done

      # Detect lite vs full import; full wins if both are present
      HAS_LITE=0
      HAS_FULL=0
      if [ -n "$FLUTTER_ROOT" ] && [ -d "$FLUTTER_ROOT/lib" ]; then
        grep -rl "package:ffuzzy/lite.dart"   "$FLUTTER_ROOT/lib" 2>/dev/null | grep -q . && HAS_LITE=1 || true
        grep -rl "package:ffuzzy/ffuzzy.dart" "$FLUTTER_ROOT/lib" 2>/dev/null | grep -q . && HAS_FULL=1 || true
      fi

      USE_LITE=0
      if [ "$HAS_LITE" = "1" ] && [ "$HAS_FULL" = "0" ]; then
        USE_LITE=1
      fi

      if [ "$USE_LITE" = "1" ]; then
        echo "[ffuzzy] lite.dart detected → lite variant (~254 KB)"
        LITE_DIR="$BUILT_PRODUCTS_DIR/ffuzzy_lite_manifest"
        mkdir -p "$LITE_DIR/src"
        cp  "$PLUGIN_DIR/../rust/Cargo.toml"           "$LITE_DIR/Cargo.toml"
        cp -r "$PLUGIN_DIR/../rust/src/."              "$LITE_DIR/src/"
        cp  "$PLUGIN_DIR/../rust/cargokit_lite.yaml"   "$LITE_DIR/cargokit.yaml"
        sh "$PLUGIN_DIR/../cargokit/build_pod.sh" "$LITE_DIR" rust_lib_ffuzzy
      else
        if [ "$HAS_LITE" = "1" ]; then
          echo "[ffuzzy] Both lite.dart and ffuzzy.dart detected → full variant wins"
        else
          echo "[ffuzzy] ffuzzy.dart detected → full variant (~531 KB)"
        fi
        sh "$PLUGIN_DIR/../cargokit/build_pod.sh" ../rust rust_lib_ffuzzy
      fi
    SHELL
    :execution_position => :before_compile,
    :input_files  => ['${BUILT_PRODUCTS_DIR}/cargokit_phony'],
    :output_files => ['${BUILT_PRODUCTS_DIR}/librust_lib_ffuzzy.a'],
  }
end
