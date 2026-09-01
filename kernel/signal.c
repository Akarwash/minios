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

    // A BLOCKED TASK MUST BE WOKEN, OR THE SIGNAL NEVER ARRIVES. Delivery happens on
    // the way back out to ring 3, and a blocked task is not on its way anywhere: it
    // is off the rotation entirely, waiting for an event that a signal is not. Ctrl-C
    // on a task parked reading a pipe would set this bit and do nothing else, which
    // is the most likely way this whole rung silently fails to work.
    //
    // Waking alone is not enough, and this is the subtle half. task_block rewinds rip
    // onto the `int 0x50` so a woken task RE-ISSUES its syscall, and that re-arm does
    // not know why the wake happened: the task would re-run its read, find the pipe
    // still empty, and block again — a loop, with the signal pending forever.
    // sig_interrupted is what breaks it. task_block reads it at the top and reports
    // TASK_BLOCK_INTERRUPTED instead of parking, the syscall returns SYSCALL_ERROR,
    // and check_signals delivers as the task leaves the kernel.
    //
    // See S5 in docs/decisions/0023-signals.md and the matching comment in
    // task_block. This is the one interaction in the rung that touches the blocking
    // design directly, and it fails silently in both directions if either half is
    // missing.
    if (t->state == TASK_BLOCKED) {
        t->state = TASK_READY;
        t->wait_reason = WAIT_NONE;
        t->sig_interrupted = 1;       // the syscall was cut short, do not re-issue it
    }
}

void signal_raise_group(uint32_t pgid, int sig) {
    // A linear scan of the task table, like scheduler_wake's. Honest and fine at this
    // scale; a kernel with many tasks would thread a per-group list instead.
    //
    // THE WHOLE GROUP IS RAISED ON, not the first match. Every stage of a pipeline
    // gets the bit, because stopping one stage of a job and leaving the others is not
    // what Ctrl-C means: the survivors would block forever on a pipe whose far end
    // has gone, or keep printing at a prompt that has already come back.
    uint32_t n = scheduler_task_count();
    for (uint32_t id = 0; id < n; id++) {
        task_t *t = scheduler_task_by_id(id);
        if (t == NULL || t->pgid != pgid) {
            continue;                 // a reaped hole, or another job
        }
        signal_raise(id, sig);        // re-checks the zombie case for each task
    }
}

// ============================================================================
// check_signals: the delivering half.
// ============================================================================
// Called at the end of irq_handler and at the end of the syscall dispatch — every
// path by which the kernel returns to ring 3. Delivery happens HERE AND NOWHERE
// ELSE, on the way out, because that is the one moment when the register frame in
// front of us is a complete, consistent, about-to-be-restored ring-3 context. That
// is exactly what a signal has to redirect and later put back.
//
// The order of the checks below is not arbitrary; each is a distinct way this goes
// wrong, and two of them fail silently. See docs/decisions/0023-signals.md.
void check_signals(registers_t *r) {
    // (S1) IS THIS FRAME EVEN GOING BACK TO A PROGRAM?
    //
    // Privilege lives in the low two bits of CS, so this asks exactly that. This
    // function runs on every interrupt return, and interrupts nest: a timer tick can
    // land while the kernel is half way through a syscall, and that tick's frame
    // returns to KERNEL code, not to ring 3. Delivering there would forge a ring-3
    // handler frame on top of a half-finished kernel operation, with kernel state
    // live in the registers, and then "return" into user code that was never going to
    // run — abandoning the kernel work and corrupting whatever it was touching.
    //
    // The symptom is not a crash here. It is random corruption in an unrelated
    // subsystem, minutes later. Without this line nothing looks wrong for a long
    // time, which is what makes it the most dangerous of the eight.
    if ((r->cs & 3) != 3) {
        return;
    }

    task_t *t = scheduler_current_task();
    if (t == NULL || t->state == TASK_ZOMBIE) {
        return;                       // nothing to deliver to
    }

    // (S6) IS A HANDLER ALREADY RUNNING?
    //
    // If so, leave the bit pending and deliver it after the handler returns through
    // SYS_SIGRETURN, which clears this flag. Delivering now would forge a second
    // frame on top of the first, and a third on top of that under repeated Ctrl-C,
    // walking the user stack down until it runs off the bottom. The symptom is a page
    // fault at an address just below the stack, which points at the stack and not at
    // the signal code that consumed it.
    //
    // ONE FLAG, NOT PER-SIGNAL MASKS. A real sigaction has a mask per handler and
    // blocks only what the handler asked to block. This blocks everything for the
    // duration, which is cruder — a SIG_KILL arriving mid-handler waits for the
    // handler to finish rather than taking effect at once — and is recorded as a
    // limitation in the ADR rather than hidden.
    if (t->sig_active) {
        return;
    }

    if (t->sig_pending == 0) {
        return;                       // the common case, and the cheapest
    }

    // The LOWEST pending signal first. Any fixed order would do — nothing here
    // depends on the priority — but it must be deterministic, so that a task with two
    // signals pending behaves the same way every time rather than depending on which
    // bit the scan happened to reach first.
    int sig = 0;
    for (int i = 1; i < MAX_SIGS; i++) {
        if (t->sig_pending & (1u << i)) {
            sig = i;
            break;
        }
    }
    if (sig == 0) {
        return;                       // only bit 0, which is not a signal
    }

    // SIG_KILL IGNORES HANDLERS ENTIRELY. A signal a program can catch is a REQUEST,
    // and a system needs at least one that is not, or a program with a handler that
    // declines to exit could never be stopped by anything. This is why `kill -9`
    // means what it does on every Unix, and why the check is here, before the handler
    // is looked at, rather than being an extra condition inside it.
    if (t->sig_handlers[sig] == 0 || sig == SIG_KILL) {
        // The default action: kill the task, with the status 128 + signal number, so
        // Ctrl-C gives 130. Clear the bit first — task_exit does not return, and a
        // tombstone carrying a pending signal it can never receive is a lie.
        t->sig_pending &= ~(1u << sig);
        task_exit(r, SIG_EXIT_STATUS(sig));
        return;
    }

    // A handler is installed: forge a frame and run it. That is the next stage; until
    // then a task with a handler simply keeps its pending bit, which is visible and
    // harmless rather than silently wrong.
}
