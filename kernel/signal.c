#include "signal.h"
#include "scheduler.h"
#include "syscall.h"      // SYSCALL_ERROR
#include "memory.h"       // user_range_ok, the ring-3 region bound
#include "gdt.h"          // the ring-3 selectors sigreturn forces
#include "usermode.h"     // USER_MODE_RFLAGS, for the IF bit sigreturn forces
#include "../libc/mem.h"  // memcpy

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

// The System V red zone: 128 bytes below RSP that a leaf function may use WITHOUT
// adjusting RSP at all. It is not scratch space — it holds live locals — and the
// compiler is entitled to assume nothing else touches it.
#define RED_ZONE_BYTES  128

// Forge the ring-3 call frame that runs `sig`'s handler, and point the task at it.
//
// This is the interesting part of the whole rung: it is, step for step, what the
// CPU does in hardware when it takes an interrupt — push enough state to resume,
// then jump to the handler — except that no hardware does any of it for a signal,
// so each step is written out here. Three of the eight bug classes are in these
// twenty lines and none of them announces itself.
static void deliver_to_handler(registers_t *r, task_t *t, int sig) {
    uint64_t sp = r->user_rsp;

    // (S2) STEP OVER THE RED ZONE FIRST, before computing anything else.
    //
    // The ABI lets a leaf function keep live locals in the 128 bytes below RSP
    // without adjusting RSP, precisely so short functions need no prologue. Building
    // the signal frame at user_rsp writes straight over them. The handler then runs
    // perfectly, returns, and the interrupted function carries on computing with
    // locals that were quietly replaced by pieces of a saved register frame — a
    // wrong answer, later, in code that has nothing to do with signals. Linux does
    // exactly this subtraction, for exactly this reason.
    sp -= RED_ZONE_BYTES;

    // (S3) ALIGN BEFORE PUSHING THE RETURN ADDRESS, not after.
    //
    // The ABI's rule is that RSP % 16 == 8 at a function's first instruction —
    // 16-byte aligned before the `call`, and the call's 8-byte push is what leaves
    // the 8. Reproducing that means aligning to 16 here and then pushing the fake
    // return address below, so the handler sees exactly the stack a real call site
    // would have produced. Get it wrong and the handler faults on entry, or much
    // more confusingly survives until the first SSE instruction the compiler emits
    // for something as ordinary as copying a struct.
    sp &= ~0xFULL;

    // Room for the saved context, which sigreturn will copy back.
    sp -= sizeof(registers_t);

    // (S4) BOUNDS-CHECK THE WHOLE SPAN, BEFORE WRITING ANY OF IT.
    //
    // user_rsp came from ring 3 and is not to be trusted: it is whatever the program
    // last put in RSP. The kernel is about to write a whole registers_t plus an
    // 8-byte return address through it, so the entire span is checked, not just its
    // start — a pointer sitting just below USER_REGION_END passes a start-only check
    // and then writes off the end of the region into kernel pages. This is the
    // confused deputy: the kernel has privileges the program does not, and the
    // program is choosing the address.
    //
    // A task whose stack pointer cannot take the frame CANNOT BE SIGNALLED SAFELY,
    // so it dies rather than the kernel writing somewhere else. That is what a real
    // system does too: a stack too broken to deliver on is a segmentation fault.
    if (!user_range_ok(sp, sizeof(registers_t) + 8)) {
        task_exit(r, SIG_EXIT_STATUS(SIG_SEGV));
        return;
    }

    // Save the interrupted context where sigreturn can find it. The destination is
    // in the task's own mapped pages, which its CR3 is currently loaded with — this
    // runs before any switch, on the interrupted task's own address space.
    memcpy((void *)sp, r, sizeof(registers_t));
    uint64_t ctx = sp;

    // The fake return address: the handler is an ordinary C function and will end
    // with `ret`, so something has to be there for it to pop. The trampoline turns
    // that `ret` into SYS_SIGRETURN.
    sp -= 8;
    *(uint64_t *)sp = t->sig_trampoline;

    // Point the task at the handler. From the program's side this is indistinguishable
    // from having been called normally: first argument in RDI, a return address on
    // the stack, correct alignment.
    r->rip      = t->sig_handlers[sig];
    r->rdi      = (uint64_t)sig;      // the handler's one argument: which signal
    r->user_rsp = sp;

    t->sig_pending &= ~(1u << sig);   // delivered; do not deliver it again
    t->sig_active   = 1;              // (S6) no second frame on top of this one
    t->sig_ctx      = ctx;            // where sigreturn restores from
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

    // A handler is installed. Everything below builds, by hand, the context the CPU
    // builds in hardware for an interrupt: save the current state somewhere the
    // resume path can find it, then point execution at the handler.
    deliver_to_handler(r, t, sig);
}

int signal_install_handler(task_t *t, int sig, uint64_t handler, uint64_t trampoline) {
    // SIG_KILL is not installable, for the same reason check_signals ignores a
    // handler for it: a signal a program can catch is a request, and there has to be
    // one that is not. Rejecting it here as well as ignoring it there means a program
    // finds out it cannot be done, rather than believing it succeeded.
    if (sig <= 0 || sig >= MAX_SIGS || sig == SIG_KILL) {
        return -1;
    }

    // Both addresses are about to become instruction pointers the kernel jumps ring-3
    // execution to, so both must lie in the ring-3 region. This is cheap and it is
    // the last chance to reject them: after this they are just numbers in a task
    // struct, and the next thing that happens to them is `r->rip = handler`.
    //
    // A handler of 0 is the way to ask for the DEFAULT action back, so it skips the
    // range check — 0 is not an address here, it is the absence of one.
    if (handler != 0 && !user_range_ok(handler, 1)) {
        return -1;
    }
    if (!user_range_ok(trampoline, 1)) {
        return -1;
    }

    t->sig_handlers[sig] = handler;
    t->sig_trampoline = trampoline;
    return 0;
}

void signal_sigreturn(registers_t *r) {
    task_t *t = scheduler_current_task();

    // (S7) IS THERE ACTUALLY A HANDLER RUNNING?
    //
    // This call OVERWRITES THE ENTIRE SAVED REGISTER FRAME from user memory. That is
    // its whole job, and it is also a privilege-escalation primitive if a program can
    // reach it whenever it likes: the frame includes CS and RFLAGS, so a program that
    // called sigreturn directly, having arranged its own bytes at sig_ctx, would be
    // choosing the privilege level and flags it resumes with.
    //
    // sig_active is what locks it shut. It is set only by delivery and cleared only
    // here, so this call is reachable exactly once per delivered signal. A program
    // that calls it at any other moment is doing something it has no legitimate
    // reason to do, and is killed rather than obliged.
    if (!t->sig_active) {
        task_exit(r, SIG_EXIT_STATUS(SIG_SEGV));
        return;
    }

    // The saved context is in user memory and the task has had a whole handler's
    // worth of execution in which to scribble on it, so it is range-checked again
    // here rather than trusted because it was valid when it was written.
    if (!user_range_ok(t->sig_ctx, sizeof(registers_t))) {
        task_exit(r, SIG_EXIT_STATUS(SIG_SEGV));
        return;
    }

    // t->sig_ctx, NOT r->user_rsp. The kernel remembers where it put the context, so
    // a handler that moved its own stack pointer — deliberately or through a bug —
    // cannot redirect the restore to bytes of its choosing. The saved location is
    // kernel state; the stack pointer is not.
    memcpy(r, (const void *)t->sig_ctx, sizeof(registers_t));

    // DO NOT TRUST THESE THREE, EVEN NOW. Everything just copied came out of user
    // memory, and the checks above only established that the memory is inside the
    // ring-3 region — not that its contents are the frame the kernel wrote there. A
    // handler can rewrite its own saved context before returning.
    //
    // So the fields that decide PRIVILEGE are not taken from it. CS and SS are forced
    // to the known ring-3 selectors, so a restored frame cannot resume in ring 0
    // however it was doctored, and IF is forced on, so a program cannot resume with
    // interrupts disabled and keep the CPU forever. Everything else — the general
    // registers, RIP, RSP — is the program's own state and is its business.
    r->cs      = GDT_SELECTOR_USER_CODE;
    r->ss      = GDT_SELECTOR_USER_DATA;
    r->rflags |= USER_MODE_RFLAGS;

    t->sig_active = 0;    // the handler is done; further signals may be delivered
    t->sig_ctx    = 0;
}
