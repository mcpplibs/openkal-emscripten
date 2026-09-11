#include "sys.h"
#include <openkal/time.h>

#include <time.h>
#include <unistd.h>

namespace {
constexpr kal_u64 kNsPerSecond = 1000000000ull;
}

extern "C" {

kal_duration kal_time_monotonic(void) {
    struct timespec ts{};
    if (::clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return static_cast<kal_u64>(ts.tv_sec) * kNsPerSecond
         + static_cast<kal_u64>(ts.tv_nsec);
}

kal_duration kal_time_wall(void) {
    struct timespec ts{};
    if (::clock_gettime(CLOCK_REALTIME, &ts) != 0) return 0;
    return static_cast<kal_u64>(ts.tv_sec) * kNsPerSecond
         + static_cast<kal_u64>(ts.tv_nsec);
}

// THE GRANULARITY IS THE HOST'S AND IT IS COARSE, WHICH IS WORTH REPORTING
// TRUTHFULLY RATHER THAN OPTIMISTICALLY.
//
// `clock_getres` answers one nanosecond on this platform, and that answer is
// about the STRUCT and not about the clock: Emscripten's monotonic clock is
// `performance.now()`, whose resolution a browser deliberately reduces -- to
// a millisecond, or coarser still, as a defence against timing attacks. Under
// node it is finer.
//
// So the honest answer is the one the host actually produces, measured rather
// than asked for: the smallest non-zero difference between two consecutive
// readings. Measured once, because it is a property of the host and not of the
// moment, and bounded so that a host which returns a constant does not spin.
kal_duration kal_time_monotonic_granularity(void) {
    static kal_u64 cached = 0;
    if (cached != 0) return cached;
    const kal_u64 first = kal_time_monotonic();
    kal_u64 best = 0;
    for (int i = 0; i < 100000 && best == 0; ++i) {
        const kal_u64 now = kal_time_monotonic();
        if (now > first) best = now - first;
    }
    // A host that never advanced within the loop is reported as the coarsest
    // thing this implementation is willing to claim rather than as zero: zero
    // would mean "infinitely fine", which is the opposite of what was
    // observed.
    cached = best != 0 ? best : kNsPerSecond / 1000;
    return cached;
}

// SLEEPING IS THE ONE OPERATION THIS PLATFORM MAY NOT BE ABLE TO PERFORM AT
// ALL, AND THE PROPERTY WORD IS WHERE THAT IS SAID.
//
// `nanosleep` is a real call under node and in a pthreads worker. On a
// browser's main thread it cannot block -- the event loop is the thing that
// would have to run -- and Emscripten's implementation there either busy-waits
// or returns immediately depending on how the module was built.
//
// `KAL_TIME_PROP_SLEEP_PRECISE` is therefore withheld unconditionally rather
// than claimed and sometimes true. A caller that needs precision reads the
// word; a caller that needs a delay gets one.
void kal_time_sleep(kal_duration ns) {
    struct timespec req{};
    req.tv_sec  = static_cast<time_t>(ns / kNsPerSecond);
    req.tv_nsec = static_cast<long>(ns % kNsPerSecond);
    while (::nanosleep(&req, &req) != 0 && oke::interrupted()) { }
}

kal_uintptr kal_time_props(void) {
    // The wall clock exists -- `Date.now()` is always there -- and the
    // monotonic clock does not advance across a suspension, because
    // `performance.now()` is measured from the document's origin and a
    // suspended page's timers do not run.
    return KAL_TIME_PROP_WALL_AVAILABLE;
}

}
