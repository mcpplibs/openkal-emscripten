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
extern "C" {

int kal_terminal_get_mode(kal_stream s, kal_uintptr* mode) {
    if (mode == nullptr) return kal_err_invalid;
    struct termios t{};
    if (::tcgetattr(static_cast<int>(s.h), &t) != 0) return oke::last();
    kal_uintptr m = 0;
    if ((t.c_lflag & ICANON) != 0) m |= KAL_TERM_LINE_EDIT;
    if ((t.c_lflag & ECHO)   != 0) m |= KAL_TERM_ECHO;
    *mode = m;
    return kal_ok;
}

int kal_terminal_set_mode(kal_stream s, kal_uintptr mode) {
    struct termios t{};
    const int fd = static_cast<int>(s.h);
    if (::tcgetattr(fd, &t) != 0) return oke::last();
    // READ, MODIFY, WRITE, AND ONLY THE TWO BITS openkal NAMES. A mode
    // composed from scratch would silently reset every other attribute of the
    // terminal -- flow control, the special characters, the baud rate -- none
    // of which this interface claims to own.
    if (mode & KAL_TERM_LINE_EDIT) t.c_lflag |=  ICANON; else t.c_lflag &= ~ICANON;
    if (mode & KAL_TERM_ECHO)      t.c_lflag |=  ECHO;   else t.c_lflag &= ~ECHO;
    // TCSANOW and not TCSADRAIN: openkal's caller has just been told what the
    // mode is and is entitled to have it take effect before its next read.
    if (::tcsetattr(fd, TCSANOW, &t) != 0) return oke::last();
    return kal_ok;
}

int kal_terminal_size(kal_stream s, kal_uintptr* cols, kal_uintptr* rows) {
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
    struct termios t{};
    if (::tcgetattr(static_cast<int>(s.h), &t) == 0) p |= KAL_TERM_PROP_MODE;
    struct winsize ws{};
    if (::ioctl(static_cast<int>(s.h), TIOCGWINSZ, &ws) == 0
        && ws.ws_col != 0 && ws.ws_row != 0) p |= KAL_TERM_PROP_SIZE;
    return p;
}

}
