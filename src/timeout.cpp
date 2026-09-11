#include "sys.h"
#include "handle.h"
#include <openkal/timeout.h>

#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

// A TIMEOUT IS A WAIT ON A DESCRIPTOR PLUS A LIMIT, and above a C library that
// is `poll` followed by the operation. The two-step is not an implementation
// detail to hide: openkal's contract is that the operation is attempted only
// once the wait said it would not block, and that is the only arrangement a
// library-level implementation can offer at all.
namespace {

constexpr kal_u64 kNsPerMs = 1000000ull;

// openkal SPELLS "NO LIMIT" AS ZERO AND `poll` SPELLS IT AS -1, which is the
// one translation in this file that is not arithmetic. A zero passed through
// unchanged would mean "return immediately", turning every unlimited wait into
// a non-blocking probe -- a mistranslation that succeeds on a ready descriptor
// and fails on a busy one, which is the hardest kind to notice.
int poll_ms(kal_u64 timeout_ns) {
    if (timeout_ns == 0) return -1;
    const kal_u64 ms = timeout_ns / kNsPerMs;
    if (ms == 0) return 1;   // a sub-millisecond limit is still a limit
    if (ms > 0x7fffffffull) return 0x7fffffff;
    return static_cast<int>(ms);
}

// Returns 1 ready, 0 timed out, -1 failed (the condition is in errno).
int wait_for(int fd, short events, kal_u64 timeout_ns) {
    struct pollfd p{};
    p.fd = fd;
    p.events = events;
    for (;;) {
        const int r = ::poll(&p, 1, poll_ms(timeout_ns));
        if (r < 0) { if (oke::interrupted()) continue; return -1; }
        return r > 0 ? 1 : 0;
    }
}

}  // namespace

extern "C" {

kal_intptr kal_timeout_read(kal_stream s, void* buf, kal_uintptr len,
                            kal_u64 timeout_ns) {
    const int fd = static_cast<int>(s.h);
    const int w = wait_for(fd, POLLIN, timeout_ns);
    if (w < 0) return oke::fail();
    if (w == 0) return -kal_err_again;
    for (;;) {
        const ssize_t r = ::read(fd, buf, len);
        if (r < 0) { if (oke::interrupted()) continue; return oke::fail(); }
        return static_cast<kal_intptr>(r);
    }
}

// THE LIMIT APPLIES TO THE WAIT AND NOT TO THE TRANSFER, and that is why this
// does not loop over a partial write. Once the descriptor is writable the
// write is attempted once and whatever it moved is reported; a loop would make
// the total time unbounded, which is the one thing a caller passing a timeout
// asked not to happen.
kal_intptr kal_timeout_write(kal_stream s, const void* buf, kal_uintptr len,
                             kal_u64 timeout_ns) {
    const int fd = static_cast<int>(s.h);
    const int w = wait_for(fd, POLLOUT, timeout_ns);
    if (w < 0) return oke::fail();
    if (w == 0) return -kal_err_again;
    for (;;) {
        const ssize_t r = ::write(fd, buf, len);
        if (r < 0) { if (oke::interrupted()) continue; return oke::fail(); }
        return static_cast<kal_intptr>(r);
    }
}

int kal_timeout_accept(kal_net_listener l, kal_u64 timeout_ns,
                       kal_net_conn* out) {
    if (out == nullptr) return kal_err_invalid;
    const int fd = oke::unpack(l.h);
    if (fd < 0) return kal_err_invalid;
    const int w = wait_for(fd, POLLIN, timeout_ns);
    if (w < 0) return oke::last();
    if (w == 0) return kal_err_again;
    for (;;) {
        const int c = ::accept(fd, nullptr, nullptr);
        if (c >= 0) { out->h = oke::pack(c); return kal_ok; }
        if (oke::interrupted()) continue;
        return oke::last();
    }
}

kal_intptr kal_timeout_recv_from(kal_datagram d, void* buf, kal_uintptr len,
                                 kal_endpoint* from, kal_u64 timeout_ns) {
    const int fd = oke::unpack(d.h);
    if (fd < 0) return -kal_err_invalid;
    const int w = wait_for(fd, POLLIN, timeout_ns);
    if (w < 0) return oke::fail();
    if (w == 0) return -kal_err_again;
    // The source translation lives in datagram.cpp and is reached through the
    // interface rather than copied: `kal_datagram_recv_from` will not block
    // now that the descriptor is readable, which is what the wait above
    // established.
    return kal_datagram_recv_from(d, buf, len, from);
}

// THE ONE MEMBER OF THIS INTERFACE WHOSE SUBJECT DOES NOT EXIST HERE.
//
// `openkal.process` is NOT provided by this implementation: there is no fork
// and no exec on this platform, so a program that calls `kal_process_spawn`
// fails at link naming the symbol, which is clause 6.2's second time.
//
// But an interface is provided in whole or not at all, and this function is a
// member of `openkal.timeout`, so the symbol must exist. What it does is
// decided by the same reasoning: no `kal_process` can be validly obtained on
// this platform, therefore EVERY handle it is given is not a valid one, and
// "the handle is not valid" is the true answer rather than a manufactured one.
//
// That is the distinction the specification draws. A function that is present
// and always fails is a defect BECAUSE THE CALLER CANNOT TELL -- it cannot
// distinguish a refusal from a condition. Here it can and it already has: the
// program that would call this could not have linked if it had obtained a
// process handle, so reaching this line at all means the caller invented one.
int kal_timeout_wait_process(kal_process, kal_u64, int*, int*) {
    return kal_err_invalid;
}

// THE WAIT'S OWN GRANULARITY, WHICH IS `poll`'s AND NOT THE CLOCK'S.
//
// `poll` takes a count of milliseconds, so no finer limit can be expressed
// however fine the clock is -- and openkal.time's granularity answers a
// different question about a different facility. Reporting the clock's answer
// here would tell a caller it could ask for a microsecond.
kal_u64 kal_timeout_granularity(void) { return kNsPerMs; }

}
