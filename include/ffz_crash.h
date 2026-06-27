// ffz_crash — optional in-process crash handler for native fault localization.
//
// dart:ffi cannot turn a native fault (SIGSEGV/abort/access violation) into a
// catchable Dart exception — the process dies. This installs a last-gasp signal
// / unhandled-exception handler that, *before* the process goes down, writes a
// backtrace to stderr (logcat on Android) and, optionally, to a breadcrumb file
// the host can read on next launch. It then re-raises so the OS/crash reporter
// still fires (it never pretends to recover — that would be unsafe after a
// memory fault).
//
// Localization fidelity depends on the BUILD, not on this code:
//   - debug/profile (not stripped): Windows prints module!func+file:line (PDB);
//     POSIX prints module+func+offset (dladdr) — map offset->line with
//     addr2line/atos against the unstripped lib.
//   - release (stripped + .debug/.pdb/.dSYM sidecar): prints module+offset;
//     symbolize the offset offline with the sidecar.
//
// All work in the handler is bounded and allocation-free on the POSIX path
// (async-signal-safe-friendly): a fixed stack buffer, write(2), open/write/close.
#ifndef FFZ_CRASH_H
#define FFZ_CRASH_H

#ifdef __cplusplus
extern "C" {
#endif

// Install the crash handler (idempotent — repeat calls only update the path).
// `breadcrumb_path` (UTF-8, may be NULL): if set, the handler also writes the
// report to this file, truncating it; the host reads it on the next launch to
// surface "last crash" diagnostics. Returns 1 on success, 0 if unsupported.
int ffz_install_crash_handler(const char *breadcrumb_path);

#ifdef __cplusplus
}
#endif
#endif  // FFZ_CRASH_H
