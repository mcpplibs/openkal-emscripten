#include "sys.h"
#include <openkal/random.h>

#include <stdlib.h>
#include <unistd.h>   // getentropy

extern "C" {

// `getentropy` AND NOT `/dev/urandom`, BECAUSE ON THIS PLATFORM THE SECOND IS
// NOT THE SAME THING.
//
// Emscripten maps `getentropy` onto the host's cryptographic generator --
// `crypto.getRandomValues` in a browser, `crypto.randomBytes` under node -- and
// that is the facility openkal.random names. MEMFS also offers a
// `/dev/urandom`, and it is a DIFFERENT source: a device node the file system
// synthesises, reached through the same open/read path as an ordinary file, so
// a program that opened it would be reading the file system rather than asking
// the host for entropy.
//
// The call is chunked because `getentropy` is specified to refuse requests
// above 256 bytes, and openkal places no such limit on its caller.
int kal_random_fill(void* out, kal_uintptr len) {
    if (out == nullptr && len != 0) return kal_err_invalid;
    auto* p = static_cast<unsigned char*>(out);
    kal_uintptr done = 0;
    while (done < len) {
        const kal_uintptr take = (len - done) > 256 ? 256 : (len - done);
        if (::getentropy(p + done, static_cast<size_t>(take)) != 0) {
            if (oke::interrupted()) continue;
            return oke::last();
        }
        done += take;
    }
    return kal_ok;
}

kal_uintptr kal_random_props(void) {
    // NEITHER WORD IS SET, AND BOTH ABSENCES ARE STATEMENTS. The host's
    // generator does not block: it is seeded before the module is
    // instantiated, so there is no waiting-for-entropy state to report. And
    // it is not a hardware instruction this implementation can see -- the
    // browser may be using one, but that is the browser's fact and not
    // something a wasm module can observe or promise.
    return 0u;
}

}
