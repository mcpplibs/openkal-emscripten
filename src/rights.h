// A directory handle with less (openkal 0.23, `kal_fs_dir_attenuate',
// SPEC.md clause 7.16): what a handle may not do, what its holder stated, and
// what it records.
//
// A table indexed by descriptor, consulted by every operation of openkal.fs that
// takes a directory: the table of the other implementations, over the C library
// this one is written on, and without what a start conveys --- a program here
// starts none. It is empty in a program that attenuates nothing, and the one
// test of `g_any' is then all an operation pays.
#pragma once
#include "sys.h"

namespace oke::rights {

inline constexpr unsigned read_only  = 1u;   // KAL_DIR_READ_ONLY
inline constexpr unsigned unchanging = 2u;   // KAL_DIR_UNCHANGING
inline constexpr unsigned recorded   = 4u;   // KAL_DIR_RECORDED

extern volatile bool g_any;

// The rights a directory descriptor carries; zero for one that carries none.
unsigned of(int fd);

// A new descriptor for the directory `fd' names, carrying the rights of `fd'
// and `add'; `record' is the holder's file for `recorded'. Returns a kal_err
// value, and writes the new descriptor to `out' on kal_ok.
int attenuate(int fd, unsigned add, int record, int* out);

// A directory `child' opened beneath `parent' by `name': it carries what the
// parent carries. Returns false when the table has no room, and the caller then
// refuses the open rather than hand out a directory with more than its base.
bool derive(int parent, int child, const char* name, oke::uptr len);

// The descriptor is being closed.
void forget(int fd);

// What a name reached beneath `fd' was found to be, for a recorded handle:
//   'r' read, 'w' written, 's' asked about and present, 'a' absent, 'l' listed.
// A name of length zero is the directory itself.
void note(int fd, char kind, const char* name, oke::uptr len);

// Whether a handle that states nothing changes beneath it knows `name' to be
// absent, from what it has listed --- without asking the file system.
bool known_absent(int fd, const char* name, oke::uptr len);

// Something beneath was changed through a handle of this tree: what was listed
// may no longer hold.
void changed(int fd);

// The program is ending: every record is written, and ended with `e'.
void end_of_program();

}  // namespace oke::rights
