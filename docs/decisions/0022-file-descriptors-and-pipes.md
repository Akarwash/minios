# 0022 - File descriptors and pipes

## Status

Accepted. Partly superseded — the decision stands, three details of the body do not.

- **Signals are no longer deferred.** Decision 1 of this ADR excluded them
  ("Signals share no machinery with this. Both are later rungs"), which was right
  about the mechanism and wrong about the consequences.
  [0023](0023-signals.md) adds them.
- **`pipe_write` against a dead reader raises `SIG_PIPE`** rather than returning
  a bare error. The body's reasoning — that an error return was enough for a
  kernel with no signals — was true when written and is no longer the behaviour:
  an error return only reaches a program that checks return values, and a writer
  that does not spins forever.
- **A console now has an end of file.** The body says signalling one would need a
  Ctrl-D-style key and a line discipline the driver does not have; [0023](0023-signals.md)
  adds exactly that, as one flag.

The descriptor table, the one-owner-per-slot rule, the reader/writer counts, the
block/wake rules and the six pipe bug classes (B1–B6) are unchanged. See
[reference/pipes.md](../reference/pipes.md) and
[reference/signals.md](../reference/signals.md) for the current state.

## Context

Ring-3 programs could write only one place. `SYS_WRITE` printed a string to the
screen, full stop; a program had no way to send its output anywhere but the console
and no way to read anything but the keyboard (`SYS_READKEY`) or a whole file off the
disk (`SYS_READFILE`). There was no notion of "output" as a thing distinct from "the
screen", so two programs could not be connected: nothing carried one program's bytes
to another's input.

The Unix answer is a **file descriptor** — a small integer naming an open
destination, so a program writes "fd 1" without knowing or caring whether that is a
screen, a file, or a pipe into another program — and a **pipe**, an in-kernel byte
buffer with a write end and a read end. With those two, a shell can run
`A | B` by handing `A`'s output end and `B`'s input end to the same pipe. This rung
adds both, and teaches the shell `|`.

The blocking machinery this builds on already existed ([0017](0017-blocking-and-sleep.md)):
a task with nothing to do parks and is woken by whatever causes the event it waited
for. A pipe is exactly that shape — a reader waits for bytes, a writer waits for room
— so it needs two new `wait_reason_t` values and a waker in the right place, not a
new mechanism. [0017](0017-blocking-and-sleep.md) predicted this: "Pipes and waiting
on the disk are the same shape."

## Decision

Give every task a fixed table of open descriptors, make pipes one kind of entry in
it, and route reads and writes through the table. Eight decisions, each taken as
stated and implemented as specified.

1. **Descriptors and pipes only.** No file redirect (`> OUT.TXT`) and no signals.
   Redirect needs streaming writes to a growing file, which the whole-file FAT32
   layer ([0020](0020-writable-fat32.md)) cannot do — it writes a file all at once.
   Signals share no machinery with this. Both are later rungs.

2. **`SYS_WRITE` and `SYS_READ` are `(fd, buf, len)` and return a count that may be
   less than `len`.** Partial transfers are the mechanism, not an edge case: a pipe
   takes only what fits, and one call moves at most a kernel staging buffer
   (`SYSCALL_IO_MAX`, 4096). Every caller loops (see B5).

3. **All programs migrate at once.** No compatibility shim and no two coexisting
   `SYS_WRITE` signatures. The old single-argument `sys_write(str)` became
   `sys_print(str)`, a loop over the new `sys_write(1, ...)`, and every call site
   moved in one change.

4. **`SYS_READKEY` stays, unchanged, beside `sys_read(0, ...)`.** The shell's line
   editor keeps using `SYS_READKEY`. Removing it would mean rewriting the shell's
   input loop, doubling this rung's blast radius for no gain.

5. **`sys_run(name, in_fd, out_fd)`, with -1 meaning "a fresh console entry".** This
   is the only way a child acquires a descriptor, because nothing can inject one into
   a running task: a task's descriptors are set at creation and thereafter only it can
   change them. A pipeline therefore hands its ends across at `SYS_RUN`.

6. **`MAX_FDS` is 8, a fixed array in `task_t`.** Not growable. The shell uses 0 and
   1 plus up to two more per pipe while wiring a stage; 8 is plenty and keeps the
   table a flat, cheaply-scanned thing.

7. **Arbitrary N-stage pipelines, not a two-stage special case.** A two-stage
   implementation is a different shape from the N-stage loop rather than a subset of
   it, so building it first would mean writing the descriptor bookkeeping twice. The
   shell has one loop that carries a read end from each stage to the next.

8. **The pipeline's exit status is the last stage's,** matching `$?` in a real shell.
   To identify the last stage among the children it reaps, `SYS_RUN` returns the
   child's task id and `SYS_WAIT` gained an optional out-pointer that reports which
   child exited — extending the existing call rather than adding a fifteenth. The
   shell keeps the status whose id matches the last stage.

### The shape of the code

A `file_t` (`kernel/file.h`) is `{kind, pipe, writable}`, owned by exactly one table
slot in one task. `file_read`/`file_write` switch on `kind`: a console reads the
keyboard / writes the screen, a pipe defers to `pipe_read`/`pipe_write`. A `pipe_t`
(`kernel/pipe.h`) is the keyboard driver's ring buffer plus a `readers` count and a
`writers` count. `SYS_PIPE` makes one pipe and two ends; `close_fd` frees an end and,
for a pipe, drops the count and frees the buffer when both counts are zero. Four new
syscalls in all (`SYS_READ` 11, `SYS_CLOSE` 12, `SYS_PIPE` 13, and the reshaped
`SYS_WRITE`), for fourteen.

## The EOF argument

End of file is the subtle part, and three questions decide it.

**Why the counts are counts, not booleans.** It is tempting to store "is there a
writer" as one bit. But the shell, setting up `A | B`, necessarily holds BOTH ends of
the pipe for a moment: it calls `SYS_PIPE`, gets both, and passes copies to the
children before closing its own. During that window a pipe has two write ends (the
shell's and `A`'s) and two read ends (the shell's and `B`'s). A boolean cannot count
that down correctly as the ends close one by one. The count is the number of live
ends, and it is the single place a pipe end is tallied.

**Why empty is not EOF.** An empty pipe means "no bytes buffered right now", which is
the normal state between a slow writer's writes. EOF means "no bytes will ever come".
The difference is whether a writer still exists: `pipe_read` on an empty pipe blocks
if `writers > 0` and returns 0 (EOF) only if `writers == 0`. Treating empty as EOF
would end a read the instant it caught up to a live writer.

**Why closing must wake.** A reader parked on an empty pipe is woken by a write. But
the last writer might not write again — it might just close, or exit (which closes).
If closing did not wake, that reader would wait forever for an EOF it cannot observe,
because observing it requires the reader to run. So `close_fd`, when it drops the last
writer, wakes `WAIT_PIPE_READ`; when it drops the last reader, wakes
`WAIT_PIPE_WRITE` so a blocked writer learns the pipe is broken. A close is an event,
and the rule from [0017](0017-blocking-and-sleep.md) — whoever causes the condition
wakes the waiters — applies to it exactly as to a write. `task_exit` closes every
descriptor a task holds precisely so a program that simply exits still delivers EOF
downstream; that fd-close loop is what makes `A | B` end rather than hang.

## Six ways this goes wrong

Five of these hang or corrupt silently, with no message and no obvious link to the
line that caused them. They are what this rung is made of. Each has a fix, a comment
at the site, and is listed here so that someone who has never seen the code can
recognise the symptom and find the cause — because working code shows the fix, not
the failure it prevents.

### B1 — the shell keeps its own copy of a pipe end

**Symptom.** The pipeline hangs. No output, no error, no prompt, and no way out —
there are no signals.

**Cause.** The shell calls `SYS_PIPE`, so both ends land in its table, and it passes
copies to the children. If it does not then close its own, `writers` never reaches
zero, the downstream reader blocks forever on an EOF that cannot arrive, and the shell
blocks in `SYS_WAIT` for a child that will never exit.

**Prevented by** the two `sys_close` calls in `run_pipeline` (`user/shell.c`),
immediately after each stage is spawned: once the child has its copy the shell drops
its own. Verified by deliberately removing the write-end close and watching
`run a.elf | run count.elf` hang with no output, then restoring it.

### B2 — closing does not wake

**Symptom.** The same hang as B1, and it survives fixing B1, which makes it look as if
B1 was not really fixed.

**Cause.** A reader blocked on an empty pipe is woken by a write. If the last writer
closes instead of writing, nothing wakes it, and it waits forever for an EOF it cannot
observe.

**Prevented by** the wake in `file_close` (`kernel/file.c`): when `--writers == 0` it
calls `scheduler_wake(WAIT_PIPE_READ)` (and symmetrically wakes `WAIT_PIPE_WRITE` when
the last reader closes). This is [0017](0017-blocking-and-sleep.md)'s rule applied to
a thing that does not look like an event: "the last writer went away" is a condition a
reader is blocked on.

### B3 — RAX written on a blocking path

**Symptom.** A woken task issues a syscall nobody called, or exits mid-pipeline for no
reason. The failure looks unrelated to pipes.

**Cause.** `task_block` rewinds `rip` onto the `int 0x50`, so the re-executed
instruction reads the syscall number back out of `RAX`. A return value written there
before blocking becomes the next syscall number — and `SYS_EXIT` is 0.

**Prevented by** `sys_read` and `sys_write` taking the register pile, writing `RAX`
themselves, and returning without touching it on the block path (they propagate
`FILE_BLOCKED` from `file_read`/`file_write`, and the dispatcher calls them as bare
statements, never `regs->rax = sys_read(...)`). This is the same discipline
`SYS_READKEY` and `SYS_WAIT` already carry.

### B4 — two places counting the same thing

**Symptom.** A pipe freed while a task still holds an end (a use-after-free that
surfaces later, somewhere unrelated), or a pipe never freed at all. Both quiet.

**Cause.** An earlier draft gave `file_t` a `refs` count as well as the `pipe_t` its
`readers`/`writers`. For a pipe end those are the same fact stored twice, so every
open and close has to keep them in agreement and nothing checks that they do.

**Prevented by** giving `file_t` NO refcount. It belongs to exactly one slot in one
task; `close_fd` frees it unconditionally. The only shared object is the `pipe_t`,
counted in one place. `file.h` says so at the type, so nobody "fixes" the apparent
duplication of `file_t` allocations (a child's inherited end is a second `file_t`, on
purpose) by sharing them.

### B5 — assuming one call moved everything

**Symptom.** Output truncated at 4096 bytes, or at some ragged number that changes
between runs.

**Cause.** `SYS_WRITE` returns how many bytes fit, which may be fewer than asked. A
caller that ignores the return value silently drops the rest; a `SYS_READ` that
assumes one call drained the stream stops early.

**Prevented by** looping at every transfer site: `sys_print` in `userlib.h` wraps the
write-until-done loop for the common case, and `COUNT.c`/`UPPER.c` loop on both reads
and writes. Verified by pushing 16KB (`F.c`) through a 4096-byte pipe into `count.elf`
and getting 16384.

### B6 — descriptors leaking into the shell's table

**Symptom.** The first pipeline works; the second or third fails to start, with an
error rather than a hang.

**Cause.** `MAX_FDS` is 8 and the shell starts with 0 and 1 used. Each pipe
temporarily consumes two more. A failure path that returns without closing, or a
missed close in the loop, permanently consumes slots until `alloc_fd` runs out.

**Prevented by** every early return in `run_pipeline` closing whatever it has already
opened, including the paths where `sys_pipe` or a stage fails to start. Verified by
running the same three-stage pipeline ten times in one session and having the tenth
behave like the first.

## Consequences

- **No file redirect, no `dup`, no `select`/`poll`.** A descriptor cannot be pointed
  at a disk file, cannot be duplicated onto a chosen number by a program (only the
  kernel inherits ends, at `SYS_RUN`), and a program cannot wait on several at once.
  Each is a later rung and none is needed for `A | B`.
- **A pipe is a byte stream with no message boundaries.** Two writes of 5 bytes and
  one read of 10 are indistinguishable from one write of 10. Programs that need
  records frame them themselves.
- **No SIGPIPE.** Writing to a pipe with no reader returns an error (`FILE_ERR`)
  rather than raising a signal, because there are no signals; the writer sees a failed
  `sys_write` and stops.
- **No partial-read offset and no seek.** `read` on a large disk file still refuses
  rather than streaming; that is [0021](0021-sys-stat.md)'s open question, and pipes
  do not change it.
- **`scheduler_wake` is a broadcast**, so every task blocked on a pipe reason wakes
  and all but the one that can proceed re-block. Harmless at this scale (a handful of
  tasks); a kernel with many blocked readers would keep a per-pipe wait queue, the
  same refinement [0017](0017-blocking-and-sleep.md) notes for the keyboard.
- **The console has no EOF.** `sys_read(0, ...)` blocks for keys and never returns 0,
  because a keyboard is never "done" and there is no Ctrl-D line discipline. A program
  reading the console until EOF does not terminate on its own.
- **Two existing syscalls changed their contract to support the last-stage status.**
  Reporting the pipeline's status as the last stage's (decision 8) needed a way to
  match a reaped child to the stage that started it, and that was bought by changing
  two calls rather than adding a fifteenth:
  - **`SYS_RUN`'s return value changed meaning.** It used to return 0 on success; it
    now returns the child's **task id** (still `SYSCALL_ERROR` on failure). Every
    surviving caller checks `== SYS_FAIL` and so still works, but *only* because a
    child's id is always **>= 1** (id 0 is the boot task, which nothing runs), so a
    successful id can never collide with the failure value. This is a silent change
    to an existing call's contract: a future caller that tested `== 0` for success —
    the old convention — would be **silently wrong**, treating every successful `run`
    as a failure. Documented in `docs/reference/syscalls.md`.
  - **`SYS_WAIT` gained an optional out-pointer.** `RDI`, when nonzero, is a
    `uint64_t *` that receives the exited child's id (0 means "do not report it", which
    is what the old no-argument `sys_wait()` now passes). This is an addition rather
    than a break — an existing `sys_wait()` still returns the status in `RAX` — but it
    is the same kind of quiet contract growth and is called out here for the same
    reason.

## Related

- The blocking and waking this builds on:
  [0017](0017-blocking-and-sleep.md), [../reference/blocking.md](../reference/blocking.md).
- The exit/wait lifecycle the fd-close loop and status reporting extend:
  [0018](0018-process-lifecycle-exit-and-wait.md).
- The syscall gate and the `(fd, buf, len)` calls:
  [0007](0007-syscalls-via-int-0x50.md), [../reference/syscalls.md](../reference/syscalls.md).
- The reference pages for this rung:
  [../reference/descriptors.md](../reference/descriptors.md),
  [../reference/pipes.md](../reference/pipes.md),
  [../reference/shell.md](../reference/shell.md).
- The whole-file FAT32 layer that is why there is no redirect yet:
  [0020](0020-writable-fat32.md).
