# Pipes

A **pipe** is an in-kernel byte buffer with a write end and a read end: bytes written
to one end come out the other, in order. It is what connects two programs, so
`run A | run B` can send `A`'s output to `B`'s input. This page documents the buffer,
the block/wake rules, and how end-of-file works. Read from `kernel/pipe.h`,
`kernel/pipe.c`, and the pipe branches of `kernel/file.c`. For the rationale and the
failure modes, see [decision 0022](../decisions/0022-file-descriptors-and-pipes.md);
for the descriptors that hold pipe ends, [descriptors.md](descriptors.md).

## The buffer

A `pipe_t` (`kernel/pipe.h`) is the **same ring buffer as the keyboard driver**
(`drivers/keyboard.c`), and is meant to stay recognisably so:

```c
#define PIPE_SIZE 4096

typedef struct pipe {
    char     buf[PIPE_SIZE];
    uint32_t read_index;    // next slot to drain
    uint32_t write_index;   // next slot to fill
    int      readers;       // file_t ends open for reading; 0 => writes are broken
    int      writers;       // file_t ends open for writing; 0 => reads see EOF
} pipe_t;
```

`write_index` chases `read_index` around the ring, and **one slot is always left
unused**, so `write_index == read_index` means empty and can never also mean full (a
full buffer holds `PIPE_SIZE - 1` bytes). That is exactly the keyboard buffer's rule.

What a pipe has that the keyboard buffer does not is the two **counts**. An end can be
closed, and both end-of-file and broken-pipe are defined by a count reaching zero. The
counts are the number of live ends: `pipe_create` starts them at 0, creating or
inheriting an end bumps one (`file_alloc_pipe`, `file_dup`), and closing an end drops
it (`file_close`). They are the single place a pipe end is counted — there is no
separate count on the `file_t`.

## Reading and writing

`pipe_write` copies as many of its bytes as fit, up to the space free, and returns
that count (which may be fewer than asked — the caller loops):

- If it wrote at least one byte, it wakes `WAIT_PIPE_READ` (a reader may now proceed).
- If zero bytes fit and `readers > 0`, the pipe is full but a reader is alive to drain
  it, so it **blocks** on `WAIT_PIPE_WRITE` (backpressure).
- If `readers == 0`, no reader can ever drain it, so it returns an **error** rather
  than blocking forever. (Unix raises SIGPIPE here; an error return is enough.)

`pipe_read` copies as many bytes as are buffered, up to what was asked, and returns
that count:

- If it read at least one byte, it wakes `WAIT_PIPE_WRITE` (a writer may now have
  room).
- If the pipe is empty and `writers > 0`, the bytes may yet come, so it **blocks** on
  `WAIT_PIPE_READ`.
- If the pipe is empty and `writers == 0`, no byte can ever arrive: it returns **0**,
  which is **EOF**.

**The buffer changes first, the wake comes second**, always — the ordering rule from
[blocking.md](blocking.md). A reader woken before the byte was actually in the buffer
would run, find nothing, and block again.

## End of file, and why closing wakes

EOF is `pipe_read` returning 0, and it happens exactly when the pipe is empty **and**
`writers == 0`. Empty on its own is not EOF: it is the ordinary state between a slow
writer's writes, and a reader that hit it while a writer still lived would block, not
finish.

So a reader waiting at an empty pipe is unblocked by one of two things: a **write**
(bytes arrived) or the **last writer closing** (no bytes will ever arrive — EOF).
Both are handled by `scheduler_wake(WAIT_PIPE_READ)`: `pipe_write` calls it after
writing, and `file_close` calls it when it drops `writers` to zero. That second wake
is the one that is easy to miss and the most likely way to hang a pipeline: without
it, a reader parked on an empty pipe whose last writer merely closed would wait
forever for an EOF it can never observe. `task_exit` closes every descriptor a task
holds, so a program that just exits still delivers EOF to whoever reads its pipe.

The `pipe_t` is freed by `file_close` when both counts reach zero: no end anywhere can
still reach the buffer, so it is nobody's.

## Failure modes

Pipes are unusually easy to get subtly wrong, and the ways they fail are mostly silent
hangs with no message. Rather than repeat them here — a reference page describes what
is, and these are about what would be — see the **"Six ways this goes wrong"** section
of [decision 0022](../decisions/0022-file-descriptors-and-pipes.md), which catalogues
them B1 through B6: the shell keeping its own copy of a pipe end (B1), closing not
waking (B2), writing RAX on a blocking path (B3), counting a pipe end twice (B4),
assuming one call moved everything (B5), and descriptors leaking into the shell's
table (B6). Each entry gives the symptom, the cause, and the line that prevents it.

## Related

- The descriptors that hold pipe ends and the syscalls over them:
  [descriptors.md](descriptors.md).
- The block/wake mechanism and the ordering rule:
  [blocking.md](blocking.md), [decision 0017](../decisions/0017-blocking-and-sleep.md).
- The shell's `|`, which creates pipes and closes its own ends:
  [shell.md](shell.md).
- The decision, the EOF argument, and the failure catalogue:
  [decision 0022](../decisions/0022-file-descriptors-and-pipes.md).

## SIGPIPE: writing to a pipe with no reader

`pipe_write` with `readers == 0` **raises `SIG_PIPE` on the writer** and returns an
error. It used to return only the error, which was all a kernel with no signals could
do — and it was not enough.

An error return only reaches a program that checks return values. A writer that does
not spins against a dead buffer forever, and the only symptom is a prompt that never
comes back. A signal reaches it regardless.

`SIG_PIPE`'s default action is to kill the writer, which is what makes this
terminate:

```
> run g.elf | run once.elf
ONCE: read 64 bytes, exiting
reap (wait):    task 2 exited (status 0), free frames: 30510, heap used: 5928
reap (wait):    task 1 exited (status 141), free frames: 30585, heap used: 1192
pipeline exited with status 0
```

141 is 128 + 13, so the writer's death is legibly `SIG_PIPE` rather than a guess.
`ONCE.ELF` reads once and exits without draining, which is exactly the case that used
to hang; before this, G wrote forever.

**`SIG_PIPE` is catchable, deliberately.** A program that wants to handle a vanished
reader itself installs a handler, and then the error return from the write is what it
sees. That is why the default is kill rather than unconditional.

See [signals.md](signals.md) and [decision 0023](../decisions/0023-signals.md).
