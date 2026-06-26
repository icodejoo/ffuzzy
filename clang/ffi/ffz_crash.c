// Cross-platform last-gasp crash handler. See include/ffz_crash.h for the
// contract and the (build-dependent) localization fidelity.
//
// Design: build the whole report into one fixed stack/static buffer using only
// bounded appends (no malloc, no stdio on the POSIX path), then emit it with
// write(2)/open(2) — these are async-signal-safe. Symbol names come from
// dladdr (POSIX) / dbghelp (Windows); line numbers come from the build's
// debug info (PDB on Windows; addr2line/atos against the unstripped lib or
// sidecar elsewhere).
#include "ffz_crash.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

// ---- shared bounded report buffer builder -------------------------------
#define FFZ_REPORT_CAP 8192
typedef struct {
    char buf[FFZ_REPORT_CAP];
    size_t len;
} ffz_report;

static void rep_str(ffz_report *r, const char *s) {
    if (!s) s = "(null)";
    size_t n = strlen(s);
    if (r->len + n > FFZ_REPORT_CAP - 1) n = FFZ_REPORT_CAP - 1 - r->len;
    memcpy(r->buf + r->len, s, n);
    r->len += n;
}
static void rep_hex(ffz_report *r, uint64_t v) {
    char tmp[19];  // "0x" + 16 hex
    tmp[0] = '0'; tmp[1] = 'x';
    int i = 18; tmp[i--] = '\0';
    if (v == 0) tmp[i--] = '0';
    while (v && i >= 2) { tmp[i--] = "0123456789abcdef"[v & 0xF]; v >>= 4; }
    rep_str(r, "0x");
    rep_str(r, tmp + i + 1);
}
static void rep_dec(ffz_report *r, long v) {
    char tmp[24]; int i = 23; tmp[i--] = '\0';  // fits LONG_MIN (20 digits + '-')
    int neg = v < 0; unsigned long u = neg ? (unsigned long)(-v) : (unsigned long)v;
    if (u == 0) tmp[i--] = '0';
    while (u && i >= 0) { tmp[i--] = (char)('0' + (u % 10)); u /= 10; }
    if (neg && i >= 0) tmp[i--] = '-';
    rep_str(r, tmp + i + 1);
}

// Path stored at install time (fixed buffer — set once, read in handler).
static char g_breadcrumb[1024];
static volatile int g_installed = 0;

// =========================================================================
// Windows
// =========================================================================
#if defined(_WIN32)
#include <windows.h>
#include <dbghelp.h>
#include <stdio.h>

static void emit_report(const ffz_report *r) {
    fwrite(r->buf, 1, r->len, stderr);
    fflush(stderr);
    if (g_breadcrumb[0]) {
        FILE *f = NULL;
        if (fopen_s(&f, g_breadcrumb, "wb") == 0 && f) {
            fwrite(r->buf, 1, r->len, f);
            fclose(f);
        }
    }
}

static LONG WINAPI ffz_win_handler(EXCEPTION_POINTERS *ep) {
    ffz_report r; r.len = 0;
    rep_str(&r, "\n*** ffz native crash: exception ");
    rep_hex(&r, ep ? ep->ExceptionRecord->ExceptionCode : 0);
    rep_str(&r, " at ");
    rep_hex(&r, ep ? (uint64_t)(uintptr_t)ep->ExceptionRecord->ExceptionAddress : 0);
    rep_str(&r, "\n");

    HANDLE proc = GetCurrentProcess();
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
    SymInitialize(proc, NULL, TRUE);  // reads ffz.pdb if alongside the dll

    void *frames[64];
    USHORT n = CaptureStackBackTrace(0, 64, frames, NULL);
    char symbuf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO *sym = (SYMBOL_INFO *)symbuf;
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    for (USHORT i = 0; i < n; i++) {
        DWORD64 addr = (DWORD64)(uintptr_t)frames[i];
        rep_str(&r, "  #"); rep_dec(&r, i); rep_str(&r, "  ");
        DWORD64 disp = 0;
        if (SymFromAddr(proc, addr, &disp, sym)) {
            rep_str(&r, sym->Name); rep_str(&r, "+"); rep_hex(&r, disp);
        } else {
            rep_hex(&r, addr);
        }
        IMAGEHLP_LINE64 line; DWORD ldisp = 0;
        line.SizeOfStruct = sizeof(line);
        if (SymGetLineFromAddr64(proc, addr, &ldisp, &line)) {
            rep_str(&r, "  ("); rep_str(&r, line.FileName);
            rep_str(&r, ":"); rep_dec(&r, (long)line.LineNumber); rep_str(&r, ")");
        }
        rep_str(&r, "\n");
    }
    SymCleanup(proc);
    emit_report(&r);
    return EXCEPTION_CONTINUE_SEARCH;  // let the default handler / crash dump run
}

int ffz_install_crash_handler(const char *breadcrumb_path) {
    g_breadcrumb[0] = '\0';
    if (breadcrumb_path) {
        size_t n = strlen(breadcrumb_path);
        if (n < sizeof(g_breadcrumb)) memcpy(g_breadcrumb, breadcrumb_path, n + 1);
    }
    if (!g_installed) {
        SetUnhandledExceptionFilter(ffz_win_handler);
        g_installed = 1;
    }
    return 1;
}

// =========================================================================
// POSIX (Linux / Android / Apple)
// =========================================================================
#else
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <dlfcn.h>

#if defined(__ANDROID__)
#include <unwind.h>
#include <android/log.h>
#else
#include <execinfo.h>  // glibc + Apple: backtrace()
#endif

static void emit_report(const ffz_report *r) {
    ssize_t w = write(STDERR_FILENO, r->buf, r->len); (void)w;
#if defined(__ANDROID__)
    // logcat lines are bounded; the breadcrumb file is the full record.
    __android_log_write(ANDROID_LOG_FATAL, "ffz", r->buf);
#endif
    if (g_breadcrumb[0]) {
        int fd = open(g_breadcrumb, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) { ssize_t x = write(fd, r->buf, r->len); (void)x; close(fd); }
    }
}

// dladdr a frame into the report: "module(symbol+offset) [addr]".
static void rep_frame(ffz_report *r, void *pc) {
    Dl_info info;
    if (dladdr(pc, &info) && info.dli_fname) {
        const char *base = strrchr(info.dli_fname, '/');
        rep_str(r, base ? base + 1 : info.dli_fname);
        if (info.dli_sname) {
            rep_str(r, "(");
            rep_str(r, info.dli_sname);
            rep_str(r, "+");
            rep_hex(r, (uint64_t)((char *)pc - (char *)info.dli_saddr));
            rep_str(r, ")");
        } else if (info.dli_fbase) {
            rep_str(r, "+");
            rep_hex(r, (uint64_t)((char *)pc - (char *)info.dli_fbase));
        }
    }
    rep_str(r, " [");
    rep_hex(r, (uint64_t)(uintptr_t)pc);
    rep_str(r, "]\n");
}

#if defined(__ANDROID__)
typedef struct { void **frames; int cap; int n; } unwind_state;
static _Unwind_Reason_Code unwind_cb(struct _Unwind_Context *ctx, void *arg) {
    unwind_state *st = (unwind_state *)arg;
    uintptr_t ip = _Unwind_GetIP(ctx);
    if (ip && st->n < st->cap) st->frames[st->n++] = (void *)ip;
    return st->n >= st->cap ? _URC_END_OF_STACK : _URC_NO_REASON;
}
static int capture(void **frames, int cap) {
    unwind_state st = {frames, cap, 0};
    _Unwind_Backtrace(unwind_cb, &st);
    return st.n;
}
#else
static int capture(void **frames, int cap) { return backtrace(frames, cap); }
#endif

static const char *signame(int sig) {
    switch (sig) {
        case SIGSEGV: return "SIGSEGV (invalid memory access)";
        case SIGABRT: return "SIGABRT (abort)";
        case SIGBUS:  return "SIGBUS (bus error)";
        case SIGILL:  return "SIGILL (illegal instruction)";
        case SIGFPE:  return "SIGFPE (arithmetic error)";
        default:      return "signal";
    }
}

static volatile sig_atomic_t g_in_handler = 0;

static void ffz_posix_handler(int sig, siginfo_t *info, void *uc) {
    (void)uc;
    // Re-entrancy guard: a fault *inside* the handler (e.g. dladdr/backtrace
    // touching a corrupt heap) must go straight to the default action, not loop.
    if (g_in_handler) { signal(sig, SIG_DFL); raise(sig); return; }
    g_in_handler = 1;
    ffz_report r; r.len = 0;
    rep_str(&r, "\n*** ffz native crash: ");
    rep_str(&r, signame(sig));
    if (info) { rep_str(&r, " at fault addr "); rep_hex(&r, (uint64_t)(uintptr_t)info->si_addr); }
    rep_str(&r, "\n");

    void *frames[64];
    int n = capture(frames, 64);
    for (int i = 0; i < n; i++) {
        rep_str(&r, "  #"); rep_dec(&r, i); rep_str(&r, "  ");
        rep_frame(&r, frames[i]);
    }
    emit_report(&r);

    // Restore the default disposition. For a synchronous hardware fault
    // (SIGSEGV/BUS/ILL/FPE) just return: the faulting instruction re-executes
    // and re-faults into the default handler, preserving the original fault
    // site for the OS core dump / crash reporter. SIGABRT has no instruction to
    // retry, so re-raise it explicitly. Never attempt to "recover".
    signal(sig, SIG_DFL);
    if (sig == SIGABRT) raise(sig);
}

int ffz_install_crash_handler(const char *breadcrumb_path) {
    g_breadcrumb[0] = '\0';
    if (breadcrumb_path) {
        size_t n = strlen(breadcrumb_path);
        if (n < sizeof(g_breadcrumb)) memcpy(g_breadcrumb, breadcrumb_path, n + 1);
    }
    if (g_installed) return 1;
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = ffz_posix_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    int sigs[] = {SIGSEGV, SIGABRT, SIGBUS, SIGILL, SIGFPE};
    for (size_t i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++)
        sigaction(sigs[i], &sa, NULL);
    g_installed = 1;
    return 1;
}
#endif
