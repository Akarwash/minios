# Blocking and sleep reference

A task that has nothing to do gives up the CPU entirely and is woken by whatever
causes the thing it was waiting for. This page documents the blocked state, the
re-arm trick that makes a block possible on a kernel with one shared kernel
stack, the idle path, and the block/wake pairing rule. Read from
`kernel/scheduler.c`, `kernel/scheduler.h`, `kernel/syscall.c`, and
`drivers/keyboard.c`. For the rationale and the alternatives rejected, see
[decision 0017](../decisions/0017-blocking-and-sleep.md).

## The state and the reason

`task_t` (`kernel/scheduler.h`) gained one state and one field:

```c
typedef enum {
    TASK_UNUSED = 0,
    TASK_READY,        // runnable, waiting for its slice
    TASK_RUNNING,      // currently on the CPU
    TASK_BLOCKED,      // waiting for an event, skipped by the rotation entirely
    TASK_ZOMBIE        // finished, kept only until someone collects its status
} task_state_t;

typedef enum {
    WAIT_NONE = 0,     // not waiting for anything
    WAIT_KEY,          // waiting for a keypress, woken by the keyboard IRQ
    WAIT_CHILD         // waiting for a child to exit, woken by that child's task_exit
} wait_reason_t;
```

The two carry different information and both are needed. The **state** takes the
task out of the rotation. The **reason** is what lets the right waker find it
again: a keypress should ready the tasks waiting for a key and leave a task
waiting for a child asleep.

`WAIT_CHILD` is the second reason, and it is the one that shows the mechanism is
general. Its waker is not a driver and not an interrupt: it is **another task**,
inside `task_exit`, on its way out. The pairing rule below does not care which —
"whoever causes the event" is the exiting child, so that is where the wake goes.
`WAIT_DISK` is the next obvious one and still a seam.

`wait_reason` is only meaningful while the task is `TASK_BLOCKED`. It is set to
`WAIT_NONE` when the task is created and cleared back to `WAIT_NONE` on wake.

## The scheduler skips, and idles

`find_next_ready` tests for `TASK_READY` rather than "not unused", so a blocked
task is stepped straight over however many times the round-robin cursor comes
round. `schedule()` also no longer sets the outgoing task `TASK_READY`
unconditionally:

```c
tasks[current]->regs = *r;
if (tasks[current]->state == TASK_RUNNING) {
    tasks[current]->state = TASK_READY;
}
```

The guard matters because `task_block` marks the current task `TASK_BLOCKED` and
*then* calls `schedule()`. An unconditional write here would put the task back
into the rotation and undo the block on the spot.

When nothing at all is runnable, `schedule()` parks rather than picking a blocked
task or spinning:

```c
int picked = find_next_ready(current);
if (picked < 0) {
    idle_until_runnable();
    picked = find_next_ready(current);   // now succeeds
}
```

```c
static void idle_until_runnable(void) {
    scheduler_idling = 1;
    while (!any_task_ready()) {
        __asm__ __volatile__("sti; hlt; cli");
    }
    scheduler_idling = 0;
}
```

Two things about that loop are load-bearing.

**Interrupts must be enabled.** An interrupt handler is the only thing that can
produce a ready task, so halting with IF clear would be a dead machine rather
than an idle one.

**`sti; hlt` must stay adjacent.** `sti` takes effect only after the following
instruction, precisely so this pair is atomic. An interrupt cannot slip into a
gap between them, spend its wakeup while we are not yet halted, and leave us
halted forever. The condition is re-read with interrupts off (the gate cleared
IF on entry, and `cli` follows each wake), so no wakeup falls between the test
and the halt.

**`scheduler_idling` is a nesting guard.** The idle loop runs with interrupts
enabled, so timer ticks keep arriving and keep calling `schedule()`. Those nested
calls return immediately. Without the guard each tick would nest one level deeper
on the single shared kernel stack and a long idle would overflow it. With it the
depth stays at one, the nested tick unwinds back into the `hlt` loop, and the
outer call (which owns the live pile) performs the eventual switch.

## Blocking by re-arming the syscall

This is the part that is not obvious. A blocked task is **not frozen in the
middle and thawed later. It is rewound to the beginning.**

```c
#define INT_INSTR_LEN 2

void task_block(registers_t *r, wait_reason_t reason) {
    tasks[current]->state = TASK_BLOCKED;
    tasks[current]->wait_reason = reason;
    r->rip -= INT_INSTR_LEN;
    schedule(r);
}
```

The CPU pushed the address of the instruction *after* `int 0x50`. Winding it back
by the length of that instruction (opcode `CD ib`: one opcode byte, one immediate
byte carrying the vector) points it at the int itself. So when the task is woken
and rescheduled, its `iretq` lands **on** the int, the syscall is issued again
from scratch, and this time it finds what it was waiting for. That last step is
not a hope: being woken is *defined* as the awaited thing having happened, and
the waker publishes the data before it wakes anyone (see the ordering rule
below).

From ring 3 none of this is visible. `sys_readkey` is one `int 0x50` that took a
while to come back.

### Why not resume in the middle

The obvious design is to freeze the task where it stands, half way through the
syscall handler, and thaw it there on wake. Two facts about this kernel forbid
it.

**The saved pile can only resume ring-3 code.** `registers_t.rip` is the value
the CPU pushed on the ring-3 to ring-0 transition, so it is always a ring-3
address. Restoring a pile can never resume at a point inside a C function in the
kernel.

**There is one kernel stack, shared by every task.** `tss.rsp0` is set once to a
single static buffer (`kernel/gdt.c`) and never updated per task. The C frames a
handler is standing on are abandoned the moment it switches away, and the next
task to enter the kernel writes over them. There is nothing to come back to.

Mid-syscall resume is what a per-task kernel stack buys you, and it is a real
design with real advantages: an operation could block at an arbitrary depth and
pick up exactly where it left off, with its locals intact. TownOS does not have
one, so it uses the tool it does have. Re-arming needs no extra memory per task,
no second stack to switch, and no care about what was live on the kernel stack at
the moment of the block, because nothing is expected to survive. It is recorded
as `TODO(per-task-kernel-stack)`.

Linux does the same thing under a different name: a signal-interrupted syscall
returns `ERESTARTSYS` and the kernel rewinds the user `rip` onto the syscall
instruction so it is reissued.

### Why not a re-check loop

The tempting wrong implementation is a loop inside the syscall handler that
re-reads the buffer until a key appears:

```c
// WRONG. Do not do this.
int c;
while ((c = keyboard_getchar()) == 0) { }
regs->rax = c;
```

That is the same busy-wait the shell used to do, moved into ring 0, where it is
strictly worse. The kernel spins with interrupts masked (the syscall gate is an
interrupt gate, so IF is clear), the timer cannot fire, the scheduler cannot run,
and every other task starves. The machine is not slow, it is stopped.

`sys_readkey` therefore has no loop at all. It is entered, finds nothing, blocks,
and **ends**. What resumes the task is a fresh entry into the same handler after
the wake.

### Voluntary and involuntary are the same switch

`task_block` drives the same `schedule()` the timer drives, on the same kind of
live on-stack pile. Only the thing that prompted the call differs. There is no
second switch routine, no yield-specific path, and no separate save/restore.

Two differences from the timer path, both benign. There is no EOI to worry about:
`int 0x50` is a software interrupt, so the PIC has no in-service bit to
acknowledge. And nothing here leaves interrupts disabled across the yield, which
matters, because the keyboard IRQ that will wake this task has to be able to
fire; the `iretq` restores the incoming task's own `rflags` with IF set.

## The wake, and the pairing rule

```c
void scheduler_wake(wait_reason_t reason) {
    for (uint32_t i = 0; i < num_tasks; i++) {
        if (tasks[i]->state == TASK_BLOCKED && tasks[i]->wait_reason == reason) {
            tasks[i]->state = TASK_READY;
            tasks[i]->wait_reason = WAIT_NONE;
        }
    }
}
```

**The pairing rule: whoever causes the event wakes the tasks waiting on it.** A
blocked task cannot wake itself, because it is not running and so cannot notice
anything. That is the entire point of blocking, and it is why the waker lives in
the driver that produced the event rather than anywhere the sleeper touches.

For `WAIT_KEY` that is the keyboard IRQ (`drivers/keyboard.c`):

```c
kbd_buffer_push(c);
scheduler_wake(WAIT_KEY);
```

**Push first, wake second.** A task woken before the character was in the buffer
could be scheduled, re-issue its read, find nothing, and block again, turning one
keypress into a wasted round trip. Publish the data, then announce it.

For `WAIT_CHILD` the same rule holds in `task_exit` (`kernel/scheduler.c`): the
status is written and the state is set to `TASK_ZOMBIE` *before* the parent is
woken. A parent woken first would re-issue `SYS_WAIT`, find a child that is not
yet marked finished, and block again.

`WAIT_CHILD` wakes **one specific task by id**, not everything blocked on the
reason:

```c
if (parent_alive(t->parent_id) && tasks[t->parent_id]->wait_reason == WAIT_CHILD) {
    tasks[t->parent_id]->state = TASK_READY;
    tasks[t->parent_id]->wait_reason = WAIT_NONE;
}
```

The broadcast `scheduler_wake(WAIT_CHILD)` would be wrong here in a way the
keyboard's broadcast is not. Every parent in the system waits on the same reason,
so one child exiting would ready all of them; each would re-issue `SYS_WAIT`, find
none of *its own* children finished, and block again. Harmless but pointless for
most, and it makes the wake say something untrue. A key belongs to whoever asked
for one; a dead child belongs to exactly one parent.

`scheduler_wake` only changes state; it does not switch tasks. That keeps it
short and safe to call from interrupt context, where the live pile belongs to
whatever was interrupted rather than to the task being woken, and leaves the
choice of what runs next to the scheduler on the next tick.

Waking is coarse: *every* task blocked on the reason is readied. With a single
reader that is invisible. If two tasks ever waited on the keyboard, one keypress
would wake both and one would block again, which is one clean extra yield rather
than a spin.

## The sharp edge: the blocking path must not write `rax`

The re-armed pile re-executes only the `int` instruction, so the CPU reads the
syscall number from whatever `rax` holds at that moment. Writing a return value
there would make the woken task issue a *different* call. And `SYS_EXIT` is 0, so
a stray `return 0` on the blocking path would halt the machine on the next
keypress.

That is why writing `rax` moved out of the dispatcher and into the handler, on
the path that actually has an answer:

```c
static void sys_readkey(registers_t *regs) {
    int c = keyboard_getchar();
    if (c != 0) {
        regs->rax = (uint64_t)c;   // only on the answering path
        return;
    }
    task_block(regs, WAIT_KEY);
}
```

The dispatcher calls it as `sys_readkey(regs); break;` rather than
`regs->rax = sys_readkey();`. `SYS_WAIT` follows the same shape: `task_wait`
writes `regs->rax` on the two paths that have an answer (a finished child's status,
or `SYSCALL_ERROR` for "you have no children") and writes nothing at all on the
path that calls `task_block`. Any future blocking syscall must do the same.

## Invariants

- `task_block` may only be called from a syscall handler reached through
  `int 0x50`. The rewind is by the length of that instruction, so a caller that
  arrived any other way would have its `rip` wound back into the middle of
  whatever precedes it. Documented in `kernel/scheduler.h` and enforced by
  convention, not by the type system.
- Only from a handler whose work can be redone from the top harmlessly, since
  waking re-runs it in full. Blocking part way through a multi-block disk
  transfer would need `TODO(per-task-kernel-stack)`.
- Nothing may write `regs->rax` on a path that blocks.
- A waker publishes its data before calling `scheduler_wake`.

## What a run looks like

Booted into the shell with nobody typing, over a six second window under `-d int`:

| | Before blocking | After |
|---|---|---|
| `int 0x50` (`v=50`) | 362,648 | **3** |
| Timer ticks (`v=40`) | 326 | 594 |
| Interrupt log size | 494 MB | 848 KB |

The three remaining syscalls are the whole of the shell's startup: `RAX=1`
(`SYS_WRITE`, the banner), `RAX=1` (`SYS_WRITE`, the prompt), and `RAX=2`
(`SYS_READKEY`, which blocks). Then silence, and the CPU sits in `hlt` between
timer ticks. Only `v=40` and `v=50` appear in the log at all: no `#PF` (0x0E), no
`#GP` (0x0D), no double fault (0x08), no triple fault. Timer ticks go *up*
because the log is no longer drowning in syscalls.

The shell behaves exactly as before: `help`, `list`, `read hello.txt`,
`return <text>`, and `run a.elf` all work, and a launched program interleaves
with the now sleeping shell. `user/shell.c` lost its `if (k == 0) continue;`
poll, because `sys_readkey` now always returns a real key.

Failure modes to recognise. If the machine halts on the first keypress after a
sleep, something wrote `rax` on the blocking path and the re-armed `int` became
`SYS_EXIT`. If a "blocked" task keeps getting slices, `schedule()` is clobbering
its state on save. If the machine freezes when every task blocks, the idle loop
is halting with interrupts masked. If the machine still burns CPU while idle,
there is a re-check loop somewhere.

## What is missing

- **No timed sleep.** Nothing can ask to be woken after a duration, so there is
  no `sleep(n)` and no timeout on a blocking call: a task blocked on an event
  that never happens waits forever. The timer already ticks and could drive it,
  but a wake-after-duration list does not exist. `TODO(timed-sleep)`.
- **Wakeup is a linear scan** over every task. Fine at this scale; a kernel with
  many sleepers would keep a per-reason wait queue and wake off the head in
  constant time.
- **Only two wait reasons.** Pipes and waiting on the disk are the same shape (a
  block paired with a wake from whatever causes the event) and each needs a new
  `wait_reason_t` and a waker in the right place, not a new mechanism. `WAIT_CHILD`
  was added exactly that way and needed no change to `task_block` at all.

## Related

- The decision and its trade-offs:
  [decision 0017](../decisions/0017-blocking-and-sleep.md).
- The scheduler this extends: [scheduling.md](scheduling.md) and
  [decision 0008](../decisions/0008-round-robin-preemptive-scheduler.md).
- The gate the re-arm rewinds onto: [syscalls.md](syscalls.md) and
  [decision 0007](../decisions/0007-syscalls-via-int-0x50.md).
- The shell that used to busy-wait, and the keyboard ring buffer the wake feeds:
  [shell.md](shell.md) and
  [decision 0016](../decisions/0016-interactive-shell.md).
- The second wait reason, and the zombie state it collects from:
  [decision 0018](../decisions/0018-process-lifecycle-exit-and-wait.md).

## A block can now be interrupted by a signal

`task_block` returns `TASK_BLOCK_INTERRUPTED` instead of parking when a signal was
raised on this task while it was blocked. The caller must then **fail its syscall**
rather than block again.

This exists because the re-arm above is unconditional. It rewinds `rip` onto the
`int 0x50` so a woken task re-issues its syscall — but it does not know *why* the
task was woken. `signal_raise` readies a blocked task so the signal can be delivered
on its way out to ring 3; without a way to say "this wake was not your event", that
task would re-run its read, find the pipe still empty (a signal is not data), and
park again. Forever, with the signal pending and nothing visibly wrong.

```c
if (tasks[current]->sig_interrupted) {
    tasks[current]->sig_interrupted = 0;
    return TASK_BLOCK_INTERRUPTED;
}
```

Every blocking call honours it — the console read, both pipe directions,
`SYS_READKEY`, `SYS_WAIT` — and a new one must too. From ring 3 the effect is that a
blocking call can return `SYS_FAIL` because a signal ran, not because anything is
wrong; a caller must not treat that as data.

See [signals.md](signals.md) and S5 in
[decision 0023](../decisions/0023-signals.md).
