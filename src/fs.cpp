#include "sys.h"
#include "handle.h"
#include <openkal/fs.h>

#include <dirent.h>
#include <fcntl.h>
#include <stddef.h>     // offsetof
#include <stdio.h>      // renameat, which POSIX places here and not in <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
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
}

// The iteration word openkal hands back and forth is a pointer to the C
// library's own directory object. That is legitimate -- clause 7.1 requires a
// released handle not to be honoured, and the iterator is released by the same
// `closedir` that ends the walk -- and it is NOT put through handle.h, because
// a DIR* is not a descriptor and packing it would be a second discipline on a
// word this file already owns.
DIR* as_dir(kal_uintptr iter) { return reinterpret_cast<DIR*>(iter); }

}  // namespace

extern "C" {

kal_uintptr kal_fs_max_name(void) { return kMaxName; }

kal_uintptr kal_fs_preopen_count(void) { return 1u; }

int kal_fs_preopen(kal_uintptr index, kal_dir* out,
                   char* name_out, kal_uintptr name_cap,
                   kal_uintptr* name_len) {
    if (index != 0 || out == nullptr) return kal_err_invalid;
    const int fd = ::open("/", O_RDONLY | O_DIRECTORY);
    if (fd < 0) return oke::last();
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
//
// WITHHELD: LOCKS, because MEMFS has no lock table and a wasm module has no
// second process to contend with -- so a lock that always succeeded would tell
// a caller it had exclusivity it was never given. CAPACITY, because the answer
// would be the module's heap limit, which is not the quantity the caller is
// asking about.
kal_uintptr kal_fs_props(kal_dir) {
    return KAL_FS_PROP_CASE_SENSITIVE | KAL_FS_PROP_LINKS
         | KAL_FS_PROP_MODIFIED_TIME  | KAL_FS_PROP_ATOMIC_RENAME
         | KAL_FS_PROP_MAKE_LINKS;
}

int kal_fs_open_dir(kal_dir base, const char* name, kal_uintptr len,
                    kal_dir* out) {
    if (out == nullptr) return kal_err_invalid;
    const int b = oke::unpack(base.h);
    if (b < 0) return kal_err_invalid;
    char n[kMaxName + 1];
    if (!cname(name, len, n)) return kal_err_invalid;
    const int fd = ::openat(b, n, O_RDONLY | O_DIRECTORY);
    if (fd < 0) return oke::last();
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

    const int fd = ::openat(b, n, of, 0666);
    if (fd < 0) return oke::last();
    out->h = oke::pack(fd);
    return kal_ok;
}

void kal_fs_close_dir(kal_dir d) {
    const int fd = oke::unpack(d.h);
    if (fd >= 0) { ::close(fd); oke::retire(d.h); }
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

int kal_fs_info(kal_dir base, const char* name, kal_uintptr len,
                kal_uintptr flags, kal_u32 wanted, kal_node_info* out) {
    if (out == nullptr) return kal_err_invalid;
    const int b = oke::unpack(base.h);
    if (b < 0) return kal_err_invalid;
    char n[kMaxName + 1];
    if (!cname(name, len, n)) return kal_err_invalid;
    struct stat st{};
    const int at = (flags & KAL_FS_NO_RESOLVE) ? AT_SYMLINK_NOFOLLOW : 0;
    if (::fstatat(b, n, &st, at) != 0) {
        // ABSENCE IS AN ANSWER AND NOT A FAILURE, which is the property the
        // conformance suite observes by its effect. `kal_node_absent` with
        // kal_ok lets a caller ask "is this there" without an error path.
        if (errno == ENOENT || errno == ENOTDIR) {
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
    if (::mkdirat(b, n, 0777) != 0) return oke::last();
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
    if (::unlinkat(b, n, 0) == 0) return kal_ok;
    if (errno == EISDIR || errno == EPERM || errno == ENOTEMPTY) {
        const int first = errno;
        if (::unlinkat(b, n, AT_REMOVEDIR) == 0) return kal_ok;
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
    if (::renameat(fd, na, td, nb) != 0) return oke::last();
    return kal_ok;
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
    if (::utimensat(b, n, ts, 0) != 0) return oke::last();
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
    if (::symlinkat(t, b, n) != 0) return oke::last();
    return kal_ok;
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

// THE WALK BORROWS A DESCRIPTOR AND MUST NOT CLOSE IT.
//
// `fdopendir` takes ownership of the descriptor it is given, and `closedir`
// closes it -- so passing the caller's `kal_dir` directly would close a handle
// the caller still holds and still owns. `dup` is therefore not an
// optimisation to skip: the iterator's lifetime and the directory handle's are
// separate, which is exactly what openkal's two words say.
int kal_fs_list_begin(kal_dir d, kal_uintptr* iter) {
    if (iter == nullptr) return kal_err_invalid;
    const int fd = oke::unpack(d.h);
    if (fd < 0) return kal_err_invalid;
    const int copy = ::dup(fd);
    if (copy < 0) return oke::last();
    if (::lseek(copy, 0, SEEK_SET) < 0) { /* a directory may refuse; harmless */ }
    DIR* dir = ::fdopendir(copy);
    if (dir == nullptr) { const int e = oke::last(); ::close(copy); return e; }
    *iter = reinterpret_cast<kal_uintptr>(dir);
    return kal_ok;
}

int kal_fs_list_next(kal_dir, kal_uintptr* iter,
                     char* name_out, kal_uintptr name_cap,
                     kal_uintptr* name_len, int* kind) {
    if (iter == nullptr || *iter == 0) return kal_err_invalid;
    DIR* dir = as_dir(*iter);
    for (;;) {
        errno = 0;
        struct dirent* e = ::readdir(dir);
        if (e == nullptr) {
            if (errno != 0) return oke::last();
            // THE END OF THE WALK RELEASES THE ITERATOR AND SAYS SO ONCE. A
            // caller that called again would otherwise pass a closed DIR* to
            // `readdir`; zeroing the word makes the second call
            // kal_err_invalid rather than undefined.
            ::closedir(dir);
            *iter = 0;
            return kal_err_not_found;
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
        return kal_ok;
    }
}

}
