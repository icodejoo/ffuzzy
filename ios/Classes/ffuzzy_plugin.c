/*
 * ffuzzy_plugin.c – iOS/macOS Flutter plugin registration stub.
 *
 * The actual functionality is provided by the C library (ffuzzy.c et al.).
 * This file only registers the plugin with the Flutter engine so that
 * the Dart side can locate the dynamic library via
 * DynamicLibrary.process() on Apple platforms.
 *
 * Swift/Obj-C plugin classes are deliberately omitted: ffuzzy is a pure
 * FFI plugin (no platform channels).
 */

#include <stdint.h>

/* Dummy exported symbol so the linker does not strip the .c file. */
__attribute__((visibility("default")))
void ffuzzy_plugin_register(void) {}
