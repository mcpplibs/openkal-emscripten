#include "sys.h"
#include <openkal/terminal.h>

#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

// THE TERMINAL IS THE INTERFACE WHERE THIS PLATFORM MOST OFTEN HAS NO ANSWER,
// AND THE PROPERTY WORD IS WHY IT IS STILL PROVIDED IN WHOLE.
//
// Under node with a real terminal, `tcgetattr` and `TIOCGWINSZ` are answered
// by Emscripten forwarding to the host. In a browser there is no terminal at
// all: the descriptors are a JavaScript callback, and neither the mode nor a
// size exists. The same module can be in either situation, and nothing at
// compile time distinguishes them.
//
// So each entry point asks the machine and the property word reports what the
// machine answered -- clause 6.2's third time. A caller that reads the word
// before setting a mode is told; a caller that does not gets a named condition
// rather than a silent no-op, which is the failure a terminal interface can
// least afford: a program that believes it turned echo off and did not is a
// program that prints a password.
//
// THE RESOURCE IS AN INTERACTIVE STREAM, AND ONLY THE HOST CAN SAY WHICH IS
// ONE. The C library answers tcgetattr for its own terminal device on every
// standard descriptor, a pipe's included (stream.cpp), so each operation asks
// oke::host_terminal first and refuses any other stream with
// kal_err_not_supported, as the interface requires.
//
// KAL_TERM_SEQUENCES IS SET WHEREVER THERE IS A TERMINAL. The only terminal a
// module reaches is node's, which writes the bytes to the host's terminal; that
// interprets ECMA-48 (on Windows libuv interprets it where the console does
// not). A request to clear the position is ignored, and get_mode reports it
// set again, as the specification says of such an environment.
extern "C" {

constexpr tcflag_t kAlters = ISTRIP | INLCR | IGNCR | ICRNL;

int kal_terminal_get_mode(kal_stream s, kal_uintptr* mode) {
    if (mode == nullptr) return kal_err_invalid;
    if (!oke::host_terminal(static_cast<int>(s.h))) return kal_err_not_supported;
    struct termios t{};
    if (::tcgetattr(static_cast<int>(s.h), &t) != 0) return oke::last();
    kal_uintptr m = 0;
    if ((t.c_lflag & ICANON) != 0) m |= KAL_TERM_LINE_EDIT;
    if ((t.c_lflag & ECHO)   != 0) m |= KAL_TERM_ECHO;
    // KAL_TERM_PASS_CONTROL IS READ FROM THREE FLAGS AND NOT FROM ISIG. The
    // position states that the environment reserves NO keystroke, so it is set
    // only where every mechanism by which this environment reserves one is off:
    // ISIG for the interrupt and its neighbours, IXON for the pair that stops
    // and starts output, IEXTEN for the one that takes the next keystroke
    // literally. Under node these are the host's own; in a browser there is no
    // terminal and the property word says so before any of this is reached.
    if ((t.c_lflag & (ISIG | IEXTEN)) == 0 &&
        (t.c_iflag & IXON) == 0)   m |= KAL_TERM_PASS_CONTROL;
    // KAL_TERM_PASS_INPUT IS READ FROM FOUR FLAGS, for the same reason: it
    // states that nothing typed is altered, which holds only where every
    // mechanism that alters input --- the high bit cleared, a line feed or a
    // carriage return rewritten into the other, a carriage return discarded ---
    // is off. Under node these are the host's own.
    if ((t.c_iflag & kAlters) == 0) m |= KAL_TERM_PASS_INPUT;
    m |= KAL_TERM_SEQUENCES;
    *mode = m;
    return kal_ok;
}

int kal_terminal_set_mode(kal_stream s, kal_uintptr mode) {
    struct termios t{};
    const int fd = static_cast<int>(s.h);
    if (!oke::host_terminal(fd)) return kal_err_not_supported;
    if (::tcgetattr(fd, &t) != 0) return oke::last();
    const bool reserved_none = (t.c_lflag & (ISIG | IEXTEN)) == 0 &&
                               (t.c_iflag & IXON) == 0;
    const bool altered_none  = (t.c_iflag & kAlters) == 0;
    // READ, MODIFY, WRITE, AND ONLY THE POSITIONS openkal NAMES. A mode
    // composed from scratch would silently reset every other attribute of the
    // terminal -- the special characters, the baud rate -- none of which this
    // interface claims to own.
    if (mode & KAL_TERM_LINE_EDIT) t.c_lflag |=  ICANON; else t.c_lflag &= ~ICANON;
    if (mode & KAL_TERM_ECHO)      t.c_lflag |=  ECHO;   else t.c_lflag &= ~ECHO;

    // A POSITION WHOSE REQUESTED VALUE IS THE ONE IN EFFECT IS NOT WRITTEN.
    // This one stands for three flags, so establishing it again would settle
    // two mechanisms the caller never asked about.
    if (((mode & KAL_TERM_PASS_CONTROL) != 0) != reserved_none) {
        if (mode & KAL_TERM_PASS_CONTROL) {
            t.c_lflag &= ~(ISIG | IEXTEN);
            t.c_iflag &= ~IXON;
        } else {
            t.c_lflag |=  (ISIG | IEXTEN);
            t.c_iflag |=   IXON;
        }
    }

    // The same rule for the position that stands for four flags. Clearing it
    // restores the rewriting of a carriage return alone, which is what a
    // terminal ordinarily applies.
    if (((mode & KAL_TERM_PASS_INPUT) != 0) != altered_none) {
        if (mode & KAL_TERM_PASS_INPUT) t.c_iflag &= ~kAlters;
        else                            t.c_iflag |=  ICRNL;
    }
    // AND A MODE IS NOT A WAY TO END THE INPUT. With line assembly off, how
    // long a read waits is decided by VMIN and VTIME, and a terminal left at
    // VMIN=0 makes `kal_stream_read' report zero --- which clause 7.4 says
    // denotes the end of the input. A caller that wants a read which gives up
    // asks `kal_timeout_read' for one.
    if ((mode & KAL_TERM_LINE_EDIT) == 0) {
        t.c_cc[VMIN]  = 1;
        t.c_cc[VTIME] = 0;
    }
    // TCSANOW and not TCSADRAIN: openkal's caller has just been told what the
    // mode is and is entitled to have it take effect before its next read.
    if (::tcsetattr(fd, TCSANOW, &t) != 0) return oke::last();
    return kal_ok;
}

int kal_terminal_size(kal_stream s, kal_uintptr* cols, kal_uintptr* rows) {
    if (!oke::host_terminal(static_cast<int>(s.h))) return kal_err_not_supported;
    struct winsize ws{};
    if (::ioctl(static_cast<int>(s.h), TIOCGWINSZ, &ws) != 0) return oke::last();
    // A ZERO DIMENSION IS NOT A SIZE. Emscripten answers the ioctl with a
    // zeroed structure where the host reports nothing, and a caller that
    // divided by the width would then fault; reporting that the size is not
    // available is the honest answer and matches the property word.
    if (ws.ws_col == 0 || ws.ws_row == 0) return kal_err_not_supported;
    if (cols != nullptr) *cols = ws.ws_col;
    if (rows != nullptr) *rows = ws.ws_row;
    return kal_ok;
}

// ASKED OF THE MACHINE, NOT WRITTEN DOWN. The two words are the two
// operations, and each is claimed only if it just answered -- which is what
// makes this word true in a browser and in a terminal without a build-time
// switch between them.
kal_uintptr kal_terminal_props(kal_stream s) {
    kal_uintptr p = 0;
    if (!oke::host_terminal(static_cast<int>(s.h))) return p;
    struct termios t{};
    if (::tcgetattr(static_cast<int>(s.h), &t) == 0) p |= KAL_TERM_PROP_MODE;
    struct winsize ws{};
    if (::ioctl(static_cast<int>(s.h), TIOCGWINSZ, &ws) == 0
        && ws.ws_col != 0 && ws.ws_row != 0) p |= KAL_TERM_PROP_SIZE;
    return p;
}

}
