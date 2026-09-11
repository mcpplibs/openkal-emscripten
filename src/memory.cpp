#include "sys.h"
#include <openkal/memory.h>

#include <stdlib.h>

extern "C" {

// THE PAGE IS NOT THE UNIT A CALLER SHOULD ASK FOR HERE, AND THE GRANULARITY
// SAYS SO.
//
// Every other implementation obtains memory from the kernel in pages and
// reports the page size, because that is the unit in which its allocation
// actually happens. This one is above a C library: the unit is whatever
// `aligned_alloc` hands back, and reporting 65536 -- wasm's page size, which
// is the unit in which the module's linear memory GROWS -- would tell a caller
// to round every request up to 64 KiB for no reason.
//
// A RECORDED DEFECT IS THE REASON THIS COMMENT EXISTS. An implementation once
// answered granularity = 1 truthfully, and a C library above it read that
// answer as a page size. The value below is therefore the largest alignment
// this implementation will honour without rounding, which is the question the
// specification asks, and it is a power of two so that a caller which does
// round can.
kal_uintptr kal_memory_granularity(void) { return 16u; }

// SIZE AND ALIGNMENT COME BACK ON THE RELEASE, WHICH IS WHAT LETS THIS
// FORWARD AT ALL.
//
// `aligned_alloc`/`free` is the pair Emscripten's libc offers, and `free`
// takes neither size nor alignment -- so openkal's release, which carries
// both, is strictly more information than the call beneath it needs. That
// direction is safe. The reverse would not be: an interface whose release
// carried less could not be implemented over a libc that required more.
void* kal_alloc(kal_uintptr size, kal_uintptr align) {
    if (size == 0) return nullptr;
    if (align == 0) align = 1;
    // `aligned_alloc` requires the size to be a multiple of the alignment and
    // the alignment to be a power of two; the specification requires neither
    // of a caller, so the adjustment belongs here.
    if ((align & (align - 1)) != 0) return nullptr;
    if (align < sizeof(void*)) align = sizeof(void*);
    const kal_uintptr rounded = (size + align - 1) & ~(align - 1);
    return ::aligned_alloc(static_cast<size_t>(align),
                           static_cast<size_t>(rounded));
}

void kal_free(void* p, kal_uintptr size, kal_uintptr align) {
    (void)size;
    (void)align;
    ::free(p);
}

}
