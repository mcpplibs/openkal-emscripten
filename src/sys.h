// The one place this implementation says what it is written on.
//
// Every other openkal implementation has a file of this name holding a
// system-call stub: a register discipline, a trap instruction, and a table of
// numbers. This one holds none of that, because Emscripten has no kernel to
// issue a call to. What it has is a C library over a JavaScript host, and this
// file is the seam between openkal's vocabulary and that library's.
//
// WHICH DIRECTION THIS SITS IN IS THE WHOLE DIFFERENCE. `openkal-linux` is
// written BENEATH a C library, so it may not call one: every name it uses
// would be a name the C library above it defines. This package is written
// ABOVE one, which the specification permits in as many words (clause 2), and
// the consequence is that the code is thin -- a forward and an error
// translation -- rather than that it is easy: what an above-libc
// implementation must get right is exactly the places where the C library's
// vocabulary and openkal's DO NOT correspond, and those are marked
// individually below and in each interface.
#pragma once

#include <errno.h>
#include <openkal/types.h>

namespace oke {

using uptr = kal_uintptr;
using iptr = kal_intptr;

// ERRNO IS A C LIBRARY'S VOCABULARY AND NOT A KERNEL'S, which matters for one
// reason: it is not a fixed numbering that this file may hardcode. The other
// implementations translate numbers they issued the call with; this one
// translates the macros the library it was compiled against defines, so a
// future Emscripten renumbering is not a silent mistranslation.
//
// EVERY UNLISTED CONDITION BECOMES kal_err_io AND THAT IS A DECISION. The
// alternative -- a generic "unknown" -- would oblige every caller to handle a
// condition openkal does not define. `kal_err_io` says the medium or the
// device reported a failure, which is what an unclassified failure from a
// library over a host actually is.
inline int translate(int e) {
    switch (e) {
    case 0:            return kal_ok;
    case EINVAL:
    case EBADF:
    case EFAULT:
    case ENAMETOOLONG: return kal_err_invalid;
    case EAGAIN:
#if defined(EWOULDBLOCK) && EWOULDBLOCK != EAGAIN
    case EWOULDBLOCK:
#endif
    case EINPROGRESS:  return kal_err_again;
    case ENOMEM:       return kal_err_no_memory;
    case ENOSPC:
    case EDQUOT:
    case EFBIG:        return kal_err_no_space;
    case EACCES:
    case EPERM:
    case EROFS:        return kal_err_permission;
    case ENOSYS:
    case ENOTSUP:
#if defined(EOPNOTSUPP) && EOPNOTSUPP != ENOTSUP
    case EOPNOTSUPP:
#endif
    case EAFNOSUPPORT:
    case EPROTONOSUPPORT: return kal_err_not_supported;
    case EPIPE:
    case ECONNRESET:
    case ENOTCONN:
    case ESHUTDOWN:    return kal_err_closed;
    case ENOENT:       return kal_err_not_found;
    case EEXIST:       return kal_err_exists;
    case ENOTEMPTY:    return kal_err_not_empty;
    case EISDIR:       return kal_err_is_directory;
    case ENOTDIR:      return kal_err_not_directory;
    default:           return kal_err_io;
    }
}

// The condition of the call that just failed, as an openkal answer.
//
// READ ONCE. `errno` is a modifiable lvalue that later calls overwrite,
// including calls this implementation makes while composing a diagnostic, so
// every site captures it in the same statement that observed the failure.
inline int last() { return translate(errno); }

// A NEGATIVE CONDITION IS HOW THE COUNT-RETURNING OPERATIONS REPORT.
// `kal_stream_read` and its neighbours return one signed word: a count, or the
// negated condition when no byte moved. This is that negation, so no site
// writes the minus sign itself and none of them can forget it.
inline iptr fail() { return -static_cast<iptr>(last()); }

// AN INTERRUPTED CALL IS RETRIED RATHER THAN REPORTED (clause 7.5). A caller
// cannot distinguish this condition from a genuine failure without knowledge
// of the environment, and an implementation that reports it produces short
// reads on any host that delivers signals.
//
// It is kept even though Emscripten's single-threaded host delivers none: the
// pthreads build does, and a retry loop that is correct on both is cheaper
// than a claim about which build this is.
inline bool interrupted() { return errno == EINTR; }

}  // namespace oke
