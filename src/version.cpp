#include "sys.h"
#include <openkal/version.h>

// What this implementation says about itself before it is used. Both answers
// are constants; openkal/version.h states why they belong to no interface.
extern "C" {

kal_u64 kal_version(void) { return KAL_VERSION; }

// THE WORD IS WRITTEN OUT, AND FOR THIS IMPLEMENTATION THAT MATTERS MORE THAN
// FOR THE OTHERS.
//
// A word derived from what happens to be linked would report a facility as
// present when the linker had merely kept it -- and here three interfaces are
// deliberately ABSENT, so a derived word would be wrong in the one direction
// that misleads a caller: it would claim `process` because some unrelated
// symbol pulled in a translation unit.
//
// WHAT IS NOT HERE, AND WHY THE ABSENCE IS THE REPORT:
//
//   PROCESS   there is no fork and no exec. A program is a module the host
//             instantiated; nothing in this environment starts a second one
//             and waits for it.
//   EXEC      there is no way to publish bytes as executable code. wasm has
//             no writable-then-executable memory at all: a module is
//             instantiated by the host from bytes it validates, which is a
//             different operation with a different owner.
//   SPACE     there is no second address space. A module has one linear
//             memory and cannot obtain another.
//
// A program that uses one of the seventeen names in those three groups fails
// AT LINK naming the symbol, which is clause 6.2's second time and is the
// mechanism rather than a defect. Providing them so that they return an error
// would be the shape clause 6.2 forbids -- present and always failing, which
// the caller cannot tell from a condition -- and it would also move a fact
// known at link time to run time.
//   TASK      only in a `-pthread` link, and the word follows the link rather
//             than being written down. Without it `src/threads/task.cpp` is
//             not compiled at all, so the eight `kal_task_*` symbols are
//             absent and the same mechanism reports them -- an interface is
//             provided in whole or not at all, and a `kal_task_start` that
//             could not start anything is the shape clause 6.2 forbids.
//
// `__EMSCRIPTEN_PTHREADS__` IS THE COMPILER'S OWN ANSWER, which is why it is
// read here in preference to a name this package could have defined: emcc
// defines it exactly when `-pthread` was passed, so the word cannot disagree
// with what was linked.
kal_u64 kal_interfaces(void) {
    kal_u64 w = KAL_IFACE_ABORT    | KAL_IFACE_STREAM   | KAL_IFACE_MEMORY
              | KAL_IFACE_ENV      | KAL_IFACE_TIME     | KAL_IFACE_RANDOM
              | KAL_IFACE_FS       | KAL_IFACE_TERMINAL
              | KAL_IFACE_NET      | KAL_IFACE_DATAGRAM | KAL_IFACE_TIMEOUT;
#if defined(__EMSCRIPTEN_PTHREADS__)
    w |= KAL_IFACE_TASK;
#endif
    return w;
}

}
