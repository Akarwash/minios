# 0017 - Blocking and sleep, by re-arming the syscall

## Status

Accepted. Extended — the decision stands, one detail of the body does not.

- **`task_block` no longer always blocks.** It returns
  `TASK_BLOCK_INTERRUPTED` when a signal was raised on the task while it was
  parked, and the caller then fails its syscall instead of blocking again. This
  was added by [0023](0023-signals.md), whose S5 explains why the re-arm
  described here makes it necessary: the rewind onto `int 0x50` does not know
  *why* a task was woken, so a task woken to receive a signal would re-issue its
  syscall, find its event still absent, and park again forever.

The re-arm itself, the block/wake pairing rule, the idle loop and the nesting
guard are unchanged. See [reference/blocking.md](../reference/blocking.md) and
[reference/signals.md](../reference/signals.md) for the current state.

## Context

The scheduler ([0008](0008-round-robin-preemptive-scheduler.md)) had no way to
express a task with nothing to do. Every task was `TASK_READY` or `TASK_RUNNING`,
so every task was always a candidate, and the round-robin cursor handed each one a
slice whether or not it could use it.

The shell made the cost of that obvious. `SYS_READKEY` was non-blocking by design
([0016](0016-interactive-shell.md)): on an empty keyboard buffer it returned 0
immediately, because blocking would have needed a way to sleep a task and wake it
from an interrupt, and no such thing existed. So `user/shell.c` polled it, calling
the syscall over and over until a key appeared. That was recorded as
`TODO(blocking-readkey)`.

The waste was not theoretical. Measured under QEMU with `-d int` over a six second
window with the shell sitting idle at its prompt and nobody typing, the kernel
serviced **362,648** `int 0x50` syscalls against 326 timer ticks, roughly 1,112
readkey calls per tick, producing a 494MB interrupt log. Every one of those was the
shell asking whether a key had arrived and being told no. An idle machine was
running flat out.

This is also the rung the next several features stand on. Waiting for a child
process to exit, waiting for a pipe to fill, and waiting for a disk block to arrive
are all the same shape: a task that cannot proceed until something else happens.

## Decision

Give a task a `TASK_BLOCKED` state and a reason it is waiting, make the scheduler
skip blocked tasks, and let a task put itself to sleep at a syscall boundary by
**re-arming the syscall** so that waking re-issues it.

**A state and a reason.** `task_state_t` gains `TASK_BLOCKED`, and `task_t` gains a
`wait_reason_t` field (`WAIT_NONE`, `WAIT_KEY`). The state takes the task out of the
rotation; the reason is what lets the right waker find it, since a keypress should
wake the tasks waiting for a key and leave the others alone. One reason exists
today. The enum is the seam where `WAIT_CHILD` and `WAIT_DISK` will slot in.

**A scheduler that skips.** `schedule()` selects only `TASK_READY` slots, so a
blocked task is stepped over however many times the cursor comes round. It also no
longer forces the outgoing task back to `TASK_READY` on save, which would have
undone a block on the spot.

**An idle that costs nothing.** When no task is runnable, `schedule()` parks in
`hlt` with interrupts enabled rather than spinning or falling back on a blocked
task. Interrupts must be enabled there: an interrupt handler is the only thing that
can produce a ready task, so halting with them masked would be a dead machine
rather than an idle one. A nesting guard stops timer ticks that land during the park
from stacking up on the single shared kernel stack.

**Blocking by re-arming the syscall, not by freezing mid-handler.** This is the part
that is not obvious, and it is forced by two facts about this kernel.

The saved register pile is the task ([0008](0008-round-robin-preemptive-scheduler.md)),
and its `rip` is the value the CPU pushed on the ring-3 to ring-0 transition. That
is always a ring-3 address. Restoring a pile can only ever resume user code, never a
point inside a C function in the kernel. Separately, there is one kernel stack
shared by every task: `tss.rsp0` is set once to a single static buffer
(`kernel/gdt.c`) and never updated per task, so the C frames a handler is standing
on are abandoned the moment it switches away, and the next task to enter the kernel
writes over them.

So a blocked task is not frozen in the middle and thawed later. It is rewound to the
beginning. `task_block` winds the saved `rip` back by the two-byte length of the
`int 0x50` instruction, so it points at the int rather than after it. When the task
is woken and rescheduled, its `iretq` lands on the int, the syscall is issued again
from scratch, and this time it finds what it was waiting for, because that is
precisely what being woken means.

**One switch routine, two reasons to call it.** `task_block` drives the same
`schedule()` the timer drives, on the same kind of live on-stack pile. Involuntary
preemption and a voluntary block are the same switch; only the thing that prompted
it differs.

**Wakeups come from whatever causes the event.** `scheduler_wake(reason)` marks every
task blocked on that reason `TASK_READY`. The keyboard IRQ calls
`scheduler_wake(WAIT_KEY)` after pushing a character into the ring buffer, in that
order, so a woken task cannot be scheduled before the character it was promised is
there. The waker only changes state; it does not switch tasks, which keeps it safe
to call from interrupt context and leaves the choice of what runs next to the
scheduler on the next tick.

**No re-check loop, anywhere.** The tempting wrong implementation is a loop inside
the syscall handler that re-reads the buffer until a key appears. That is the same
busy-wait moved into ring 0, where it is worse: the kernel spins, the scheduler
cannot run, and every other task starves. `sys_readkey` is entered, finds nothing,
blocks, and ends. What resumes the task is a fresh entry into the same handler after
the wake.

## Consequences

- **An idle machine costs nothing.** Over the same six second idle window that
  previously logged 362,648 syscalls, the kernel now services **3**: one `SYS_WRITE`
  for the shell's banner, one for its prompt, and one `SYS_READKEY` that goes to
  sleep. The interrupt log fell from 494MB to 848KB, and only timer (`v=40`) and
  syscall (`v=50`) vectors appear at all: no page fault (`0x0E`), no `#GP` (`0x0D`),
  no double fault (`0x08`), no triple fault. With the shell as the only task and it
  blocked, the CPU is halted in `hlt` between timer ticks rather than spinning.

- **Blocking is invisible from ring 3.** The shell behaves exactly as before:
  `help`, `list`, `read HELLO.TXT`, `return`, and `run A.ELF` all work, and a
  launched program interleaves with the now sleeping shell. From the program's side
  a blocking read is one `int 0x50` that took a while to come back. `user/shell.c`
  lost its `if (k == 0) continue;` poll, because `sys_readkey` now always returns a
  real key.

- **This is the primitive the next rungs reuse.** Process exit and wait, pipes, and
  waiting on the disk are all a block paired with a wake from whatever causes the
  event. Each needs a new `wait_reason_t` and a waker in the right place, not a new
  mechanism.

- **A task can only block at a syscall entry point, and only by re-issuing that
  syscall on wake.** It cannot block at an arbitrary point deep in kernel code,
  because the saved pile holds only the ring-3 `rip` and the kernel stack is shared,
  so there is nothing to come back to. In practice this means `task_block` is
  callable only from a syscall handler, and only from one whose work can be redone
  from the top harmlessly. Blocking mid-operation, for instance part way through a
  multi-block disk transfer, would need a per-task kernel stack to return to:
  `TODO(per-task-kernel-stack)`.

- **The re-arm depends on the caller having arrived through `int 0x50`.** The rewind
  is by the length of that instruction, so a task that reached `task_block` any other
  way would have its `rip` wound back into the middle of whatever precedes it. The
  invariant is documented in `kernel/scheduler.h` and enforced by convention, not by
  the type system.

- **The blocking path must not write `rax`, and that is a sharp edge.** The re-armed
  pile re-executes only the `int` itself, so the CPU reads the syscall number from
  whatever `rax` holds at that moment. Writing a return value there would make the
  woken task issue a different call, and since `SYS_EXIT` is 0, a stray `return 0`
  on the blocking path would halt the machine on the next keypress. Writing `rax`
  therefore moved out of the dispatcher and into the handler, on the path that
  actually has an answer.

- **Wakeup is a linear scan.** `scheduler_wake` walks every task looking for ones
  blocked on the reason. That is fine at this scale (a handful of tasks) and honest,
  but a kernel with many sleepers would keep a per-reason wait queue and wake off the
  head in constant time.

- **There is no timed sleep.** Nothing can ask to be woken after a duration, so there
  is no `sleep(n)` and no timeout on a blocking call: a task blocked on an event that
  never happens waits forever. The timer already ticks and could drive it, but a
  wake-after-duration list does not exist. Recorded as `TODO(timed-sleep)`.

- **A spurious wake is harmless but is not specially handled.** If a woken task found
  the buffer empty it would simply block again, which is one more clean yield rather
  than a spin. This cannot happen today, because only a push wakes a `WAIT_KEY` task.

- **Waking is still coarse.** `scheduler_wake` readies *every* task blocked on the
  reason, so if two tasks ever waited on the keyboard, one keypress would wake both
  and one would block again. With a single reader this is invisible.

## Related

- The scheduler this extends: [0008](0008-round-robin-preemptive-scheduler.md),
  [../reference/scheduling.md](../reference/scheduling.md).
- The syscall gate the re-arm rewinds onto: [0007](0007-syscalls-via-int-0x50.md),
  [../reference/syscalls.md](../reference/syscalls.md).
- The shell that busy-waited, and the `TODO(blocking-readkey)` this closes:
  [0016](0016-interactive-shell.md), [../reference/shell.md](../reference/shell.md).
- Reference page: [../reference/blocking.md](../reference/blocking.md).
