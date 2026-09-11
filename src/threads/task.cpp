#include "../sys.h"
#include <openkal/task.h>

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

// `__EMSCRIPTEN_PTHREADS__` IS THE COMPILER'S OWN ANSWER, and it is used here
// in preference to a name this package could have defined for itself. emcc
// defines it exactly when `-pthread` was passed, so there is no second word
// that can disagree with the compilation -- which is the failure mode a
// package-invented `OKE_THREADS` would have: a manifest that set the define
// and not the flag, or the reverse, and a capability word that then lied.
#include <emscripten/threading.h>

// THREADS ARE A LINK-TIME DECISION ON THIS PLATFORM, WHICH NO OTHER
// IMPLEMENTATION HAS TO SAY.
//
// Emscripten compiles `pthread_create` either way. Whether it can create
// anything depends on `-pthread`, which selects a different C library build,
// a different memory model (a SharedArrayBuffer) and a different loader
// contract (a worker pool the host must be able to spawn). So the capability
// is not a property of the machine this runs on and cannot be discovered at
// run time by anything this package can ask.
//
// The `threads` feature is therefore where the decision is made -- beside the
// rest of the link -- and `kal_task_props` reports the consequence. The
// interface is PROVIDED either way: clause 6.2's third time is a capability
// word read before use, and withholding KAL_TASK_PROP_PARALLEL while
// `kal_task_start` reports kal_err_not_supported is that mechanism. Providing
// a `kal_task_start` that silently ran the entry point on the calling context
// would be worse than either: a caller that asked for concurrency and got
// sequence has been given a program that deadlocks the first time two tasks
// wait on each other.
#if defined(__EMSCRIPTEN_PTHREADS__)

extern "C" {

// THIS FILE DEFINES THE TASK INTERFACE OR IT DEFINES NOTHING, and which one is
// decided by whether `-pthread` was passed -- not by a switch inside the
// functions.
//
// Clause 6.1: an implementation provides an interface IN WHOLE OR NOT AT ALL,
// and the specification names "present and always fails" as a defect because
// the caller cannot tell it from a condition. Measured 2026-09-11, with the
// eight symbols present and `kal_task_start` reporting kal_err_not_supported:
//
//   DID NOT HOLD  an execution context starts
//   DID NOT HOLD  four execution contexts start
//   DID NOT HOLD  contexts that ran at the same time have identities distinct
//
// The suite is right. So without the switch this translation unit is empty,
// the eight `kal_task_*` symbols do not exist, a program that uses one fails
// at LINK naming it, and `kal_interfaces()` -- keyed on the same
// `__EMSCRIPTEN_PTHREADS__` -- does not claim the interface. That is clause
// 6.2's second time, and it is the treatment `openkal.process`,
// `openkal.exec` and `openkal.space` get on this platform for the same
// reason: the facility is not there.
//
// AN EMPTY TRANSLATION UNIT RATHER THAN A REFUSAL TO COMPILE. `#error` was
// tried and is wrong: `mcpp build` inside this package, with no feature
// selected, is a legitimate build of the twelve interfaces that do not need
// the switch, and it failed.

namespace {
// The handle is the pthread_t, packed nowhere: `pthread_t` on this platform is
// a pointer, and openkal's word is pointer-sized. handle.h is for descriptors.
static_assert(sizeof(pthread_t) <= sizeof(kal_uintptr),
              "a task handle must fit in one word");

struct Start { void (*entry)(void*); void* arg; };

void* trampoline(void* p) {
    auto* s = static_cast<Start*>(p);
    void (*entry)(void*) = s->entry;
    void* arg = s->arg;
    ::free(s);
    entry(arg);
    return nullptr;
}
}  // namespace

int kal_task_start(void (*entry)(void*), void* arg, kal_task* out) {
    if (entry == nullptr || out == nullptr) return kal_err_invalid;
    // THE ARGUMENT PAIR OUTLIVES THIS CALL, so it cannot be on this stack.
    // Freed by the new context rather than by this one, which is the only
    // arrangement that needs no synchronisation between them.
    auto* s = static_cast<Start*>(::malloc(sizeof(Start)));
    if (s == nullptr) return kal_err_no_memory;
    s->entry = entry;
    s->arg = arg;
    pthread_t th{};
    const int rc = ::pthread_create(&th, nullptr, &trampoline, s);
    if (rc != 0) { ::free(s); errno = rc; return oke::last(); }
    out->h = reinterpret_cast<kal_uintptr>(th);
    return kal_ok;
}

int kal_task_join(kal_task t) {
    if (t.h == 0) return kal_err_invalid;
    const int rc = ::pthread_join(reinterpret_cast<pthread_t>(t.h), nullptr);
    if (rc != 0) { errno = rc; return oke::last(); }
    return kal_ok;
}

kal_uintptr kal_task_parallelism(void) {
    const int n = ::emscripten_num_logical_cores();
    return n > 0 ? static_cast<kal_uintptr>(n) : 1u;
}


// Yielding is a real operation here: a worker gives way to the host's
// scheduler.
void kal_task_yield(void) { ::sched_yield(); }

// THE IDENTITY IS THE CONTEXT'S OWN AND IS NOT AN INDEX. A caller may compare
// two answers and may not derive anything else from one, which is what
// `pthread_self` gives; in a single-context link it is a constant, and a
// constant is a correct identity when there is one context.
kal_uintptr kal_task_current(void) {
    return reinterpret_cast<kal_uintptr>(::pthread_self());
}

// SUSPENSION ON A WORD, AND ON THIS PLATFORM IT IS THE MACHINE'S OWN
// INSTRUCTION RATHER THAN A LIBRARY CALL.
//
// wasm has `memory.atomic.wait32` and `memory.atomic.notify`, which Emscripten
// exposes as `emscripten_atomic_wait_u32` and `..._notify`. They are the exact
// shape openkal names -- compare a word, sleep until it changes or a timeout
// elapses -- so this is the one place in this implementation where the
// forwarding goes to the platform rather than to its C library.
//
// THE WAIT INSTRUCTION IS FORBIDDEN ON A BROWSER'S MAIN THREAD, which the
// specification has no word for and which is why KAL_TASK_PROP_PREEMPTIVE is
// withheld: the host will trap rather than block. A caller in that position
// has no way to suspend at all, and saying so through the property word is
// better than a call that terminates the program.
int kal_task_wait(const kal_u32* word, kal_u32 expected, kal_u64 timeout_ns) {
    if (word == nullptr) return kal_err_invalid;
    // A negative timeout means "no limit" to the intrinsic; openkal spells
    // that as zero, so the two vocabularies are translated rather than passed
    // through.
    const double ms = timeout_ns == 0
        ? -1.0
        : static_cast<double>(timeout_ns) / 1000000.0;
    const auto r = ::emscripten_atomic_wait_u32(
        const_cast<kal_u32*>(word), expected, ms);
    switch (r) {
    case ATOMICS_WAIT_OK:            return kal_ok;
    case ATOMICS_WAIT_NOT_EQUAL:     return kal_ok;  // the word already moved
    case ATOMICS_WAIT_TIMED_OUT:     return kal_err_again;
    default:                         return kal_err_not_supported;
    }
}

int kal_task_wake(const kal_u32* word, kal_uintptr count, kal_uintptr* woken) {
    if (word == nullptr) return kal_err_invalid;
    const int n = ::emscripten_atomic_notify(
        const_cast<kal_u32*>(word),
        count == 0 ? INT32_MAX : static_cast<int>(count));
    if (n < 0) return kal_err_not_supported;
    if (woken != nullptr) *woken = static_cast<kal_uintptr>(n);
    return kal_ok;
}

kal_uintptr kal_task_props(void) {
    // PARALLEL and WAIT_TIMEOUT are claimed because the wasm atomics
    // intrinsics above answer them. THREAD_LOCAL is claimed because
    // `-pthread` makes `thread_local` real on this platform.
    //
    // PREEMPTIVE IS WITHHELD, and that is the platform's sharpest departure:
    // a worker is preempted by the host's scheduler, but a browser's MAIN
    // thread is not -- it runs to the end of its task, and the wait
    // instruction traps there rather than blocking. A caller cannot be told
    // which of the two it is in, so the weaker claim is the true one.
    return KAL_TASK_PROP_PARALLEL | KAL_TASK_PROP_WAIT_TIMEOUT
         | KAL_TASK_PROP_THREAD_LOCAL;

}

}

#endif  // __EMSCRIPTEN_PTHREADS__
