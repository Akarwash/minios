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
    run: started a.elf
    AAAAAAAAAAAAAAAAAAAAreap (wait):    task 1 exited (status 0), free frames: 30585, heap used: 1192
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
    run: started d.elf
    D: starting E
    ED: not waiting, exiting
    reap (sweeper): task 1 exited (status 0), free frames: 30513, heap used: 1816
    run: d.elf exited with status 0
    > EEEEEEEEEEEEEE
    > reap (sweeper): task 2 exited (status 7), free frames: 30585, heap used: 1192

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
line reports fewer free frames than E's (30513 against 30585 here): when D is swept
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

## I.ELF — the malloc test

The first program that asks for memory at runtime. It `malloc`s 300 blocks of
sizes from 1 to 1400 bytes (about 210KB in all, so the heap has to grow past its
first 64KB slab several times), checks each is 8-byte aligned, writes a pattern
into each that depends on the block **and** the byte offset, and only then verifies
every pattern, so a block that overlaps another has already had its pattern
overwritten by the time the check runs. It frees them in a fixed shuffled order,
then asks for one 200000-byte block, which fits only if every freed block coalesced
back into one span **across the slab boundaries**, and which must come back at the
address of the very first block (first-fit on a heap that is one free block again).
Then `calloc` on that reused memory, which must be zero, and a 3MB request, which
must fail with NULL rather than anything louder. It exits **0** only if every check
passes, with a distinct non-zero status per failure mode (listed at the top of
`I.c`), and prints one line either way.

    > run i.elf
    run: started i.elf
    syscall: SYS_MMAP rejected a length outside the 2MB heap slot
    malloc: heap grow: SYS_MMAP refused another slab
    I: 300 blocks allocated, verified, freed in shuffled order, and reused
    reap (wait):    task 2 exited (status 0), free frames: 30071, heap used: 1448
    run: i.elf exited with status 0

The two lines before the verdict are the 3MB request being refused: the kernel says
why it declined the slab, malloc says it gave up, and malloc returns NULL, which is
the outcome being tested. **The reap line is the other half of the test.** malloc
never gives a slab back, so this program exits holding four slabs it asked for at
runtime; ten consecutive runs must print the same free frame count and the same
`heap used` every time, and a count that steps down by a few slabs per run means
the address-space teardown is not freeing the heap slot with the rest (M2 in
[decision 0024](../../docs/decisions/0024-user-memory-and-libc.md)).

## J.ELF — the `SYS_MMAP` / `SYS_MUNMAP` abuse test

Every other fixture asks the kernel for something it should grant. J asks for things
it must **refuse**, and checks that each refusal changed nothing: a zero length, a
length larger than the whole 2MB heap slot, a release of an address that was never
mapped, a release of its own code and of its own stack, a release of the second page
of a region, a release with the wrong length, a second release of a region already
given back, a ninth region when eight is the cap, and a region that would cross the
top of the slot. Between the abuses it does the ordinary thing (map, check the memory
reads as zero, write a pattern, read it back, release), so "the kernel refused it" is
distinguishable from "the kernel refuses everything". It exits **0** only if every
call answered as `include/syscalls.h` promises, with a distinct non-zero status per
check (listed at the top of `J.c`), and prints one line either way.

    > run j.elf
    syscall: SYS_MMAP rejected a length outside the 2MB heap slot
    syscall: SYS_MMAP rejected a length outside the 2MB heap slot
    syscall: SYS_MMAP rejected a length outside the 2MB heap slot
    syscall: SYS_MUNMAP rejected an address that is not a region's start
    syscall: SYS_MUNMAP rejected an address that is not a region's start
    syscall: SYS_MUNMAP rejected an address that is not a region's start
    syscall: SYS_MUNMAP rejected an address that is not a region's start
    syscall: SYS_MUNMAP rejected an address that is not a region's start
    syscall: SYS_MUNMAP rejected an address that is not a region's start
    syscall: SYS_MMAP rejected: no free region slot
    syscall: SYS_MMAP rejected a length outside the 2MB heap slot
    run: started j.elf
    syscall: SYS_MMAP rejected a region that would cross the 2MB ceiling
    syscall: SYS_MMAP rejected a region that would cross the 2MB ceiling
    J: every abuse refused, every region released, memory intact
    reap (wait):    task 2 exited (status 0), free frames: 30073, heap used: 1448
    run: j.elf exited with status 0

The kernel's `syscall: ... rejected` lines are the refusals, one per abuse, printed
by the kernel exactly as it prints a rejected out-of-bounds buffer; that the shell's
`run: started` line lands in the middle of them is scheduler timing. Two things to
read out of the run. The program is still **running** after each refusal: a kernel
that unmapped the code slot on request would take J down at its next instruction
fetch, and one that accepted a 3MB request would strand frames or map over the
kernel's half of the address space. And the free frame count comes back to the
baseline `run a.elf` leaves, although the largest thing J maps is the entire slot
(512 frames). J talks to the syscalls directly rather than through `malloc`, because
`malloc` is built on this contract and cannot test it. See
[user-memory.md](../../docs/reference/user-memory.md) and
[decision 0024](../../docs/decisions/0024-user-memory-and-libc.md).

## K.ELF — the printf test

One line per `printf` specifier, each with values chosen so the output can be
checked against `K.c` by eye; and, because `printf` returns the number of bytes it
wrote, every return value is compared with the length the line must have if every
specifier did its job, so a wrong digit or a format that stopped early is a wrong
count and a non-zero exit (1 + the index of the failing line). The last three lines
are the failures `printf` is **designed** to have: `%q` is not a specifier it knows,
so the format stops there, prints nothing for it and returns -1; and a `%s` given a
pointer outside the ring-3 address space (a NULL, and 16) is refused before anything
reads through it. The `%q` line is silenced from the compiler's own format check for
that one line, which is the check that actually defends a program from a mismatched
format (M5 in [decision 0024](../../docs/decisions/0024-user-memory-and-libc.md)).

    > run k.elf
    %d: 0 42 -42
    %u: 0 4294967295
    %x: 0 ff deadbeef
    %s: hello
    %c: ToS
    100% done
    mixed: answer=42, 2a, !%
    0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
    0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
    stop here:
    bad %s pointers:
    bad %s pointers:
    K: 11 lines checked
    run: started k.elf
    reap (wait):    task 1 exited (status 0), free frames: 30071, heap used: 1448
    run: k.elf exited with status 0

The 144-character line wraps at the screen's 80 columns; it is one `printf` call,
longer than the 128-byte staging buffer, so the flush in the middle of a line is
exercised as well as the one at the end.

## Why every loop is bounded

A program that never exits used to leave its parent blocked in `SYS_WAIT` with no way
back short of a reboot, so `run <that program>` would make the shell permanently
unusable. `A`–`F` and `I`–`K` therefore run a fixed number of rounds and call `sys_exit` at
the bottom.

**Signals changed the stakes but not the rule.** Ctrl-C and `kill` can now stop a
runaway task ([signals.md](../../docs/reference/signals.md)), so a bounded loop is a
convenience rather than the only protection. Keep them bounded anyway: a fixture that
needs a keystroke to finish cannot be run unattended, and a test whose result depends
on how fast somebody presses a key is not a test. `COUNT` and `UPPER` are
bounded differently: they loop until end of file, which a **pipe** always delivers
(the writer closes) but a **console** never does, so they terminate when piped and do
not when run alone on the console — which is why they are pipeline fixtures, not
standalone ones. There is no crt0 either, so falling off the end of `_start` is
undefined behaviour rather than an implicit `exit(0)`: the call has to be written out.
