#include "sys.h"
#include <openkal/abort.h>

#include <stdlib.h>
#include <unistd.h>

extern "C" {

// THE MESSAGE GOES OUT BEFORE THE PROGRAM ENDS, AND THROUGH THE DESCRIPTOR
// RATHER THAN THROUGH stdio.
//
// `write(2)` reaches the host directly; `fputs` would place the bytes in a
// buffer that `_Exit` does not flush and that `abort` is not obliged to. The
// specification requires the message to be delivered, and on a platform whose
// output is a JavaScript callback the difference between the two is whether
// anything is printed at all.
KAL_NORETURN void kal_abort(const char* msg, kal_uintptr len) {
    if (msg != nullptr && len != 0) {
        kal_uintptr done = 0;
        while (done < len) {
            const ssize_t r = ::write(2, msg + done, len - done);
            if (r < 0) { if (oke::interrupted()) continue; break; }
            if (r == 0) break;
            done += static_cast<kal_uintptr>(r);
        }
        const char nl = '\n';
        (void)::write(2, &nl, 1);
    }
    // `abort` and not `_Exit`: the specification's abnormal termination is
    // meant to be observable as abnormal, and on this platform that is the
    // host's unhandled-trap report rather than an exit status -- a wasm module
    // that returns from its entry point has terminated normally whatever value
    // it returned.
    ::abort();
}

// `_Exit` and not `exit`: openkal's termination does not run a C program's
// atexit handlers or flush its stdio, because openkal has neither and a caller
// that borrowed this interface from inside a C program would be surprised to
// find its own handlers run by a call it made through another vocabulary.
//
// The streams this implementation hands out are descriptors, so there is
// nothing of openkal's own left buffered to lose.
KAL_NORETURN void kal_exit(int code) { ::_Exit(code); }

}
