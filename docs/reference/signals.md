# Signals reference

A signal is what an interrupt is, one layer up. The CPU interrupts a program,
saves enough state to resume it, runs a handler, and puts it back; the kernel does
the same to a ring-3 program. The difference is that **no hardware does any of
it** — every step the processor performs for an interrupt is written by hand in
`kernel/signal.c`.

Read from `kernel/signal.c`, `kernel/signal.h`, `include/signals.h`,
`user/trampoline.asm`, and the delivery call sites at the end of `irq_handler`
(`kernel/isr.c`) and `syscall_handler` (`kernel/syscall.c`). For why it is built
this way, and for the eight ways it goes wrong, see
[decision 0023](../decisions/0023-signals.md).

## The signals this kernel has

| Signal | Number | Raised by | Default action | Catchable |
|---|---|---|---|---|
| `SIG_INT` | 2 | Ctrl-C, `kill <id>` | kill, status 130 | yes |
| `SIG_KILL` | 9 | `kill <id> 9` | kill, status 137 | **no** |
| `SIG_SEGV` | 11 | the kernel, when it cannot deliver safely | kill, status 139 | yes |
| `SIG_PIPE` | 13 | writing to a pipe with no reader left | kill, status 141 | yes |

A task killed by signal N exits with status **128 + N**, which is the shell
convention everywhere: an exit status has no room to say "this was a signal rather
than a return value", so 128 + N carves out a range an ordinary `exit(n)` is not
expected to use. It is a convention, not a guarantee — a program *can* `exit(130)`
itself — which is why the shell reports the number rather than claiming a signal
caused it.

**`SIG_KILL` cannot be caught.** `SYS_SIGNAL` rejects a handler for it and
`check_signals` ignores one if it somehow got there. A signal a program can catch
is a *request*, and a system needs at least one that is not, or a program with a
handler that declines to exit could never be stopped.

## Two halves: raising and delivering

The split is the design. They are kept apart so that raising is safe from anywhere
and delivery happens in exactly one place.

**Raising** sets a bit and returns.

```c
void signal_raise(uint32_t id, int sig);         // one task
void signal_raise_group(uint32_t pgid, int sig); // every task in a group
```

That is all it does, which is what makes it safe to call from the keyboard IRQ —
running on somebody else's stack with interrupts off — and safe for a syscall
handler to call on itself. A zombie cannot be signalled: it has already exited, and
setting a bit on a tombstone would make `kill` report success on a dead id.

The pending set is a **set, not a queue**. Raising `SIG_INT` twice before either is
delivered leaves one bit and produces one delivery, so holding Ctrl-C does not build
a backlog of handler runs.

**Delivering** happens in `check_signals(registers_t *r)`, called at the end of
`irq_handler` **and** at the end of the syscall dispatch — every path by which the
kernel returns to ring 3, and nowhere else. That moment is the only one at which the
frame in front of the kernel is a complete, consistent, about-to-be-restored ring-3
context, which is exactly what a signal has to redirect and later put back.

Both call sites are needed. The IRQ path is what makes a Ctrl-C land in the middle
of a compute loop. The syscall path is what makes a signal raised by `SYS_KILL` take
effect at once rather than at the next timer tick, and it is the path that delivers
to a task whose blocking syscall was just cut short.

`check_signals` does its checks in this order, and the order matters:

1. **Is this frame going back to ring 3 at all?** `if ((r->cs & 3) != 3) return;`
   Interrupts nest, and a tick landing mid-syscall returns into kernel code.
2. **Is a handler already running?** If so, leave the bit pending.
3. **Is anything pending?** The common case, and the cheapest.
4. **Take the lowest pending signal.** Any fixed order would do; it must be
   deterministic so a task with two signals pending behaves the same way every time.
5. **No handler, or `SIG_KILL`?** `task_exit(r, 128 + sig)`.
6. **Otherwise** forge the frame and run the handler.

At most one signal per call. A second is delivered on the next way out — which for a
killed task never comes, and for a handled one is after the handler returns.

## Delivering to a handler

This is the part with no hardware behind it. The kernel builds, by hand, the call
frame a real `call` would have produced.

```c
uint64_t sp = r->user_rsp;
sp -= 128;                    // step over the red zone: live locals live there
sp &= ~0xFULL;                // align to 16 BEFORE the push below
sp -= sizeof(registers_t);
if (!user_range_ok(sp, sizeof(registers_t) + 8)) {   // the WHOLE span
    task_exit(r, 128 + SIG_SEGV);
    return;
}
memcpy((void *)sp, r, sizeof(registers_t));
uint64_t ctx = sp;
sp -= 8;
*(uint64_t *)sp = t->sig_trampoline;   // the handler's "return address"

r->rip      = t->sig_handlers[sig];
r->rdi      = sig;                     // the handler's one argument
r->user_rsp = sp;
t->sig_pending &= ~(1u << sig);
t->sig_active  = 1;
t->sig_ctx     = ctx;
```

Four things in that are load-bearing and each is a distinct way to get it wrong;
[0023](../decisions/0023-signals.md) covers all four (S2, S3, S4, S6) with symptoms.
In short: the 128 bytes below `rsp` hold live locals and must be stepped over; the
ABI wants `rsp % 16 == 8` at the handler's first instruction, which means aligning
*before* the push; `user_rsp` came from ring 3 and the whole span written through it
must be checked, not just its start; and `sig_active` stops a second frame being
forged on top of the first.

From the program's side this is indistinguishable from an ordinary call: argument in
`RDI`, a return address on the stack, correct alignment.

## The trampoline, and why it lives in the program

A handler is an ordinary C function. It ends with `ret`, which pops a return address
and jumps to it. There is no real caller, so the kernel writes the trampoline's
address there:

```asm
global sigreturn_trampoline
sigreturn_trampoline:
    mov rax, SYS_SIGRETURN
    int SYSCALL_VECTOR
```

**The kernel cannot pick this address.** Programs are separately linked ELFs at a
fixed `0x400000`, and no kernel-owned page is mapped into a program's address space,
so no kernel-side address is executable from ring 3. The program supplies it
instead: `userlib.h` passes `&sigreturn_trampoline` as `SYS_SIGNAL`'s third
argument and the kernel stores it per task. Linux has the same problem and answers
it the same way, with the vDSO page mapped into every process.

The user-facing wrapper hides the third argument, so a program writes:

```c
sys_signal(SIG_INT, my_handler);
```

## Returning: `SYS_SIGRETURN`

`sigreturn` restores the saved context over the live frame, so the stub's `iretq`
resumes the interrupted instruction rather than returning from a syscall.

It is **locked shut**, because it overwrites the entire register frame from user
memory and that is a privilege-escalation primitive if a program can reach it at
will — the frame includes CS and RFLAGS. Four things guard it:

- **`sig_active` must be set.** Delivery sets it and this clears it, so the call is
  reachable exactly once per delivered signal. Otherwise the task is killed.
- **The context is range-checked again**, because a handler has had a whole run in
  which to scribble on it.
- **The context is read from `t->sig_ctx`, not `r->user_rsp`**, so a handler that
  moved its stack pointer cannot redirect the restore to bytes of its choosing.
- **CS, SS and IF are not taken from user memory.** They are forced to the known
  ring-3 selectors and IF forced on, so a doctored frame cannot resume in ring 0 or
  with interrupts disabled.

Everything else — the general registers, RIP, RSP — is the program's own state and
is its business.

## Signals and blocked tasks

**This is the subtlest interaction in the system**, and it fails silently in both
directions. It touches the blocking design ([blocking.md](blocking.md)) directly.

Delivery happens on the way out to ring 3. A blocked task is not on its way
anywhere — it is off the rotation entirely, waiting for an event that a signal is
not. So raising alone would set a bit that is never acted on.

Waking it is not enough either. `task_block` rewinds `rip` onto the `int 0x50` so a
woken task **re-issues its syscall**, and the re-arm does not know *why* it was
woken. A task woken for a signal re-runs its read, finds the pipe still empty — a
signal is not data — and blocks again. Forever.

So `signal_raise` does this:

```c
if (t->state == TASK_BLOCKED) {
    t->state = TASK_READY;
    t->wait_reason = WAIT_NONE;
    t->sig_interrupted = 1;        // the syscall was cut short
}
```

and `task_block` answers it:

```c
if (tasks[current]->sig_interrupted) {
    tasks[current]->sig_interrupted = 0;
    return TASK_BLOCK_INTERRUPTED;
}
```

The syscall then fails with `SYSCALL_ERROR` instead of parking, and `check_signals`
delivers as the task leaves the kernel. Every blocking call honours it: the console
read, both pipe directions, `SYS_READKEY` and `SYS_WAIT`.

**What this means for a ring-3 program.** A blocking call can now return
`SYS_FAIL` because a signal arrived, not because anything is wrong. A program that
treats that as data gets junk — the shell's `read_line` did exactly that during
development, storing `-1` as a character, so a Ctrl-C at the prompt left invisible
bytes in the line buffer and the next command was rejected as unknown for no visible
reason. The rule is: **a blocking call returning `SYS_FAIL` may simply mean a signal
ran; retry or move on.**

## Process groups and the foreground

A signal from the keyboard is addressed to a **job**, not a task. A three-stage
pipeline is three tasks and one thing to the person who typed it.

Every task has a `pgid`, inherited from its parent unless `SYS_RUN` is asked for
something else. A group is **named after the task that leads it**, which is the Unix
convention and here also makes a fresh group id unique for free: task ids are never
reused, so a new group cannot collide with a live one. Task 0, the shell, is in
group 0.

The **foreground group** is the one Ctrl-C is addressed to, and it is **declared,
never inferred** — `SYS_SETFG(pgid)`. It cannot be derived from `SYS_RUN`, and
`D.ELF` is the proof: D starts E and exits without waiting, so a foreground inferred
from "most recently started" would follow to E and stay there while the user sits at
a prompt.

A task may name only **its own group, or a group one of its own children is in**.
Without that rule any program could take the keyboard and never give it back, and
nothing could take it away — there is no privileged task here that could. The same
rule governs joining a group at `SYS_RUN`.

The shell's job pattern:

```c
run_pipeline(...);
sys_setfg(job_pgid);
wait_for_all();
sys_setfg(SHELL_PGID);   // unconditional
prompt();
```

The last `sys_setfg` runs whether the job succeeded, failed to start, or was killed.
A shell that restored the foreground only on success would lose the keyboard the
first time anything went wrong.

**Tasks the keyboard cannot reach are a consequence of this, not a bug.** Anything
in a non-foreground group — which is what `run d.elf` leaves behind — cannot be
reached by Ctrl-C at all. `ps` finds it and `kill` stops it; see
[shell.md](shell.md).

## Ctrl-C and Ctrl-D

`drivers/keyboard.c` tracks left ctrl exactly as it tracks shift: set on press,
cleared on release. Right ctrl is not tracked and cannot be until extended
scancodes are (`TODO(extended-scancodes)`); it works by accident, for the reason
given there.

- **Ctrl-C** raises `SIG_INT` on the foreground group.
- **Ctrl-D** sets a `console_eof` flag rather than pushing a character.

**Neither pushes a character, and both return before the character path's
`scheduler_wake(WAIT_KEY)`** — the same discipline the modifier keys already follow.
A wake with an empty ring hands every sleeper a wasted round trip, so holding ctrl
and tapping a key would otherwise spin the scheduler.

Console EOF is the whole of this kernel's line discipline: one flag, test-and-cleared
by the reader, reported as a zero-byte read. `file_read` drains buffered characters
*before* checking it, so typing `abc` then Ctrl-D gives a reader `abc` and then 0,
rather than losing the three characters already typed. Without it a program shaped as
"read fd 0 until EOF" could not terminate when run on its own — `run count.elf` was
exactly that case.

There is **one flag for the whole machine**, not one per descriptor, because there is
one keyboard. With a single foreground group the task that consumes it is the one the
user meant; real job control would have to move it onto the console descriptor.

## Writing a program that catches a signal

```c
#include "../userlib.h"

static volatile unsigned long caught;   // volatile: nothing the compiler sees writes it

static void on_interrupt(int sig) {
    (void)sig;
    caught++;
    sys_print("\n[caught SIG_INT]\n");
}

void _start(void) {
    sys_signal(SIG_INT, on_interrupt);   // install BEFORE any real work
    /* ... */
    sys_exit(0);
}
```

Three things to get right:

- **Install before doing anything that matters.** A signal arriving before the call
  finds no handler and takes the default action, which is to kill the program.
- **Anything the handler touches must be `volatile`.** The handler is called by the
  kernel, not by code the compiler can see, so from its point of view nothing ever
  writes those variables and it may cache them in registers.
- **Keep the handler small.** It runs on the program's own stack, in the middle of
  whatever the program was doing, with every other signal blocked until it returns.

`user/tests/H.c` is the worked example and is written so the **resume** is what is
tested, not just the delivery: it counts the interrupts and prints the total, so a
loop that lost its place produces a wrong number rather than a plausible one.

## Limitations

No per-signal masks (one `sig_active` flag blocks everything for a handler's
duration), no `sigaction`, no `SIGCHLD`, no job control beyond a single foreground
group, no `SIGSTOP` or `SIGCONT`, and delivery only on an interrupt return — so a
task spinning with interrupts somehow disabled would never receive one. Nothing in
this kernel can reach that state, but it is a property of the design rather than an
accident. See [0023](../decisions/0023-signals.md) for the full list and why.
