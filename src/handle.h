// Handle construction shared by the interfaces whose handles are owned.
//
// The specification requires that a released handle not be treated as valid,
// and recommends dividing the word into an index and a generation. That is what
// this file does. It is not a translation table: the descriptor is recovered
// arithmetically from the word, and the array holds only generations, so the
// implementation retains the property clause 7.1 requires.
#pragma once
#include "sys.h"

namespace oke {

inline constexpr int kMaxDescriptor = 65536;

// THE SPLIT IS DERIVED FROM THE WIDTH OF A POINTER, AND THE COPY THAT WAS NOT
// DID NOT COMPILE HERE.
//
// This file was taken from `openkal-linux`, where the index occupies the low
// 32 bits and the generation the high 32. `kal_uintptr` is 32 bits on wasm32,
// so `<< 32` is a shift by the whole width -- undefined behaviour, and
// diagnosed:
//
//   handle.h:22: warning: shift count >= width of type
//
// The defect is the general one this ecosystem has recorded before: a value
// carried from one layer to another acquires the RECEIVING layer's
// requirements, and a constant that was a property of the source machine
// travelled as if it were a property of the scheme.
//
// 17 bits is the index, because `kMaxDescriptor + 1` needs 17. Everything
// above it is the generation: 15 bits here, 47 on a 64-bit host, from one
// expression rather than two.
//
// THE GENERATION WRAPS, AND ON THIS PLATFORM IT WRAPS SOONER. A descriptor
// opened and released 32768 times returns to a generation a stale handle could
// name. That is a bounded guarantee rather than an absolute one on every
// width; it is stated here because the bound differs by a factor of four
// billion between the two, and a reader who knew only the 64-bit number would
// draw the wrong conclusion.
inline constexpr unsigned kIndexBits = 17;
inline constexpr uptr     kIndexMask = (static_cast<uptr>(1) << kIndexBits) - 1;

inline unsigned* generations() {
    static unsigned g[kMaxDescriptor];
    return g;
}

inline uptr pack(int fd) {
    if (fd < 0 || fd >= kMaxDescriptor) return 0;
    return (static_cast<uptr>(generations()[fd]) << kIndexBits)
         | (static_cast<uptr>(fd) + 1u);
}

// Returns the descriptor, or -1 if the word does not name a live one.
//
// THIS ACCEPTS A WORD THAT WAS NEVER PACKED, AND SILENTLY. A bare descriptor N
// has the shape of a packed handle naming N-1 whose generation is still zero,
// so this returns N-1 for it rather than -1. Nothing here can tell the two
// apart: the word is one machine word and carries no tag.
//
// The consequence is that a handle of the OTHER discipline must never reach
// this function. openkal.stream's handles are bare descriptors (stream.cpp
// states why), and src/timeout.cpp used to pass one here and wait upon the
// descriptor below the one it then transferred upon. Owned handles --- kal_file,
// kal_dir, kal_net_listener, kal_net_conn, kal_datagram --- are the whole of
// this function's domain.
inline int unpack(uptr h) {
    const int fd = static_cast<int>(h & kIndexMask) - 1;
    if (fd < 0 || fd >= kMaxDescriptor) return -1;
    if (static_cast<unsigned>(h >> kIndexBits) != generations()[fd]) return -1;
    return fd;
}

inline void retire(uptr h) {
    const int fd = unpack(h);
    if (fd >= 0) {
        // MASKED TO WHAT THE WORD CAN CARRY. Incrementing without the mask
        // would let the counter grow past the bits `pack` keeps, so a handle
        // packed at generation 32768 would compare equal to one packed at
        // generation 0 -- a stale handle honoured, which is the one thing this
        // file exists to prevent.
        const unsigned span =
            static_cast<unsigned>((static_cast<uptr>(1)
                                   << (sizeof(uptr) * 8 - kIndexBits)) - 1);
        generations()[fd] = (generations()[fd] + 1u) & span;
    }
}

}  // namespace oke
