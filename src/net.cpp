#include "sys.h"
#include "handle.h"
#include <openkal/net.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

// SOCKETS EXIST ON THIS PLATFORM AND ARE NOT WHAT THEY LOOK LIKE, WHICH IS THE
// REASON THIS INTERFACE IS PROVIDED WITH A CAPABILITY WORD RATHER THAN OMITTED.
//
// Emscripten implements the BSD socket calls, and beneath them there is no IP
// stack: a `connect` becomes a WebSocket to a proxy that the host must be
// configured to reach, and a `listen` requires that proxy to accept on the
// module's behalf. So the SYMBOLS are honest -- the calls are implemented and
// return real conditions -- while the SEMANTICS are a different transport.
//
// Clause 6.2's three times decide the treatment. Absence at link would be
// false: the calls exist. A capability word is the mechanism for "how does it
// behave within an interface it provides", and that is exactly this
// situation -- so the interface is provided, `kal_net_props` reports what the
// transport can do, and each call returns the condition the host produced
// rather than a manufactured one.
//
// THE ALTERNATIVE WAS CONSIDERED AND IS WORSE. Omitting `openkal.net` would
// make a program that speaks to a WebSocket-proxied server fail to LINK on the
// one platform where that is the normal way to reach a network.
namespace {

// THE ENDPOINT IS BYTES IN NETWORK ORDER AND A PORT IN HOST ORDER, which is
// openkal's own arrangement and is not the same as sockaddr's. The port is the
// part that differs, and it is the part a translation gets wrong silently: a
// byte-swapped port connects to the wrong service rather than failing.
bool to_sockaddr(const kal_endpoint* e, sockaddr_storage* ss, socklen_t* len) {
    if (e == nullptr) return false;
    ::memset(ss, 0, sizeof(*ss));
    if (e->addr_len == 4) {
        auto* a = reinterpret_cast<sockaddr_in*>(ss);
        a->sin_family = AF_INET;
        a->sin_port = ::htons(static_cast<unsigned short>(e->port));
        ::memcpy(&a->sin_addr, e->addr, 4);
        *len = sizeof(sockaddr_in);
        return true;
    }
    if (e->addr_len == 16 || e->addr_len == 20) {
        auto* a = reinterpret_cast<sockaddr_in6*>(ss);
        a->sin6_family = AF_INET6;
        a->sin6_port = ::htons(static_cast<unsigned short>(e->port));
        ::memcpy(&a->sin6_addr, e->addr, 16);
        // The scope identifier occupies the four bytes openkal appends, and it
        // is the reason 20 is a length rather than a mistake.
        if (e->addr_len == 20) ::memcpy(&a->sin6_scope_id, e->addr + 16, 4);
        *len = sizeof(sockaddr_in6);
        return true;
    }
    return false;
}

bool from_sockaddr(const sockaddr_storage* ss, kal_endpoint* out) {
    if (out == nullptr) return false;
    ::memset(out, 0, sizeof(*out));
    if (ss->ss_family == AF_INET) {
        const auto* a = reinterpret_cast<const sockaddr_in*>(ss);
        ::memcpy(out->addr, &a->sin_addr, 4);
        out->addr_len = 4;
        out->port = ::ntohs(a->sin_port);
        return true;
    }
    if (ss->ss_family == AF_INET6) {
        const auto* a = reinterpret_cast<const sockaddr_in6*>(ss);
        ::memcpy(out->addr, &a->sin6_addr, 16);
        out->addr_len = 16;
        if (a->sin6_scope_id != 0) {
            ::memcpy(out->addr + 16, &a->sin6_scope_id, 4);
            out->addr_len = 20;
        }
        out->port = ::ntohs(a->sin6_port);
        return true;
    }
    return false;
}

int family_of(const kal_endpoint* e) {
    return e->addr_len == 4 ? AF_INET : AF_INET6;
}

}  // namespace

extern "C" {

int kal_net_connect(const kal_endpoint* to, kal_net_conn* out) {
    if (to == nullptr || out == nullptr) return kal_err_invalid;
    sockaddr_storage ss{};
    socklen_t len = 0;
    if (!to_sockaddr(to, &ss, &len)) return kal_err_invalid;
    const int fd = ::socket(family_of(to), SOCK_STREAM, 0);
    if (fd < 0) return oke::last();
    for (;;) {
        if (::connect(fd, reinterpret_cast<sockaddr*>(&ss), len) == 0) break;
        if (oke::interrupted()) continue;
        const int e = oke::last();
        ::close(fd);
        return e;
    }
    out->h = oke::pack(fd);
    return kal_ok;
}

int kal_net_listen(const kal_endpoint* local, kal_net_listener* out) {
    if (local == nullptr || out == nullptr) return kal_err_invalid;
    sockaddr_storage ss{};
    socklen_t len = 0;
    if (!to_sockaddr(local, &ss, &len)) return kal_err_invalid;
    const int fd = ::socket(family_of(local), SOCK_STREAM, 0);
    if (fd < 0) return oke::last();
    // SO_REUSEADDR IS NOT SET, AND THAT IS DELIBERATE. openkal's listen says
    // "bind this endpoint"; a reuse option changes what happens when the
    // endpoint is already in use, which is a decision the caller did not make
    // and would not be told about.
    if (::bind(fd, reinterpret_cast<sockaddr*>(&ss), len) != 0
        || ::listen(fd, 128) != 0) {
        const int e = oke::last();
        ::close(fd);
        return e;
    }
    out->h = oke::pack(fd);
    return kal_ok;
}

int kal_net_accept(kal_net_listener l, kal_net_conn* out) {
    if (out == nullptr) return kal_err_invalid;
    const int fd = oke::unpack(l.h);
    if (fd < 0) return kal_err_invalid;
    for (;;) {
        const int c = ::accept(fd, nullptr, nullptr);
        if (c >= 0) { out->h = oke::pack(c); return kal_ok; }
        if (oke::interrupted()) continue;
        return oke::last();
    }
}

// A CONNECTION'S STREAM IS A BARE DESCRIPTOR, and this is the second and last
// place the two handle disciplines meet. See kal_fs_stream for the whole of
// the reason; the rule is that the conversion is one-way and exists once per
// owned kind.
kal_stream kal_net_stream(kal_net_conn c) {
    const int fd = oke::unpack(c.h);
    return kal_stream{fd < 0 ? ~static_cast<kal_uintptr>(0)
                             : static_cast<kal_uintptr>(fd)};
}

int kal_net_peer(kal_net_conn c, kal_endpoint* out) {
    const int fd = oke::unpack(c.h);
    if (fd < 0 || out == nullptr) return kal_err_invalid;
    sockaddr_storage ss{};
    socklen_t len = sizeof(ss);
    if (::getpeername(fd, reinterpret_cast<sockaddr*>(&ss), &len) != 0)
        return oke::last();
    return from_sockaddr(&ss, out) ? kal_ok : kal_err_not_supported;
}

int kal_net_local(kal_net_conn c, kal_endpoint* out) {
    const int fd = oke::unpack(c.h);
    if (fd < 0 || out == nullptr) return kal_err_invalid;
    sockaddr_storage ss{};
    socklen_t len = sizeof(ss);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&ss), &len) != 0)
        return oke::last();
    return from_sockaddr(&ss, out) ? kal_ok : kal_err_not_supported;
}

int kal_net_listener_local(kal_net_listener l, kal_endpoint* out) {
    const int fd = oke::unpack(l.h);
    if (fd < 0 || out == nullptr) return kal_err_invalid;
    sockaddr_storage ss{};
    socklen_t len = sizeof(ss);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&ss), &len) != 0)
        return oke::last();
    return from_sockaddr(&ss, out) ? kal_ok : kal_err_not_supported;
}

int kal_net_shutdown(kal_net_conn c, int direction) {
    const int fd = oke::unpack(c.h);
    if (fd < 0) return kal_err_invalid;
    int how;
    switch (direction) {
    case KAL_SHUT_READ:  how = SHUT_RD;   break;
    case KAL_SHUT_WRITE: how = SHUT_WR;   break;
    case KAL_SHUT_BOTH:  how = SHUT_RDWR; break;
    default: return kal_err_invalid;
    }
    if (::shutdown(fd, how) != 0) return oke::last();
    return kal_ok;
}

void kal_net_close(kal_net_conn c) {
    const int fd = oke::unpack(c.h);
    if (fd >= 0) { ::close(fd); oke::retire(c.h); }
}

void kal_net_close_listener(kal_net_listener l) {
    const int fd = oke::unpack(l.h);
    if (fd >= 0) { ::close(fd); oke::retire(l.h); }
}

// NEITHER WORD IS CLAIMED, AND BOTH ABSENCES ARE PROPERTIES OF THE TRANSPORT
// RATHER THAN OF THE CODE ABOVE.
//
//   IPV6       the calls accept an AF_INET6 address and the proxy beneath
//              them speaks a WebSocket URL, which carries a host and not an
//              address family. A caller told IPv6 was available would expect
//              a v6 route to exist.
//   HALFCLOSE  `shutdown` returns success and a WebSocket has no half-closed
//              state to enter: the peer is not told, so a program waiting for
//              the end of input would wait for ever. This is precisely the
//              condition the specification says must not be reported as
//              working.
kal_uintptr kal_net_props(void) { return 0u; }

}
