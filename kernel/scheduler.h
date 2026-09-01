#ifndef SCHEDULER_H
#define SCHEDULER_H

#include "isr.h"
#include "paging.h"
#include "file.h"
#include "../include/types.h"

// ============================================================================
// A round-robin preemptive scheduler.
// ============================================================================
// The trick that makes this work: the register pile the interrupt stub pushes
// onto the kernel stack (a registers_t, see isr.h) IS the task. It holds every
// GPR plus the rip/cs/rflags/rsp/ss that iretq will restore. To switch tasks we
// (1) save the interrupted pile into the current task's slot, then (2) copy a
// DIFFERENT task's slot over the SAME live pile, so the stub restores what it
// thinks is the same program but is really the next one. See schedule() and
// docs/reference/scheduling.md.

// A generous, arbitrary cap on how many tasks we TRACK. This is NOT the old
// storage ceiling: the task_t structs are now heap-allocated (kmalloc, see
// task_register in scheduler.c), so the kernel heap that used to be missing
// exists. This only
// bounds the size of the pointer-bookkeeping array in scheduler.c, not where the
// tasks live. 64 is arbitrary; raise it freely.
#define MAX_TASKS_LIMIT 64

// How many descriptors a task can hold open at once. A small fixed array in each
// task_t (fds below), not growable: 8 is plenty for the shell (0 and 1 in use, up
// to two more per pipe in a pipeline) and keeps the table a flat, cheaply-scanned
// thing. Raising it is a one-line change; a program that needs more than a handful
// of open descriptors is not something this kernel runs.
#define MAX_FDS 8

// Report every lifecycle event that returns memory to the pools, with the free
// frame count after it. Set to 0 to silence the lot.
//
// ON BY DEFAULT, AND DELIBERATELY SO. These lines are the only leak test this
// kernel has: run the same thing ten times and the count printed after each must be
// identical from the second onwards. A slow monotonic decrease means something a
// dead or never-born task held is not coming back, and without the lines the only
// symptom is a machine that runs out of memory after a long session, with nothing
// to point at the cause.
//
// It lives in the header rather than in scheduler.c because two files print under
// it now: scheduler.c reports a task being reaped, and syscall.c reports a `run`
// that failed to create one. The second is not a lifecycle event in the usual sense
// but is measured the same way and for the same reason, since a failed create had
// been quietly stranding a whole address space every time.
#define LIFECYCLE_DEBUG 1

typedef enum {
    TASK_UNUSED = 0,   // slot never filled (.bss zero-init lands here)
    TASK_READY,        // runnable, waiting for its slice
    TASK_RUNNING,      // currently on the CPU
    TASK_BLOCKED,      // waiting for an event, skipped by the rotation entirely
    TASK_ZOMBIE        // exited; off the rotation for good. Heavy resources may already
                       // be freed; the struct survives only as a tombstone holding
                       // exit_status until the parent reads it.
} task_state_t;

// WHAT a blocked task is waiting for, so the right waker can find it. A task with
// nothing to do is not enough on its own: when a keypress arrives, the keyboard
// IRQ has to be able to pick out the tasks waiting for a KEY and leave alone the
// ones waiting for something else. The reason is that discriminator.
//
// Two reasons today, and they show the shape generalises: the waker does not have
// to be a driver. WAIT_KEY is woken by the keyboard IRQ, an outside event arriving;
// WAIT_CHILD is woken by task_exit, another TASK reaching a point in its own life.
// Both are "a task cannot proceed until something else happens", which is why one
// mechanism serves both. The enum (rather than a bare flag) is still the seam where
// the next reason slots in: WAIT_DISK when a task waits on a block to arrive. Each
// new reason gets a waker at whatever causes that event.
typedef enum {
    WAIT_NONE = 0,     // not waiting for anything (the only valid value when READY)
    WAIT_KEY,          // waiting for a keypress, woken by the keyboard IRQ
    WAIT_CHILD,        // waiting for a child to exit, woken by task_exit
    WAIT_PIPE_READ,    // waiting to read a pipe (empty, writer alive), woken by a write or the last writer closing
    WAIT_PIPE_WRITE    // waiting to write a pipe (full, reader alive), woken by a read or the last reader closing
} wait_reason_t;

// The parent id of a task nobody started: task 0, which kernel_main creates before
// any task exists to be its parent. It is deliberately an impossible id rather than
// a valid one (0 would mean "task 0 is its own parent", and reaping would then look
// for a tombstone reader that is really itself). parent_alive rejects it outright,
// so an exiting task with no parent is reaped by the sweeper rather than waiting
// forever for a wait() that nobody can issue.
#define TASK_NO_PARENT  0xFFFFFFFFu

// One task is its saved register pile plus a little bookkeeping. The pile is the
// context to restore; the address space is the memory the task runs in. Each task
// now owns a private page-table tree (per-process paging), so two tasks can use
// the same virtual address for different physical memory. It still has no kernel
// stack of its own (see the limitations in the ADR).
typedef struct task {
    registers_t regs;         // the saved/forged interrupt frame: IS the task

    // This task's private page-table tree, and the CR3 value that loads it.
    //
    // BOTH BECOME ZERO ONCE THE TASK HAS BEEN SWEPT. A task that exits is left as a
    // TASK_ZOMBIE, and the reap sweeper (scheduler.c) later frees its whole tree and
    // then sets `aspace = NULL` and `cr3 = 0`, because the memory those two fields
    // described has gone back to the frame pool and will be handed to somebody else.
    // Leaving a stale pointer here would be a use-after-free waiting to be loaded
    // into CR3. NULL is also the flag the two freeing paths (the sweeper and
    // task_wait) test to decide whether the tree still needs tearing down, so
    // whichever runs first wins and the other does nothing.
    address_space_t *aspace;  // private page-table tree, NULL once swept
    uint64_t cr3;             // physical PML4 base to load on switch (== aspace->pml4_phys), 0 once swept

    task_state_t state;
    wait_reason_t wait_reason;  // meaningful only while state == TASK_BLOCKED
    uint32_t id;

    // Who called SYS_RUN to create this task, or TASK_NO_PARENT. This is what makes
    // the task table a forest rather than a flat list, and it is read by exactly two
    // things: task_wait, to find the caller's children, and the sweeper, to decide
    // whether a zombie still has somebody who might read its exit status.
    uint32_t parent_id;

    // The status this task passed to SYS_EXIT, masked to 0..255. MEANINGFUL ONLY
    // WHILE state == TASK_ZOMBIE: before the exit it is 0 and means nothing, and
    // after the parent has read it the struct is freed. The mask is what keeps a
    // status distinguishable from the SYSCALL_ERROR (-1) that SYS_WAIT returns when
    // the caller has no children at all.
    int32_t exit_status;

    // This task's PROCESS GROUP: the unit Ctrl-C acts on, rather than the single
    // task. A pipeline is several tasks that are one job to the person who typed it,
    // and interrupting the job has to reach all of them, so the group is what a
    // signal from the keyboard is addressed to.
    //
    // Inherited from the parent unless SYS_RUN is asked for something else, so a
    // program that knows nothing about groups is simply in its parent's. Task 0 (the
    // shell) is in group 0. A group created by SYS_RUN_GROUP_NEW is NAMED AFTER ITS
    // LEADER — the pgid is the leading task's own id — which is the Unix convention
    // and, more usefully here, guarantees uniqueness for free: task ids are never
    // reused, so a group id can never collide with a live group.
    uint32_t pgid;

    // The set of signals raised on this task and not yet delivered, one bit per
    // signal number (see include/signals.h). A SET, NOT A QUEUE: raising SIG_INT
    // twice before either is delivered leaves one bit and produces one delivery,
    // which is why holding Ctrl-C does not build a backlog of handler runs.
    //
    // Written by signal_raise, which may run in interrupt context on another task's
    // stack, and cleared at delivery. Nothing here needs a lock: this kernel is
    // single-CPU and every path that touches it runs with IF clear (an interrupt
    // gate cleared it on entry).
    uint32_t sig_pending;

    // This task's open descriptors, indexed 0..MAX_FDS-1, NULL where unused. fd 0
    // and fd 1 are a console by convention (input and output); a child in a pipeline
    // has one or both replaced by an inherited pipe end. The number in a descriptor
    // is an index into THIS task's table only — it means nothing in another task's.
    // See kernel/file.h and docs/reference/descriptors.md.
    file_t *fds[MAX_FDS];
} task_t;

// Forge a never-run task from a program FILE: read `name` (an 8.3 filename such
// as "A.ELF") off the FAT32 volume, load its ELF segments into a fresh private
// address space, map a fresh stack at the fixed stack VA, and fill its saved
// pile so it looks like it was interrupted at its first instruction (the entry
// point from the file's ELF header), then mark it TASK_READY. Because the
// address space is private, every task's stack sits at the SAME virtual address
// on different physical frames.
//
// `parent_id` is the task that asked for this one (SYS_RUN passes the caller's id
// via scheduler_current_id), or TASK_NO_PARENT for a task the kernel started
// itself before there was any task to be its parent. It is recorded now because it
// cannot be recovered later: it is what lets the creator wait for this task to
// exit, and what tells the sweeper whether anybody is still going to read this
// task's exit status.
//
// `in_fd` and `out_fd` are the descriptors in the CALLER's table to inherit as the
// child's fd 0 and fd 1, or -1 to give the child a fresh console end there (the
// default). This is the ONLY way a child acquires a descriptor: nothing can inject
// one into a running task, so a pipeline passes ends here at creation. in_fd must be
// a read end and out_fd a write end; a wrong-direction or out-of-range fd fails the
// create. -1/-1 (kernel_main's boot task, and an ordinary `run`) inherits nothing.
//
// Returns the task id, or -1 if the file is missing, is not a program this
// kernel accepts, the heap or frame pool is out of memory, or an inherited fd is
// bad. A failed load creates no task, leaks nothing (any inherited-end count it took
// is undone), and must not disturb the ones that succeeded. Implemented in
// scheduler.c.
//
// `pgid_req` says which process group the child joins:
//   TASK_PGID_INHERIT (0)  the parent's group, which is what a caller that knows
//                          nothing about groups gets, and the old behaviour.
//   TASK_PGID_NEW          a new group led by this child, so pgid == its own id.
//   anything else          that existing group, permitted only under the same rule
//                          scheduler_set_foreground enforces (the caller's own
//                          group, or a group one of its children is already in).
//                          This is how a shell puts every stage of a pipeline into
//                          the one group: the first stage leads it, the rest join.
// A request the caller is not allowed to make fails the create rather than silently
// falling back to inheritance, so a shell cannot half-build a job group and not know.
int task_create_from_file(const char *name, uint32_t parent_id, int in_fd, int out_fd,
                          uint32_t pgid_req);

// The two special values of task_create_from_file's `pgid_req`. 0 is safe as
// "inherit" because group 0 is task 0's own group and no other task can ever lead
// it: pgids name their leader, and task 0 is the only task with id 0.
#define TASK_PGID_INHERIT  0u
#define TASK_PGID_NEW      0xFFFFFFFFu

// Pick task 0 and enter it. Does not return (control only ever comes back into
// the kernel through an interrupt, where schedule() runs).
void scheduler_start(void);

// Block the current task on `reason` and switch away NOW, rather than waiting for
// the next timer tick. `r` is the live on-stack pile of the syscall the caller is
// serving, the same kind of frame the timer hands schedule().
//
// CALLABLE ONLY FROM A SYSCALL HANDLER. The task is resumed by re-entering the
// syscall from the top (see the re-arm in scheduler.c), which is only meaningful
// for a task that arrived through `int 0x50`. Today the one caller is SYS_READKEY.
//
// This does not return in any useful sense: control leaves through the redirected
// iretq, and this kernel entry is over. Nothing a caller writes after it runs on
// the blocking path.
void task_block(registers_t *r, wait_reason_t reason);

// Make every task blocked on `reason` runnable again. Called by whatever CAUSES
// the event, which today means the keyboard IRQ calling scheduler_wake(WAIT_KEY)
// once it has a key to hand over.
//
// This only changes state; it does NOT switch tasks. A woken task goes back into
// the rotation and the next ordinary schedule() picks it up. That keeps the wake
// safe to call from interrupt context, where switching would be wrong.
void scheduler_wake(wait_reason_t reason);

// End the current task with `status` and switch away for good. `r` is the live
// on-stack pile of the SYS_EXIT the caller is serving.
//
// CALLABLE ONLY FROM A SYSCALL HANDLER, for the same reason task_block is: it
// hands `r` straight to schedule(), which overwrites that pile in place with
// another task's, so `r` must be the live interrupt frame the stub is about to pop
// rather than a copy. Reached any other way it would redirect a frame nobody is
// going to iretq from.
//
// Unlike task_block this does NOT rewind rip: the task is never going to run
// again, so there is nothing to re-issue. It does paperwork only (status, state,
// waking a waiting parent) and frees NOTHING, because the dying task's page tables
// are still loaded in CR3 at this moment; the freeing happens later, from the
// sweeper in schedule(), once the switch has moved CR3 off this task's tree.
//
// Does not return in any useful sense: control leaves through the redirected
// iretq into a different task, and this kernel entry is over.
void task_exit(registers_t *r, int status);

// Block the caller until one of its children exits, and deliver that child's exit
// status in `r->rax`. If a child has ALREADY exited this returns immediately with
// its status; if the caller has no children at all it returns SYSCALL_ERROR rather
// than blocking forever on something that can never happen.
//
// CALLABLE ONLY FROM A SYSCALL HANDLER, and for a sharper reason than task_exit:
// on the waiting path this calls task_block, which rewinds the saved rip by the
// length of `int 0x50` so the woken task re-issues SYS_WAIT. A caller that arrived
// any other way would have its rip wound back into the middle of whatever precedes
// it. It also inherits task_block's rule that the handler's work must be safe to
// redo from the top, which it is: the scan is a pure read of the task table.
//
// This is any-child, not waitpid: it takes no argument and reaps whichever child
// exited first. See docs/decisions/0018-process-lifecycle-exit-and-wait.md.
void task_wait(registers_t *r);

// The id of the task currently on the CPU. Exists so a syscall handler can record
// who made a request (SYS_RUN stamps the new task's parent_id with it) without
// scheduler.c having to export the whole task table.
uint32_t scheduler_current_id(void);

// The foreground process group: the one the keyboard's Ctrl-C is addressed to.
// Group 0, the shell's, at boot.
//
// DECLARED, NOT INFERRED. It is not "the group of the task most recently started",
// and D.ELF is the proof that it cannot be: D starts E and exits without waiting, so
// an inferred foreground would follow to E and stay there while the user sits at a
// prompt, with Ctrl-C reaching a background program and never the shell. A task says
// which group is in front, and says it again when it stops being true. See
// scheduler_set_foreground and docs/decisions/0023-signals.md.
uint32_t scheduler_foreground_pgid(void);

// Make `pgid` the foreground group. Returns 0 on success, -1 if `caller_id` is not
// allowed to ask for it.
//
// THE PERMISSION RULE IS THE WHOLE POINT of this being a function rather than an
// assignment: a task may name only its OWN group, or a group held by at least one of
// its own children. Without that rule any program could take the keyboard and never
// give it back, and nothing could take it away again.
int scheduler_set_foreground(uint32_t caller_id, uint32_t pgid);

// How many task slots have ever been filled: the bound for a scan over the table.
// A HIGH-WATER MARK, NOT A LIVE COUNT — ids are never reused and reaped tasks leave
// permanent NULL holes, so every caller pairs this with a NULL check. Exposed for
// kernel/signal.c, which has to walk the table to find a whole process group.
uint32_t scheduler_task_count(void);

// The live task with this id, or NULL if the id is out of range or names a slot
// that has been reaped. Exposed so kernel/signal.c can raise a signal on a task
// that is NOT the running one — the whole point of a signal — without the task
// table leaving scheduler.c.
//
// The returned task may be a TASK_ZOMBIE: a tombstone is still a live slot, and
// the caller is the one that knows whether a dead task is an error for it.
task_t *scheduler_task_by_id(uint32_t id);

// The task currently on the CPU: the caller of whatever syscall is being served.
// Exposed (unlike the rest of the table) because the descriptor syscalls
// (SYS_READ/WRITE/CLOSE/PIPE) all operate on the CALLER's own fd table, and the
// table itself stays private to scheduler.c. Never call this outside a syscall
// handler: between syscalls `current` names whoever the round-robin last picked.
task_t *scheduler_current_task(void);

// The switch itself, called from the timer IRQ with a pointer to the live pile.
// Only TASK_READY tasks are candidates: a blocked task is skipped entirely, and if
// nothing at all is runnable this idles the CPU (see the hlt idle in scheduler.c)
// rather than spinning or resuming a task that is still waiting.
void schedule(registers_t *r);

#endif
