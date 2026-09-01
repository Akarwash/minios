# Kernel test fixtures

These are ring-3 programs that exist to prove a piece of the kernel works. They are
not part of the machine's runtime: nobody would want them on a computer they were
using. The shell (`../shell.c`) is the real program, and `../userlib.h` and
`../user.ld` are the runtime every program here compiles against.

They build exactly like any other user program — freestanding, static,
`-mcmodel=small`, linked at `0x400000` — and they land in the **root directory** of
the disk image alongside `SHELL.ELF`, because `fs/fat32.c` can only look up a bare
8.3 name in the root. The directory is a source-tree convention; the disk has no
directories. Run one from the shell with `run a.elf` (the lookup is
case-insensitive).

Each program is deliberately strange in some way. **Read the paragraph before you
change one**: what looks like an oversight is usually the thing being tested.

The transcripts below are **captured output**, pasted as the machine printed it,
interleaving and all. Regenerate them from a real boot whenever the reap line's
fields change — see the maintenance note in
[`docs/reference/shell.md`](../../docs/reference/shell.md).

## A.ELF — the ordinary case

Prints `A` twenty times with a delay between each, then exits with status 0. It has
a zero-initialised global so the binary has a real `.bss`, which keeps the ELF
loader's zero-fill honest: a program with no `.bss` would load correctly even if
that step were missing.

    > run a.elf
    Arun: started a.elf
    AAAAAAAAAAAAAAAAAAAreap (wait):    task 1 exited (status 0), free frames: 30587, heap used: 616
    run: a.elf exited with status 0

This is the baseline for the memory test. Run it ten times and the free frame count
must be identical from the first onwards.

`reap (wait):` names the code that freed the task's memory — here the shell's own
`SYS_WAIT`, which is what reaps a child in every ordinary case. The other label,
`reap (sweeper):`, is what D and E exist to produce.

## B.ELF — a second, longer program

Identical in shape to A but sixty rounds instead of twenty, so the two are visibly
different lengths of work. It exists so the scheduler has more than one file to
interleave and the loader is proven on more than one binary. Exits with status 0.

## C.ELF — a non-zero exit status

Prints `C` forty times, then exits with **status 3**. Everything else in the system
exits 0, which is also what an uninitialised field and a dropped value look like.
Three is a number nothing else produces, so `run: c.elf exited with status 3` at the
prompt proves the value survived the whole trip: out of the program's `sys_exit`,
through the kernel's mask, into the zombie's `exit_status`, back through the
parent's `SYS_WAIT`, and into a number printer in ring 3.

## D.ELF — a parent that does not wait

Starts `E.ELF` and then exits immediately, **without calling `sys_wait`**. That
omission is the entire program; it is not a bug and must not be tidied away.

D exists to orphan E. Normally a child is reaped by its parent's `SYS_WAIT` before
the scheduler's sweeper ever sees it — the exiting child wakes its parent and
switches straight to it, so the parent re-enters `SYS_WAIT` before a timer tick has
gone by. That leaves two pieces of `reap_sweep` unreached in every other test
here: the free path that tears a zombie's address space down, and the branch of
`parent_alive` that answers "no" and drops an unwatched tombstone. `run d.elf` is
what reaches them, and the output below shows how.

    > run d.elf
    D: starting E
    D: not waiting, exiting
    Ereap (sweeper): task 1 exited (status 0), free frames: 30515, heap used: 952
    run: started d.elf
    run: d.elf exited with status 0
    > EEEEEEEEEEEEEE
    > reap (sweeper): task 2 exited (status 7), free frames: 30587, heap used: 616

Both D and E are reaped by the **sweeper**, and which path frees which is decided
by where they sit in the task table, not by luck. The table is `shell` = 0, `D` =
1, `E` = 2. When D exits it wakes the shell, its parent, but `find_next_ready` scans
forward from the slot *after* D and so reaches E at slot 2 before it wraps back to
the shell at slot 0: E, not the shell, is what runs next, and it gets a full
timeslice. The next timer tick enters `schedule` with `current` = E, and
`reap_sweep` — which runs at the top of every `schedule` and frees any zombie that
is not the running task — finds D and frees its address space. That is D's
`reap (sweeper):` line. The shell's `SYS_WAIT` runs only afterwards, finds D's
address space already gone, and quietly collects the tombstone alone, so **D
produces no `reap (wait):` line at all, even though it was waited on.**

That outcome hangs entirely on the task-table order and on how `find_next_ready`
walks it. Reorder the table, or change the scan so the shell is reached before E
when D exits, and D would be reaped by the shell's `SYS_WAIT` and its line would
read `reap (wait):` instead. Treat the labels here as a fact about today's
scheduler, not a guarantee: if the table or `find_next_ready` changes, work out
again which path frees D before trusting this output.

Two more things the run shows. The prompt returns while E is still printing,
because the shell waited for D and not for E and stays usable throughout. And D's
line reports fewer free frames than E's (30515 against 30587 here): when D is swept
E is still running and holding its own address space, and E's line is the count
coming back to the baseline once E is gone too.

## E.ELF — the orphan

Prints `E` fifteen times, then exits with status 7. **Nobody will ever read that
7.** By the time E exits, D is long gone, so E's tombstone has no reader and the
sweeper drops it rather than keeping a fact nobody can ask for. Seven is distinctive
purely so that it would be obvious, rather than plausible, if it ever did turn up.

Run on its own (`run e.elf`) it is an ordinary short program reaped by the shell's
wait, and status 7 is duly printed. It is only interesting as D's child.

## F.ELF — the multi-cluster write

Writes a 16KB file, `FTEST.TXT`, and checks it read back. The shell's own `write`
can only make a single-cluster file (a typed line is far shorter than a 512-byte
cluster), so the write path's real work — allocating and linking a 32-cluster chain
and reading it back in order — is only ever exercised here.

It is self-checking, so nobody eyeballs 16KB. It fills a buffer with numbered,
fixed-width lines (the HUGE.TXT idea), writes it, reads it back into a second
buffer, and compares byte for byte and on length. It exits **0** only on an exact
match, and with a distinct non-zero status otherwise, so `run: f.elf exited with
status N` names the failure: 1 write failed, 2 read failed, 3 length mismatch, 4
content mismatch.

    > run f.elf
    run: started f.elf
    F: FTEST.TXT 16384 bytes written and verified
    run: f.elf exited with status 0

16KB is deliberate: big enough to be a real 32-cluster chain, small enough to stay
under the shell's 32KB read buffer, so afterwards `read FTEST.TXT` still prints the
whole thing for a human spot check. On the host, `mtype -i disk.img ::/FTEST.TXT |
wc -c` reports 16384.

## G.ELF — the backpressure writer

Writes 16384 bytes to `fd 1` in a repeating 16-byte pattern, looping on partial
writes, then exits 0. It is the upstream half of a backpressure test and is kept
that simple on purpose.

`run g.elf | run count.elf` prints `16384`, and that number is the whole point: 16384
is four times the kernel's `PIPE_SIZE` (4096), so the pipe fills and G must **block
and resume at least three times** to get everything through. If it printed fewer than
16384, either G stopped looping on partial writes or backpressure regressed — the two
things this fixture exists to catch.

    > run g.elf | run count.elf
    16384
    pipeline exited with status 0

Run alone (`run g.elf`), it floods the screen with the pattern. That is expected, not
a bug — it is a 16KB writer with nowhere quieter to write when there is no pipe.

## COUNT.ELF — the downstream end of a pipe

Reads `fd 0` until end of file, counts the bytes, prints the count to `fd 1`, exits
0. It exists to be the reader half of a pipeline: `run a.elf | run count.elf` prints
`20`, because A writes twenty bytes into the pipe and COUNT reads them all.

It is the fixture that proves EOF. Piped after a writer, COUNT blocks on the empty
pipe between the writer's slow writes, and it only ever finishes because closing the
last write end (which A's exit does) makes a read return 0. So a run that prints the
right total and returns is evidence that a close is an event that unblocks an
EOF-waiting reader, not just that bytes flow. It loops on partial reads: a pipe hands
over only what has been written so far, so one read is never assumed to drain the
stream.

    > run a.elf | run count.elf
    20
    pipeline exited with status 0

Run alone (`run count.elf`, `fd 0` the console), it reads keys and counts them but
never finishes, because a console has no EOF to send — there is no Ctrl-D. That is
expected, not a bug; see [decision 0022](../../docs/decisions/0022-file-descriptors-and-pipes.md).

## UPPER.ELF — a streaming middle stage

Reads `fd 0` until EOF, uppercases each byte, writes it to `fd 1`, exits 0. It has a
pipe on **both** sides, so `run a.elf | run upper.elf | run count.elf` exercises
three stages and a program that both reads and writes a pipe. It streams — passing
bytes through as it reads them rather than buffering the whole input — so it also
shows a pipeline making incremental progress and shows backpressure: when the pipe
ahead of it fills, its write blocks until the next stage drains it, which is correct
rather than a hang.

It loops on partial reads **and** partial writes: a write into a pipe takes only what
fits and returns that count, so the remainder is retried, and a write returning `<= 0`
means the stage downstream is gone and there is nothing left to do.

    > run a.elf | run upper.elf | run count.elf
    20
    pipeline exited with status 0

## Why every loop is bounded

There is no way to kill a task and there are no signals. A program that never exits
leaves its parent blocked in `SYS_WAIT` with no way back short of a reboot, so
`run <that program>` would make the shell permanently unusable. `A`–`F` therefore run
a fixed number of rounds and call `sys_exit` at the bottom. `COUNT` and `UPPER` are
bounded differently: they loop until end of file, which a **pipe** always delivers
(the writer closes) but a **console** never does, so they terminate when piped and do
not when run alone on the console — which is why they are pipeline fixtures, not
standalone ones. There is no crt0 either, so falling off the end of `_start` is
undefined behaviour rather than an implicit `exit(0)`: the call has to be written out.
