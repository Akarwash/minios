#ifndef KERNEL_SIGNAL_H
#define KERNEL_SIGNAL_H

#include "isr.h"                  // registers_t
#include "../include/types.h"
#include "../include/signals.h"   // SIG_INT and friends, shared with ring 3
#include "scheduler.h"            // task_t, which the handler-install call names

// scheduler.h does not include this header, so the pair does not cycle. The
// dependency runs one way on purpose: the scheduler owns the task struct and knows
// nothing about signals; signals are a thing done TO a task.

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

// Raise `sig` on EVERY live task in process group `pgid`. This is what the keyboard
// uses: Ctrl-C is addressed to a job, and a job is a group, because a three-stage
// pipeline is three tasks and one thing to the person who typed it. Interrupting
// only one of them would leave the other two running against a dead neighbour.
//
// Raising on a group that contains nothing is a silent no-op, which is the right
// answer for a job whose stages have all already exited.
void signal_raise_group(uint32_t pgid, int sig);

// Deliver at most one pending signal to the CURRENT task, if this frame is on its
// way back to ring 3. `r` MUST be the live interrupt frame the stub is about to pop,
// never a copy: delivery rewrites it in place, and the default action hands it to
// task_exit, which hands it to schedule().
//
// Called at the end of irq_handler AND at the end of the syscall dispatch. Both,
// not either: the IRQ path is what makes a Ctrl-C during a compute loop take effect,
// and the syscall path is what makes a signal raised by SYS_KILL take effect at once
// rather than waiting for the next timer tick.
//
// At most ONE signal per call. A second pending signal is delivered on the next way
// out, which for a killed task never comes and for a handled one is after the
// handler returns.
void check_signals(registers_t *r);

// Install `handler` (a ring-3 address, or 0 to restore the default action) for
// `sig` on task `t`, recording `trampoline` as the address a delivered handler
// returns through. Returns 0, or -1 for a signal that cannot be handled, or an
// address outside the ring-3 region. Backs SYS_SIGNAL.
int signal_install_handler(task_t *t, int sig, uint64_t handler, uint64_t trampoline);

// Restore the context saved when a handler was delivered, into the live frame `r`.
// Backs SYS_SIGRETURN, and is reachable only from the trampoline a handler returns
// through. DOES NOT RETURN NORMALLY in any useful sense: on success `r` now holds
// the interrupted context, so the stub's iretq resumes the program where the signal
// found it, not after this call. On abuse it kills the task instead.
void signal_sigreturn(registers_t *r);

#endif
