# System call reference

TownOS lets a ring-3 program request kernel services through a single software
interrupt, `int 0x50`. This page documents the gate, the calling convention, the
nineteen calls that exist, and the pointer checks that guard the untrusted ones. Read
from `kernel/syscall.c`, `kernel/isr_stubs.asm`, `kernel/isr.c`,
`include/syscalls.h`, `drivers/keyboard.c`, and `user/userlib.h`. For the
rationale and the alternatives considered, see
[decision 0007](../decisions/0007-syscalls-via-int-0x50.md). The calls the
interactive shell needs (`SYS_READKEY`, `SYS_LIST`, `SYS_RUN`, `SYS_READFILE`,
`SYS_WRITEFILE`, `SYS_DELETE`, `SYS_FREECOUNT`, and now `SYS_STAT`) are covered here
and in [shell.md](shell.md); see
[decision 0016](../decisions/0016-interactive-shell.md),
[decision 0020](../decisions/0020-writable-fat32.md), and
[decision 0021](../decisions/0021-sys-stat.md). The descriptor calls (`SYS_WRITE` and
`SYS_READ` on a fd, `SYS_CLOSE`, `SYS_PIPE`) have their own pages,
[descriptors.md](descriptors.md) and [pipes.md](pipes.md); see
[decision 0022](../decisions/0022-file-descriptors-and-pipes.md). The signal calls
(`SYS_SIGNAL`, `SYS_KILL`, `SYS_SIGRETURN`, `SYS_SETFG`, `SYS_TASKS`) have their own
page, [signals.md](signals.md); see
[decision 0023](../decisions/0023-signals.md).

## The doorway

There is exactly one syscall entry point: IDT vector `SYSCALL_VECTOR` (0x50),
installed by `isr_install()` in `kernel/isr.c` with the flags byte `GATE_USER`
(0xEE) = present, **DPL 3**, 64-bit interrupt gate. The DPL is what lets ring-3
`int 0x50` reach the gate; every other gate is DPL 0, so this is the only vector
a user program can raise on purpose. It is an interrupt gate, not a trap gate, so
the CPU clears IF on entry: a syscall runs with interrupts masked and cannot be
nested by another syscall or an IRQ.

When ring-3 code runs `int 0x50`, the CPU switches to the ring-0 stack from
`tss.rsp0`, pushes the usual interrupt frame, and vectors to `syscall_stub`
(`kernel/isr_stubs.asm`). The stub pushes a dummy error code (a software
interrupt carries none) and the vector number, saves all 15 general-purpose
registers, and calls `syscall_handler(registers_t*)` with the frame pointer in
RDI. This is the exact same shape as every other interrupt stub; the syscall path
adds no new stack layout.

## Calling convention

| Register | Role |
|----------|------|
| RAX | syscall number (in), return value (out) |
| RDI | first argument |
| RSI | second argument |
| RDX | third argument |

This is System V argument order, with RAX carrying the call number in and the
result out. `syscall_handler` (`kernel/syscall.c`) switches on `regs->rax` and
writes the result back into `regs->rax`; when the stub runs `POP_GPRS` and
`iretq`, the caller finds the return value in RAX.

The numbers themselves live in `include/syscalls.h`, which is deliberately
standalone: numbers and nothing else, no types, no declarations, no includes. A
ring-3 program can include it without pulling in any kernel header, which it must
not do (kernel pages are not user-readable).

## The calls

| Number | Name | Arguments | Returns | Effect |
|--------|------|-----------|---------|--------|
| 0 | `SYS_EXIT` | RDI = exit status | does not return | Ends the calling task with that status. |
| 1 | `SYS_WRITE` | RDI = fd, RSI = buffer, RDX = length | bytes written (may be < length), or -1 | Writes the counted buffer to that descriptor (screen, or a pipe). |
| 2 | `SYS_READKEY` | none | one character (never 0) | Pops one key from the keyboard ring buffer, sleeping the caller until one arrives. |
| 3 | `SYS_LIST` | RDI = buffer, RSI = size | number of names, or -1 | Writes the root directory's file names into the buffer, newline-separated. |
| 4 | `SYS_RUN` | RDI = filename, RSI = in_fd, RDX = out_fd (-1 = fresh console), RCX = group request | child's task id, or -1 | Loads and starts the named program, giving it those descriptors as fd 0/1 and putting it in the requested process group. |
| 5 | `SYS_READFILE` | RDI = filename, RSI = buffer, RDX = size | bytes read, or -1 | Reads a whole file into the buffer. |
| 6 | `SYS_WAIT` | RDI = `uint64_t *out_id` or 0 | a child's exit status (0..255), or -1 | Blocks until any child exits; reports its id through out_id when nonzero. |
| 7 | `SYS_WRITEFILE` | RDI = filename, RSI = buffer, RDX = length | 0 on success, -1 on failure | Creates or wholly replaces a file with the buffer's bytes. |
| 8 | `SYS_DELETE` | RDI = filename | 0 on success, -1 on failure | Deletes a file from the root directory. |
| 9 | `SYS_FREECOUNT` | none | free-cluster count | Reports how many clusters on the volume are free. |
| 10 | `SYS_STAT` | RDI = filename, RSI = `uint64_t *out_size` | 0 on success, -1 if not found | Reports a file's size without reading it. |
| 11 | `SYS_READ` | RDI = fd, RSI = buffer, RDX = length | bytes read, 0 at EOF, or -1 | Reads from that descriptor (keyboard, or a pipe); blocks when there is nothing yet. |
| 12 | `SYS_CLOSE` | RDI = fd | 0, or -1 on a bad fd | Closes the descriptor; a pipe end's close may wake a peer or free the pipe. |
| 13 | `SYS_PIPE` | RDI = `int[2]` out | 0 (writes `[read_fd, write_fd]`), or -1 | Creates a pipe and puts its two ends in the caller's table. |
| 14 | `SYS_SIGNAL` | RDI = signal, RSI = handler (0 = default), RDX = trampoline | 0, or -1 | Installs a ring-3 handler for that signal on the calling task. `SIG_KILL` is refused. |
| 15 | `SYS_KILL` | RDI = task id, RSI = signal | 0, or -1 | Raises that signal on that task. |
| 16 | `SYS_SIGRETURN` | none | does not return normally | Restores the context saved when a handler was delivered. Only the trampoline calls it. |
| 17 | `SYS_SETFG` | RDI = pgid | 0, or -1 | Makes that process group the foreground, the one Ctrl-C is addressed to. |
| 18 | `SYS_TASKS` | RDI = `task_info_t` buffer, RSI = size | number of entries, or -1 | Fills the buffer with one entry per live task. |

**`SYS_RUN` grew a fourth argument in RCX**, the process group the child joins: 0 to
inherit the caller's (the old behaviour), `SYS_RUN_GROUP_NEW` for a new group led by
the child, or an existing group id to join. The three arguments already there keep
exactly their old meanings and values, so every existing call site means what it
always did. RCX rather than R10 because this kernel enters through `int 0x50`, which
— unlike the `syscall` instruction — does not clobber it, so the fourth argument sits
where the System V C ABI already puts it. A group the caller may not join **fails the
run** rather than quietly inheriting: a half-built job group is not something a
caller could detect afterwards.

**A blocking call can now return -1 because a signal arrived.** When a signal is
raised on a task parked in a syscall, the kernel wakes it, fails the call, and
delivers the handler on the way out — deliberately, so the call cannot silently
re-issue and swallow the signal. `SYS_READKEY`, `SYS_READ`, `SYS_WRITE` and
`SYS_WAIT` are all affected. A caller must not treat that -1 as data; see
[signals.md](signals.md).

`SYS_EXIT` **ends the calling task**, and no longer halts the machine. That was
what it meant when there was no scheduler and no parent to return to; now the task
leaves the rotation for good, its address space goes back to the frame allocator,
and a parent blocked in `SYS_WAIT` is woken with its status.

The status is **masked to 0..255**. That is not tidiness: `SYS_WAIT` returns the
status in RAX and returns `(uint64_t)-1` for "you have no children", so an
unmasked status of -1 would be indistinguishable from the error. Masking makes the
two ranges disjoint by construction.

Exiting is a **two-phase death**. `task_exit` does paperwork only — mask the
status, mark the task `TASK_ZOMBIE`, wake the parent, switch away — and frees
nothing at all, because the task calling it is the task currently on the CPU and
its address space is the one CR3 points at. The freeing happens later, from the
scheduler's sweeper and from the parent's `SYS_WAIT`. See
[scheduling.md](scheduling.md) and
[decision 0018](../decisions/0018-process-lifecycle-exit-and-wait.md).

`SYS_WAIT` is **any-child, not `waitpid`**. Its one argument (`RDI`) is only an
optional out-pointer for the id of whichever child exited, not a way to name one to
wait for: a caller with several children is still told about whichever it finds
finished first and cannot ask about a particular one — it just learns, afterwards,
which one that was. (A shell running a pipeline uses that to match the reaped child
to its last stage.) If a finished child is already waiting, it returns that
child's status immediately and frees the tombstone; if the caller has children but
none has finished, it blocks with `WAIT_CHILD` until one exits; if the caller has
no children at all, it returns `(uint64_t)-1` rather than blocking forever on
something that can never happen.

There is no crt0 and nothing wraps `_start`, so **a program must call `SYS_EXIT`
itself**. One that falls off the end of `_start` executes whatever bytes follow it.

`SYS_READKEY` is the consumer end of the keyboard ring buffer (`drivers/keyboard.c`,
documented in [shell.md](shell.md)). It is **blocking**: on an empty buffer the
kernel parks the calling task and the keyboard IRQ wakes it when a key arrives, so
the call costs nothing while it waits and there is no reason to poll it. From ring
3 the wait is invisible, one `int 0x50` that took a while to come back, and the
call always returns a real character. It used to return 0 immediately on an empty
buffer and be polled in a loop, which burned every slice the caller was given; see
[blocking.md](blocking.md) and
[decision 0017](../decisions/0017-blocking-and-sleep.md).

`SYS_READKEY` and `SYS_WAIT` do not return through the dispatcher, and `SYS_EXIT`
does not return at all. The blocking path rewinds the saved `rip` onto the `int` so
the woken task re-issues the call, and the CPU reads the syscall number from `rax`
when it does, so **nothing may write `rax` on a blocking path**. Both blocking
handlers therefore take the register pile and write `regs->rax` themselves, only on
the path that has an answer, rather than returning a value for the dispatcher to
store. Since `SYS_EXIT` is 0, a stray `return 0` here would not merely corrupt a
result: the re-armed `int` would read 0 out of RAX and kill the caller. The
dispatcher's cases for these three are bare statements with no `regs->rax =` in
front of them, and that is load-bearing, not style.

`SYS_LIST`, `SYS_RUN`, and `SYS_READFILE` are the shell's data calls. `SYS_LIST`
fills a buffer through `fat32_list_names`; `SYS_READFILE` fills one through
`fat32_read_file`; `SYS_RUN` copies in a filename and calls
`task_create_from_file`, which loads the program and registers it with the
scheduler (a failed load returns -1 and never faults the kernel). Each takes an
untrusted pointer from ring 3, checked as described below.

`SYS_RUN` also records **who is running the program**: it passes
`scheduler_current_id()` as the new task's `parent_id`, which is what makes a later
`SYS_WAIT` from the same task find it. The id has to be read here, inside the
syscall, where "the current task" still means the caller; by the time the new task
runs, `current` is somebody else.

`SYS_WRITEFILE`, `SYS_DELETE`, and `SYS_FREECOUNT` are the write side, added with
the writable filesystem ([decision 0020](../decisions/0020-writable-fat32.md)).
`SYS_WRITEFILE` is the mirror of `SYS_READFILE`: it copies the filename in and
bounds-checks the source `[buf, buf+len)` range before `fat32_write_file` creates or
wholly replaces the file. `SYS_DELETE` copies a filename in and calls
`fat32_delete`. `SYS_FREECOUNT` takes no pointer at all — a number crosses the
boundary by value — and returns `fat32_free_count`, which walks the whole FAT; it
exists so the shell's `free` command, and the leak test built on it, can watch the
free-cluster count from ring 3, the same idea as `SYS_RUN` reporting the free-frame
count one layer up. **None of the three blocks**, so unlike `SYS_READKEY` and
`SYS_WAIT` none has the RAX-discipline problem below: each computes an answer and
returns it through the dispatcher normally.

`SYS_STAT` reports a file's size without reading it, wrapping `fat32_stat`
([decision 0021](../decisions/0021-sys-stat.md)). It is what lets the shell's
`read` ask how big a file is before it commits a buffer, so it can tell a missing
file from one too large for the buffer and report the size instead of a bare
failure. Its `out_size` pointer is a **write target**: the kernel writes a
`uint64_t` through it, so the whole `[ptr, ptr+8)` range is bounds-checked with
`user_range_ok` exactly as `SYS_READFILE` checks its destination buffer — a
start-only check would let a pointer just below `USER_REGION_END` have the kernel
write off the end of the region. It returns `(uint64_t)-1` when the file is not
found (a directory or a non-8.3 name folds into the same error), and like the write
side it **does not block**, so it has no RAX-discipline problem.

An **unknown syscall number** is not fatal. `syscall_handler` prints the offending
number and returns `(uint64_t)-1` in RAX; a bad request from ring 3 must never
fault or halt the kernel.

## Untrusted pointers are bounded over their whole range

Every pointer these calls take comes from ring 3 and is **untrusted**: a program
could pass a kernel address and turn the kernel into a confused deputy, reading or
writing memory it is not allowed to. `kernel/syscall.c` has two shared helpers, and
every call that takes a pointer uses one of them before it touches a byte.

`user_range_ok(ptr, len)` confirms that all of `[ptr, ptr+len)` lies inside the
ring-3 region (`USER_REGION_START`..`USER_REGION_END`, i.e. 4-8M, the constants
`kernel/memory.c` reserves). It is careful about overflow: `ptr + len` can wrap on a
crafted length and a wrapped sum compares as comfortably small, so `len` is checked
against the room above `ptr` (`USER_REGION_END - ptr`) rather than by forming the sum.
`SYS_WRITE` and `SYS_READ` bound their counted buffers with it — and copy them through
a kernel staging buffer, since a counted buffer may contain zero bytes and need not be
NUL-terminated, so `copy_user_string` would be wrong for it. `SYS_LIST`,
`SYS_READFILE`, and `SYS_WRITEFILE` bound theirs. The **write-target pointers** —
`SYS_STAT`'s `out_size`, `SYS_PIPE`'s `int[2]`, and `SYS_WAIT`'s `out_id` — are bounded
the same way before the kernel writes through them, which is not just a read check: a
pointer just below `USER_REGION_END` could otherwise have the kernel write off the end
of the region.

`copy_user_string(ptr, dst, cap)` copies a NUL-terminated string in from ring 3 with a
length cap, so a string with no terminator cannot walk off the region: it bounds-checks
the start pointer, then copies until a NUL, until the cap, or until `USER_REGION_END`,
whichever comes first, and always NUL-terminates. `SYS_RUN`, `SYS_READFILE`,
`SYS_WRITEFILE`, `SYS_DELETE`, and `SYS_STAT` copy their filenames in with it.

This is the same category of check as the ELF loader's segment bounds
([elf-loading.md](elf-loading.md)). It still checks virtual addresses against the fixed
region constants rather than walking the caller's page tables, so it is not yet full
per-process validation — recorded as a TODO in `kernel/syscall.c` and in
[../project-status.md](../project-status.md) — but it bounds the whole range and caps
the length. `SYS_WRITE` was once a start-pointer-only stopgap that did neither; when it
became a counted `(fd, buf, len)` call ([descriptors.md](descriptors.md)) it moved onto
`user_range_ok` like the rest.

## The ring-3 side

`user/userlib.h` shows the caller's half. The raw `int 0x50` is wrapped in
`always_inline` helpers built on inline asm with explicit register constraints
(`"a"` = RAX, `"D"` = RDI, `"S"` = RSI, `"d"` = RDX), one per arity: `syscall0`
through `syscall3`, with `sys_write`, `sys_exit`, `sys_wait`, `sys_readkey`,
`sys_list`, `sys_run`, `sys_readfile`, `sys_writefile`, `sys_delete`,
`sys_freecount`, and `sys_stat` over them. `SYSCALL_VECTOR` reaches the `int`
instruction as an immediate through an `"i"` constraint so the vector stays a named
constant. `always_inline` is kept: it folds the trap
directly into the caller, so every instruction the program runs is inside its own
mapped text and there is no call through a symbol the (relocation-free) loader
would have to resolve. It used to be load-bearing for a sharper reason, back when
an out-of-line helper could land in kernel pages at 1M and fault a ring-3 call.

The strings the program prints need no special section any more. A program is now
linked on its own at 0x400000 (`user/user.ld`), so its ordinary `.rodata` already
lands in the 4-8M user region where a ring-3 pointer is allowed to point. When the
programs were compiled into the kernel, a plain string literal would have landed
in the kernel's `.rodata` at 1M, where the pointer would both fail the bounds
check above and fault a ring-3 read; that is why the old build forced them into a
`.user_rodata` section by hand.

## What a run looks like

The machine boots into `SHELL.ELF`, which prints a prompt and blocks in
`SYS_READKEY`, echoing each key with `SYS_WRITE` to fd 1, tokenizing each line, and
dispatching the commands in [shell.md](shell.md). Booted under QEMU and driven by a
scripted key sequence, a real session prints this (regenerated from an actual boot;
the free-frame and heap numbers are that boot's):

```
> help
commands:
  list                 list files in the root directory
  read <file>          print a file's contents
  write <file> <text>  write the rest of the line to a file (creates/replaces)
  delete <file>        delete a file
  free                 how many clusters on the volume are free
  run <file>           run a program and wait for it to finish
  help                 show this list
  clear                clear the screen
  return <text>        print the text back
> list
HELLO.TXT
TEST.TXT
BIG.TXT
A.ELF
B.ELF
C.ELF
SHELL.ELF
D.ELF
E.ELF
HUGE.TXT
F.ELF
COUNT.ELF
UPPER.ELF
G.ELF
> read HELLO.TXT
Hello from FAT32!
> run a.elf
run: started a.elf
AAAAAAAAAAAAAAAAAAAAreap (wait):    task 1 exited (status 0), free frames: 30587, heap used: 616
run: a.elf exited with status 0
>
```

`run a.elf` calls `SYS_RUN`, which loads `A.ELF` as a new task with the shell as
its parent, and then `SYS_WAIT`, which blocks the shell until A is finished. A's
`SYS_WRITE` output appears while the shell sleeps, which is the scheduler running
what is left after the shell steps out of the rotation; the prompt comes back only
once `SYS_WAIT` returns A's status. **The `A`s running straight into the reap line
is real, not a typo**: A prints no trailing newline, so its twenty `A`s and the
kernel's report share a line. Exactly how many `A`s precede the report, and whether
one slips into the shell's own `run: started` line, is a matter of scheduler timing
and can differ on another boot. The `reap (wait):` line is the `LIFECYCLE_DEBUG`
report at the moment A's address space goes back to the pools; its two numbers — the
free frame count and `heap used` — are the leak test, and both must return to the
same value after identical work. `run c.elf` prints `exited with status 3`,
which is the proof that the number survives the whole trip from the child's RDI to
the parent's RAX. Over the session `-d int` shows only
timer (`v=40`), keyboard (`v=41`), and syscall (`v=50`) vectors, all at `cpl=3` for
the ring-3 traps, with no `#GP` (0x0D), no `#PF` (0x0E), no double fault (0x08), no
triple fault, and no disk IRQ (0x4E). `v=50` now appears only when the shell has
something to do: over a six second idle window at the prompt the kernel services
three syscalls, down from 362,648 when `SYS_READKEY` was polled.

Passing a bad fd, a wrong-direction fd, or an out-of-region buffer to `SYS_WRITE` or
`SYS_READ` returns `-1` (silently — a rejected descriptor is a routine program error,
not worth a console line), and the disk and directory calls reject an out-of-region
buffer or filename the same way (those do print a one-line reason), none copying
anything from kernel memory.

## Related

- Why one DPL 3 gate and not `syscall`/`sysret`:
  [decision 0007](../decisions/0007-syscalls-via-int-0x50.md).
- The IDT gate, the flags byte, and the stub shape:
  [idt.md](idt.md).
- The ring-3 drop that precedes any syscall:
  [user-mode.md](user-mode.md) and
  [decision 0006](../decisions/0006-user-mode-with-separate-pages.md).
- The region the pointer check reuses: [memory-map.md](memory-map.md).
- The shell that uses `SYS_READKEY`/`SYS_LIST`/`SYS_RUN`/`SYS_READFILE`, and the
  keyboard ring buffer behind `SYS_READKEY`: [shell.md](shell.md) and
  [decision 0016](../decisions/0016-interactive-shell.md).
- How a syscall sleeps its caller and is re-issued on wake:
  [blocking.md](blocking.md) and
  [decision 0017](../decisions/0017-blocking-and-sleep.md).
- What `SYS_EXIT` and `SYS_WAIT` do to the task table, and who frees what:
  [scheduling.md](scheduling.md) and
  [decision 0018](../decisions/0018-process-lifecycle-exit-and-wait.md).
