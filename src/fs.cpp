#include "sys.h"
#include "handle.h"
#include "rights.h"
#include <openkal/fs.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>     // offsetof
#include <stdio.h>      // renameat, which POSIX places here and not in <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sched.h>
#include <unistd.h>

// THE FILE SYSTEM IS THE INTERFACE WHERE THIS PLATFORM'S VOCABULARY AND
// openkal's DIVERGE MOST, so the divergences are named here rather than at
// twenty-five call sites.
//
// 1. openkal HAS NO AMBIENT WORKING DIRECTORY. Every name is resolved against
//    a `kal_dir`, which is what makes a program's reach a property of the
//    handles it was given. POSIX has `openat` and Emscripten implements it, so
//    the correspondence is exact and no name is ever passed to a call that
//    would resolve it against a hidden location.
//
// 2. THE ROOT PREOPEN IS THIS IMPLEMENTATION'S ONE POLICY DECISION. A program
//    has to start with at least one directory or it can reach nothing, and on
//    a platform with no command line there is no one to tell it which. MEMFS
//    mounts at `/`, so `/` is what is offered -- one preopen, named `/`.
//    A wasm module's file system is its own, so this grants no reach outside
//    the module.
//
// 3. NAMES CARRY LENGTHS AND POSIX CALLS TAKE NUL. Every entry point that
//    receives a name copies it into a bounded buffer. `kal_fs_max_name`
//    reports that bound, so a caller can know it before it is refused rather
//    than after.
namespace {

constexpr kal_uintptr kMaxName = 1024;

// THE RIGHTS OF A DIRECTORY HANDLE (0.23, rights.h): an operation that would
// change something beneath a read-only handle is refused before it is asked of
// the file system; one that did change something is recorded, and what a handle
// that states nothing changes has listed no longer holds.
inline bool refused_write(int b) { return (oke::rights::of(b) & oke::rights::read_only) != 0; }
inline void wrote(int b, const char* n, kal_uintptr l) {
    if (!oke::rights::g_any) return;
    oke::rights::changed(b);
    oke::rights::note(b, 'w', n, l);
}
inline void asked(int b, bool present, const char* n, kal_uintptr l) {
    if (oke::rights::g_any) oke::rights::note(b, present ? 's' : 'a', n, l);
}
inline bool absence(int e) { return e == ENOENT || e == ENOTDIR; }

// A name copied for a call that wants NUL termination. Returns false when the
// name does not fit, which the caller reports as kal_err_invalid -- the same
// answer POSIX gives for ENAMETOOLONG.
bool cname(const char* name, kal_uintptr len, char (&out)[kMaxName + 1]) {
    if (name == nullptr || len > kMaxName) return false;
    ::memcpy(out, name, static_cast<size_t>(len));
    out[len] = '\0';
    return true;
}

int kind_of(mode_t m) {
    if (S_ISREG(m))  return kal_node_file;
    if (S_ISDIR(m))  return kal_node_directory;
    if (S_ISLNK(m))  return kal_node_link;
    return kal_node_other;
}

constexpr kal_u64 kNsPerSecond = 1000000000ull;

// `self_size` IS THE CALLER'S STRUCTURE AND NOT THIS ONE, WHICH IS WHY EVERY
// WRITE BELOW IS BOUNDED BY IT.
//
// A consumer built against a later revision of this structure may ask an
// earlier implementation, and the reverse: a consumer built against an EARLIER
// revision asks THIS one, whose `sizeof` is larger. Writing every field would
// then write past the end of the caller's object.
//
// Measured 2026-09-11 by the conformance suite, which passes a deliberately
// short `self_size` and checks the bytes after it:
//
//   DID NOT HOLD  an enquiry writes no more of the structure than the caller
//                 stated
//
// `offsetof` plus the field's own width is the bound, taken per field rather
// than as one threshold, because the fields are not written in address order
// and a single cut-off would be a second statement of the layout.
template <typename T>
bool room_for(const kal_node_info* out, unsigned long offset) {
    return out->self_size >= offset + sizeof(T);
}

void fill(const struct stat& st, kal_u32 wanted, kal_node_info* out) {
    // `present` is itself a field of the caller's structure, and a caller whose
    // structure is too short to hold it asked a question this enquiry cannot
    // answer at all.
    if (!room_for<kal_u32>(out, offsetof(kal_node_info, present))) return;
    out->present = 0;
    if ((wanted & KAL_INFO_KIND)
        && room_for<int>(out, offsetof(kal_node_info, kind))) {
        out->kind = kind_of(st.st_mode);
        out->present |= KAL_INFO_KIND;
    }
    if ((wanted & KAL_INFO_SIZE)
        && room_for<kal_u64>(out, offsetof(kal_node_info, size))) {
        out->size = static_cast<kal_u64>(st.st_size);
        out->present |= KAL_INFO_SIZE;
    }
    if ((wanted & KAL_INFO_MODIFIED)
        && room_for<kal_u64>(out, offsetof(kal_node_info, modified_ns))) {
        out->modified_ns = static_cast<kal_u64>(st.st_mtime) * kNsPerSecond
#if defined(st_mtime)
                         + static_cast<kal_u64>(st.st_mtim.tv_nsec)
#endif
            ;
        out->present |= KAL_INFO_MODIFIED;
    }
    if ((wanted & KAL_INFO_WRITABLE)
        && room_for<int>(out, offsetof(kal_node_info, writable))) {
        // THE MODE BITS AND NOT AN `access` CALL. `access` answers for the
        // process's identity, and MEMFS has one identity; the mode is what the
        // file records, which is the property openkal names.
        out->writable = (st.st_mode & S_IWUSR) != 0 ? 1 : 0;
        out->present |= KAL_INFO_WRITABLE;
    }
    if ((wanted & KAL_INFO_IDENTITY)
        && room_for<kal_u64[2]>(out, offsetof(kal_node_info, identity))) {
        out->identity[0] = static_cast<kal_u64>(st.st_dev);
        out->identity[1] = static_cast<kal_u64>(st.st_ino);
        out->present |= KAL_INFO_IDENTITY;
    }
    // Version 0.13. MEMFS keeps the mode a node was given, so whether a node
    // may be started is recorded, and it is reported for a file only.
    if ((wanted & KAL_INFO_EXECUTABLE) && S_ISREG(st.st_mode)
        && room_for<int>(out, offsetof(kal_node_info, executable))) {
        out->executable = (st.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) != 0 ? 1 : 0;
        out->present |= KAL_INFO_EXECUTABLE;
    }
}

// The iteration word openkal hands back and forth is a pointer to the C
// library's own directory object. That is legitimate -- clause 7.1 requires a
// released handle not to be honoured, and the iterator is released by the same
// `closedir` that ends the walk -- and it is NOT put through handle.h, because
// a DIR* is not a descriptor and packing it would be a second discipline on a
// word this file already owns.
// AN ITERATOR IS A LISTING, AND THE LISTINGS NOT YET READ TO THE END ARE KEPT
// WITH THEIR DIRECTORY, BECAUSE CLOSING THE DIRECTORY MUST END THEM. fs.h
// releases an iterator "by reading past the end or by closing the directory
// that produced it", and the walk holds a duplicate of the directory's
// descriptor, so closing the directory's descriptor does not reach it.
// Measured on the Linux implementation, which had the same shape
// (mcpplibs/openkal-linux#34): a C library above that stopped reading a
// directory early kept a descriptor for every directory it closed.
//
// The lock spins rather than suspends because the build without threads has
// nothing to suspend upon, and the build with them needs nothing more.
struct listing {
    DIR*     dir;
    int      of;       // the descriptor of the directory it was begun upon
    listing* next;
    int      busy;     // a context is inside kal_fs_list_next with it
    int      doomed;   // its directory was closed while it was busy
};

listing*     g_listings = nullptr;
volatile int g_listings_held = 0;

void listings_lock() {
    while (__atomic_exchange_n(&g_listings_held, 1, __ATOMIC_ACQUIRE)) ::sched_yield();
}
void listings_unlock() { __atomic_store_n(&g_listings_held, 0, __ATOMIC_RELEASE); }

void listing_release(listing* s) { ::closedir(s->dir); ::free(s); }

// Ends every listing begun upon `of'. A listing another context is reading is
// marked rather than released, and that context releases it on its way out
// (clause 6.6).
void listings_end(int of) {
    listing* done = nullptr;
    listings_lock();
    for (listing** p = &g_listings; *p != nullptr;) {
        listing* s = *p;
        if (s->of != of || s->doomed) { p = &s->next; continue; }
        if (s->busy) { s->doomed = 1; p = &s->next; continue; }
        *p = s->next; s->next = done; done = s;
    }
    listings_unlock();
    while (done != nullptr) { listing* s = done; done = s->next; listing_release(s); }
}

// Takes `s' for the calling context, or reports that it is not an open listing
// of `of'. The word is found among the open listings rather than read through.
bool listing_take(listing* s, int of) {
    for (;;) {
        listings_lock();
        listing* p = g_listings;
        while (p != nullptr && p != s) p = p->next;
        if (p == nullptr || p->of != of || p->doomed) { listings_unlock(); return false; }
        if (!p->busy) { p->busy = 1; listings_unlock(); return true; }
        listings_unlock();
        ::sched_yield();
    }
}

void listing_give(listing* s, bool ended) {
    listings_lock();
    s->busy = 0;
    const bool release = ended || s->doomed;
    if (release) {
        for (listing** p = &g_listings; *p != nullptr; p = &(*p)->next)
            if (*p == s) { *p = s->next; break; }
    }
    listings_unlock();
    if (release) listing_release(s);
}

}  // namespace

extern "C" {

kal_uintptr kal_fs_max_name(void) { return kMaxName; }

kal_uintptr kal_fs_preopen_count(void) { return 1u; }

// ONE DIRECTORY FOR THE LIFE OF THE PROGRAM, as a preopen is (0.12.0). The
// root was opened again at each enquiry, so each answer was another handle:
// a walk begun on one and continued on the next was refused as a walk of
// another directory, and each enquiry kept a descriptor open.
int kal_fs_preopen(kal_uintptr index, kal_dir* out,
                   char* name_out, kal_uintptr name_cap,
                   kal_uintptr* name_len) {
    if (index != 0 || out == nullptr) return kal_err_invalid;
    static volatile int root = -1;
    int fd = __atomic_load_n(&root, __ATOMIC_ACQUIRE);
    if (fd < 0) {
        const int opened = ::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (opened < 0) return oke::last();
        int expected = -1;
        if (__atomic_compare_exchange_n(&root, &expected, opened, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) fd = opened;
        else { ::close(opened); fd = expected; }
    }
    out->h = oke::pack(fd);
    if (name_len != nullptr) *name_len = 1;
    if (name_out != nullptr && name_cap != 0) name_out[0] = '/';
    return kal_ok;
}

// WHAT THIS FILE SYSTEM CAN DO, AND EVERY ABSENT WORD IS A MEASUREMENT.
//
//   CASE_SENSITIVE   MEMFS compares bytes.
//   LINKS            symbolic links exist and are followed.
//   MODIFIED_TIME    a modification time is kept.
//   ATOMIC_RENAME    `rename` is a single operation on an in-memory tree.
//   MAKE_LINKS       `symlink` is implemented.
//   EXECUTABLE       the mode a node was given is kept, and `chmod` changes it.
//
//   REMOVE_OPEN      a name is removed at once although a file of that name is
//                    open: MEMFS unlinks the node from its directory and an open
//                    stream keeps the node (measured: the name is absent, the
//                    file reads back, and its directory is removed). 0.17.
//
// WITHHELD: LOCKS, because MEMFS has no lock table and a wasm module has no
// second process to contend with -- so a lock that always succeeded would tell
// a caller it had exclusivity it was never given. CAPACITY, because the answer
// would be the module's heap limit, which is not the quantity the caller is
// asking about. RENAME_NO_REPLACE, because the C library has no rename that
// refuses to replace (no `renameat2`, and `linkat` reports EMLINK on MEMFS,
// so the link-then-unlink form is not available either), and a test followed
// by a rename is two acts; kal_fs_rename_flags says so (below). LOCAL_ENDPOINT,
// because there is no socket bound to a name on this platform (net.cpp).
//   ATTENUATE        a handle with less (0.23, rights.h); READ_ONLY and
//                    UNCHANGING state what this handle carries.
//
// HARD_LINKS is withheld (0.25): `linkat` reports EMLINK on MEMFS, so no file
// receives a second name, and kal_fs_hard_link says so (below).
//
// LIST_INFO is withheld: the listing gives a name and a kind, and the rest of
// an entry is asked of it (kal_fs_list_next_info).
kal_uintptr kal_fs_props(kal_dir d) {
    const unsigned rights = oke::rights::of(oke::unpack(d.h));
    return KAL_FS_PROP_CASE_SENSITIVE | KAL_FS_PROP_LINKS
         | KAL_FS_PROP_MODIFIED_TIME  | KAL_FS_PROP_ATOMIC_RENAME
         | KAL_FS_PROP_MAKE_LINKS | KAL_FS_PROP_EXECUTABLE
         | KAL_FS_PROP_REMOVE_OPEN | KAL_FS_PROP_RESOLVED_NAME
         | KAL_FS_PROP_ATTENUATE
         | ((rights & oke::rights::read_only) ? KAL_FS_PROP_READ_ONLY : 0u)
         | ((rights & oke::rights::unchanging) ? KAL_FS_PROP_UNCHANGING : 0u);
}

int kal_fs_open_dir(kal_dir base, const char* name, kal_uintptr len,
                    kal_dir* out) {
    if (out == nullptr) return kal_err_invalid;
    const int b = oke::unpack(base.h);
    if (b < 0) return kal_err_invalid;
    char n[kMaxName + 1];
    if (!cname(name, len, n)) return kal_err_invalid;
    const unsigned rights = oke::rights::of(b);
    if ((rights & oke::rights::unchanging) && oke::rights::known_absent(b, n, len)) {
        asked(b, false, n, len);
        return kal_err_not_found;
    }
    const int fd = ::openat(b, n, O_RDONLY | O_DIRECTORY);
    if (fd < 0) {
        const int why = errno;
        if (rights && absence(why)) asked(b, false, n, len);
        return oke::translate(why);
    }
    // A directory opened beneath a handle with rights has them; where the table
    // has no room the directory is not handed out with more than its base.
    if (rights && !oke::rights::derive(b, fd, n, len)) { ::close(fd); return kal_err_no_memory; }
    if (rights) asked(b, true, n, len);
    out->h = oke::pack(fd);
    return kal_ok;
}

int kal_fs_open(kal_dir base, const char* name, kal_uintptr len,
                kal_uintptr flags, kal_file* out) {
    if (out == nullptr) return kal_err_invalid;
    const int b = oke::unpack(base.h);
    if (b < 0) return kal_err_invalid;
    char n[kMaxName + 1];
    if (!cname(name, len, n)) return kal_err_invalid;

    // THE ACCESS MODE IS NOT A BIT PAIR IN POSIX, which is the one place this
    // translation is not mechanical: O_RDONLY, O_WRONLY and O_RDWR are three
    // values of one field, while openkal's READ and WRITE are two independent
    // bits. Neither set is the whole request being made, so the mapping is
    // written out rather than or-ed.
    const bool r = (flags & KAL_OPEN_READ)  != 0;
    const bool w = (flags & KAL_OPEN_WRITE) != 0;
    int of = 0;
    if (r && w)      of = O_RDWR;
    else if (w)      of = O_WRONLY;
    else if (r)      of = O_RDONLY;
    else return kal_err_invalid;   // a request for neither names no operation

    if (flags & KAL_OPEN_CREATE)    of |= O_CREAT;
    if (flags & KAL_OPEN_EXCLUSIVE) of |= O_EXCL;
    if (flags & KAL_OPEN_TRUNCATE)  of |= O_TRUNC;
    if (flags & KAL_OPEN_APPEND)    of |= O_APPEND;

    const unsigned rights = oke::rights::of(b);
    const bool changes = (flags & (KAL_OPEN_WRITE | KAL_OPEN_CREATE | KAL_OPEN_TRUNCATE | KAL_OPEN_APPEND)) != 0;
    if (changes && (rights & oke::rights::read_only)) return kal_err_permission;
    if (!(flags & KAL_OPEN_CREATE) && (rights & oke::rights::unchanging) && oke::rights::known_absent(b, n, len)) {
        asked(b, false, n, len);
        return kal_err_not_found;
    }
    const int fd = ::openat(b, n, of, 0666);
    if (fd < 0) {
        const int why = errno;
        if (rights && absence(why)) asked(b, false, n, len);
        return oke::translate(why);
    }
    if (rights) {
        if (changes) wrote(b, n, len);
        else oke::rights::note(b, 'r', n, len);
    }
    out->h = oke::pack(fd);
    return kal_ok;
}

void kal_fs_close_dir(kal_dir d) {
    const int fd = oke::unpack(d.h);
    if (fd < 0) return;
    // Before the close, because after it the number may already name another
    // directory whose enumerations are not this one's.
    listings_end(fd);
    oke::rights::forget(fd);
    ::close(fd); oke::retire(d.h);
}

void kal_fs_close_file(kal_file f) {
    const int fd = oke::unpack(f.h);
    if (fd >= 0) { ::close(fd); oke::retire(f.h); }
}

// A STREAM OVER A FILE IS A BARE DESCRIPTOR, AND THE TWO DISCIPLINES MEET HERE.
//
// `kal_file` is owned and packed; `kal_stream` is borrowed and bare. This is
// the one function that converts between them, and it is the function
// `openkal-linux` got wrong in the mirror direction: a bare descriptor passed
// to the unpacking routine reads as a packed handle naming the descriptor
// BELOW it. Nothing in a machine word can tell the two apart, so the
// conversion exists exactly once and in this direction only.
kal_stream kal_fs_stream(kal_file f) {
    const int fd = oke::unpack(f.h);
    return kal_stream{fd < 0 ? ~static_cast<kal_uintptr>(0)
                             : static_cast<kal_uintptr>(fd)};
}

int kal_fs_seek(kal_file f, kal_i64 offset, int whence, kal_u64* result) {
    const int fd = oke::unpack(f.h);
    if (fd < 0) return kal_err_invalid;
    int w;
    switch (whence) {
    case KAL_SEEK_SET:     w = SEEK_SET; break;
    case KAL_SEEK_CURRENT: w = SEEK_CUR; break;
    case KAL_SEEK_END:     w = SEEK_END; break;
    default: return kal_err_invalid;
    }
    const off_t r = ::lseek(fd, static_cast<off_t>(offset), w);
    if (r < 0) return oke::last();
    if (result != nullptr) *result = static_cast<kal_u64>(r);
    return kal_ok;
}

int kal_fs_truncate(kal_file f, kal_u64 size) {
    const int fd = oke::unpack(f.h);
    if (fd < 0) return kal_err_invalid;
    if (::ftruncate(fd, static_cast<off_t>(size)) != 0) return oke::last();
    return kal_ok;
}

// TRANSFERS AT AN OFFSET (openkal 0.20): the C library's `pread' and `pwrite',
// which this platform's file system answers without the position (WASI's
// `fd_pread' and `fd_pwrite' where the program runs under WASI). The write is
// completed as a stream's is; an interruption is retried.
kal_intptr kal_fs_read_at(kal_file f, void* buf, kal_uintptr len, kal_u64 offset) {
    const int fd = oke::unpack(f.h);
    if (fd < 0) return -kal_err_invalid;
    for (;;) {
        const ssize_t r = ::pread(fd, buf, len, static_cast<off_t>(offset));
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) return -oke::last();
        return static_cast<kal_intptr>(r);
    }
}

kal_intptr kal_fs_write_at(kal_file f, const void* buf, kal_uintptr len, kal_u64 offset) {
    const int fd = oke::unpack(f.h);
    if (fd < 0) return -kal_err_invalid;
    const auto* p = static_cast<const unsigned char*>(buf);
    kal_uintptr done = 0;
    while (done < len) {
        const ssize_t r = ::pwrite(fd, p + done, len - done, static_cast<off_t>(offset + done));
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) return done != 0 ? static_cast<kal_intptr>(done) : -oke::last();
        if (r == 0) break;
        done += static_cast<kal_uintptr>(r);
    }
    return static_cast<kal_intptr>(done);
}

int kal_fs_info(kal_dir base, const char* name, kal_uintptr len,
                kal_uintptr flags, kal_u32 wanted, kal_node_info* out) {
    if (out == nullptr) return kal_err_invalid;
    const int b = oke::unpack(base.h);
    if (b < 0) return kal_err_invalid;
    char n[kMaxName + 1];
    if (!cname(name, len, n)) return kal_err_invalid;
    struct stat st{};
    const int at = (flags & KAL_FS_NO_RESOLVE) ? AT_SYMLINK_NOFOLLOW : 0;
    const unsigned rights = oke::rights::of(b);
    const bool known = (rights & oke::rights::unchanging) && oke::rights::known_absent(b, n, len);
    if (known) errno = ENOENT;
    if (known || ::fstatat(b, n, &st, at) != 0) {
        // ABSENCE IS AN ANSWER AND NOT A FAILURE, which is the property the
        // conformance suite observes by its effect. `kal_node_absent` with
        // kal_ok lets a caller ask "is this there" without an error path.
        if (errno == ENOENT || errno == ENOTDIR) {
            if (rights) asked(b, false, n, len);
            // BOUNDED FOR THE SAME REASON, and this is the branch a short
            // structure is most likely to reach: asking whether a name exists
            // is what a caller with a minimal structure asks.
            if (!room_for<kal_u32>(out, offsetof(kal_node_info, present)))
                return kal_ok;
            out->present = 0;
            if ((wanted & KAL_INFO_KIND)
                && room_for<int>(out, offsetof(kal_node_info, kind))) {
                out->kind = kal_node_absent;
                out->present |= KAL_INFO_KIND;
            }
            return kal_ok;
        }
        return oke::last();
    }
    if (rights) asked(b, true, n, len);
    fill(st, wanted, out);
    return kal_ok;
}

int kal_fs_file_info(kal_file f, kal_u32 wanted, kal_node_info* out) {
    if (out == nullptr) return kal_err_invalid;
    const int fd = oke::unpack(f.h);
    if (fd < 0) return kal_err_invalid;
    struct stat st{};
    if (::fstat(fd, &st) != 0) return oke::last();
    fill(st, wanted, out);
    return kal_ok;
}

int kal_fs_mkdir(kal_dir base, const char* name, kal_uintptr len) {
    const int b = oke::unpack(base.h);
    if (b < 0) return kal_err_invalid;
    char n[kMaxName + 1];
    if (!cname(name, len, n)) return kal_err_invalid;
    if (refused_write(b)) return kal_err_permission;
    if (::mkdirat(b, n, 0777) != 0) return oke::last();
    wrote(b, n, len);
    return kal_ok;
}

// ONE REMOVAL FOR BOTH KINDS, WHICH POSIX DOES NOT OFFER.
//
// openkal has one `remove`; POSIX has `unlink` for a file and `rmdir` for a
// directory, and using the wrong one reports EISDIR or ENOTDIR. The second
// call is made only when the first reported exactly that, so a genuine
// permission or not-found condition is never retried as the other kind.
int kal_fs_remove(kal_dir base, const char* name, kal_uintptr len) {
    const int b = oke::unpack(base.h);
    if (b < 0) return kal_err_invalid;
    char n[kMaxName + 1];
    if (!cname(name, len, n)) return kal_err_invalid;
    if (refused_write(b)) return kal_err_permission;
    if (::unlinkat(b, n, 0) == 0) { wrote(b, n, len); return kal_ok; }
    if (errno == EISDIR || errno == EPERM || errno == ENOTEMPTY) {
        const int first = errno;
        if (::unlinkat(b, n, AT_REMOVEDIR) == 0) { wrote(b, n, len); return kal_ok; }
        // The directory removal's own condition is the one reported: it is the
        // call that matched the object.
        if (errno == ENOTEMPTY) return kal_err_not_empty;
        if (errno == ENOTDIR)   return oke::translate(first);
        return oke::last();
    }
    return oke::last();
}

int kal_fs_rename(kal_dir from, const char* a, kal_uintptr alen,
                  kal_dir to, const char* b, kal_uintptr blen) {
    const int fd = oke::unpack(from.h);
    const int td = oke::unpack(to.h);
    if (fd < 0 || td < 0) return kal_err_invalid;
    char na[kMaxName + 1], nb[kMaxName + 1];
    if (!cname(a, alen, na) || !cname(b, blen, nb)) return kal_err_invalid;
    if (refused_write(fd) || refused_write(td)) return kal_err_permission;
    if (::renameat(fd, na, td, nb) != 0) return oke::last();
    wrote(fd, na, alen);
    wrote(td, nb, blen);
    return kal_ok;
}

// kal_fs_rename WITH A FLAGS WORD (version 0.17). A word of zero is the
// rename above. KAL_RENAME_NO_REPLACE is not claimed by kal_fs_props, so it is
// kal_err_not_supported and renames nothing, as clause 6.2 requires of a
// request upon a volume that does not claim it; the handles and names are
// checked first, so an invalid request is reported as one. A position the
// specification has not assigned is kal_err_invalid.
int kal_fs_rename_flags(kal_dir from, const char* a, kal_uintptr alen,
                        kal_dir to, const char* b, kal_uintptr blen,
                        kal_uintptr flags) {
    if ((flags & ~static_cast<kal_uintptr>(KAL_RENAME_NO_REPLACE)) != 0) return kal_err_invalid;
    if (flags == 0) return kal_fs_rename(from, a, alen, to, b, blen);
    const int fd = oke::unpack(from.h);
    const int td = oke::unpack(to.h);
    if (fd < 0 || td < 0) return kal_err_invalid;
    char na[kMaxName + 1], nb[kMaxName + 1];
    if (!cname(a, alen, na) || !cname(b, blen, nb)) return kal_err_invalid;
    return kal_err_not_supported;
}

int kal_fs_set_modified(kal_file f, kal_u64 modified_ns) {
    const int fd = oke::unpack(f.h);
    if (fd < 0) return kal_err_invalid;
    struct timespec ts[2];
    ts[0].tv_sec = 0; ts[0].tv_nsec = UTIME_OMIT;
    ts[1].tv_sec  = static_cast<time_t>(modified_ns / kNsPerSecond);
    ts[1].tv_nsec = static_cast<long>(modified_ns % kNsPerSecond);
    if (::futimens(fd, ts) != 0) return oke::last();
    return kal_ok;
}

int kal_fs_set_modified_at(kal_dir base, const char* name, kal_uintptr len,
                           kal_u64 modified_ns) {
    const int b = oke::unpack(base.h);
    if (b < 0) return kal_err_invalid;
    char n[kMaxName + 1];
    if (!cname(name, len, n)) return kal_err_invalid;
    struct timespec ts[2];
    ts[0].tv_sec = 0; ts[0].tv_nsec = UTIME_OMIT;
    ts[1].tv_sec  = static_cast<time_t>(modified_ns / kNsPerSecond);
    ts[1].tv_nsec = static_cast<long>(modified_ns % kNsPerSecond);
    if (refused_write(b)) return kal_err_permission;
    if (::utimensat(b, n, ts, 0) != 0) return oke::last();
    wrote(b, n, len);
    return kal_ok;
}

// Whether a node may be started, version 0.13. The mode keeps three bits for
// one property: a class that may read the node may start it, and clearing
// clears all three.
int kal_fs_set_executable_at(kal_dir base, const char* name, kal_uintptr len,
                             int executable) {
    const int b = oke::unpack(base.h);
    if (b < 0) return kal_err_invalid;
    char n[kMaxName + 1];
    if (!cname(name, len, n)) return kal_err_invalid;
    if (refused_write(b)) return kal_err_permission;
    struct stat st{};
    if (::fstatat(b, n, &st, 0) != 0) return oke::last();
    if (S_ISDIR(st.st_mode)) return kal_err_is_directory;
    if (!S_ISREG(st.st_mode)) return kal_err_invalid;
    const mode_t mode = st.st_mode & 07777;
    const mode_t next = executable != 0 ? (mode | ((mode & 0444) >> 2))
                                        : (mode & ~static_cast<mode_t>(0111));
    if (next == mode) return kal_ok;
    if (::fchmodat(b, n, next, 0) != 0) return oke::last();
    wrote(b, n, len);
    return kal_ok;
}

// LOCKS AND CAPACITY ARE NOT PROVIDED, AND THE PROPERTY WORD SAID SO FIRST.
//
// These two are the only members of this interface whose absence is reported
// by a capability word rather than by an absent symbol: they are declared by
// `openkal.fs`, which this implementation provides in whole, so the symbol
// must exist. `kal_fs_props` withholds KAL_FS_PROP_LOCKS and
// KAL_FS_PROP_CAPACITY, and a caller that reads the word before calling is
// told. A caller that does not is told here, by name, rather than being given
// an exclusivity that does not exist.
int kal_fs_lock(kal_file, kal_u64, kal_u64, kal_uintptr) {
    return kal_err_not_supported;
}

int kal_fs_unlock(kal_file, kal_u64, kal_u64) {
    return kal_err_not_supported;
}

int kal_fs_capacity(kal_dir, kal_u64*, kal_u64*) {
    return kal_err_not_supported;
}

int kal_fs_link_create(kal_dir base, const char* name, kal_uintptr len,
                       const char* target, kal_uintptr target_len,
                       kal_uintptr flags) {
    (void)flags;   // MEMFS has no directory-link distinction to select
    const int b = oke::unpack(base.h);
    if (b < 0) return kal_err_invalid;
    char n[kMaxName + 1], t[kMaxName + 1];
    if (!cname(name, len, n) || !cname(target, target_len, t))
        return kal_err_invalid;
    if (refused_write(b)) return kal_err_permission;
    if (::symlinkat(t, b, n) != 0) return oke::last();
    wrote(b, n, len);
    return kal_ok;
}

// A SECOND NAME FOR A FILE (openkal 0.25). MEMFS stores one name for each
// node (`linkat` reports EMLINK), so KAL_FS_PROP_HARD_LINKS is not claimed and
// the request is refused after its arguments are examined, as clause 6.2 allows
// for an operation a resource's property governs.
int kal_fs_hard_link(kal_dir from, const char* a, kal_uintptr alen,
                     kal_dir to,   const char* b, kal_uintptr blen) {
    char na[kMaxName + 1], nb[kMaxName + 1];
    if (oke::unpack(from.h) < 0 || oke::unpack(to.h) < 0
        || !cname(a, alen, na) || !cname(b, blen, nb))
        return kal_err_invalid;
    return kal_err_not_supported;
}

kal_intptr kal_fs_link_read(kal_dir base, const char* name, kal_uintptr len,
                            char* out, kal_uintptr cap) {
    const int b = oke::unpack(base.h);
    if (b < 0) return -kal_err_invalid;
    char n[kMaxName + 1];
    if (!cname(name, len, n)) return -kal_err_invalid;
    // THE FULL LENGTH IS REPORTED EVEN WHEN IT DOES NOT FIT, which `readlinkat`
    // cannot do: it returns what it wrote and gives no way to learn the rest.
    // So a buffer of this implementation's own is used and the copy is bounded
    // afterwards, which is what lets a caller size a buffer with one call.
    char buf[kMaxName + 1];
    const ssize_t r = ::readlinkat(b, n, buf, sizeof(buf));
    if (r < 0) return oke::fail();
    if (out != nullptr && cap != 0) {
        const kal_uintptr take =
            static_cast<kal_uintptr>(r) < cap ? static_cast<kal_uintptr>(r) : cap;
        ::memcpy(out, buf, static_cast<size_t>(take));
    }
    return static_cast<kal_intptr>(r);
}

// THE NAME WITH EVERY LINK FOLLOWED (openkal 0.23) is the one the file system
// keeps for a descriptor of it: opening follows every link, and Emscripten's
// FS records the path it reached, which `/proc/self/fd/N' reads back. One
// lookup, as the environment's own `realpath' of a file it opened is; the
// full length is reported when it does not fit, as kal_fs_link_read does.
kal_intptr kal_fs_resolved_name(kal_dir base, const char* name, kal_uintptr len,
                                char* out, kal_uintptr cap) {
    const int b = oke::unpack(base.h);
    if (b < 0) return -kal_err_invalid;
    char n[kMaxName + 1];
    if (!cname(name, len, n)) return -kal_err_invalid;
    const unsigned rights = oke::rights::of(b);
    if ((rights & oke::rights::unchanging) && oke::rights::known_absent(b, n, len)) {
        asked(b, false, n, len);
        return -kal_err_not_found;
    }
    const int fd = ::openat(b, n, O_RDONLY);
    if (fd < 0) {
        const int why = errno;
        if (rights && absence(why)) asked(b, false, n, len);
        return -oke::translate(why);
    }
    if (rights) asked(b, true, n, len);
    char link[32], buf[kMaxName + 1];
    ::snprintf(link, sizeof link, "/proc/self/fd/%d", fd);
    const ssize_t r = ::readlink(link, buf, sizeof buf);
    const int why = errno;
    ::close(fd);
    if (r < 0) { errno = why; return oke::fail(); }
    if (out != nullptr && cap != 0) {
        const kal_uintptr take =
            static_cast<kal_uintptr>(r) < cap ? static_cast<kal_uintptr>(r) : cap;
        ::memcpy(out, buf, static_cast<size_t>(take));
    }
    return static_cast<kal_intptr>(r);
}

// THE WALK BORROWS A DESCRIPTOR AND MUST NOT CLOSE IT.
//
// `fdopendir` takes ownership of the descriptor it is given, and `closedir`
// closes it -- so passing the caller's `kal_dir` directly would close a handle
// the caller still holds and still owns. A descriptor of the walk's own is
// therefore not an optimisation to skip: the iterator's lifetime and the
// directory handle's are separate, which is exactly what openkal's two words say.
//
// OPENED AGAIN, NOT DUPLICATED (0.12.0). A duplicate shares the directory's
// position, and this file system does not move a directory's back to its
// start: a second walk of a directory began where the first had ended, and
// listed nothing (measured by openkal 0.23's listing with attributes, which
// lists a directory listed before). The directory opened through itself has
// a position of its own, as the other implementations' walks do.
int kal_fs_list_begin(kal_dir d, kal_uintptr* iter) {
    if (iter == nullptr) return kal_err_invalid;
    const int fd = oke::unpack(d.h);
    if (fd < 0) return kal_err_invalid;
    const int copy = ::openat(fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (copy < 0) return oke::last();
    DIR* dir = ::fdopendir(copy);
    if (dir == nullptr) { const int e = oke::last(); ::close(copy); return e; }
    auto* s = static_cast<listing*>(::malloc(sizeof(listing)));
    if (s == nullptr) { ::closedir(dir); return kal_err_no_memory; }
    s->dir = dir; s->of = fd; s->busy = 0; s->doomed = 0;
    listings_lock(); s->next = g_listings; g_listings = s; listings_unlock();
    *iter = reinterpret_cast<kal_uintptr>(s);
    if (oke::rights::g_any) oke::rights::note(fd, 'l', "", 0);
    return kal_ok;
}

int kal_fs_list_next(kal_dir d, kal_uintptr* iter,
                     char* name_out, kal_uintptr name_cap,
                     kal_uintptr* name_len, int* kind) {
    // A closed directory is a released handle (clause 6.7), and closing it
    // released the iterator too, which fs.h states.
    const int of = oke::unpack(d.h);
    if (of < 0 || iter == nullptr || *iter == 0) return kal_err_invalid;
    auto* s = reinterpret_cast<listing*>(*iter);
    if (!listing_take(s, of)) return kal_err_invalid;
    DIR* dir = s->dir;
    for (;;) {
        errno = 0;
        struct dirent* e = ::readdir(dir);
        if (e == nullptr) {
            if (errno != 0) { const int err = oke::last(); listing_give(s, false); return err; }
            // THE END OF THE WALK RELEASES THE ITERATOR AND SAYS SO ONCE. A
            // caller that called again would otherwise pass a closed DIR* to
            // `readdir`; zeroing the word makes the second call
            // kal_err_invalid rather than undefined. The end is kal_ok with a
            // name of length zero, as fs.h states from 0.23; this
            // implementation reported kal_err_not_found until 0.12.0.
            listing_give(s, true);
            *iter = 0;
            if (name_len != nullptr) *name_len = 0;
            return kal_ok;
        }
        // `.` AND `..` ARE NOT ENTRIES OF THE DIRECTORY IN openkal's MODEL.
        // They name the directory itself and its parent, and openkal has no
        // parent relation at all -- a program's reach is the handles it holds.
        // Reporting them would offer a way upward that the interface refuses.
        if (::strcmp(e->d_name, ".") == 0 || ::strcmp(e->d_name, "..") == 0)
            continue;
        const kal_uintptr n = ::strlen(e->d_name);
        if (name_len != nullptr) *name_len = n;
        if (name_out != nullptr && name_cap != 0) {
            const kal_uintptr take = n < name_cap ? n : name_cap;
            ::memcpy(name_out, e->d_name, static_cast<size_t>(take));
        }
        if (kind != nullptr) {
            switch (e->d_type) {
            case DT_REG:  *kind = kal_node_file;      break;
            case DT_DIR:  *kind = kal_node_directory; break;
            case DT_LNK:  *kind = kal_node_link;      break;
            case DT_UNKNOWN: {
                // A file system that does not fill `d_type` is not a defect to
                // paper over with `kal_node_other`: the caller asked what this
                // entry is, and one `fstatat` answers.
                struct stat st{};
                if (::fstatat(::dirfd(dir), e->d_name, &st,
                              AT_SYMLINK_NOFOLLOW) == 0)
                    *kind = kind_of(st.st_mode);
                else
                    *kind = kal_node_other;
                break;
            }
            default:      *kind = kal_node_other;     break;
            }
        }
        listing_give(s, false);
        return kal_ok;
    }
}

// THE LISTING WITH WHAT IT TOLD OF THE ENTRY (0.23). This file system's listing
// gives a name and a kind and nothing else, so a field beyond the kind is asked
// of the entry itself --- the request a caller would make, made here so that the
// caller crosses this interface once. KAL_FS_PROP_LIST_INFO is not claimed.
int kal_fs_list_next_info(kal_dir d, kal_uintptr* iter, char* name_out, kal_uintptr name_cap,
                          kal_uintptr* name_len, kal_u32 wanted, kal_node_info* info) {
    if (info == nullptr || info->self_size < sizeof(kal_u32) * 2) return kal_err_invalid;
    const int of = oke::unpack(d.h);
    char own[kMaxName + 1];
    kal_uintptr n = 0;
    int kind = 0;
    const int e = kal_fs_list_next(d, iter, own, kMaxName, &n, &kind);
    if (e != kal_ok) return e;
    if (*iter == 0) {
        // The end of the listing: no entry, and nothing to describe.
        if (name_len) *name_len = 0;
        info->present = KAL_INFO_KIND;
        if (room_for<int>(info, offsetof(kal_node_info, kind))) info->kind = kal_node_absent;
        return kal_ok;
    }
    if (name_len) *name_len = n;
    if (name_out && name_cap) ::memcpy(name_out, own, static_cast<size_t>(n < name_cap ? n : name_cap));
    own[n] = '\0';
    struct stat st{};
    if ((wanted & ~KAL_INFO_KIND) != 0 && ::fstatat(of, own, &st, AT_SYMLINK_NOFOLLOW) == 0) {
        fill(st, wanted, info);
    } else {
        info->present = 0;
    }
    if (room_for<int>(info, offsetof(kal_node_info, kind))) { info->kind = kind; info->present |= KAL_INFO_KIND; }
    return kal_ok;
}

// A HANDLE WITH LESS (0.23, rights.h).
int kal_fs_dir_attenuate(kal_dir d, const kal_dir_rights* rights, kal_dir* out) {
    const int fd = oke::unpack(d.h);
    if (fd < 0 || rights == nullptr || out == nullptr) return kal_err_invalid;
    if (rights->self_size < offsetof(kal_dir_rights, record)) return kal_err_invalid;
    constexpr kal_uintptr known = KAL_DIR_READ_ONLY | KAL_DIR_UNCHANGING | KAL_DIR_RECORDED;
    if ((rights->flags & ~known) != 0) return kal_err_invalid;
    int record = -1;
    if (rights->flags & KAL_DIR_RECORDED) {
        if (rights->self_size < sizeof(kal_dir_rights)) return kal_err_invalid;
        record = oke::unpack(rights->record.h);
        if (record < 0) return kal_err_invalid;
        // Opened for writing: the record is appended to, not read.
        const int fl = ::fcntl(record, F_GETFL);
        if (fl < 0 || (fl & O_ACCMODE) == O_RDONLY) return kal_err_invalid;
    }
    int nfd = -1;
    const int e = oke::rights::attenuate(fd, static_cast<unsigned>(rights->flags), record, &nfd);
    if (e != kal_ok) return e;
    out->h = oke::pack(nfd);
    return kal_ok;
}

}
