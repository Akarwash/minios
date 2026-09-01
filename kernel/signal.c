#include "signal.h"
#include "scheduler.h"

// ============================================================================
// The raising half of signals.
// ============================================================================
// Raising a signal SETS A BIT AND RETURNS. It does not run a handler, does not
// kill anything, and does not switch tasks. That is what makes it callable from
// the keyboard IRQ, which is where Ctrl-C comes from and which is running on
// somebody else's stack with interrupts off.
//
// The other half — turning a pending bit into a dead task or a running handler —
// is check_signals, and it lands in the next stage. Until then a raised signal is
// recorded and nothing acts on it, which is deliberate: the bit is the interface
// between "something happened" and "the program finds out", and the two halves
// are worth building and reading separately.

void signal_raise(uint32_t id, int sig) {
    if (sig <= 0 || sig >= MAX_SIGS) {
        return;                       // not a signal this kernel has
    }

    task_t *t = scheduler_task_by_id(id);
    if (t == NULL) {
        return;                       // no such task, or a reaped hole
    }

    // A ZOMBIE CANNOT BE SIGNALLED. It has already exited; the struct survives only
    // as a tombstone holding an exit status for its parent to read. Setting a bit
    // here would record a signal for a task that will never run again to receive it,
    // and `kill` on a dead id would report success rather than failure.
    if (t->state == TASK_ZOMBIE) {
        return;
    }

    // One bit per signal. Raising the same signal twice before it is delivered
    // collapses to one delivery, which is what every Unix does: the pending set is a
    // SET, not a queue, so holding Ctrl-C does not build a backlog of handler runs.
    t->sig_pending |= (1u << sig);
}
