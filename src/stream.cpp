#include "sys.h"
#include <openkal/stream.h>

#include <emscripten.h>
#include <unistd.h>

// THE HOST IS ASKED WHETHER A STANDARD STREAM IS A TERMINAL, BECAUSE THE C
// LIBRARY ANSWERS FOR ITS OWN DEVICE AND NOT FOR THE HOST'S STREAM.
//
// Emscripten wires descriptors 0, 1 and 2 to its terminal device whatever the
// host has behind them, so `isatty' is 1 under a pipe as under a terminal.
// Measured with emsdk 6.0.9 under node 26: `node prog.js | cat' and
// `node prog.js < /dev/null > /dev/null' both report isatty 1, 1, 1, and
// tcgetattr succeeds on each. node knows what its own streams are
// (`tty.isatty'); a page has no terminal at all. Another descriptor is a file
// or a socket of this C library, and the library's answer is the host's there.
bool oke::host_terminal(int fd) {
    if (::isatty(fd) != 1) return false;
    if (fd > 2) return true;
    return EM_ASM_INT({
        if (typeof process === 'undefined' || !process.versions || !process.versions.node)
            return 0;
        try { return require('tty').isatty($0) ? 1 : 0; } catch (e) { return 0; }
    }, fd) != 0;
}

extern "C" {

// BARE DESCRIPTORS, AND THAT IS A DISCIPLINE RATHER THAN A SHORTCUT.
//
// The standard streams are BORROWED: openkal has no operation that releases
// one, so there is no released-handle condition for the generation counter in
// handle.h to detect, and packing them would put two conventions on one word.
// `openkal-linux` records what that cost when a packed word reached the
// unpacking function that accepts a bare one silently.
kal_stream kal_stdin (void) { return kal_stream{0}; }
kal_stream kal_stdout(void) { return kal_stream{1}; }
kal_stream kal_stderr(void) { return kal_stream{2}; }

// ONE SIGNED WORD: the count, or the negated condition when no byte moved.
kal_intptr kal_stream_write(kal_stream s, const void* buf, kal_uintptr len) {
    const auto* p = static_cast<const unsigned char*>(buf);
    kal_uintptr done = 0;
    while (done < len) {
        const ssize_t r = ::write(static_cast<int>(s.h), p + done, len - done);
        if (r < 0) {
            if (oke::interrupted()) continue;
            // A PARTIAL WRITE IS REPORTED AS A COUNT AND NOT AS A FAILURE.
            // The caller has to know how many bytes reached the stream before
            // the condition, because the remainder is what it must retry.
            if (done != 0) return static_cast<kal_intptr>(done);
            return oke::fail();
        }
        if (r == 0) break;
        done += static_cast<kal_uintptr>(r);
    }
    return static_cast<kal_intptr>(done);
}

kal_intptr kal_stream_read(kal_stream s, void* buf, kal_uintptr len) {
    for (;;) {
        const ssize_t r = ::read(static_cast<int>(s.h), buf, len);
        if (r < 0) { if (oke::interrupted()) continue; return oke::fail(); }
        // A short read is reported as it occurred. Unlike a short write it
        // carries information the caller requires: zero denotes end of input.
        return static_cast<kal_intptr>(r);
    }
}

// THERE IS NOTHING BENEATH THIS TO COMMIT, AND SAYING SO IS THE ANSWER.
//
// On a kernel, flush means fsync: push the medium's cache to the medium.
// Emscripten's default file system is MEMFS -- the "medium" is the module's
// own linear memory -- so a successful `fsync` and a refused one both mean the
// same thing here, which is that the bytes are as durable as they are going
// to get.
//
// Reporting a failure would be worse than useless: it would oblige every
// caller to distinguish "this stream has no medium" from "the medium could not
// be reached", on a platform where the first is always the answer. So the
// conditions that mean "this object has nothing to commit" are kal_ok, exactly
// as they are on the systems that do have media.
int kal_stream_flush(kal_stream s) {
    if (::fsync(static_cast<int>(s.h)) == 0) return kal_ok;
    if (errno == EINVAL || errno == ENOTSUP || errno == EBADF
#if defined(ENOTTY)
        || errno == ENOTTY
#endif
#if defined(EOPNOTSUPP) && EOPNOTSUPP != ENOTSUP
        || errno == EOPNOTSUPP
#endif
        ) return kal_ok;
    return oke::last();
}

// INTERACTIVE MEANS "A PERSON IS AT THE OTHER END", and the host is who knows
// (oke::host_terminal above): under node with a terminal it is true, under a
// pipe or in a browser it is false. In particular this does not assume that
// descriptor 2 is a terminal because a browser console looks like one.
kal_uintptr kal_stream_props(kal_stream s) {
    return oke::host_terminal(static_cast<int>(s.h))
         ? KAL_STREAM_PROP_INTERACTIVE : 0u;
}

}
