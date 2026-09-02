# Shell reference

TownOS boots into an interactive shell that is a ring-3 program, `SHELL.ELF`,
loaded off the disk like any other. It reads typed commands and runs them using
nothing but syscalls: it holds no privilege and touches the keyboard, screen,
filesystem, and loader only through the `int 0x50` gate. This page documents the
read-match-do loop, the keyboard ring buffer behind `SYS_READKEY`, the syscalls the
shell needs, the tokenizer, and the command table. Read from
`user/shell.c`, `user/userlib.h`, `drivers/keyboard.c`, and `kernel/syscall.c`.
For the rationale, see [decision 0016](../decisions/0016-interactive-shell.md); for
the `write`/`delete`/`free` commands, [decision 0020](../decisions/0020-writable-fat32.md).

## The command table

The names are TownOS's own and deliberately not the Unix ones.

| Command | Argument | Effect |
|---------|----------|--------|
| `list` | none | List the files in the root directory. |
| `read` | a filename | Print that file's contents. |
| `write` | a filename, then text | Write the rest of the line to the file, creating or replacing it. |
| `delete` | a filename | Delete the file. |
| `free` | none | Print how many clusters on the volume are free. |
| `run` | a filename | Run that program and wait for it to finish, then report its exit status. |
| `help` | none | Print the command list. |
| `clear` | none | Clear the screen (scroll it away with newlines). |
| `return` | text | Print the text back. |
| `ps` | none | List the live tasks: id, parent, process group, state, and a zombie's exit status. |
| `kill` | a task id, optionally a signal | Send that signal to that task, defaulting to `SIG_INT` (2). |

An empty line does nothing. Any other first word prints `unknown command: <word>`.
Filenames are 8.3 and case-insensitive, so `read hello.txt` finds `HELLO.TXT`.

`write NAME.TXT the rest of this line` stores everything after the filename
verbatim — internal spaces kept, no trailing newline added — so what was typed
becomes the file. An empty remainder (`write EMPTY.TXT`) writes a zero-length file.
A typed line is far shorter than one 512-byte cluster, so `write` only ever makes
single-cluster files; the multi-cluster path is exercised by `F.ELF`
([user/tests/README.md](../../user/tests/README.md)), not by typing. `write` and
`delete` reject a name that will not fit 8.3 (`write my-notes.text hello` →
`write: my-notes.text is not an 8.3 name (max 8 chars, dot, 3 chars)`), checked in
the shell before the syscall, because that failure is the user's and fixable by
retyping — the one case worth telling apart from a disk error. See
[decision 0020](../decisions/0020-writable-fat32.md).

**Command names are case-SENSITIVE and filenames are not.** `strcmp` compares
bytes, so `RUN a.elf` prints `unknown command: RUN`, while `run A.ELF` and
`run a.elf` both work because the FAT32 layer uppercases the name before looking
it up. This is only reachable now that shift exists (see
[keyboard.md](keyboard.md)); it is left as it is, because the shell's commands are
its own vocabulary and matching them loosely buys nothing.

## Pipelines

A line containing `|` is a **pipeline**: `run A | run B | run C` runs the stages at
once and connects each stage's output to the next stage's input through a pipe, so
`B` reads what `A` writes. Every stage must be `run <file>` — the other commands are
builtins that write to the shell's own console rather than to a child, so they cannot
be a stage. Up to four stages (`SHELL_MAX_SEGS`); more is rejected. A line with no
`|` takes exactly the single-command path it always has.

The shell creates one pipe per join, gives each stage the right ends via
`sys_run(name, in_fd, out_fd)` (the read end of the pipe behind it, the write end of
the pipe ahead of it, or a console end at the two outer edges), and then **closes its
own copies of every pipe end immediately**, which is load-bearing: the shell created
the pipe so it holds both ends, and if it kept them the write-end count would never
reach zero, the downstream reader would wait forever for an EOF that cannot arrive,
and the shell would then wait forever for a child that never exits. It waits for all
N stages and reports the **last** stage's exit status, matching `$?` in a real shell;
it matches the reaped child against the last stage by id (`sys_wait_id`).

**A slow pipeline is not a hang.** With three or more stages, a middle stage that
must consume all of its input before it produces any output — a counter, a sort —
leaves the stage after it blocked and idle for the whole run, and a full pipe
likewise blocks the stage before it (backpressure). From the outside that is
indistinguishable from a hang, but it is correct: the downstream stage is asleep
waiting for bytes that only arrive when the middle stage finishes, and it wakes and
runs the moment they do. See
[decision 0022](../decisions/0022-file-descriptors-and-pipes.md).

## The read-match-do loop

`_start` (`user/shell.c`) prints a prompt and then loops:

1. **Read a line.** `read_line` builds the line one keystroke at a time by calling
   `SYS_READKEY` in a loop. A printable character is appended to a fixed line
   buffer and echoed with `SYS_WRITE` so the user sees it. Enter (`\n`) ends the
   line. Backspace (`\b`) removes the last character and erases it on screen, and
   is guarded so it cannot chew back into the prompt. The line buffer is fixed at
   `SHELL_LINE_MAX` (128); once full, further printable characters are dropped
   rather than overflowed.

   **What can be typed** is the whole of printable US-layout ASCII: shift and caps
   lock are handled in the driver, so uppercase letters and the shifted symbols
   (`!@#$%^&*()_+{}|:"<>?~`) all arrive as ordinary characters. Arrow keys and
   function keys do not arrive at all, so there is no line history and no cursor
   movement within the line — backspace is the only edit. See
   [keyboard.md](keyboard.md).
2. **Tokenize.** `next_token` splits the line on spaces, in place. The first token
   is the command; a second token, where a command takes one, is the argument.
3. **Match and do.** A chain of string compares dispatches to one of the commands
   above. `list`, `read`, `write`, `delete`, `free`, and `run` call the syscalls
   below; `help`, `clear`, and `return` are handled with `SYS_WRITE` alone. `write`
   extracts the filename token first and then hands the untouched remainder of the
   line to the write call as the contents.
4. Reprint the prompt and loop.

Step 1 costs nothing while the user is thinking. `SYS_READKEY` blocks (see below),
so the read loop turns exactly once per keystroke and the shell is asleep the rest
of the time. It used to be a busy-wait, spinning on the syscall until a key
arrived, because the kernel had no way to sleep a task; see
[blocking.md](blocking.md).

## The keyboard ring buffer

The keyboard is a producer/consumer queue. The producer is the keyboard IRQ; the
consumer is `SYS_READKEY`. They are connected by a fixed circular buffer in
`drivers/keyboard.c`.

**The producer.** The keyboard IRQ (`keyboard_callback`) runs with interrupts
masked and must be short, so it does the least possible work: it reads one
scancode, updates the shift and caps-lock state if that is what the scancode was,
decodes the make code to ASCII through one of two tables, and pushes the character
into the ring buffer, then wakes any task asleep waiting for a key. A modifier key
does none of that and returns early, pushing nothing and waking nobody
([keyboard.md](keyboard.md)). It does not echo and does not run any command. Before the shell became a ring-3
program, this callback called `shell_handle_keypress` and did the whole line edit
and dispatch inside the interrupt; the ring buffer is what keeps that out of
interrupt context now.

**The wake, and its ordering.** The IRQ is what *causes* the event a sleeping
reader waits for, so waking is its job: a blocked task cannot notice a key
arriving, because it is not running. `kbd_buffer_push` comes first and
`scheduler_wake(WAIT_KEY)` second, and the order matters. A task woken before the
character was in the buffer could be scheduled, re-issue its read, find nothing,
and go back to sleep, turning one keypress into a wasted round trip. The wake only
marks tasks ready, it does not switch to them, which keeps this handler short. See
[blocking.md](blocking.md).

**The consumer.** `SYS_READKEY` (`kernel/syscall.c`) calls `keyboard_getchar`,
which pops one character from the ring, or returns 0 if it is empty. That 0 never
reaches ring 3: it is the signal for the handler to block the caller.

**The two indices.** `write_index` is the slot the next produced character goes
into; `read_index` is the slot the next consumed character comes out of. They chase
each other around `KBD_BUFFER_SIZE` (128) slots, each advance stepping forward one
slot and wrapping modulo the size.

- `write_index == read_index` means **empty**.
- To keep empty distinguishable from full with that one rule, one slot is always
  left unused: the ring holds at most `KBD_BUFFER_SIZE - 1` characters. If a full
  buffer were allowed to wrap `write_index` onto `read_index`, full would look
  exactly like empty.
- **Full-drop policy.** If advancing `write_index` would land on `read_index`, the
  ring is full and the new character is dropped, not stored. Dropping a keypress
  the user can retype is the lesser evil; overwriting input already accepted, and
  collapsing the empty/full distinction, is worse.

**No lock is needed.** The producer runs in the keyboard IRQ and the consumer in
the syscall handler; both the IRQ gate and the syscall gate clear IF on entry, so
on a single CPU the two never run at the same instant and the indices need no
guard.

**The empty sentinel.** 0 is a safe "nothing waiting" value because the scancode
table maps every unmapped key to 0 and the producer only pushes non-zero
characters, so a real 0 never enters the ring.

## The shell's syscalls

They follow the calling convention in [syscalls.md](syscalls.md): RAX carries the
number in and the result out, arguments in RDI/RSI/RDX. The ring-3 wrappers are in
`user/userlib.h`.

| Number | Name | Arguments | Returns |
|--------|------|-----------|---------|
| 2 | `SYS_READKEY` | none | one character (never 0) |
| 3 | `SYS_LIST` | RDI = buffer, RSI = size | number of names, or -1 |
| 4 | `SYS_RUN` | RDI = filename pointer | 0 on success, -1 on failure |
| 5 | `SYS_READFILE` | RDI = filename, RSI = buffer, RDX = size | bytes read, or -1 |
| 6 | `SYS_WAIT` | none | a child's exit status (0..255), or -1 |
| 7 | `SYS_WRITEFILE` | RDI = filename, RSI = buffer, RDX = length | 0, or -1 |
| 8 | `SYS_DELETE` | RDI = filename | 0, or -1 |
| 9 | `SYS_FREECOUNT` | none | free-cluster count |
| 10 | `SYS_STAT` | RDI = filename, RSI = `uint64_t *out_size` | 0, or -1 if not found |

- **`SYS_READKEY`** pops one buffered key (above). Blocking: on an empty buffer the
  kernel parks the calling task, and the keyboard IRQ wakes it when it pushes a
  character. From ring 3 the wait is invisible, so the call always returns a real
  character. See [blocking.md](blocking.md).
- **`SYS_LIST`** walks the FAT32 root directory (through `fat32_list_names`, the
  buffer-filling sibling of `fat32_list_root`) and writes the file names into the
  caller's buffer, one per line, NUL-terminated. Names that do not all fit are
  dropped from the end. Returns the count.
- **`SYS_RUN`** copies the filename into the kernel and calls
  `task_create_from_file`, which loads the program into a fresh address space and
  registers it with the scheduler, recording the shell as its parent. The launched
  program joins the round-robin. A missing or malformed program is reported and
  skipped, so a failed run returns -1 and never faults the kernel.
- **`SYS_WAIT`** blocks until any child of the shell exits and returns that child's
  exit status. This is what makes `run` a command rather than a detach: `cmd_run`
  calls it straight after a successful `SYS_RUN`, so the prompt does not reappear
  until the program is finished. The shell costs no CPU while it waits, and the
  program's output appears during the wait. See
  [decision 0018](../decisions/0018-process-lifecycle-exit-and-wait.md).
- **`SYS_READFILE`** reads a whole file (through `fat32_read_file`) into the
  caller's buffer and returns the byte count. The bytes are raw and not
  NUL-terminated; the shell terminates them before printing the buffer as a
  string. `read` needs this because the shell is ring 3 and cannot call
  `fat32_read_file` itself.
- **`SYS_WRITEFILE`** writes the caller's buffer (through `fat32_write_file`) to a
  named file, creating or wholly replacing it, and returns 0 or -1. It is the
  mirror of `SYS_READFILE` and validated the same way; `write` passes it the rest
  of the typed line. It does not block.
- **`SYS_DELETE`** removes a named file (through `fat32_delete`) and returns 0 or
  -1. `delete` uses it. It does not block.
- **`SYS_FREECOUNT`** returns `fat32_free_count`, the number of free clusters on
  the volume — a full FAT walk, no pointer, no block. `free` prints it, and the
  leak test watches it hold steady across write/delete cycles.
- **`SYS_STAT`** reports a file's size (through `fat32_stat`) without reading any
  of it, writing the size through the `out_size` pointer. `read` uses it to ask how
  big a file is before it commits a buffer, so it can tell a missing file from one
  too big to read and report the size instead of a bare failure. The `out_size`
  pointer is a write target and is bounds-checked over its whole `[ptr, ptr+8)`
  range. It does not block. See
  [decision 0021](../decisions/0021-sys-stat.md).

### `read` and files that do not fit

The shell's file buffer is `SHELL_FILE_MAX` (32768) bytes and it asks
`SYS_READFILE` for at most 32767 of them, holding one back for the NUL it appends
before printing. A file larger than that will not fit, and `read` needs to know
that *before* it reads — which is what `SYS_STAT` is for.

**`read` stats first, then decides.** `cmd_read` calls `SYS_STAT` for the name and
branches on the result:

```
> read nosuch.txt
read: no such file: nosuch.txt
> read huge.txt
read: huge.txt is 40981 bytes, the buffer holds 32767
> read hello.txt
Hello from FAT32!
```

- **Not found** (`SYS_STAT` returns -1) → `read: no such file: X`. A directory or a
  non-8.3 name folds into this, since none of them names a file to read.
- **Too big** (the reported size exceeds the buffer) → `read: X is N bytes, the
  buffer holds M`, with both numbers, so the reason is unambiguous.
- **Otherwise** → read it with `SYS_READFILE` and print it. The size is already
  known to fit, so a failure here is a genuine disk or filesystem error, which the
  old `read: cannot read X` line now names on its own.

`HUGE.TXT` is 40981 bytes and is on the disk for exactly this reason: the Makefile
generates it and copies it on every `make run`. Before `SYS_STAT`, all three
outcomes printed the same `read: cannot read X`, so a missing file, a too-large
file, and a disk error were indistinguishable; the size is what tells them apart.

**There is deliberately no partial read.** `read` on a large file still refuses; it
just now says why. Showing a prefix would need an offset argument on `SYS_READFILE`
— read bytes `[off, off+len)` rather than the whole file — which changes its
contract and every caller of it, and is a rung of its own. This also retired an
unreachable "showing the first N bytes, the file may be longer" notice the shell
used to carry (`TODO(read-truncation)`, now gone): `SYS_READFILE` delivers the
whole file or refuses it, never a prefix, so that notice could only ever fire for a
file of exactly the buffer size, which is complete. See
[decision 0021](../decisions/0021-sys-stat.md).

**The child no longer has to exit on its own.** A program with an unbounded loop
used to leave the shell blocked in `SYS_WAIT` forever, with a reboot the only way
back, which is why every program under `user/` runs a fixed number of rounds and
calls `sys_exit` at the bottom. Ctrl-C now interrupts the job in front, and `kill`
reaches one the keyboard cannot (see [Signals at the prompt](#signals-at-the-prompt)
below). The fixtures keep their bounded loops, which are now a convenience rather
than a necessity.

**Printing the status uses `printf`.** The shell used to carry its own
`print_uint`, ten call sites of a hand-built decimal conversion, because there was
no libc to reach for, and three fixtures carried the same dozen lines again under
another name. `printf` (`libc/printf.c`, linked into every program) replaced all
four. It supports `%d %u %x %s %c %%` and nothing else, so an `unsigned long` size
or count is cast to `unsigned int` at the call site. See
[user-memory.md](user-memory.md).

**The untrusted pointers.** Every pointer these take comes from ring 3 and is
checked before the kernel touches it. Buffers go through `user_range_ok`, which
confirms the whole `[ptr, ptr+len)` range lies in the ring-3 region
(`USER_REGION_START`..`USER_REGION_END`) and is careful about overflow (a crafted
length that would make `ptr+len` wrap is caught by comparing `len` against the room
above `ptr`, not by forming the sum). Filenames go through `copy_user_string`,
which bounds-checks the start pointer and copies with a length cap, so a string
with no terminator cannot walk out of the region. This is the same security
boundary as the loader's segment check and stricter than the `SYS_WRITE` stopgap
in the same file.

## The tokenizer

`next_token` (`user/userlib.h`) splits a string on a separator, in place, and is
reentrant.

```
char *next_token(char **pos, char sep);
```

- **Reentrant (`strtok_r` style).** The caller holds the current position in
  `*pos`; there is no hidden global. This deliberately avoids `strtok`'s single
  static cursor, which makes it non-reentrant: a second tokenization, even inside a
  called function, clobbers the first.
- **In place, no copying.** The separator after a token is overwritten with a NUL
  and the returned pointer points into the caller's buffer, so **the input buffer
  is modified**. That is what lets the shell match a token with a plain string
  compare and still reach the untouched remainder of the line (which is how
  `return` echoes the rest of the line).
- **Skips leading separators.** A run of separators before a token is stepped over,
  so `run   A.ELF` (extra spaces) yields `run` then `A.ELF`, never empty tokens in
  between. This is the non-obvious part, and the reason an empty or all-separator
  remainder returns null (no more tokens) rather than a zero-length token.

It lives in the user runtime rather than `libc/string.c` because the user build
compiles a single freestanding translation unit and links no kernel objects, so
`libc/string.c` is unreachable from ring 3, and the kernel never tokenizes, so it
would be dead code there. See
[decision 0016](../decisions/0016-interactive-shell.md).

## What there is to run

The shell is the only program the machine is *for*. Everything else on the disk is
a kernel test fixture: a ring-3 program that exists to prove a piece of the kernel
works, kept in `user/tests/` and documented one paragraph each in
[user/tests/README.md](../../user/tests/README.md). They land in the disk's root
directory alongside `SHELL.ELF`, because `fs/fat32.c` can only look up a bare 8.3
name in the root, so `run a.elf` finds them (the lookup is case-insensitive).

| Program | What it does | Exit status | What it proves |
|---------|--------------|-------------|----------------|
| `A.ELF` | prints `A` 20 times | 0 | the ordinary case, and a real `.bss` for the loader's zero-fill |
| `B.ELF` | prints `B` 60 times | 0 | a second, longer binary for the loader and the interleave |
| `C.ELF` | prints `C` 40 times | 3 | a non-zero status survives the trip back to the prompt |
| `D.ELF` | starts `E.ELF`, exits without waiting | 0 | orphans E; see below |
| `E.ELF` | prints `E` 15 times | 7 | the orphan nobody reaps |
| `H.ELF` | prints `H` 40 times, catches `SIG_INT` | 0 | a program that is interrupted and RESUMED, not killed |
| `ONCE.ELF` | reads fd 0 once and exits | 0 | the reader that leaves early, so a writer gets `SIG_PIPE` |

Four more fixtures are not in the table because their point is elsewhere: `F.ELF`,
the multi-cluster write test, is self-checking and exits 0 only on an exact
read-back; `G.ELF`, `COUNT.ELF` and `UPPER.ELF` exist to be joined by `|` (see
[Pipelines](#pipelines) above). All four are described with their transcripts in
[user/tests/README.md](../../user/tests/README.md).

### What `run d.elf` demonstrates

D and E exist together to reach code that no other test can. In every ordinary
case a child is reaped by its parent's `SYS_WAIT` before the scheduler's sweeper
ever sees it: the exiting child wakes its parent and switches straight to it, so
the parent re-enters `SYS_WAIT` before a timer tick has gone by. That leaves the
free path inside `reap_sweep`, and the branch of `parent_alive` that answers "no",
unreachable. A zombie whose parent is already dead is the only way in — hence D,
whose entire program is that it does *not* call `sys_wait`.

```
> run d.elf
run: started d.elf
D: starting E
ED: not waiting, exiting
reap (sweeper): task 1 exited (status 0), free frames: 30513, heap used: 1816
run: d.elf exited with status 0
> EEEEEEEEEEEEEE
> reap (sweeper): task 2 exited (status 7), free frames: 30585, heap used: 1192
```

That is captured output, pasted as the machine printed it, which is why it looks
untidy. Two tasks and a shell are printing into one screen with no locking, so a
line can land in the middle of another one (`Ereap (sweeper):` is an `E` from the
orphan arriving between the shell's characters), and the order of the `run:` lines
against the reap line is a scheduling artefact rather than a fixed sequence. E's
line comes after a `>` because it needed the keypress described below to appear.

Four things to read out of that:

- **The prompt comes back while E is still printing.** The shell waited for D, not
  for E, and stays usable throughout — typing while the E's arrive buffers the keys
  normally.
- **E's line says `reap (sweeper):`, not `reap (wait):`.** The label names the path
  that freed the address space, and the two are otherwise indistinguishable on
  screen. If E's line ever says `reap (wait):`, something collected a zombie that
  should have had no reader.
- **Nobody reads E's status 7.** By the time E exits its parent's slot is NULL, so
  the sweeper drops the tombstone rather than keeping a fact nobody can ask for.
  Seven is distinctive purely so it would be obvious, not plausible, if it ever
  turned up at the prompt.
- **The free frame count comes back to the baseline** — 30585 here — from a path
  that had never executed before.

One timing quirk worth knowing: if the machine is completely idle when the orphan
exits (nothing runnable, the shell blocked in `SYS_READKEY`), the sweeper's line
does not appear until the next wake event, such as a keypress. `schedule()` returns
at its idling guard before `reap_sweep` runs, and during the idle loop the zombie is
still `current`, which the sweeper skips by design. The memory comes back on the
next scheduling event, so this is deferred, not leaked. See
[scheduling.md](scheduling.md).

**Maintenance note.** The reap-line examples on this page are captured output, not
prose, and they go stale the moment the reap line's fields change: adding `heap
used:` to it invalidated every document quoting one, all at once and silently.
Regenerate them from a real boot whenever those fields change, and paste what the
machine prints rather than editing the old numbers into shape. The same examples
appear in [`user/tests/README.md`](../../user/tests/README.md).

### What `run h.elf` demonstrates

H is the fixture for the other half of signals: a program that is interrupted and
**put back**, rather than killed. It installs a `SIG_INT` handler, then prints forty
`H`s with a delay between each. Press Ctrl-C while it runs, several times if you
like: each press prints one `[H caught SIG_INT]` line, the `H`s carry on from exactly
where they stopped, and at the end the program reports how many interrupts it caught
and exits 0 — not 130, which is what the same key does to `run b.elf`, which has no
handler.

What is being tested is the resume, not the handler. Printing a line from a handler
only shows that the kernel forged a call frame and jumped to it; the interesting half
is the return. The handler's `ret` lands on the trampoline, the trampoline raises
`SYS_SIGRETURN`, and the kernel copies the saved context back over the live register
frame. If the run of `H`s came out short, or the final total did not match the number
of presses, the restore would be wrong. See [signals.md](signals.md).

### What `run g.elf | run once.elf` demonstrates

ONCE is the downstream half of the `SIG_PIPE` test, and the whole program is that it
does **not** drain its input. `G.ELF` writes 16384 bytes, four times the pipe's
capacity, so it has to block and resume several times to finish; ONCE reads one
bufferful, says how much arrived, and exits, which closes the last read end. G's next
write finds no reader, the kernel raises `SIG_PIPE` on it, and the default action
kills it with status 141 (128 + 13), so the pipeline ends instead of G spinning
against a buffer nobody will ever drain. Before signals, that same run left G writing
forever, and the only symptom was a prompt that never came back.

```
> run g.elf | run once.elf
ONCE: read 64 bytes, exiting
reap (wait):    task 5 exited (status 0), free frames: 30510, heap used: 5928
reap (wait):    task 4 exited (status 141), free frames: 30585, heap used: 1192
pipeline exited with status 0
```

The pipeline's own status is 0 because a pipeline reports its **last** stage's
status, and ONCE exited normally; G's 141 is on its reap line. Captured output, like
the transcript above: the free-frame and heap numbers are that boot's.

## Signals at the prompt

**Ctrl-C** interrupts the job in front. The shell puts every job — a pipeline and a
plain `run` alike — in its own process group, hands the keyboard to that group before
waiting, and takes it back afterwards on every path, including the failure ones. So
Ctrl-C during `run b.elf` stops `b.elf` (status 130) and returns you to a prompt, and
Ctrl-C on a three-stage pipeline stops all three stages, because they are one group.

At an idle prompt Ctrl-C reaches the **shell itself**, and the shell catches it: it
abandons the half-typed line and prints a fresh prompt, which is what every real
shell does. That handler is not politeness, it is a safety net — without it a Ctrl-C
that reached task 0 would kill it, and since nothing waits on task 0 there would not
even be a reap line, just a machine that stops responding with no way to start
another shell.

**Ctrl-D** ends console input, so a program that reads until end of file finishes:

```
> run count.elf
hello
5
run: count.elf exited with status 0
```

### `ps` and `kill`

```
> run d.elf
...
> ps
  id  parent  pgid  state    status
  0   -       0     running
  2   1       1     ready
> kill 2
reap (sweeper): task 2 exited (status 130), free frames: 30585, heap used: 1192
```

**These two exist because the keyboard cannot reach everything, and that is by
design rather than an oversight.** Ctrl-C is addressed to the foreground group only.
After `run d.elf`, `E.ELF` is in D's group — a group this shell is not in and cannot
hand the keyboard to — so **Ctrl-C can never reach it**. `ps` shows that it exists
and which group it is in; `kill` stops it. Without both, this kernel would have tasks
that nothing could stop.

They are a pair and neither is much use alone: `kill` needs an id, and an id the user
has to guess is not an interface. The `pgid` column is the one to read when something
will not die.

`kill <id>` sends `SIG_INT` — the same signal Ctrl-C sends, so it is "Ctrl-C, but
aimed". `kill <id> 9` sends `SIG_KILL`, which **cannot be caught**, for a program
that has a handler and declines to stop. A catchable signal is a request.

See [signals.md](signals.md) and
[decision 0023](../decisions/0023-signals.md).

## Building and running the shell

`user/shell.c` is built exactly like the other user programs (`-mcmodel=small`,
freestanding, static, linked at 0x400000 with `user/user.ld`) but with an explicit
Makefile rule, since its source is lowercase `shell.c` and its on-disk name is
uppercase `SHELL.ELF`. `make run` copies it onto the image, and `kernel_main`
launches it. See [../building.md](../building.md).

## Related

- The syscall gate, convention, and the `SYS_WRITE` stopgap this sits beside:
  [syscalls.md](syscalls.md), [decision 0007](../decisions/0007-syscalls-via-int-0x50.md).
- The loader `SYS_RUN` calls: [elf-loading.md](elf-loading.md),
  [decision 0015](../decisions/0015-elf-program-loading.md).
- The filesystem `SYS_LIST` and `SYS_READFILE` read through:
  [fat32.md](fat32.md), [decision 0014](../decisions/0014-read-only-fat32.md).
- The scheduler a launched program joins, and how it leaves again:
  [scheduling.md](scheduling.md),
  [decision 0018](../decisions/0018-process-lifecycle-exit-and-wait.md).
- The sleep behind a blocking `SYS_READKEY`: [blocking.md](blocking.md),
  [decision 0017](../decisions/0017-blocking-and-sleep.md).
- The decision behind all of this: [decision 0016](../decisions/0016-interactive-shell.md).
