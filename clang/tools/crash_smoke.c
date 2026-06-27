// Crash-handler smoke test: install the handler, force a NULL dereference, and
// let the handler print a backtrace before the process dies. Used by CI to
// verify ffz_crash.c works on each OS. argv[1] (optional) = breadcrumb path.
//
// Build (with the handler): cc -g -O0 -I include tools/crash_smoke.c \
//                              ffi/ffz_crash.c [-ldbghelp | -ldl] -o crash_smoke
// Run: ./crash_smoke out.log 2> stderr.txt ; the process exits via the signal,
//      so callers should tolerate the non-zero exit and grep the output.
#include "ffz_crash.h"
#include <stdio.h>

// External linkage on purpose: with -rdynamic this lands in .dynsym so dladdr()
// can name it on Linux (where local symbols are invisible to dladdr). CI greps
// the backtrace for "boom" to prove the crash site is symbolized.
int boom(volatile int *p) { return *p; }

int main(int argc, char **argv) {
    ffz_install_crash_handler(argc > 1 ? argv[1] : 0);
    printf("handler installed; forcing a fault...\n");
    fflush(stdout);
    volatile int *p = (volatile int *)0;
    return boom(p);
}
