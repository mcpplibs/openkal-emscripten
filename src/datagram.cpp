#include "sys.h"
#include "handle.h"
#include <openkal/datagram.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

// DATAGRAMS ARE PROVIDED FOR THE SAME REASON `openkal.net` IS, and with the
// same honesty about what is beneath them: Emscripten implements the calls,
// and what carries the bytes is whatever the host was configured to proxy.
//
// The address translation is the same problem as in net.cpp and is written out
// again rather than shared, because the two interfaces are separate in the
// specification and a change to one must not silently reach the other.
namespace {

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

}  // namespace

extern "C" {

int kal_datagram_open(const kal_endpoint* local, kal_datagram* d) {
    if (local == nullptr || d == nullptr) return kal_err_invalid;
    sockaddr_storage ss{};
    socklen_t len = 0;
    if (!to_sockaddr(local, &ss, &len)) return kal_err_invalid;
    const int fd = ::socket(local->addr_len == 4 ? AF_INET : AF_INET6,
                            SOCK_DGRAM, 0);
    if (fd < 0) return oke::last();
    if (::bind(fd, reinterpret_cast<sockaddr*>(&ss), len) != 0) {
        const int e = oke::last();
        ::close(fd);
        return e;
    }
    d->h = oke::pack(fd);
    return kal_ok;
}

int kal_datagram_local(kal_datagram d, kal_endpoint* out) {
    const int fd = oke::unpack(d.h);
    if (fd < 0 || out == nullptr) return kal_err_invalid;
    sockaddr_storage ss{};
    socklen_t len = sizeof(ss);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&ss), &len) != 0)
        return oke::last();
    return from_sockaddr(&ss, out) ? kal_ok : kal_err_not_supported;
}

// A DATAGRAM IS SENT WHOLE OR NOT AT ALL, so there is no partial-send loop
// here and there must not be one: retrying the remainder of a datagram would
// send a second, shorter datagram, which is a different message.
kal_intptr kal_datagram_send_to(kal_datagram d, const void* buf,
                                kal_uintptr len, const kal_endpoint* to) {
    const int fd = oke::unpack(d.h);
    if (fd < 0 || to == nullptr) return -kal_err_invalid;
    sockaddr_storage ss{};
    socklen_t alen = 0;
    if (!to_sockaddr(to, &ss, &alen)) return -kal_err_invalid;
    for (;;) {
        const ssize_t r = ::sendto(fd, buf, len, 0,
                                   reinterpret_cast<sockaddr*>(&ss), alen);
        if (r < 0) { if (oke::interrupted()) continue; return oke::fail(); }
        return static_cast<kal_intptr>(r);
    }
}

kal_intptr kal_datagram_recv_from(kal_datagram d, void* buf, kal_uintptr len,
                                  kal_endpoint* from) {
    const int fd = oke::unpack(d.h);
    if (fd < 0) return -kal_err_invalid;
    sockaddr_storage ss{};
    socklen_t alen = sizeof(ss);
    for (;;) {
        const ssize_t r = ::recvfrom(fd, buf, len, 0,
                                     reinterpret_cast<sockaddr*>(&ss), &alen);
        if (r < 0) { if (oke::interrupted()) continue; return oke::fail(); }
        // THE SENDER IS REPORTED WHEN IT IS KNOWN AND THE COUNT IS RETURNED
        // EITHER WAY. A datagram whose source the transport did not supply is
        // still a datagram that arrived, and losing it because its origin is
        // unknown would be the worse answer.
        if (from != nullptr && !from_sockaddr(&ss, from))
            ::memset(from, 0, sizeof(*from));
        return static_cast<kal_intptr>(r);
    }
}

void kal_datagram_close(kal_datagram d) {
    const int fd = oke::unpack(d.h);
    if (fd >= 0) { ::close(fd); oke::retire(d.h); }
}

// NEITHER WORD IS CLAIMED. IPv6 for the reason net.cpp gives, and broadcast
// because there is no local segment to broadcast on: the module's peer is a
// proxy, and a datagram to a broadcast address reaches it and stops.
kal_uintptr kal_datagram_props(void) { return 0u; }

}
