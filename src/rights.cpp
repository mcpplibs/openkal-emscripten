#include "rights.h"
#include "handle.h"
#include <openkal/env.h>

#include <dirent.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// The rights of directory handles here (openkal 0.23, rights.h): the table of
// openkal-linux, written on this C library's calls.

namespace oke::rights {

volatile bool g_any = false;

namespace {

// One lock for the table, the records and the caches. It spins rather than
// suspends because the build without threads has nothing to suspend upon.
volatile int g_held = 0;
struct held {
    held() { while (__atomic_exchange_n(&g_held, 1, __ATOMIC_ACQUIRE)) ::sched_yield(); }
    ~held() { __atomic_store_n(&g_held, 0, __ATOMIC_RELEASE); }
};

using u64 = unsigned long long;

u64 hash(const char* s, uptr n, u64 h = 1469598103934665603ull) {
    for (uptr i = 0; i < n; ++i) { h ^= static_cast<unsigned char>(s[i]); h *= 1099511628211ull; }
    return h;
}

// --- a set of 64-bit keys, with a byte beside each -----------------------------
struct keyset {
    u64* keys = nullptr;          // zero is never a key
    unsigned char* vals = nullptr;
    uptr cap = 0, used = 0;

    bool grow() {
        const uptr ncap = cap ? cap * 2 : 256;
        auto* nk = static_cast<u64*>(::calloc(ncap, sizeof(u64)));
        auto* nv = static_cast<unsigned char*>(::malloc(ncap));
        if (!nk || !nv) { ::free(nk); ::free(nv); return false; }
        for (uptr i = 0; i < cap; ++i) {
            if (keys[i] == 0) continue;
            uptr j = keys[i] & (ncap - 1);
            while (nk[j] != 0) j = (j + 1) & (ncap - 1);
            nk[j] = keys[i]; nv[j] = vals[i];
        }
        ::free(keys); ::free(vals);
        keys = nk; vals = nv; cap = ncap;
        return true;
    }
    bool add(u64 k, unsigned char v) {
        if (k == 0) k = 1;
        if ((used + 1) * 2 > cap && !grow()) return false;
        uptr j = k & (cap - 1);
        while (keys[j] != 0) { if (keys[j] == k) return false; j = (j + 1) & (cap - 1); }
        keys[j] = k; vals[j] = v; ++used;
        return true;
    }
    int find(u64 k) const {
        if (k == 0) k = 1;
        if (cap == 0) return -1;
        uptr j = k & (cap - 1);
        while (keys[j] != 0) { if (keys[j] == k) return vals[j]; j = (j + 1) & (cap - 1); }
        return -1;
    }
    void clear() { ::free(keys); ::free(vals); keys = nullptr; vals = nullptr; cap = used = 0; }
};

// --- the trees ---------------------------------------------------------------
constexpr int kRoots = 64;
constexpr uptr kBatch = 64 * 1024;

struct root {
    bool     live = false;
    int      refs = 0;
    unsigned flags = 0;
    int      base = -1;        // a descriptor of the root, to list beneath
    int      record = -1;      // opened to append
    char*    rprefix = nullptr;
    uptr     rplen = 0;
    keyset   seen;
    char*    batch = nullptr;
    uptr     batched = 0;
    keyset   listed;           // directory path -> 1
    keyset   names;            // (directory path, name) -> kind
    uptr     budget = 0;
};
root g_roots[kRoots];

struct entry {
    int      fd = -1;
    int      root = -1;
    unsigned flags = 0;
    char*    prefix = nullptr;
    uptr     plen = 0;
};
constexpr int kEntries = 4096;
entry g_entries[kEntries];
unsigned short g_slot[kMaxDescriptor];   // entry index + 1; zero for none

entry* entry_of(int fd) {
    if (fd < 0 || fd >= kMaxDescriptor || g_slot[fd] == 0) return nullptr;
    return &g_entries[g_slot[fd] - 1];
}

entry* new_entry(int fd) {
    if (fd < 0 || fd >= kMaxDescriptor) return nullptr;
    for (int i = 0; i < kEntries; ++i)
        if (g_entries[i].fd < 0) { g_entries[i].fd = fd; g_slot[fd] = static_cast<unsigned short>(i + 1); return &g_entries[i]; }
    return nullptr;
}

void drop_entry(entry* e) {
    if (!e) return;
    if (e->fd >= 0 && e->fd < kMaxDescriptor) g_slot[e->fd] = 0;
    ::free(e->prefix);
    *e = entry{};
}

int new_root() {
    for (int i = 0; i < kRoots; ++i) if (!g_roots[i].live) { g_roots[i] = root{}; g_roots[i].live = true; return i; }
    return -1;
}

uptr budget_for_preference() {
    switch (kal_env_resources()) {
        case KAL_RESOURCES_PERFORMANCE: return uptr{1} << 20;
        case KAL_RESOURCES_ECONOMY:     return 0;    // nothing kept
        default:                        return uptr{1} << 16;
    }
}

char* copy_of(const char* s, uptr n) {
    auto* p = static_cast<char*>(::malloc(n + 1));
    if (p) { ::memcpy(p, s, n); p[n] = '\0'; }
    return p;
}

// The path of `name' beneath the root of `e' --- after `lead', where given ---
// into `out'; its length, or 0 when it does not fit. "." components are
// dropped; the root itself is "" written as length 1.
uptr full_path(const entry& e, const char* name, uptr len, char* out, uptr cap,
               const char* lead = nullptr, uptr lead_len = 0) {
    uptr o = 0;
    auto put = [&](const char* s, uptr n) {
        if (n == 0 || (n == 1 && s[0] == '.')) return true;
        if (o + n + 1 >= cap) return false;
        if (o) out[o++] = '/';
        ::memcpy(out + o, s, n); o += n;
        return true;
    };
    auto parts = [&](const char* s, uptr n) {
        uptr start = 0;
        for (uptr i = 0; s && i <= n; ++i)
            if (i == n || s[i] == '/') { if (!put(s + start, i - start)) return false; start = i + 1; }
        return true;
    };
    if (!parts(lead, lead_len) || !parts(e.prefix, e.plen) || !parts(name, len)) return 0;
    out[o] = '\0';
    return o == 0 ? 1 : o;
}

// --- the record ----------------------------------------------------------------
void flush(root& r) {
    if (r.record < 0 || r.batched == 0) return;
    uptr at = 0;
    while (at < r.batched) {
        const ssize_t w = ::write(r.record, r.batch + at, r.batched - at);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) break;
        at += static_cast<uptr>(w);
    }
    r.batched = 0;
}

void append(root& r, char kind, const char* path, uptr n) {
    if (r.record < 0) return;
    if (!r.batch) { r.batch = static_cast<char*>(::malloc(kBatch)); if (!r.batch) return; }
    if (n + 2 > kBatch) return;
    if (r.batched + n + 2 > kBatch) flush(r);
    r.batch[r.batched++] = kind;
    ::memcpy(r.batch + r.batched, path, n); r.batched += n;
    r.batch[r.batched++] = '\0';
}

// --- the listings of a tree that does not change ---------------------------------
u64 name_key(u64 dir, const char* s, uptr n) { return hash(s, n, dir ^ 0x9e3779b97f4a7c15ull); }

bool list(root& r, const char* dir, uptr dlen, u64 key) {
    if (r.listed.find(key) >= 0) return true;
    if (r.budget == 0) return false;
    char path[4096];
    if (dlen + 1 >= sizeof path) return false;
    if (dlen == 0) { path[0] = '.'; path[1] = '\0'; }
    else { ::memcpy(path, dir, dlen); path[dlen] = '\0'; }
    const int fd = ::openat(r.base, path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return false;
    DIR* d = ::fdopendir(fd);
    if (!d) { ::close(fd); return false; }
    uptr count = 0;
    bool ok = true;
    for (;;) {
        errno = 0;
        const struct dirent* e = ::readdir(d);
        if (!e) { ok = errno == 0; break; }
        const uptr n = ::strlen(e->d_name);
        if ((n == 1 && e->d_name[0] == '.') || (n == 2 && e->d_name[0] == '.' && e->d_name[1] == '.')) continue;
        if (count >= r.budget) { ok = false; break; }
        r.names.add(name_key(key, e->d_name, n), e->d_type);
        ++count;
    }
    ::closedir(d);
    if (!ok) return false;
    r.budget -= count;
    r.listed.add(key, 1);
    return true;
}

}  // namespace

unsigned of(int fd) {
    if (!g_any) return 0;
    held h;
    const entry* e = entry_of(fd);
    return e ? e->flags : 0;
}

int attenuate(int fd, unsigned add, int record, int* out) {
    const int nfd = ::fcntl(fd, F_DUPFD_CLOEXEC, 3);
    if (nfd < 0) return oke::last();
    // The record is appended to through a description of its own, so that the
    // holder's position is not moved and every write appends: the name this
    // file system keeps for the descriptor, opened again to append.
    int rec = -1;
    if (add & recorded) {
        char link[32], path[1024];
        ::snprintf(link, sizeof link, "/proc/self/fd/%d", record);
        const ssize_t n = ::readlink(link, path, sizeof path - 1);
        if (n > 0) { path[n] = '\0'; rec = ::open(path, O_WRONLY | O_APPEND | O_CLOEXEC); }
        if (rec < 0) { ::close(nfd); return kal_err_invalid; }
    }
    held h;
    const entry* from = entry_of(fd);
    entry* e = new_entry(nfd);
    if (!e) { ::close(nfd); if (rec >= 0) ::close(rec); return kal_err_no_memory; }
    const unsigned inherited = from ? from->flags : 0;
    // A NEW ROOT WHERE THERE IS SOMETHING NEW TO RECORD OR TO STATE; otherwise
    // the handle is the same tree with fewer rights.
    const bool new_tree = from == nullptr || (add & (recorded | unchanging)) != 0;
    if (new_tree) {
        const int ri = new_root();
        if (ri < 0) { drop_entry(e); ::close(nfd); if (rec >= 0) ::close(rec); return kal_err_no_memory; }
        root& r = g_roots[ri];
        r.flags = inherited | add;
        r.refs = 1;
        r.base = ::fcntl(nfd, F_DUPFD_CLOEXEC, 3);
        r.record = rec;
        r.budget = (r.flags & unchanging) ? budget_for_preference() : 0;
        // A RECORD ALREADY KEPT FOR THE TREE GOES ON BEING KEPT, and its names
        // stay relative to the directory it is of.
        if (from && from->root >= 0 && !(add & recorded) && g_roots[from->root].record >= 0) {
            const root& f = g_roots[from->root];
            r.record = ::fcntl(f.record, F_DUPFD_CLOEXEC, 3);
            r.flags |= recorded;
            char lead[4096];
            uptr n = full_path(*from, "", 0, lead, sizeof lead, f.rprefix, f.rplen);
            if (n == 1 && lead[0] == '\0') n = 0;
            r.rprefix = copy_of(lead, n);
            r.rplen = r.rprefix ? n : 0;
        }
        e->root = ri;
        e->prefix = copy_of("", 0);
        e->plen = 0;
    } else {
        e->root = from->root;
        if (e->root >= 0) g_roots[e->root].refs++;
        e->prefix = copy_of(from->prefix ? from->prefix : "", from->plen);
        e->plen = e->prefix ? from->plen : 0;
    }
    e->flags = inherited | add;
    g_any = true;
    *out = nfd;
    return kal_ok;
}

bool derive(int parent, int child, const char* name, uptr len) {
    if (!g_any) return true;
    held h;
    const entry* p = entry_of(parent);
    if (!p) return true;
    entry* e = new_entry(child);
    if (!e) return false;
    e->flags = p->flags;
    e->root = p->root;
    if (e->root >= 0) g_roots[e->root].refs++;
    char buf[4096];
    const uptr n = full_path(*p, name, len, buf, sizeof buf);
    const uptr plen = (n == 1 && buf[0] == '\0') ? 0 : n;
    e->prefix = copy_of(buf, plen);
    e->plen = e->prefix ? plen : 0;
    return true;
}

void forget(int fd) {
    if (!g_any) return;
    held h;
    entry* e = entry_of(fd);
    if (!e) return;
    const int ri = e->root;
    drop_entry(e);
    if (ri >= 0 && --g_roots[ri].refs <= 0) {
        // The last handle of the tree: what it recorded is written now. The
        // root itself stays until the program ends, which writes its `e'.
        flush(g_roots[ri]);
        g_roots[ri].names.clear();
        g_roots[ri].listed.clear();
        if (g_roots[ri].base >= 0) { ::close(g_roots[ri].base); g_roots[ri].base = -1; }
    }
}

void note(int fd, char kind, const char* name, uptr len) {
    if (!g_any) return;
    held h;
    const entry* e = entry_of(fd);
    if (!e || e->root < 0 || !(e->flags & recorded)) return;
    root& r = g_roots[e->root];
    if (r.record < 0) return;
    char buf[4096];
    uptr n = full_path(*e, name, len, buf, sizeof buf, r.rprefix, r.rplen);
    if (n == 0) return;
    if (n == 1 && buf[0] == '\0') { buf[0] = '.'; buf[1] = '\0'; }
    if (!r.seen.add(hash(buf, n, static_cast<u64>(static_cast<unsigned char>(kind)) * 0x100000001b3ull), 1)) return;
    append(r, kind, buf, n);
}

bool known_absent(int fd, const char* name, uptr len) {
    if (!g_any) return false;
    held h;
    const entry* e = entry_of(fd);
    if (!e || e->root < 0 || !(e->flags & unchanging)) return false;
    root& r = g_roots[e->root];
    if (r.base < 0) return false;
    char path[4096];
    const uptr n = full_path(*e, name, len, path, sizeof path);
    if (n == 0 || (n == 1 && path[0] == '\0')) return false;
    uptr start = 0;
    u64 dir = hash("", 0);
    for (uptr i = 0; i <= n; ++i) {
        if (i < n && path[i] != '/') continue;
        if (!list(r, path, start == 0 ? 0 : start - 1, dir)) return false;
        const int kind = r.names.find(name_key(dir, path + start, i - start));
        if (kind < 0) return true;
        if (i == n) return false;
        if (kind != DT_DIR) return false;
        dir = hash(path, i);
        start = i + 1;
    }
    return false;
}

void changed(int fd) {
    if (!g_any) return;
    held h;
    const entry* e = entry_of(fd);
    if (!e || e->root < 0) return;
    root& r = g_roots[e->root];
    r.names.clear();
    r.listed.clear();
    if (r.flags & unchanging) r.budget = budget_for_preference();
}

void end_of_program() {
    if (!g_any) return;
    static volatile int ended = 0;
    if (__atomic_exchange_n(&ended, 1, __ATOMIC_ACQ_REL)) return;
    held h;
    for (auto& r : g_roots) {
        if (!r.live || r.record < 0) continue;
        if (!r.batch) r.batch = static_cast<char*>(::malloc(kBatch));
        if (r.batch) {
            if (r.batched + 2 > kBatch) flush(r);
            r.batch[r.batched++] = 'e';
            r.batch[r.batched++] = '\0';
        }
        flush(r);
    }
}

}  // namespace oke::rights

namespace {
// A program that ends by returning from main, or through exit, writes its
// records as the C library runs its destructors.
[[gnu::destructor]] void records_at_exit() { oke::rights::end_of_program(); }
}  // namespace
