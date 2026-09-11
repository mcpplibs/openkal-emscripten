#include "sys.h"
#include <openkal/env.h>

#include <emscripten.h>
#include <stdlib.h>
#include <string.h>

// THE ARGUMENTS ARE THE ONE THING A C LIBRARY DOES NOT OFFER A LIBRARY.
//
// Everything else in this implementation is a forward: openkal asks for a
// thing and libc has a function that answers it. `argc`/`argv` are different,
// because in C they are PARAMETERS OF MAIN -- a program that does not pass
// them on has not kept them, and nothing in the library will hand them back.
//
// THREE MECHANISMS WERE TRIED AND THE PLATFORM REFUSED TWO OF THEM.
//
// 1. `.init_array` with a `(argc, argv, envp)` signature, which is how a
//    musl-derived C library passes them to an initialiser. It does not link:
//
//      wasm-ld: error: constructor functions cannot take arguments:
//               capture(int, char**, char**)
//
//    wasm's start section takes no arguments, so this is not a difference in
//    convention to be careful about -- it is forbidden outright. (The
//    ecosystem's record of glibc and musl passing initialiser arguments
//    differently is what made this the first thing tried, and the record was
//    about a mechanism that exists.)
//
// 2. A weak interposer on the entry point. Emscripten's libc exports
//    `__main_argc_argv` and calls the program's `main` from it, so there is no
//    seam a library can occupy: the name belongs to the program.
//
// 3. THE HOST, WHICH IS WHERE THE ARGUMENTS ACTUALLY ARE. Measured with
//    emsdk 6.0.9 under node:
//
//      C argc=3   argv[0]=<script path>   argv[1]=alpha   argv[2]=beta
//      process.argv = [<node>, <script path>, alpha, beta]
//      Module['arguments'] is not defined in this build
//
//    so `process.argv.slice(1)` IS the vector emscripten hands `main`, and it
//    is reachable from a library through `EM_ASM` -- the documented C-to-JS
//    escape, which needs no link flag and no exported runtime method.
//
// A JS ESCAPE IN AN IMPLEMENTATION THAT IS OTHERWISE PURE FORWARDING IS WORTH
// JUSTIFYING. This package sits ABOVE a C library on a JavaScript host; the
// host is the platform, in the same sense that a kernel is the platform for
// `openkal-linux`, and asking it is the analogue of issuing a system call.
// It is confined to this one question, because this is the one question the C
// library between them does not answer.
//
// AND INDEX ZERO HAS A SECOND, JS-FREE SOURCE. musl sets `__progname_full`
// from `argv[0]`, measured as the script's path under node, so a browser --
// where there is no `process` and no command line -- still reports the name
// the program was started by. The specification requires the count to be at
// least one for exactly that reason: a name always exists, and its absence is
// an empty string rather than a missing element.
namespace {

extern "C" char* __progname;
extern "C" char* __progname_full;

// The host's vector, or zero when there is no host command line. Asked once:
// the value cannot change during a run, and `EM_ASM` crosses into JS.
kal_uintptr host_arg_count() {
    static kal_intptr cached = -1;
    if (cached < 0) {
        cached = static_cast<kal_intptr>(EM_ASM_INT({
            if (typeof process !== 'undefined' && process.argv
                && process.argv.length > 1) {
                return process.argv.length - 1;
            }
            return 0;
        }));
    }
    return static_cast<kal_uintptr>(cached);
}

// Copies host argument `index` into `out`, and returns its full length in
// bytes whether or not it fitted -- which is what lets a caller size a buffer
// with one call and fill it with a second.
kal_intptr host_arg(kal_uintptr index, char* out, kal_uintptr cap) {
    // NO EMSCRIPTEN RUNTIME HELPER AND NO ALLOCATION, and both were measured
    // rather than chosen.
    //
    // The first version used `stringToUTF8` into a `_malloc` scratch buffer,
    // because `stringToUTF8` writes at most its third argument INCLUDING a
    // terminator while openkal's copy is unterminated -- so writing directly
    // with a capacity of `cap + 1` would put a NUL one byte past the caller's
    // buffer. Measured under node:
    //
    //   Aborted(malloc() called but not included in the build
    //           - add `_malloc` to EXPORTED_FUNCTIONS)
    //
    // A library cannot require a link-time export list from every program that
    // uses it. `TextEncoder` is a platform API in both node and every browser
    // and needs nothing exported, `HEAPU8.set` copies without allocating, and
    // the encoded length is then the byte count openkal reports -- one
    // mechanism instead of a helper for the length and another for the copy.
    const int n = EM_ASM_INT({
        var a = (typeof process !== 'undefined' && process.argv)
              ? process.argv.slice(1) : [];
        var i = $0;
        if (i >= a.length) return -1;
        var enc = new TextEncoder().encode(a[i]);
        if ($1 !== 0 && $2 !== 0) {
            var take = Math.min(enc.length, $2);
            HEAPU8.set(enc.subarray(0, take), $1);
        }
        return enc.length;
    }, static_cast<int>(index), out, static_cast<int>(cap));
    return n < 0 ? -kal_err_not_found : static_cast<kal_intptr>(n);
}

kal_intptr copy_out(const char* s, kal_uintptr n, char* out, kal_uintptr cap) {
    // THE LENGTH IS REPORTED WHETHER OR NOT IT FITS, which is what lets a
    // caller size a buffer with one call and fill it with a second. A
    // truncating copy that reported the truncated length would make the second
    // call impossible.
    if (out != nullptr && cap != 0) {
        const kal_uintptr take = n < cap ? n : cap;
        ::memcpy(out, s, static_cast<size_t>(take));
    }
    return static_cast<kal_intptr>(n);
}

}  // namespace

extern "C" {

// AT LEAST ONE, ALWAYS. The host's vector already contains the name at index
// zero when there is a host command line; when there is not -- a module a page
// instantiated -- the name alone is the vector, and the count is one.
kal_uintptr kal_env_arg_count(void) {
    const kal_uintptr n = host_arg_count();
    return n != 0 ? n : 1u;
}

kal_intptr kal_env_arg(kal_uintptr index, char* out, kal_uintptr cap) {
    if (host_arg_count() != 0) return host_arg(index, out, cap);
    // No host command line: index zero is the name musl recorded, and there is
    // nothing after it.
    if (index != 0) return -kal_err_not_found;
    const char* name = __progname_full != nullptr ? __progname_full
                     : (__progname != nullptr ? __progname : "");
    return copy_out(name, ::strlen(name), out, cap);
}

// `getenv` TAKES A NUL-TERMINATED NAME AND openkal PASSES A LENGTH, so the
// name is copied. Bounded rather than allocated: a variable name longer than
// this is not a name, and an allocation here would make a lookup able to fail
// for want of memory.
kal_intptr kal_env_var(const char* name, kal_uintptr name_len,
                       char* out, kal_uintptr cap) {
    if (name == nullptr) return -kal_err_invalid;
    char key[256];
    if (name_len == 0 || name_len >= sizeof(key)) return -kal_err_invalid;
    ::memcpy(key, name, static_cast<size_t>(name_len));
    key[name_len] = '\0';
    const char* v = ::getenv(key);
    if (v == nullptr) return -kal_err_not_found;
    return copy_out(v, ::strlen(v), out, cap);
}

// `environ` IS THE ENUMERATION, AND IT IS THE ONLY ONE. POSIX offers no
// indexed accessor, so the count is a walk and so is the lookup; both are
// what every program that has ever printed its environment does.
kal_uintptr kal_env_var_count(void) {
    extern char** environ;
    if (environ == nullptr) return 0;
    kal_uintptr n = 0;
    while (environ[n] != nullptr) ++n;
    return n;
}

kal_intptr kal_env_var_at(kal_uintptr index, char* out, kal_uintptr cap) {
    extern char** environ;
    if (environ == nullptr) return -kal_err_not_found;
    kal_uintptr n = 0;
    while (environ[n] != nullptr) ++n;
    if (index >= n) return -kal_err_not_found;
    const char* s = environ[index];
    return copy_out(s, ::strlen(s), out, cap);
}

}
