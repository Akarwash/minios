#ifndef KERNEL_SIGNAL_H
#define KERNEL_SIGNAL_H

#include "isr.h"                  // registers_t
#include "../include/types.h"
#include "../include/signals.h"   // SIG_INT and friends, shared with ring 3

// ============================================================================
// Raising and delivering signals.
// ============================================================================
// Raising is cheap and can happen anywhere, including inside an interrupt: it
// only sets a bit. Delivery is the expensive, dangerous half, and it happens in
// exactly one place, on the way back out to ring 3. Keeping the two apart is what
// makes it safe for the keyboard IRQ to raise a signal on a task that is not
// running, and for a syscall handler to raise one on itself.
//
// See docs/reference/signals.md, and docs/decisions/0023-signals.md for the eight
// ways this goes wrong and the lines that stop each one.

// Raise `sig` on one task by id. A no-op if the id names no live task, or names a
// zombie: a task that has already exited cannot be signalled, and pretending
// otherwise would leave bits set on a tombstone.
//
// Safe to call from interrupt context. It sets a pending bit and, for a BLOCKED
// task, readies it so the signal can be delivered on its way out of the kernel;
// it never switches tasks and never touches a register pile.
void signal_raise(uint32_t id, int sig);

#endif
