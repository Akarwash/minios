# 0023 - Signals

## Status

Accepted.

## Context

A ring-3 program could be started and could be waited for, and that was the whole
of its lifecycle from outside. Nothing could interrupt one. A program with a long
loop, or one blocked reading a pipe whose far end had gone quiet, ran until it
chose to stop; `run b.elf` meant sixty rounds of `B` whether you wanted them or
not, and a pipeline whose reader had exited left its writer spinning against a
dead buffer with no way back to a prompt but a reboot. The `run` command's own
comment admitted it: "There is no way to kill a task and there are no signals, so
if the program never calls `sys_exit`, this shell blocks here forever."

A **signal** is what an interrupt is, one layer up. The CPU interrupts a program,
saves enough state to resume it, runs a handler, and puts it back; the kernel does
the same to a ring-3 program. The difference — and the entire content of this rung
— is that **no hardware does any of it**. For a CPU interrupt the processor pushes
the frame, switches stack, and vectors through the IDT. For a signal, every one of
those steps is written by hand: the kernel builds a call frame on the program's own
stack, points `rip` at the handler, and arranges for the handler's eventual `ret`
to come back into the kernel so the saved frame can be restored.

That "arranges for" is the part with no obvious answer, and it drives several of
the decisions below.

Two pieces of existing machinery are load-bearing here and neither was built with
signals in mind. The **blocking design** ([0017](0017-blocking-and-sleep.md))
resumes a woken task by rewinding `rip` onto its `int 0x50` so the syscall is
re-issued from scratch — which interacts with signal delivery in a way that fails
silently in both directions (S5 below). And the **descriptor and pipe layer**
([0022](0022-file-descriptors-and-pipes.md)) explicitly deferred signals: "Signals
share no machinery with this. Both are later rungs." That was right about the
mechanism and wrong about the consequences — `pipe_write` against a dead reader
returned an error precisely because a signal was the thing it could not raise.

## Decision

Add signals: a per-task pending set, delivery on the way out to ring 3, default
actions, catchable handlers with a full save and restore, process groups so the
keyboard can address a job rather than a task, and a declared foreground. Nine
decisions, each taken as stated.

1. **Process groups.** One `uint32_t pgid` on `task_t`, inherited from the parent
   unless the caller asks for a new one. A group is named after the task that leads
   it, which is the Unix convention and here also makes a fresh group id unique for
   free: task ids are never reused, so no live group can collide with a new one. The
   shell puts every stage of a pipeline in one group, so Ctrl-C reaches all of them.
   A three-stage pipeline is three tasks and one thing to the person who typed it,
   and interrupting one stage while the others run is not what Ctrl-C means.

2. **Foreground is declared, not inferred.** `SYS_SETFG(pgid)` says which group the
   keyboard is talking to. It cannot be derived from `SYS_RUN`, and `D.ELF` is the
   proof: D starts E and exits without waiting, so a foreground inferred from "most
   recently started" would follow to E and stay there while the user sits at a
   prompt, with every Ctrl-C going to a background program and none reaching the
   shell.

3. **A task may set the foreground only to its own group, or to a group containing
   one of its own children.** Without this rule any program could name some other
   group as the foreground and never give the keyboard back — and nothing could take
   it away, because there is no privileged task here that could. The child clause is
   the only reason the rule is not simply "your own group": a shell puts a pipeline
   into a group of its children and must then name a group that is not its own. The
   same rule governs joining a group at `SYS_RUN`, so it is written once and used
   twice.

4. **`SYS_KILL(id, sig)` and a `kill` command**, for tasks the keyboard cannot
   reach. That such tasks exist is a consequence of decision 2, not an oversight:
   Ctrl-C goes to the foreground group only, so anything in another group — which is
   exactly what `run d.elf` leaves behind — is unreachable from the keyboard by
   design.

5. **`SYS_TASKS` and a `ps` command**, so `kill` takes a looked-up id rather than a
   guessed one. The two are a pair and neither is much use alone. This is also the
   first time the task table has been visible from outside the kernel at all, and so
   the kernel's first debugging tool.

6. **Handlers, not kill-only.** A signal that can only kill is half a mechanism, and
   the forged frame and the trampoline are the interesting part — the part that
   makes a signal an interrupt rather than a fatal condition. A later rung would
   have had to add it, on top of code that had never had to be resumable.

7. **`SIGPIPE`.** `pipe_write` with `readers == 0` raises a signal rather than
   returning an error. An error return only reaches a program that checks return
   values; a signal reaches one that does not. The default action kills the writer,
   which is what lets `run g.elf | run once.elf` terminate rather than spinning.

8. **Ctrl-D**, because it needs exactly the same ctrl tracking as Ctrl-C. It sets a
   flag that makes a console read report end of input, which is the whole of this
   kernel's line discipline. Without it `run count.elf` on its own — a program shaped
   as "read fd 0 until EOF" — could not terminate, which verification 9 of the pipes
   rung found and could not fix.

9. **A signalled blocked task wakes, delivers on the way out, and its interrupted
   syscall returns `SYSCALL_ERROR`.** It must not silently re-issue. See S5.

### Mechanism, briefly

Raising and delivering are deliberately separate halves.

**Raising** sets a bit in `task_t.sig_pending` and returns. That is all, which is
what makes it safe to call from the keyboard IRQ — running on somebody else's stack
with interrupts off — and safe for a syscall handler to call on itself. The pending
set is a **set, not a queue**: raising the same signal twice before it is delivered
produces one delivery, so holding Ctrl-C does not build a backlog of handler runs.

**Delivering** happens in `check_signals`, called at the end of `irq_handler` and at
the end of the syscall dispatch — every path by which the kernel returns to ring 3,
and nowhere else. That moment is the only one at which the register frame in front
of the kernel is a complete, consistent, about-to-be-restored ring-3 context, which
is exactly what a signal has to redirect and later put back. Both call sites are
needed: the IRQ path is what makes a Ctrl-C land during a compute loop, and the
syscall path is what makes a signal raised by `SYS_KILL` take effect at once rather
than at the next timer tick.

With no handler, the default action is `task_exit(r, 128 + sig)`, so Ctrl-C gives
130. With a handler, the kernel forges on the program's own stack the call frame a
real `call` would have produced, points `rip` at the handler, passes the signal
number in `rdi`, and writes the **trampoline** address where a return address
belongs. The handler ends in an ordinary `ret`, lands on the trampoline, and the
trampoline raises `SYS_SIGRETURN`, which copies the saved context back over the live
frame. The program resumes at the interrupted instruction knowing nothing happened.

**`SIG_KILL` ignores handlers entirely**, checked before the handler is looked at. A
signal a program can catch is a *request*; a system needs at least one that is not,
or a program with a handler that declines to exit could never be stopped.

## Eight ways this goes wrong

These are not hypotheticals; each is a real defect that this rung either hit or
would have hit. **Six of the eight fail silently**, and none of them is inferable
from reading the finished code, because working code shows the fix and never the
failure. That is the entire reason this section exists.

### S1. Delivering while returning to ring 0

- **Symptom.** Random corruption, usually in an unrelated subsystem, minutes later.
  Silent.
- **Cause.** `check_signals` runs on every interrupt return, and interrupts nest. A
  timer tick can land while the kernel is half way through a syscall, and that tick's
  frame returns to *kernel* code. Delivering there drops a ring-3 handler frame on
  top of a half-finished kernel operation, with kernel state live in the registers,
  and then "returns" into user code that was never going to run — abandoning the
  kernel's work and corrupting whatever it was touching.
- **The line that prevents it**, first in `check_signals`:
  ```c
  if ((r->cs & 3) != 3) return;   // this frame returns to ring 0, not to a program
  ```
  Privilege lives in the low two bits of CS, so this is literally asking "is this
  frame going back to a program".

### S2. The red zone

- **Symptom.** A handler runs correctly, and then the interrupted function computes
  the wrong answer. Silent.
- **Cause.** The System V ABI lets a leaf function use the 128 bytes below `rsp`
  without adjusting it, so short functions need no prologue. That space holds *live
  locals*. Building the signal frame at `user_rsp` writes straight over them.
- **The line that prevents it**, before anything else in delivery:
  ```c
  sp -= RED_ZONE_BYTES;   // 128
  ```
  Linux does exactly this subtraction, for exactly this reason.

### S3. Stack alignment

- **Symptom.** The handler faults on its first instruction, or — much more
  confusingly — survives until some SSE instruction partway in. Not silent, but
  thoroughly misleading.
- **Cause.** The ABI requires `rsp % 16 == 8` at a function's first instruction:
  16-byte aligned before the `call`, and the call's 8-byte push leaves the 8. A
  handler entered on a differently-aligned stack breaks the first instruction that
  assumes it, which is often a compiler-emitted `movaps` for something as ordinary as
  a struct copy.
- **The lines that prevent it**: align to 16 *before* pushing the fake return
  address, so the handler sees the alignment a real call site produces.
  ```c
  sp &= ~0xFULL;
  sp -= sizeof(registers_t);
  /* ... */
  sp -= 8;
  *(uint64_t *)sp = t->sig_trampoline;
  ```

### S4. Unvalidated `user_rsp`

- **Symptom.** A malicious or broken program makes the kernel write anywhere it
  likes. Silent until it matters, and then catastrophic.
- **Cause.** `r->user_rsp` came from ring 3 — it is whatever the program last put in
  `RSP` — and the kernel is about to write a whole `registers_t` plus a return
  address through it. This is the confused deputy: the kernel has privileges the
  program does not, and the program is choosing the address.
- **The lines that prevent it**: range-check the **entire span**, not the start. A
  pointer sitting just below `USER_REGION_END` passes a start-only check and then
  writes off the end of the region into kernel pages.
  ```c
  if (!user_range_ok(sp, sizeof(registers_t) + 8)) {
      task_exit(r, SIG_EXIT_STATUS(SIG_SEGV));
      return;
  }
  ```
  A task whose stack cannot take the frame cannot be signalled safely, so it dies
  rather than the kernel writing elsewhere. That is what a segmentation fault *is*.

### S5. Delivering to a blocked task

**The subtlest interaction in the rung, and the one most likely to make the whole
thing silently not work.** It touches [0017](0017-blocking-and-sleep.md)'s design
directly.

- **Symptom.** Ctrl-C on a task blocked reading a pipe appears to do nothing at all.
  Or, once half-fixed, the handler runs and the syscall silently restarts as if
  nothing had happened. Silent both ways.
- **Cause.** Two compounding problems. First, delivery happens on the way out to
  ring 3, and a blocked task is not on its way anywhere — it is off the rotation
  entirely, waiting for an event that a signal is not. So the bit is set and nothing
  else happens, ever.

  Second, and worse: waking the task is not enough. `task_block` rewinds `rip` onto
  the `int 0x50` so a woken task **re-issues its syscall**, and that re-arm does not
  know *why* the task was woken. A task woken for a signal re-runs its read, finds
  the pipe still empty — a signal is not data — and blocks again. Forever, with the
  signal pending and nothing visibly wrong.
- **The lines that prevent it**, in `signal_raise`:
  ```c
  if (t->state == TASK_BLOCKED) {
      t->state = TASK_READY;
      t->wait_reason = WAIT_NONE;
      t->sig_interrupted = 1;        // the syscall was cut short
  }
  ```
  and at the top of `task_block`:
  ```c
  if (tasks[current]->sig_interrupted) {
      tasks[current]->sig_interrupted = 0;
      return TASK_BLOCK_INTERRUPTED;
  }
  ```
  The syscall then fails with `SYSCALL_ERROR` instead of parking, and `check_signals`
  delivers as the task leaves the kernel. Every blocking call honours it: the console
  read, both pipe directions, `SYS_READKEY`, and `SYS_WAIT`.

  It is commented at both ends, because either half alone looks complete and is not.

### S6. Nested delivery during a handler

- **Symptom.** Stack exhaustion under repeated Ctrl-C, surfacing as a page fault at
  an address just below the user stack — which points at the stack and not at the
  signal code that consumed it.
- **Cause.** A handler is running; another signal arrives; a second frame is forged
  on top of the first, and a third on top of that, walking the user stack down until
  it runs off the bottom.
- **The lines that prevent it**: one flag per task, `sig_active`, set at delivery and
  cleared by `SYS_SIGRETURN`. `check_signals` returns early while it is set, leaving
  the bit pending for after the handler finishes.

  This is **cruder than per-signal masks** and deliberately so: a real `sigaction`
  blocks only what a handler asked to block, whereas this blocks everything for the
  handler's duration. The cost is recorded in Consequences rather than hidden.

### S7. `sigreturn` without a live frame

- **Symptom.** None, until it is used as a privilege-escalation primitive.
- **Cause.** `SYS_SIGRETURN` overwrites the **entire** saved register frame from user
  memory. That is its whole job, and it means a program that can call it at will —
  having first arranged its own bytes where the context is read from — is choosing
  the CS and RFLAGS it resumes with.
- **The lines that prevent it.** Reject unless `sig_active` is set, so the call is
  reachable exactly once per delivered signal; range-check the context again before
  copying, because a handler has had a whole run in which to scribble on it; read
  from `t->sig_ctx` rather than `r->user_rsp`, so a handler that moved its stack
  pointer cannot redirect the restore; and do not trust the restored privilege
  fields at all:
  ```c
  r->cs      = GDT_SELECTOR_USER_CODE;
  r->ss      = GDT_SELECTOR_USER_DATA;
  r->rflags |= USER_MODE_RFLAGS;   // IF forced on
  ```
  CS and RFLAGS come out of user memory, which is precisely why they are overwritten
  with known-good values rather than accepted. Forcing IF also stops a program
  resuming with interrupts disabled and keeping the CPU forever.

### S8. Ctrl-C reaching the shell

- **Symptom.** Ctrl-C kills the shell and the machine is left with nothing running.
  Observed exactly once, deliberately, before the fix: no reap line appeared, because
  nothing waits on task 0, and the machine simply stopped echoing.
- **Cause.** The foreground defaults to the shell's group, correctly — the shell is
  what the person is typing at. But if a `sys_setfg` call fails, or a future edit
  forgets one, Ctrl-C at any moment hits task 0, and there is no way to start another
  shell.
- **The lines that prevent it.** The shell registers a `SIG_INT` handler of its own
  that does nothing but abandon the current line and print a fresh prompt — which is
  what every real shell does with an interrupt at a prompt. So even when Ctrl-C does
  reach the shell, it behaves rather than dying.

## Alternatives considered

**Kill-only signals, no handlers.** Much smaller: no forged frame, no trampoline, no
`sigreturn`, and S2, S3, S4, S6 and S7 all vanish with it. Rejected because the
forged frame is the thing worth building — it is where a signal stops being "a way to
kill something" and becomes an interrupt with a save and a restore — and because a
later rung wanting handlers would have to add resumability to code that had never
needed it.

**Inferring the foreground from `SYS_RUN`.** No new syscall, no permission rule,
nothing for a program to get wrong. Rejected on the evidence of `D.ELF`, as decision
2 sets out: a parent that exits without waiting leaves the inference pointing at a
background task while the user is at a prompt.

**A kernel-chosen trampoline address.** The obvious shape — put two instructions
somewhere fixed and have every program return through it. There is nowhere to put
it: programs are separately linked ELFs at a fixed `0x400000` and no kernel-owned
page is mapped into a program's address space, so no kernel-side address is
executable from ring 3. Linux has the same problem and answers it the same way, with
a page (the vDSO) mapped into every process. Here the program supplies the address
instead, and `userlib.h` hides the argument.

**Per-signal masks (`sigaction`).** The correct answer, and more machinery than this
rung can carry. One `sig_active` flag solves S6 completely; what it costs is
precision, listed below.

**Overloading an existing `SYS_RUN` argument to request a new process group.** A
sentinel in `out_fd`, or a bit stolen from an fd number. Rejected as unreadable: a
fourth argument in RCX leaves the existing three meaning exactly what they always
meant, with 0 as the old behaviour. RCX rather than R10 because this kernel enters
through `int 0x50`, which — unlike the `syscall` instruction — does not clobber it,
so the fourth argument sits where the System V C ABI already puts it.

## Consequences

What this buys: a running program can be interrupted and killed; a program can catch
an interrupt and resume exactly where it was; a pipeline is one job to the keyboard;
a task the keyboard cannot reach can still be found and stopped; a console can report
end of input; and a writer against a dead pipe terminates instead of spinning.

What it does not buy, all deliberate:

- **No per-signal masks.** One `sig_active` flag blocks *every* signal for the
  duration of a handler. A `SIG_KILL` arriving mid-handler waits for the handler to
  finish rather than taking effect at once.
- **No `sigaction`.** No flags, no restart semantics, no alternate signal stack. A
  handler is one address, installed by `SYS_SIGNAL`.
- **No `SIGCHLD`.** A parent learns about a child only by calling `SYS_WAIT`.
- **No job control beyond a single foreground group.** No background jobs, no `fg`,
  no `bg`, no job table. One group is in front and the rest are not.
- **No `SIGSTOP` or `SIGCONT`.** Nothing can be paused and resumed, only interrupted
  or killed.
- **Delivery only on an interrupt return.** A task spinning with interrupts somehow
  disabled would never receive a signal. Nothing in this kernel can arrive at that
  state — `USER_MODE_RFLAGS` sets IF and `sigreturn` forces it back on — but it is a
  property of the design rather than an accident, and worth naming.
- **One `console_eof` flag for the whole machine**, not one per descriptor. There is
  one keyboard, and with a single foreground group the task that consumes the EOF is
  the one the user meant. Real job control would have to move it onto the console
  descriptor.
- **Right ctrl is not tracked**, because extended scancodes still are not
  (`TODO(extended-scancodes)` in `drivers/keyboard.c`). It happens to work by
  accident, for the reason given there.
- **The `SYS_RUN` ABI changed.** A binary built against an older `userlib.h` leaves
  junk in RCX, which the permission check rejects, and every `run` from it fails.
  All programs are rebuilt together from this tree, so this is only a hazard for a
  stale `disk.img`.

## References

- [`docs/reference/signals.md`](../reference/signals.md) — how the finished thing works.
- [0017](0017-blocking-and-sleep.md) — the blocking design S5 interacts with.
- [0018](0018-process-lifecycle-exit-and-wait.md) — exit statuses and the reaping paths.
- [0022](0022-file-descriptors-and-pipes.md) — the pipes SIGPIPE completes.
- [0019](0019-keyboard-modifier-state-in-the-driver.md) — why ctrl is tracked in the driver.
