# Chapter 23: Signals

> Read chapter 3 (interrupts), chapter 19 (blocking and sleep) and chapter 10
> (user mode) first. This chapter is the three of them combined: it does to a
> program what chapter 3's hardware does to the CPU, it collides with chapter 19's
> re-arm in a way that needs care, and every awkward part of it exists because the
> handler runs at ring 3.

## Where we are

Everything a program in TownOS has ever learned, it learned by **asking**. A
syscall, a read, a wait. The program decides when to find something out, and the
kernel answers.

There is no way to tell a program anything.

Which means, concretely, that nothing in your kernel can stop anything. Start
`b.elf` and it runs sixty rounds. Start a pipeline and you wait it out. `task_exit`
only works when a task calls it on itself, and there is no key combination that
does anything at all.

## Rule 1: a signal is an interrupt, one layer up

The parallel is exact, and it is the reason the design looks the way it does.

| | interrupt | signal |
|---|---|---|
| who gets interrupted | the CPU | a ring-3 program |
| handler table | the IDT, indexed by vector | `sig_handlers[]`, indexed by signal number |
| state saved | `registers_t` | `registers_t` |
| where the frame goes | the ring-0 stack, from `rsp0` | the program's own ring-3 stack |
| how you get back | `iretq` | you have to invent one |

Stop the thing wherever it is, look up a handler in a table, run it, put the thing
back so it cannot tell. Same five steps in both columns.

## Rule 2: there is no hardware, so you write all of it

Here is the difference that matters.

An interrupt is performed by silicon. Between two instructions the CPU checks a
pin, and if it is asserted and `IF` is set, it runs an acknowledge cycle to get a
vector, indexes the IDT, checks the gate's DPL, clears `IF`, loads `rsp0` from the
TSS, switches stacks, pushes five values, and jumps. All before your code runs.

A signal has **none of that**. No pin. No PIC. No hardware check between
instructions asking whether a task has something pending. No gate, no automatic
push, no `iretq` defined for it.

So every one of those steps becomes kernel C. You pick the moment to check. You
build the frame. You invent the way back.

That is the whole shape of the chapter: take chapter 3's list and implement it by
hand.

## Rule 3: raising is setting a bit

```c
tasks[i]->sig_pending |= (1u << SIG_INT);
```

That is the entire act of raising a signal. A bit in kernel memory.

The keyboard IRQ, when ctrl is held and `c` arrives, does that and returns.
Nothing is delivered. No frame, no handler, no jump. The program is still running
the same instruction stream and has no idea.

Which is the same discipline as pushing a byte into the keyboard ring buffer, for
the same reason: an interrupt handler runs with interrupts disabled, so it does the
least possible work and gets out.

And notice what follows from a signal being just a bit. It can be raised with no
hardware anywhere near it:

```c
sys_kill(12, SIG_INT);
```

Nobody pressed anything. A program made a syscall and the kernel set a bit.
Identical mechanism.

## Rule 4: delivery happens on the way back to ring 3

```c
void irq_handler(registers_t *regs) {
    ...
    if (interrupt_handlers[regs->int_no])
        interrupt_handlers[regs->int_no](regs);

    check_signals(regs);      // last thing before the stub's iretq
}
```

`*regs` is the frame the stub is about to restore. Editing it redirects execution.

That is not a new trick. It is exactly what `schedule()` does when it switches
tasks: overwrite the saved frame in place and let the existing return path deliver
somebody else. Chapter 12 called it lying to the return path about who it was
serving. Signals lie about *where* rather than *who*, and it means signals need no
new assembly at all.

### The check is free

There is no polling, and no "how often should I look". Every ring-3 task is
interrupted by the timer 100 times a second, at a point where its full register
state is already saved in a form you can edit. The hardware you built in chapter 3
supplies the periodicity for nothing.

### But only when returning to ring 3

```c
if ((r->cs & 3) != 3) return;
```

Privilege lives in CS (chapter 9), so this line is asking "is this frame going back
to a program, or into kernel code". If a nested interrupt is returning into the
kernel and you deliver there, you drop a ring-3 handler frame on top of a
half-finished kernel operation. The corruption surfaces later, somewhere else,
with nothing pointing back here.

## Rule 5: the default action needs no machinery at all

```c
if (t->sig_handlers[sig] == 0) {
    task_exit(r, 128 + sig);
    return;
}
```

That is the whole of the kill path. No frame forging, no trampoline. The task
becomes a zombie, its parent is woken, the sweeper frees it. Every piece of that is
chapter 20, already built and already tested.

`128 + sig` is a convention, and it is why a program killed by Ctrl-C on Linux
reports 130. `SIG_INT` is 2.

Worth noticing: **Ctrl-C works end to end before any of the hard parts exist.**
That makes it a natural checkpoint, and it is why the build plan verifies it before
touching handlers.

## Rule 6: the frame goes on the user's stack, and three things bite

If a handler *is* registered, the kernel has to make the program call a function it
never called.

```c
uint64_t sp = r->user_rsp;
sp -= 128;                                    // (1)
sp &= ~0xFULL;                                // (2)
sp -= sizeof(registers_t);
copy_to_user(sp, r, sizeof(registers_t));     // (3)
uint64_t ctx = sp;
sp -= 8;
write_u64_to_user(sp, t->sig_trampoline);     // fake return address

r->rip      = t->sig_handlers[sig];
r->rdi      = sig;
r->user_rsp = sp;
```

Then `check_signals` returns, the stub pops registers, `iretq` fires, and the
program is running its handler at ring 3 without ever having called it.

The frame goes on the **user** stack rather than the kernel one, because the
handler is ring-3 code and cannot read a kernel address. That single fact generates
all three of the following.

**(1) The red zone.** The System V ABI lets a leaf function use the 128 bytes below
`rsp` without adjusting it. Build your frame at `user_rsp` and you write over live
locals of the function you interrupted. The handler runs perfectly and then the
interrupted function computes the wrong answer, which is about as unpleasant as a
bug gets. Linux skips 128 bytes for exactly this reason.

**(2) Alignment.** The ABI requires `rsp % 16 == 8` at function entry, after the
return address has been pushed. Get it wrong and the handler faults on its first
SSE instruction, for no visible reason. Align before pushing the return address, so
the handler sees what a real `call` would have produced.

**(3) `user_rsp` is untrusted.** It came from ring 3, and the kernel is about to
write a whole `registers_t` through it. That is chapter 11's confused deputy in its
purest form: an unprivileged program supplying an address that privileged code
writes to. Range-check the entire span, not the start.

## Rule 7: inventing the way back

The handler is an ordinary C function. It ends with `ret`, which pops a return
address off the stack and jumps there.

**What return address?** The kernel forged this frame. Nobody called the handler.
There is nothing to return to.

And it cannot simply be the interrupted instruction, because the program's real
register state, all fifteen of them plus flags, is saved in kernel memory. A `ret`
restores none of it.

So the fake return address points at a stub in user memory whose entire body is a
syscall:

```asm
sigreturn_trampoline:
    mov rax, SYS_SIGRETURN
    int 0x50
```

Two instructions. That is the **trampoline**, and it is the piece that replaces
`iretq`.

```c
static void sys_sigreturn(registers_t *r) {
    copy_from_user(r, t->sig_ctx, sizeof(registers_t));
    r->cs      = USER_CODE_SELECTOR;
    r->ss      = USER_DATA_SELECTOR;
    r->rflags |= RFLAGS_IF;
    t->sig_active = 0;
}
```

It overwrites the whole saved frame with the context stashed at delivery, and the
stub's `iretq` puts the program back on the exact instruction it was on.

### Why those three forced lines matter

`sigreturn` restores CS and RFLAGS **from user memory**. A program that could call
it at will, with a stack it controls, could set CS to a ring-0 selector and hand
itself the kernel.

So it is rejected unless a `sig_active` flag is set, the context address comes from
the kernel's own record rather than from `user_rsp` (a handler could have moved its
stack pointer), and CS, SS and the interrupt flag are forced from known values
rather than trusted.

`iretq` is safe because the hardware defines what it will accept. `sigreturn` is a
syscall you wrote, so its safety is entirely your problem.

## Rule 8: who gets the signal is declared, not detected

Ctrl-C has to reach the running program, not task 0, or you kill your own shell.

There is nothing about a task that makes it the keyboard's owner. Nothing in the
kernel tracks it, because until signals nothing needed to: `sys_readkey` serves
whoever calls it, and only the shell ever called it.

So the shell declares it, because the shell is the only thing that knows it is
about to stop reading the keyboard:

```c
run_pipeline(...);
sys_setfg(job_pgid);
wait_for_all();
sys_setfg(0);           // take it back, unconditionally
```

### Why it cannot be inferred

The obvious shortcut is "the child of a task that is about to wait". No syscall
needed. `D.ELF` breaks it:

```
shell runs D          foreground = D
D runs E              foreground = E     <- inferred, and wrong
D exits without waiting
shell prints a prompt
```

E runs for another ten seconds while you sit at a prompt with Ctrl-C wired to it.
The inference fails because `sys_run` cannot know whether the parent intends to
wait. Only the parent knows, so the parent says.

### Why a group and not a task

```
run a.elf | run upper.elf | run count.elf
```

Ctrl-C has to reach all three, or you kill the middle and leave the others blocked
on pipes forever. So every task carries a `pgid`, the shell puts a whole job in one
group, and the keyboard raises on the group.

You might reasonably ask why not follow the pipes and signal whatever is on the
other end. Three reasons. D and E have no pipe at all, so that case gets nothing.
For real pipelines the propagation **already works** through EOF: kill `a`, its fds
close, `writers` hits zero, `upper` reads EOF and exits, and the cascade continues
(that is B2 from chapter 22). And the cascade is *graceful*, which is the wrong
response to Ctrl-C: kill `a` in `a | sort | count` and `sort` will tidily sort
everything it has and `count` will count it, when what you wanted was for it to
stop.

Two mechanisms, two questions. EOF means "the input ended, wind down properly". A
group signal means "stop, now".

### And one that cannot be caught

A signal a program can catch is a request. There has to be one that is not, so
`SIG_KILL` ignores `sig_handlers` entirely and always takes the default action.
Otherwise a program with a handler that ignores everything is unkillable.

## Rule 9: the collision with chapter 19

The subtlest thing in the rung, and it is invisible until you test it.

A task is blocked reading a pipe. You press Ctrl-C. The signal is raised, so you
wake the task, so `check_signals` can deliver on its way out.

Except `task_block` rewound its `rip` onto the `int 0x50` so that a woken task
**re-issues its syscall**. That re-arm is still armed. The task re-runs the read,
finds the pipe still empty, and blocks again. `check_signals` never runs on a
return to ring 3, and Ctrl-C appears to do nothing at all.

The fix is a flag:

```c
t->state = TASK_READY;
t->wait_reason = WAIT_NONE;
t->sig_interrupted = 1;
```

and, on re-entry, a syscall that sees `sig_interrupted` clears it and returns
`SYSCALL_ERROR` instead of blocking again. Now the task reaches the return path
with a pending signal, and delivery happens.

This is the price of chapter 19's design, and it is worth seeing as a price rather
than a defect. Re-arming is what made blocking cheap and made `wait` almost free in
chapter 20. It also means "wake this task" and "wake this task and do not let it go
straight back to sleep" are different operations, and signals need the second one.

Unix has this exact problem and the exact same answer, which is why `read()` can
return `EINTR`.

## What this still is not

- **No per-signal masks.** One `sig_active` flag stops a handler being interrupted
  by another signal, which is enough to prevent the frames stacking until the user
  stack runs out. Real systems have a per-signal mask and `sigprocmask`.
- **No `sigaction`.** No flags, no restart-on-interrupt, no alternate signal stack.
- **No `SIGCHLD`.** A parent still finds out about a dead child by calling `wait`.
- **No `SIGSTOP` or `SIGCONT`**, so no job control beyond one foreground group. No
  suspending a job and resuming it.
- **Delivery only on an interrupt return.** A signal cannot reach a task that never
  returns to ring 3. In practice the timer guarantees it does, but the guarantee
  comes from the timer and not from the signal mechanism.

## Exercises

1. Ctrl-C is pressed. List every step from the keypress to the handler's first
   instruction, and say which of those steps hardware performs.
2. `check_signals` returns immediately when `(r->cs & 3) != 3`. Construct the
   scenario that check prevents, and describe what the corruption would look like.
3. Why is the signal frame built on the user stack rather than the kernel stack?
   Give the reason in terms of what the handler is allowed to read.
4. The frame skips 128 bytes before it starts. Explain what those bytes are, and
   describe a program that would visibly break without the skip.
5. A handler ends with `ret`. Explain why a fake return address is needed at all,
   and why it cannot just be the interrupted instruction's address.
6. `sigreturn` restores CS and RFLAGS from user memory. Design the attack, then
   name the three guards that stop it.
7. A task is blocked in `WAIT_PIPE_READ` when it is signalled. Trace what happens
   with and without the `sig_interrupted` flag.
8. Explain why foreground cannot be inferred from `sys_run`, using `D.ELF` and
   `E.ELF` as the example.
9. Killing `a` in `a | upper | count` causes the whole pipeline to unwind on its
   own through EOF. Given that, argue for group signals anyway, using `sort` as the
   middle stage.
10. `SIG_KILL` ignores registered handlers. Explain why at least one signal must
    behave that way, and what a program could otherwise do.
