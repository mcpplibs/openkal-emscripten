/* THE PROBE FOR C'S NON-LOCAL JUMPS, as this platform lowers them.
 *
 * Two frames of C stand between the sigsetjmp and the siglongjmp, and each
 * frame holds a volatile local that the jump must not have restored: a
 * register-cached int that came back as its pre-jump value would describe a
 * lowering that rewound the frames rather than unwound them, which is the
 * observation this program exists to make. A second jump goes back over both
 * frames to the origin, so each buffer is used once in each direction.
 *
 * sigsetjmp and siglongjmp are POSIX's, and this C library declares them, as
 * every C library does, only to a unit that asks for POSIX; the engine
 * compiles C at the standard alone. The unit states what it asks for. (The
 * probe was first compiled on 2026-11-12, by the first engine that resolves
 * the key in its manifest; without this line it did not compile.) */
#define _POSIX_C_SOURCE 200809L
#include <setjmp.h>

static volatile int visited;

static void level_two(sigjmp_buf* back) {
    visited |= 4;
    siglongjmp(*back, 7); /* the return this frame never reaches */
}

static int level_one(sigjmp_buf* back) {
    sigjmp_buf own;
    volatile int kept = 1;
    if (sigsetjmp(own, 1) == 0) {
        kept = 2;
        level_two(&own);
        return -1; /* the jump skips this */
    }
    /* Control arrived over level_two's frame. The volatile local of THIS frame
     * kept the value it held when the jump was taken, and the deepest frame
     * ran exactly once. */
    if (kept != 2 || visited != 5) return -2;
    visited |= 8;
    siglongjmp(*back, 9); /* a second jump, over this frame too */
    return -3;
}

int main(void) {
    sigjmp_buf origin;
    volatile int kept = 1;
    visited = 0;
    if (sigsetjmp(origin, 1) == 0) {
        kept = 2;
        visited |= 1;
        return level_one(&origin);
    }
    /* Both jumps held: the states are exactly the ones written before them. */
    return (kept == 2 && visited == 13) ? 0 : -4;
}
