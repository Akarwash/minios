#ifndef SIGNALS_H
#define SIGNALS_H

// ============================================================================
// Signal numbers, shared by the kernel and by ring-3 programs.
// ============================================================================
// A signal is what an interrupt is, one layer up. The CPU interrupts a program,
// saves its state, runs a handler, and puts it back; the kernel does the same to
// a ring-3 program, except that no hardware does any of it. Every step the CPU
// performs for an interrupt — save the frame, switch to the handler, restore on
// return — is written by hand in kernel/signal.c.
//
// This header holds NUMBERS ONLY, no kernel types and no code, because ring-3
// programs include it through user/userlib.h exactly as they include
// include/syscalls.h. Anything needing a kernel type belongs in kernel/signal.h.
//
// The numbers match the POSIX ones for the four signals this kernel has. They
// could have been 1, 2, 3, 4 — nothing here talks to another system — but a
// reader who knows that SIGINT is 2 and SIGKILL is 9 should not have to check,
// and a program's exit status of 130 (128 + SIG_INT) should mean on this machine
// what it means everywhere else. See docs/reference/signals.md.

#define SIG_INT   2    // interrupt: Ctrl-C, and the default `kill` signal. Catchable.
#define SIG_KILL  9    // kill: always the default action, never catchable. See below.
#define SIG_SEGV 11    // the kernel could not deliver safely; the task is killed
#define SIG_PIPE 13    // wrote to a pipe whose last reader has gone

// How many signals a task tracks. The pending set is a uint32_t bitmask, so 32 is
// the ceiling this representation allows as well as far more than the four above.
#define MAX_SIGS  32

// A task killed by signal N exits with status 128 + N, so Ctrl-C gives 130. This is
// the shell convention on every Unix, and it exists because an exit status has no
// room to say "this was a signal, not a return value": 128 + N carves out a range
// that an ordinary `exit(n)` is not expected to use. It is a convention, not a
// guarantee — a program CAN exit(130) itself — which is why the shell reports the
// number rather than claiming to know a signal caused it.
#define SIG_EXIT_STATUS(sig)  (128 + (sig))

#endif
