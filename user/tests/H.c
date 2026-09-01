// H.ELF: a program that CATCHES Ctrl-C instead of dying from it.
//
// Every other fixture here proves something about how a program is started, fed, or
// stopped. This one proves the opposite: that a program can be interrupted and put
// back. It registers a SIG_INT handler, then loops printing like B.ELF, and the run
// to look at is `run h.elf` followed by Ctrl-C several times — each one should print
// one handler line, the H's should carry on from exactly where they stopped, and the
// program should finish on its own and exit 0.
//
// WHAT IS BEING TESTED IS THE RESUME, NOT THE HANDLER. Printing a line from a
// handler only shows that the kernel forged a call frame and jumped to it. The
// interesting half is what happens when the handler returns: the trampoline raises
// SYS_SIGRETURN, the kernel copies the saved context back over the live register
// frame, and the loop continues with its counter intact. If the count came out short
// or the program drifted, the restore would be wrong — so the loop is bounded and
// the total is printed at the end, where a wrong number is visible rather than
// plausible.
//
// The handler is deliberately as small as a handler can be. It runs on the program's
// own stack, in the middle of whatever the loop was doing, so anything it touches
// beyond printing would be testing that as well as the delivery.

#include "../userlib.h"

#define H_ROUNDS  40

// Bumped by the handler, read by the main loop. VOLATILE IS LOAD-BEARING: the
// handler is called by the kernel, not by any code the compiler can see, so from the
// compiler's point of view nothing ever writes this. Without volatile it is entitled
// to cache the value in a register and print a stale one at the end.
static volatile unsigned long handled;

static void print_ulong(unsigned long v) {
    char buf[21];
    int i = 20;
    buf[i] = '\0';
    do {
        buf[--i] = (char)('0' + (v % 10));
        v /= 10;
    } while (v != 0);
    sys_print(&buf[i]);
}

// The SIG_INT handler. `sig` is the signal number the kernel passed in RDI; it is
// unused because this program only ever registers one handler, and naming it keeps
// the signature the one sys_signal expects.
static void on_interrupt(int sig) {
    (void)sig;
    handled++;
    sys_print("\n[H caught SIG_INT]\n");
}

void _start(void) {
    // Install the handler BEFORE the loop starts. A signal arriving in the window
    // before this call would find no handler and take the default action, which is to
    // kill this program — so there must be no useful work above it.
    if (sys_signal(SIG_INT, on_interrupt) == (unsigned long)-1) {
        sys_print("H: could not install a SIG_INT handler\n");
        sys_exit(1);
    }

    for (unsigned long i = 0; i < H_ROUNDS; i++) {
        sys_print("H");
        user_delay();
    }

    // The count is the point. It says how many times the loop was interrupted and
    // resumed, and that it ran all forty rounds regardless.
    sys_print("\nH: finished, caught ");
    print_ulong(handled);
    sys_print(" interrupts\n");
    sys_exit(0);
}
