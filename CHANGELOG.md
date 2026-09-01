# Changelog

All notable changes to MiniOS are recorded here. The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## How to read this file

**Entries are append-only. Each one describes the code at the release it sits
under, and is not edited afterwards — not even once it has gone stale.**

A name appearing below is therefore evidence that it existed *then*, and no
evidence at all that it exists now. Several entries talk about `task_create`,
`build_user_space`, `.user_text` and `user/A.c`. All of those were real; none of
them still are. Programs are loaded from disk by `elf_load_file`
([elf-loading.md](docs/reference/elf-loading.md)) and the fixtures live in
[`user/tests/`](user/tests/README.md). The entries are nonetheless correct, because
what an entry claims is that this is what shipped in that release, and it is.

The reason for the rule is the one that also freezes the bodies of
[architecture decision records](docs/decisions/README.md). A changelog's whole job
is to say what changed and when. Rewriting an old entry into today's vocabulary
produces a file in which nothing ever changed name: it goes on listing the change
while destroying the evidence of it, and a reader tracing how a subsystem got its
present shape finds every step described in words that did not exist at that step.
A stale entry under a dated heading is honest, and its heading is how a reader
knows to discount it. A retrofitted one is a claim nobody can check.

For what is true **now**, read [`docs/reference/`](docs/reference/), which is
corrected on sight, or the source.

## [Unreleased]

### Added

- Signals, the last rung of phase 1: a way to interrupt a running ring-3 program,
  and to have it resume afterwards. **A signal is what an interrupt is, one layer
  up, and no hardware does any of it** — every step the CPU performs for an
  interrupt is written by hand in a new `kernel/signal.{h,c}`. `include/signals.h`
  holds the four numbers (`SIG_INT` 2, `SIG_KILL` 9, `SIG_SEGV` 11, `SIG_PIPE` 13)
  shared with ring 3, and a task killed by signal N exits with status 128 + N, so
  Ctrl-C gives 130. Raising and delivering are deliberately separate halves:
  `signal_raise`/`signal_raise_group` set a bit in `task_t.sig_pending` and return,
  which is what makes them safe to call from the keyboard IRQ, while `check_signals`
  delivers at the end of `irq_handler` **and** at the end of the syscall dispatch —
  every path back to ring 3, and nowhere else. The pending set is a set, not a
  queue, so holding Ctrl-C produces one delivery rather than a backlog. With no
  handler the default action is `task_exit(r, 128 + sig)`; with one, the kernel
  forges on the program's own stack the call frame a real `call` would have produced
  (step over the 128-byte red zone, align to 16 *before* pushing the fake return
  address, range-check the whole span it is about to write through), points `rip` at
  the handler, passes the signal in RDI, and writes a trampoline address where a
  return address belongs. **`SIG_KILL` ignores handlers entirely**: a signal a
  program can catch is a request, and a system needs one that is not.
- `SYS_SIGNAL` (14), `SYS_KILL` (15), `SYS_SIGRETURN` (16), `SYS_SETFG` (17) and
  `SYS_TASKS` (18), for nineteen calls. `SYS_SIGRETURN` restores the context saved
  at delivery and is locked shut behind a `sig_active` flag, re-range-checks the
  context, reads it from the kernel-remembered `sig_ctx` rather than the program's
  `user_rsp`, and forces CS, SS and the interrupt flag rather than trusting the
  values it just copied out of user memory — it overwrites the entire register
  frame, which is a privilege-escalation primitive if reachable at will.
- The signal trampoline, `user/trampoline.asm`, assembled into every user program.
  **The kernel cannot pick this address**: programs are separately linked ELFs at a
  fixed `0x400000` with no kernel-owned page mapped into them, so no kernel-side
  address is executable from ring 3. The program supplies it and `userlib.h` hides
  the argument, so a program writes `sys_signal(SIG_INT, handler)`. Linux answers
  the same problem the same way, with the vDSO.
- Process groups (`task_t.pgid`), so Ctrl-C addresses a **job** rather than a task —
  a three-stage pipeline is three tasks and one thing to whoever typed it. A group
  is named after its leading task, which makes a new group id unique for free since
  task ids are never reused. `SYS_RUN` grows a fourth argument in RCX (0 inherit,
  `SYS_RUN_GROUP_NEW`, or a group to join), leaving its existing three arguments
  meaning exactly what they always did; RCX rather than R10 because `int 0x50`,
  unlike the `syscall` instruction, does not clobber it.
- A **declared** foreground group, `SYS_SETFG`, never inferred. `D.ELF` is the proof
  it cannot be inferred: D starts E and exits without waiting, so a foreground
  derived from "most recently started" would follow to E and stay there while the
  user sits at a prompt. A task may name only its own group or a group one of its
  own children is in — without that rule any program could take the keyboard and
  never give it back, and no privileged task exists here that could take it away.
- `ps` and `kill` shell commands, over `SYS_TASKS` and `SYS_KILL`, with a new
  `include/taskinfo.h` carrying the `task_info_t` that crosses the ring boundary
  (its state vocabulary is deliberately separate from the kernel's `task_state_t`,
  mapped by an explicit switch, so splitting an internal state cannot silently
  change what a compiled `ps` prints). **They exist because the keyboard cannot
  reach everything, by design**: Ctrl-C goes to the foreground group only, so
  `E.ELF` after `run d.elf` is unreachable from the keyboard. `ps` finds it and
  `kill` stops it; without both, the kernel has tasks nothing can stop.
- Left-ctrl tracking in `drivers/keyboard.c`, on the same pattern as shift.
  **Ctrl-C and Ctrl-D push no character and return before the character path's
  `scheduler_wake(WAIT_KEY)`**, exactly as the modifier keys do — a wake with an
  empty ring gives every sleeper a wasted round trip, so holding ctrl would
  otherwise spin the scheduler.
- Ctrl-D and a console end of file, the whole of this kernel's line discipline: one
  `console_eof` flag, test-and-cleared by the reader, reported as a zero-byte read.
  `file_read` drains buffered characters *before* checking it, so typing `abc` then
  Ctrl-D yields `abc` and then 0. This makes `run count.elf` on its own terminable,
  which verification 9 of the pipes rung found impossible.
- `SIG_PIPE`: `pipe_write` with `readers == 0` now raises a signal instead of only
  returning an error, so `run g.elf | run once.elf` terminates rather than leaving
  the writer spinning against a dead buffer. An error return reaches only a program
  that checks return values; a signal reaches one that does not. It is catchable on
  purpose, so a program can handle a vanished reader itself.
- Test fixtures `H.ELF` (catches `SIG_INT`, counts the catches and prints the total,
  so the **resume** is what is tested rather than just the delivery) and `ONCE.ELF`
  (reads fd 0 once and exits, the reader that leaves early).
- `docs/decisions/0023-signals.md` and `docs/reference/signals.md`. The ADR carries
  a catalogue of the **eight ways this goes wrong (S1–S8), six of them silently** —
  delivering while returning to ring 0, the red zone, stack alignment, an
  unvalidated `user_rsp`, delivering to a blocked task, nested delivery during a
  handler, `sigreturn` without a live frame, and Ctrl-C reaching the shell — each
  with its symptom, its cause, and the lines that prevent it. None is inferable from
  reading the working code, because working code shows the fix and never the failure.

- File descriptors and pipes, so one program's output can be another's input and the
  shell can run `run A | run B | run C`. Every task gains a fixed table of open
  destinations (`task_t.fds[MAX_FDS]`, `MAX_FDS` = 8) in a new `kernel/file.{h,c}`: a
  `file_t` is `{kind, pipe, writable}`, **owned by exactly one slot in one task with
  no reference count** — the only shared object is the `pipe_t`, whose reader/writer
  counts are the single place a pipe end is tallied, so there is nothing for a second
  count to drift against. fd 0 is a console input end (reads the keyboard) and fd 1 a
  console output end (writes the screen) by a convention the kernel enforces via the
  `writable` flag, not the hardware. `SYS_WRITE` (1) is reshaped from
  `(str)`-prints-a-string to `(fd, buf, len)`-returns-a-count-that-may-be-less; three
  syscalls join it — `SYS_READ` (11), `SYS_CLOSE` (12), `SYS_PIPE` (13) — for
  fourteen. Counted buffers are copied through a 4096-byte kernel staging buffer
  (`SYSCALL_IO_MAX`), so a transfer larger than that comes back partial and the caller
  loops; `user_range_ok` now guards `SYS_WRITE`/`SYS_READ` too, and the write-target
  pointers of `SYS_STAT`/`SYS_PIPE`/`SYS_WAIT` are bounds-checked before the kernel
  writes through them. A new `kernel/pipe.{h,c}` is the keyboard driver's ring buffer
  (one slot left unused so empty is distinguishable from full) plus a `readers` and a
  `writers` count: `pipe_write` blocks on a full pipe with a live reader
  (backpressure) and errors on a pipe with no reader, `pipe_read` blocks on an empty
  pipe with a live writer and returns 0 (EOF) on an empty pipe with no writer —
  **empty is not finished; empty with no writer is**. Two new `wait_reason_t` values,
  `WAIT_PIPE_READ`/`WAIT_PIPE_WRITE`, woken by a write, a read, or the close of the
  last end on the other side; **`close_fd` (via `file_close`) wakes on a count reaching
  zero**, because a reader parked on an empty pipe whose last writer merely closes
  would otherwise wait forever for an EOF it cannot observe. `sys_read`/`sys_write`
  take the register pile and write RAX themselves, leaving it untouched on the block
  path (the re-armed `int 0x50` reads the syscall number back out of RAX; a stray 0 is
  `SYS_EXIT`), dispatched as bare statements. A child acquires a descriptor only at
  `SYS_RUN`, now `(name, in_fd, out_fd)` (RSI/RDX, -1 = fresh console):
  `task_create_from_file` validates the ends against the caller's table (in a read
  end, out a write end), `file_dup`s each into a new `file_t` of the child's own
  pointing at the same `pipe_t`, and swaps it in for the default console — all before
  the task is registered, so no failure has to unwind a schedulable task, and every
  failure undoes the inherited-end counts. `task_exit` closes every descriptor a task
  holds before it becomes a zombie, which is what makes a pipe writer's exit deliver
  EOF downstream. The shell (`user/shell.c`) splits a line on `|` into up to four
  stages, creates a pipe per join, starts each stage, and **closes its own copies of
  each pipe end immediately** (keeping them would leave `writers` > 0 forever and hang
  the pipeline), closing whatever it opened on every early return so descriptors do
  not leak into its 8-slot table across runs. It waits for all N stages and reports
  the last stage's status (`$?`), matched by id: `SYS_RUN` now returns the child's
  task id and `SYS_WAIT` gained an optional out-pointer (RDI) that reports which child
  exited — extending call 6 rather than adding a fifteenth. All ring-3 programs
  migrated at once: the old single-argument `sys_write` became `sys_print` (a
  write-until-done loop), and every call site moved. Two new fixtures,
  `user/tests/COUNT.c` (reads fd 0 to EOF, prints the byte count — the downstream half
  of the EOF condition) and `user/tests/UPPER.c` (a streaming uppercase middle stage
  with a pipe on both sides). Verified under QEMU: `run a.elf | run count.elf` prints
  `20`, `run a.elf | run upper.elf | run count.elf` and a four-stage pipeline print
  `20`, ten consecutive three-stage runs hold the free-frame count and the kernel heap
  steady, 16KB (`F.c`) through a 4096-byte pipe into `count.elf` reports `16384`
  (proving the writer blocked and resumed rather than dropping bytes), six idle seconds
  under `-d int` move the `int 0x50` count by zero, and deliberately removing the
  shell's write-end close made the pipeline hang with no output (the classic bug),
  restored by putting it back. Builds warning-clean beyond the pre-existing RWX
  warning. The decision, the EOF argument, and a catalogue of the six silent ways pipes
  hang or corrupt (B1–B6) are in `docs/decisions/0022-file-descriptors-and-pipes.md`;
  see also `docs/reference/descriptors.md`, `docs/reference/pipes.md`, and the updated
  `docs/reference/syscalls.md`, `shell.md`, and `scheduling.md`.

- `SYS_STAT` (10), the eleventh syscall: report a file's size without reading it,
  so a caller can size a buffer before it reads. It wraps `fat32_stat`, which had
  existed since the read-only rung but had never been reachable from ring 3, and it
  takes `RDI` = filename, `RSI` = `uint64_t *out_size`, returning 0 or
  `SYSCALL_ERROR` when the file is not found (a directory or a non-8.3 name folds
  into the same error). Both pointers are untrusted: the filename is copied in and
  length-capped like `SYS_RUN`'s, and the **`out_size` pointer is a write target**,
  so its whole `[ptr, ptr+8)` range is bounds-checked with `user_range_ok` — the
  same check `SYS_READFILE` applies to its destination — rather than only its start,
  which would let a pointer just below `USER_REGION_END` have the kernel write a
  `uint64_t` into kernel pages. Like the write-side calls it **does not block**, so
  it has none of the RAX-discipline problem `SYS_READKEY`/`SYS_WAIT` carry. Added to
  `include/syscalls.h`, `kernel/syscall.c`, and `user/userlib.h` (`sys_stat`). This
  fixes two problems the writable-FAT32 rung's `HUGE.TXT` (40981 bytes) had exposed.
  `fat32_read_file` refuses a file larger than the caller's buffer outright, so
  `read HUGE.TXT` printed `read: cannot read huge.txt` — and that one line was three
  different failures (absent, too big, disk error) wearing one message. `cmd_read`
  (`user/shell.c`) now **stats first, then decides**: not found →
  `read: no such file: X`; found but larger than the buffer →
  `read: X is N bytes, the buffer holds M` (both numbers, e.g. `read: HUGE.TXT is
  40981 bytes, the buffer holds 32767`); otherwise read and print as before, with
  the old `read: cannot read X` now reachable only for a genuine disk error. There
  are **no partial reads and no offset** — `read` still delivers the whole file or
  none, it just says why when it declines; a prefix would need an offset on
  `SYS_READFILE`, a rung of its own. This also retired the shell's unreachable
  "showing the first N bytes, the file may be longer" notice (`TODO(read-truncation)`,
  removed from `user/shell.c` and `docs/reference/shell.md`), which could only ever
  fire for a file of exactly the buffer size, which is complete. Verified under QEMU:
  `read HUGE.TXT` reports `read: HUGE.TXT is 40981 bytes, the buffer holds 32767`,
  `read NOSUCH.TXT` reports `read: no such file: NOSUCH.TXT` (distinct from the
  too-big case), and `read HELLO.TXT` and `read BIG.TXT` still print their contents
  whole. Builds warning-clean. See `docs/decisions/0021-sys-stat.md`,
  `docs/reference/syscalls.md`, `docs/reference/shell.md`, and
  `docs/reference/fat32.md`.

- Writing to the FAT32 filesystem: files can now be created, replaced, and deleted
  by name, and what the kernel writes survives a reboot. `fs/fat32.c` gains the
  whole write side. `fat32_set_entry` writes a FAT entry into **every** copy of the
  table (a read-modify-write per copy, preserving the reserved top four bits),
  because updating only the first copy is the classic FAT-writer bug whose symptom
  is entirely off-machine — MiniOS reads the first copy and stays self-consistent
  while the host's tools see the copies disagree. `find_free_cluster` scans the FAT
  for a zero entry a **block at a time** (128 entries per read, ~1009 reads for a
  full scan rather than ~129000), starting from a persistent `fs_next_free_hint`
  (the in-memory twin of FSInfo's next-free field) and wrapping once. `alloc_chain`
  builds a chain claiming each cluster the instant it is found so it cannot be
  handed out twice, and on running out of space partway frees what it took;
  `free_chain` reads each slot's next pointer before zeroing it, bounds the walk at
  the data-cluster count so a cycle fails instead of hanging, and pulls the hint
  back to the lowest cluster freed. `find_free_dirent` finds a `0x00`/`0xE5` slot
  and, when the root directory is full, **grows** it (allocate a cluster, zero-fill
  it — an un-zeroed cluster reads back as garbage entries — and link it on);
  `write_dirent_at` is a read-modify-write of the 32-byte entry into its block.
  **`fat32_write_file` is the rung, and its seven-step order is the safety of it**:
  8.3-convert the name; look it up (remember old start cluster and slot, free
  nothing); `alloc_chain` the new contents; write the data, zero-filling the last
  cluster past `len` so no previous file's bytes leak; build the directory entry;
  **write that entry — the single commit point, one 32-byte write inside one block
  that `disk_write` moves whole or not at all**; and only then `free_chain` the old
  chain. A crash before the commit loses only unreferenced clusters and never
  touches the file already on disk; a crash after has already succeeded; there is no
  instant where the name resolves to a half-written file. `fat32_delete` runs the
  same argument in reverse — free the data first, then mark the entry `0xE5`, so a
  crash leaves a recoverable lost chain rather than a live entry pointing at freed
  clusters. The **FSInfo** sector is invalidated, not maintained: after every write
  and delete its free count and next-free hint are set to `0xFFFFFFFF` ("unknown,
  recount"), and its three signatures (`0x41615252`@0, `0x61417272`@484,
  `0xAA550000`@508) are verified before a byte is written into it, since the sector
  number comes from a possibly-corrupt boot sector; the `fat32_bpb` struct gained
  `fs_info` at offset 48 (layout guard bumped 48→50). `fat32_free_count` walks the
  whole FAT counting free clusters, a full recount that trusts no cached total so it
  can be the yardstick a leak test measures against. Three syscalls expose the write
  side (`include/syscalls.h`, `kernel/syscall.c`, `user/userlib.h`): `SYS_WRITEFILE`
  (7, the mirror of `SYS_READFILE`, filename copied in and the source range
  bounds-checked the same way), `SYS_DELETE` (8), and `SYS_FREECOUNT` (9, so the
  free count is readable from ring 3); none blocks, so none has the RAX-discipline
  problem `SYS_READKEY`/`SYS_WAIT` do. The shell (`user/shell.c`) gains `write NAME
  the rest of the line` (stored verbatim, no trailing newline; an empty remainder
  makes a zero-length file), `delete NAME`, and `free`; `write` and `delete` reject
  a non-8.3 name locally with a message that names the problem, the one failure that
  is the user's and fixable by retyping. `user/tests/F.c` (`F.ELF`) writes a 16KB
  file — a real 32-cluster chain, which typing cannot produce — reads it back, and
  self-checks byte-for-byte and on length, exiting 0 only on an exact match and with
  a distinct status per failure. Verified under QEMU with the headless driver and
  cross-checked on the host with mtools: `write NOTES.TXT hello world` then `read`
  prints exactly `hello world` and survives a full quit-and-restart; `mdir`/`mtype`
  read every written file back correctly and the two FAT copies are byte-identical
  (`md5` match) after every write, proving both are updated in lockstep; `run f.elf`
  exits 0 and `mtype ::/FTEST.TXT | wc -c` reports 16384; ten `write T.TXT hello` /
  `delete T.TXT` cycles hold `free` at exactly its baseline (128714); replacing
  `NOTES.TXT aaa` with `NOTES.TXT bbbbbbbb` leaves the free count unchanged and
  `read` shows only `bbbbbbbb`; twenty files cross the 16-per-cluster directory
  boundary and all appear with no host-reported damage; `write my-notes.text hello`
  is rejected naming the 8.3 problem; `write EMPTY.TXT` makes a 0-byte file;
  `delete NOSUCH.TXT` fails cleanly; the existing `run a.elf`/`c.elf`/`d.elf`,
  `read BIG.TXT`, and `list` behave as before; and six idle seconds under `-d int`
  move the `int 0x50` count by zero. See
  `docs/decisions/0020-writable-fat32.md`, `docs/reference/fat32.md`,
  `docs/reference/syscalls.md`, and `docs/reference/shell.md`.

- Shift and caps lock (`drivers/keyboard.c`), so uppercase letters and the shifted
  symbols `!@#$%^&*()_+{}|:"<>?~` can be typed for the first time. Scancodes 0x2A
  (left shift) and 0x36 (right shift) previously mapped to 0, the table's
  "unmapped" value, and 0x3A (caps lock) had no entry at all, so `return HELLO` was
  not a command that failed but one that could not be entered. Added a second
  translation table, `scancode_to_ascii_shift[128]`, laid out identically to the
  existing one and kept adjacent to it so a wrong entry shows up as a column that
  does not line up; two flags, `shift_held` (mirrors the hardware: set on press,
  cleared on release) and `caps_on` (invented by the driver, toggled on press and
  never cleared); and a `table_for()` helper that picks between them. **A release
  is masked before it is compared**: left shift's release arrives as 0xAA, not
  0x2A, so a check against the raw byte matches nothing and shift turns on and
  stays on forever. **Shift and caps lock combine by XOR, and for letters only** —
  with caps lock on, holding shift gives a lowercase letter, which is what a real
  keyboard does, and OR-ing them would also make caps lock shift the number row.
  **A modifier press pushes nothing and wakes nobody**, guaranteed by early returns
  before `kbd_buffer_push` and `scheduler_wake(WAIT_KEY)`: waking readies every
  task blocked on a key, so a shift press that reached it would have them all
  re-issue `SYS_READKEY`, find an empty buffer and block again, undoing the idle
  quiet that blocking exists to get. Measured: holding shift for six seconds at an
  idle prompt moves the `int 0x50` count by zero. Extended scancodes (arrows, right
  ctrl, right alt) are **documented and still not handled** — they arrive as a 0xE0
  prefix whose bit 7 is set, so the release branch swallows it and the next byte is
  decoded as an ordinary key, which is harmless today by luck rather than design
  (`TODO(extended-scancodes)`). Ring 3 still receives one finished character and
  never sees a scancode. See [docs/reference/keyboard.md](docs/reference/keyboard.md)
  and [decision 0019](docs/decisions/0019-keyboard-modifier-state-in-the-driver.md).

- `HUGE.TXT` (40981 bytes) on the disk image, generated by the Makefile and copied
  by a new phony `disk-testfiles` target on every `make run`. Nothing on the disk
  had exceeded 16KB, so the path taken when a file does not fit the caller's buffer
  had never once run. It is generated by `make` rather than added to
  `tools/mkdisk.sh` because that script refuses to touch an image that already
  exists, so anything added there reaches a freshly formatted disk and no other.
  What it revealed: `fat32_read_file` refuses an oversized file outright rather
  than filling the buffer, so `read HUGE.TXT` prints `read: cannot read huge.txt`
  and the shell's "showing the first 32767 bytes" notice turns out to be
  unreachable — it can only fire for a file of exactly 32767 bytes, which is
  complete (`TODO(read-truncation)`). Behaviour unchanged; the reference page now
  says so.

- A process lifecycle: a task can now end, and its memory comes back. `SYS_EXIT`
  (0) takes an exit status in RDI and ends the calling task instead of halting the
  machine; `SYS_WAIT` (6) blocks a task until any child of it exits and returns
  that child's status. `task_state_t` gained `TASK_ZOMBIE` (a tombstone, not a
  process: no code, no stack, and after the sweep no address space, just an
  `exit_status` and an id for whoever is waiting), `wait_reason_t` gained
  `WAIT_CHILD`, and `task_t` gained `parent_id` and `exit_status`. `SYS_RUN`
  records `scheduler_current_id()` as the new task's parent, which is what a later
  `SYS_WAIT` matches on; the boot task's parent is `TASK_NO_PARENT`, since nothing
  can ever wait on it. **Death is two-phase.** `task_exit` does paperwork only: it
  masks the status to 0..255, stores it, marks the task `TASK_ZOMBIE`, wakes the
  parent, and switches away, freeing nothing at all. It cannot free anything: the
  task calling `SYS_EXIT` is the task on the CPU, its stack is the stack, and its
  address space is the tree CR3 points at, so freeing any of it would hand the
  running machine's own memory back to the allocator, and nothing would fault at
  that moment. **Cleanup is split by weight.** `reap_sweep()`, called at the top of
  `schedule()`, tears down the address space of any zombie **except `current`**,
  and that one exception is the entire safety argument; by the time `schedule()`
  runs again somebody else is on the CPU with their own CR3. If the zombie's parent
  is already gone, the tombstone is pointless, so the `task_t` is `kfree`d and the
  slot NULLed on the spot (orphans lose their tombstone; there is no reparenting
  because there is no `init`). If the parent is alive, the struct survives holding
  the status until `task_wait` collects it and frees it, because that struct *is*
  the answer the parent came for. `paging_destroy_address_space` (`kernel/paging.c`)
  is the teardown: it walks `pml4[0] → pdpt[0] → pd` and descends into exactly two
  PD entries by index (`USER_PD_INDEX_CODE`, `USER_PD_INDEX_STACK`), freeing leaf
  pages before their page table, then the PD, PDPT, PML4, and the handle. The
  explicit two-entry list is the whole safety of the function: the kernel half is
  cloned by value into every tree, so a generic "free everything present" loop would
  return the live kernel's mappings to the frame allocator and kill the machine
  minutes later somewhere unrelated, with no message pointing anywhere near here.
  Its precondition, that `as` must not be the tree in CR3, has no check behind it
  and the same delayed, misattributed death when violated. Because tasks are now
  freed, `tasks[]` has **NULL holes**: `any_task_ready`, `find_next_ready`, and
  `scheduler_wake` each gained a NULL check, `schedule()` carries a defensive early
  return for a NULL `current` that should be unreachable, `num_tasks` became a high
  water mark rather than a live count, and **ids are never reused**, so a stale id
  can only ever name nothing rather than a different task that inherited the number.
  The `WAIT_CHILD` wake is by id rather than a `scheduler_wake` broadcast, because
  every parent waits on the same reason and a broadcast would ready all of them to
  re-issue `SYS_WAIT`, find none of their own children finished, and block again;
  and like the keyboard's, it publishes first (status and `TASK_ZOMBIE` set before
  the wake). The RAX rule from blocking now covers two syscalls: `task_wait` writes
  `regs->rax` itself on the two paths that have an answer (a status, or
  `SYSCALL_ERROR` for "no children", which is why the status is masked, so the two
  ranges cannot collide) and writes nothing on the path that blocks, and `SYS_EXIT`
  writes it never. `SYSCALL_ERROR` moved from `kernel/syscall.c` to
  `kernel/syscall.h` so the scheduler and the dispatcher share one spelling. On the
  ring-3 side, `sys_exit(int)` and `sys_wait(void)` joined `user/userlib.h`;
  `user/A.c`, `B.c`, and `C.c` gained bounded loops (20, 60, and 40 rounds) and call
  `sys_exit` at the bottom (C exits 3, so the status is visibly not a default), and
  the shell's `run` now calls `SYS_WAIT` after a successful `SYS_RUN` and prints
  `run: A.ELF exited with status 0`, so the prompt returns only when the program is
  finished. The bound is load-bearing rather than cosmetic: there is no way to kill
  a task and there are no signals, so a program that never exits leaves the shell
  blocked with no way back short of a reboot (`TODO(kill-and-signals)`). There is no
  crt0, so a program that falls off the end of `_start` runs into whatever bytes
  follow it. Ten consecutive `run A.ELF` return the free-frame count to the same
  value every time, which is the only thing that distinguishes freeing most of it
  from freeing all of it. See `docs/decisions/0018-process-lifecycle-exit-and-wait.md`,
  `docs/reference/scheduling.md`, and `docs/reference/paging.md`.
- `frame_free_count()` (`kernel/memory.c`), a diagnostic that reports how many
  frames are currently free, and a `LIFECYCLE_DEBUG`-gated `reap:` line that prints
  it at the moment a dead task's address space is destroyed. Nothing in the kernel
  makes a decision on this number; it exists so a leak can be *observed* rather than
  argued about, by checking the count comes back to the same value after the same
  work. It walks the whole bitmap on every call and must stay off any hot path.
- Blocking and sleep in the scheduler, so a task with nothing to do gives up the
  CPU entirely instead of spinning, and is woken by whatever causes the event it
  waited for. `task_state_t` gained `TASK_BLOCKED` and `task_t` gained a
  `wait_reason_t` (`WAIT_NONE`, `WAIT_KEY`): the state takes a task out of the
  rotation, and the reason is what lets the right waker find it, since a keypress
  should ready the tasks waiting for a key and leave the others asleep.
  `schedule()` now picks only `TASK_READY` slots, so a blocked task is stepped
  over however many times the cursor comes round, and it no longer forces the
  outgoing task back to `TASK_READY` on save, which would have undone a block on
  the spot. When nothing at all is runnable it parks in `sti; hlt` (interrupts
  must be enabled there: an interrupt handler is the only thing that can produce a
  ready task, so halting with them masked would be a dead machine rather than an
  idle one, and the two instructions must stay adjacent because `sti` takes effect
  only after the following instruction, so a wakeup cannot slip into the gap), with
  a `scheduler_idling` guard so timer ticks landing during the park do not stack up
  on the single shared kernel stack. A task puts itself to sleep with
  `task_block(regs, reason)`, which **re-arms the syscall** rather than freezing
  mid-handler: it winds the saved `rip` back by `INT_INSTR_LEN` (2, the length of
  `int 0x50`) so the pile points at the int rather than after it, then drives the
  same `schedule()` the timer drives. A woken task's `iretq` therefore lands on the
  int, the syscall is issued again from scratch, and this time it finds what it was
  waiting for, because that is precisely what being woken means. Re-arming rather
  than resuming in the middle is forced by two facts about this kernel: the saved
  pile's `rip` is always a ring-3 address, so restoring a pile can never resume
  inside a C function in the kernel; and `tss.rsp0` points at one static buffer
  shared by every task, so the C frames a handler stands on are abandoned the
  moment it switches away. Mid-operation blocking would need a per-task kernel
  stack, recorded as `TODO(per-task-kernel-stack)`. `scheduler_wake(reason)` marks
  every task blocked on that reason `TASK_READY` and is called by whatever causes
  the event, today `keyboard_callback` after it pushes a character (push first,
  wake second, so a woken task cannot be scheduled before the character it was
  promised is there); the waker only changes state and does not switch tasks, which
  keeps it safe to call from interrupt context. `SYS_READKEY` became blocking:
  `sys_readkey` now takes the register pile, writes `regs->rax` itself on the path
  that has an answer, and calls `task_block(regs, WAIT_KEY)` on an empty buffer,
  with no re-check loop anywhere (a loop inside the handler would be the same
  busy-wait moved into ring 0, where the kernel spins with interrupts masked and
  every other task starves). Writing `rax` moved out of the dispatcher because the
  re-armed pile re-executes only the `int`, so the CPU reads the syscall number
  from whatever `rax` holds, and since `SYS_EXIT` is 0 a stray `return 0` on the
  blocking path would halt the machine on the next keypress. `user/shell.c` lost
  its `if (k == 0) continue;` poll, closing `TODO(blocking-readkey)`. Measured
  under QEMU with `-d int` over a six second window with the shell idle at its
  prompt and nobody typing, the kernel went from servicing **362,648** `int 0x50`
  syscalls (a 494MB interrupt log) to **3**: one `SYS_WRITE` for the banner, one
  for the prompt, and one `SYS_READKEY` that goes to sleep, with the log down to
  848KB and only timer (`v=40`) and syscall (`v=50`) vectors appearing at all, no
  page fault (`0x0E`), `#GP` (`0x0D`), double fault (`0x08`), or triple fault. The
  shell behaves exactly as before (`help`, `list`, `read HELLO.TXT`, `return`, and
  `run A.ELF` all work, and a launched program interleaves with the now sleeping
  shell). There is still no timed sleep, so nothing can ask to be woken after a
  duration and a blocking call has no timeout (`TODO(timed-sleep)`), and wakeup is
  a linear scan rather than a per-reason wait queue. See
  `docs/decisions/0017-blocking-and-sleep.md` and `docs/reference/blocking.md`.
- An interactive shell, built as a ring-3 program (`user/shell.c`, booted off the
  disk as `SHELL.ELF`) rather than kernel code. It reads typed commands and runs
  them using nothing but syscalls, which is the point: a fully fenced-in program,
  holding no privilege and touching the keyboard, screen, filesystem, and loader
  only through `int 0x50`, runs an interactive shell, and that is the proof the
  syscall boundary is complete. The commands are MiniOS's own, deliberately not the
  Unix names: `list` (list the root directory), `read <file>` (print a file's
  contents), `run <file>` (load and start a program), `help`, `clear`, and `return
  <text>` (echo). The shell reads a line a key at a time, echoing printable
  characters and handling backspace, tokenizes it in place, matches the first word,
  and reprints the prompt; an empty line does nothing and an unknown word prints
  `unknown command: <word>`. Four new syscalls back it, each an entry in the `int
  0x50` dispatcher (`kernel/syscall.c`): `SYS_READKEY` (2) pops one key from a new
  keyboard ring buffer or returns 0 when none is waiting; `SYS_LIST` (3) writes the
  root directory's names into a caller buffer, newline-separated; `SYS_RUN` (4)
  copies in a filename and calls `task_create_from_file`, so the launched program
  joins the scheduler and interleaves with the shell; `SYS_READFILE` (5) reads a
  whole file into a caller buffer. `SYS_READKEY` is non-blocking by design: this
  kernel cannot sleep a task, so on an empty buffer it returns 0 immediately and the
  shell busy-waits, recorded as `TODO(blocking-readkey)`. Every pointer these calls
  take from ring 3 is untrusted and checked before the kernel touches it, better
  than the `SYS_WRITE` stopgap it sits beside: `user_range_ok` bounds the whole
  `[ptr, ptr+len)` destination range (overflow-safe, checking the length against the
  room above the pointer rather than forming a sum that can wrap), and
  `copy_user_string` copies a filename in with a length cap so a string with no
  terminator cannot walk off the region. The keyboard IRQ was reduced to a producer:
  `keyboard_callback` now only decodes one scancode and pushes the character into a
  fixed 128-slot ring buffer (write and read indices, wrap modulo size, one slot
  always left unused so empty and full stay distinguishable, newest key dropped on a
  full buffer), keeping real work out of interrupt context. The tokenizer,
  `next_token` in `user/userlib.h`, is a reentrant (`strtok_r`-style) in-place
  splitter: it holds the cursor in a caller pointer with no hidden global, skips
  leading separators so repeated spaces do not yield empty tokens, and shreds the
  line by overwriting each separator with a NUL. It lives in the user runtime rather
  than `libc/string.c` because the user build links no kernel objects (so
  `libc/string.c` is unreachable from ring 3) and the kernel never tokenizes (so it
  would be dead there). `user/userlib.h` also gained `syscall0`/`syscall2`/`syscall3`
  (only `syscall1` existed) and wrappers `sys_readkey`/`sys_list`/`sys_run`/
  `sys_readfile`; `fs/fat32.c` gained `fat32_list_names`, the buffer-filling sibling
  of `fat32_list_root`, sharing the same `walk_directory` through a new name sink so
  listing and lookup still go through one walk. `kernel_main` now boots `SHELL.ELF`
  alone (the letter-printers `A/B/C.ELF` stay on the disk so `run A.ELF` can start
  one on demand), and the Makefile builds `SHELL.ELF` with an explicit rule (its
  source is lowercase `shell.c` but its 8.3 disk name is uppercase). Verified under
  QEMU by a scripted key session (keys injected through the monitor, the VGA text
  buffer dumped after each command): the prompt appears at boot; `help` prints the
  command list; `list` prints the seven files; `read hello.txt` prints `Hello from
  FAT32!`; `run a.elf` prints `run: started a.elf` and then A's output interleaves
  with the live prompt; `return hello world` prints `hello world`; `asdf` prints
  `unknown command: asdf` without faulting; and backspace correctly edits the line.
  `-d int` over the session showed only timer (`v=40`), keyboard (`v=41`), and
  syscall (`v=50`) vectors, with no page fault (`0x0E`), `#GP` (`0x0D`), double fault
  (`0x08`), triple fault, or disk IRQ (`0x4E`). See
  `docs/decisions/0016-interactive-shell.md` and `docs/reference/shell.md`.
- An ELF64 program loader (`kernel/elf.c`, `kernel/elf.h`), and with it, user
  programs that are files rather than parts of the kernel. Previously the three
  ring-3 programs were compiled into the kernel image (`user/user_program.c` into
  a `.user_text` section at 4M inside `minios.bin`) and copied out of the kernel
  by `task_create`, so changing a program meant recompiling the kernel and the
  set of programs was fixed at kernel link time. They are now separately
  compiled, statically linked ELF64 binaries (`user/A.c`, `B.c`, `C.c` linked
  with `user/user.ld`) that live on the FAT32 image as `A.ELF`, `B.ELF`, `C.ELF`
  and are read, validated, and loaded at runtime. The loader parses only the ELF
  header and the program headers (section headers describe the file for linkers
  and debuggers and tell a loader nothing), and for each `PT_LOAD` segment it
  bounds-checks the destination, allocates frames, maps them into the target
  address space with flags derived from the segment's own, copies `p_filesz`
  bytes, and zeroes from there up to `p_memsz`. Validation happens before
  interpretation throughout: the file is confirmed to be at least header-sized
  before the header is read as one, the magic before the class, the class before
  anything 64-bit-shaped is dereferenced, `e_phentsize` before the table is
  strided, and the program header table's own bounds before a single entry is
  read out of it; then each segment is checked to lie inside the file, to have
  `p_memsz >= p_filesz`, and to be page-aligned. Every rejection names its
  reason. The in-file range check subtracts from the known-good file size rather
  than adding two untrusted 64-bit values, whose sum can wrap and compare as
  comfortably small. The segment bounds check is the security boundary of the
  whole feature and is commented as such: a program header is an instruction
  from an untrusted file reading "write these bytes to this address", so without
  it a crafted or merely corrupt file names any address it likes and the kernel
  maps a page there and copies attacker-chosen bytes in, including over the
  kernel; the destination range is checked against `[USER_REGION_START,
  USER_STACK_BASE)` before a frame is allocated, stopping below the fixed user
  stack so a segment cannot collide with it. This is the same category of check
  as the `SYS_WRITE` pointer validation and the stricter of the two, bounding the
  whole `[start, end)` range rather than just the start. The zero-fill from file
  size to memory size is equally load-bearing: that gap is uninitialised data
  (`.bss`) the format deliberately does not store, and `alloc_frame` returns a
  frame holding whatever the last user left in it, so skipping the zeroing makes
  a program's globals come up holding stale garbage and the program work or fail
  depending on what ran before it. Frames are zeroed whole and then overwritten
  with the file bytes, which writes some bytes twice and makes the boundary case
  impossible to get wrong. Both structs are `__attribute__((packed))` with
  compile-time size guards (64 and 56 bytes), the same trap as the Multiboot mmap
  entry and the FAT32 BPB. `task_create_from_file` (`kernel/scheduler.c`) is
  deliberately the same shape as the old `task_create`: private address space,
  user half filled, stack mapped, frame forged, with only the source of the bytes
  and the entry address differing (the ELF header's `e_entry` rather than a
  compile-time symbol); the forge moved into one shared `task_register` so both
  paths used identical logic, and the page tree, the stack mapping, `schedule()`
  and the CR3 switch are untouched. A missing or malformed file costs only its
  own task: `kernel_main` prints the reason, skips it, and runs the rest. Also
  adds `print_hex` to the screen driver (and deletes the duplicate static copy
  that `kernel/heap.c` was carrying), since addresses and offsets are read in hex
  in every other tool. Verified by the one demonstration that cannot be faked:
  editing `user/A.c` to print `Z`, rebuilding only `A.ELF`, copying it onto the
  image, and booting a byte-identical `minios.bin` (same MD5) printed `ZBCZBC`.
  Also verified that a 520-byte text file named `BAD.ELF` is rejected with `not
  an ELF file (bad magic)` and a nonexistent `MISSING.ELF` with `not found on the
  disk`, both without faulting and without stopping the three real programs, and
  that `-d int` shows only timer (`v=40`) and syscall (`v=50`) vectors: no page
  (`0x0E`), GP (`0x0D`), or double (`0x08`) fault, no triple fault, no disk IRQ.
  See `docs/decisions/0015-elf-program-loading.md` and
  `docs/reference/elf-loading.md`.
- A separate build for user programs (`user/user.ld`, `user/userlib.h`, and the
  `USER_*` rules in the `Makefile`). Each program is built with `-ffreestanding
  -m64 -mno-red-zone -mcmodel=small -fno-pie -no-pie -nostdlib -nodefaultlibs
  -static -Wall -Wextra` against its own linker script. `-mcmodel=small` rather
  than the kernel's `-mcmodel=kernel` is required, not stylistic: the kernel
  model assumes every symbol lives in the top 2GB of the address space and
  produces relocation errors on code linked at 0x400000, the same trap the
  original user-mode work hit. `user/user.ld` sets `ENTRY(_start)`, links at
  0x400000, and declares two load segments (R+X for text and rodata, R+W for data
  and bss), starting every loadable segment on a 4096-byte boundary AND rounding
  its size up to a whole number of pages via a trailing `. = ALIGN(4096)` inside
  each output section (which is what rounds the size, not just the start). That
  alignment is a deliberate contract with the loader, which requires it and
  rejects a file that violates it: the loader maps whole pages, so a segment
  starting or ending mid-page would put two segments in one page and the second
  mapping would have to merge rather than replace, and since the linker script is
  entirely under our control, satisfying it there is far cheaper than supporting
  the general case in the kernel. `-Wl,-z,max-page-size=4096` keeps the linker
  from padding segments out to its default 2MB. `user/userlib.h` is the whole
  runtime a program gets: `always_inline` inline-asm syscall wrappers over the
  standalone `include/syscalls.h` and `include/vectors.h`, plus the delay loop.
  Each program carries a zero-initialised global so it has a real `.bss` (its
  data segment has `filesz` 0 and `memsz` 0x1000), which keeps the loader's
  zero-fill honest: a program with no `.bss` would load correctly even if that
  step were missing. The binaries are copied onto the image by a PHONY
  `disk-programs` target that `make run` depends on, so they are refreshed on
  every boot; the image itself is still created once and left alone, but a stale
  program binary looks exactly like a loader bug and must never survive a
  rebuild. `user/*.ELF` is git-ignored. See `docs/building.md`.
- `fat32_stat(name, &size)` (`fs/fat32.c`): report a file's size from its
  directory entry without reading any of its contents. Reading a file means
  allocating a buffer for it first, which means knowing the size first, and
  `fat32_read_file` cannot answer that (it needs the buffer up front). The
  alternative a caller would otherwise reach for, a fixed buffer assumed to be
  big enough, is a size limit that fails quietly the day a file outgrows it.
  Stat and read now share one root-directory lookup path.
- A read-only FAT32 filesystem (`fs/fat32.c`, `fs/fat32.h`), the layer that gives
  the disk driver's numbered blocks meaning. Three calls: `fat32_init` parses the
  boot sector and caches the volume geometry, `fat32_list_root` prints the root
  directory (name, and either a size or `<DIR>`), and `fat32_read_file(name, buf,
  bufsize, &out_size)` reads a file by 8.3 name into a caller's buffer and
  reports its real length. Read-only on purpose: reading needs the boot sector,
  cluster chains, and directory entries, while writing additionally needs
  free-cluster search, chain updates, mirroring every FAT copy, directory entry
  updates, directory growth, and crash ordering, which is where filesystems get
  hard and where bugs corrupt data instead of merely failing. It is also the
  complete prerequisite for the next rung (a program image is read, never
  written), so this is not half a step; write support is separate work, marked
  `TODO(fat32-write)`. Also out of scope: long filenames (LFN entries are
  skipped, so a long-named file is invisible to MiniOS), permissions, and
  subdirectory creation; lookups are root-directory only, though the walk itself
  takes any starting cluster and a subdirectory lists as `<DIR>`. The BPB struct
  is `__attribute__((packed))` because its fields sit at unaligned offsets
  (`bytes_per_sector` is 16 bits at offset 11) and padding would silently shift
  every field after the first misalignment, the same trap as the Multiboot mmap
  entry; a compile-time size guard fails the build if padding returns.
  `sectors_per_fat` is read from the 32-bit FAT32 field, not the 16-bit legacy
  one (which is zero here and would put the data area on top of the FAT), and the
  parsed geometry is sanity-checked (512-byte sectors, power-of-two cluster size,
  the `0xAA55` signature at offset 510, non-zero FAT length) before anything is
  computed from it. `block_of_cluster(n) = first_data_block + ((n - 2) *
  sectors_per_cluster)`: the `- 2` is because FAT slots 0 and 1 are reserved, so
  cluster 2 is the first that can hold data and sits at offset 0 of the data
  area; contents shifted by exactly two clusters' worth of bytes is the symptom
  of getting it wrong. Every FAT entry is masked with `0x0FFFFFFF` before
  comparison, since only the low 28 bits are meaningful and an unmasked entry
  makes end-of-chain detection fail intermittently; after masking, `0x00000000`
  is free, `0x0FFFFFF7` is bad, `>= 0x0FFFFFF8` ends the chain, and anything else
  is the next cluster. Only the first FAT copy is read (a writer would have to
  update all of them). The start cluster is recombined from its two halves,
  `(high << 16) | low`, which sit at opposite ends of the 32-byte directory
  entry. Directory scanning stops at a `0x00` first byte (never-used, so nothing
  follows), skips `0xE5` (deleted), skips attribute `0x0F` (long-filename
  fragment, checked before the volume-id bit because an LFN entry sets that bit
  too), skips the volume label (`0x08`), and marks subdirectories (`0x10`).
  Helpers convert `"HELLO.TXT"` to the 11-byte space-padded uppercase on-disk
  form `"HELLO   TXT"` and back for display. Reads follow the chain one cluster
  at a time into a `kmalloc`'d buffer (the heap, not the stack, since a cluster
  can be 128KB at the format's maximum) and trim the last cluster to the size in
  the directory entry, so the stale bytes after the end of the file never reach
  the caller. One cluster is always one `disk_read`, guaranteed at compile time
  because `sectors_per_cluster` and `disk_read`'s count are both single bytes.
  Both the directory walk and the file read are bounded by the volume's cluster
  count and return -1 rather than spinning on a self-referential chain, the same
  reasoning as the disk driver's bounded polling. Verified under QEMU with a
  temporary self-test in `kernel_main` (added, verified, removed) that printed
  the geometry (`512 B/sector, 1 sectors/cluster, first data block 2050, root
  cluster 2`), listed the root (`HELLO.TXT 17`, `TEST.TXT 19`, `BIG.TXT 16384`),
  read and compared `HELLO.TXT`, verified all 16384 bytes of the 32-cluster
  `BIG.TXT` against a known repeating pattern, and confirmed a missing file
  returns -1: `PASS (small file)`, `PASS (multi-cluster file)`, `PASS (missing
  file returns -1)`. A negative control (removing the `- 2`) failed those checks,
  which is what makes the passes mean something. The `-d int` log showed only
  timer (`v=40`) and syscall (`v=50`) vectors: no page (`0x0E`), GP (`0x0D`), or
  double (`0x08`) fault, no triple fault, and still no disk IRQ (`v=4e`). See
  `docs/decisions/0014-read-only-fat32.md` and `docs/reference/fat32.md`.
- A formatted disk image with test files (`tools/mkdisk.sh`). The image was 16MB
  of zeros with no filesystem; it is now 64MB and formatted FAT32, with three
  test files copied in. 64MB rather than 16MB because FAT32 is only legal with at
  least 65525 clusters, which 16MB cannot reach with a sane cluster size, so
  formatting tools either refuse or silently produce FAT16. Formatting uses
  mtools (`mformat -i disk.img -F ::`, then `mcopy -i disk.img file ::/`), which
  edits the image file directly as a bare FAT volume, so it needs no `sudo` and
  no mounting; mtools is now a build dependency (`brew install mtools`, `apt
  install mtools`). The volume is a "superfloppy" (it starts at block 0, no
  partition table), which is why block 0 is the boot sector. The test files are
  `HELLO.TXT` and `TEST.TXT` (short known strings) and `BIG.TXT` (16384 bytes of
  a repeating 16-byte pattern), the last deliberately larger than one cluster so
  chain following is genuinely exercised: a single-cluster file reads correctly
  even when the chain logic is completely broken. The `Makefile`'s `disk.img`
  rule now calls the script and `make run` still depends on it; both the rule and
  the script skip an image that already exists, since `make run` calls into this
  on every boot and reformatting would silently destroy the disk's contents.
  `disk.img` stays git-ignored. See `docs/building.md`.
- A polled ATA PIO disk driver (`drivers/disk.c`, `drivers/disk.h`): `disk_read`
  and `disk_write` move any run of contiguous 512-byte blocks between a disk and a
  buffer on the primary ATA bus, addressed by LBA28. A disk is treated as a flat
  array of 512-byte blocks numbered from zero; every transfer names an exact block
  and the driver only moves its bytes (choosing and naming blocks is a
  filesystem's job, still absent). Deliberately the simplest correct driver: the
  CPU polls the status port and copies every 16-bit word itself, no interrupts and
  no DMA, so a transfer freezes the whole machine (the scheduler cannot preempt
  mid-transfer), an accepted PIO limitation, not a bug. The LBA28 block number is
  split across the LBA-low/mid/high ports (bits 0-23) and the low nibble of the
  drive/head port (bits 24-27, OR'd with 0xE0 to select the master and enable LBA
  mode); READ SECTORS (0x20) and WRITE SECTORS (0x30) start a transfer and a block
  is 256 words at the 16-bit data port (`port_word_in`/`port_word_out`), not 512
  byte reads. A write issues CACHE FLUSH (0xE7) and waits, because it is not
  durable until the drive commits its buffer; a read needs no flush because it
  changes nothing. Two spots that bite are handled and commented: a ~400ns settle
  after drive select (read alt-status 0x3F6 four times, discard) so the status
  bits are valid, and every poll loop bounded by a large cap that returns -1 on
  timeout so a missing or wrong-bus disk fails loudly instead of hanging. Because
  the driver polls, `disk_init` sets nIEN in the device control register (0x3F6)
  so the drive never asserts IRQ14, and does a minimal presence check (status 0xFF
  = floating bus / no drive) printing whether a disk was detected. Add
  `port_word_out` to `drivers/ports.c`/`.h`, the write-side mirror of
  `port_word_in`. The `Makefile` gains a `disk.img` target (`qemu-img create -f
  raw disk.img 16M`, git-ignored) and `make run` attaches it to the primary bus
  with `-drive file=disk.img,format=raw,if=ide,index=0,media=disk`. Verified under
  QEMU with a temporary self-test in `kernel_main` (added, verified, removed):
  writing a known pattern and reading it back into a zeroed buffer compared byte
  for byte printed `DISK TEST: PASS` for one block and for two contiguous blocks,
  the `-d int` log showed no page (`0x0E`), GP (`0x0D`), or double (`0x08`) fault
  and no disk IRQ (`v=4e`), and the three ring-3 tasks kept interleaving "ABC"
  afterward. See `docs/decisions/0013-ata-pio-disk-driver.md` and
  `docs/reference/disk.md`.
- Per-process paging: a private page-table tree per task (`kernel/paging.c`,
  `kernel/paging.h`). Each task now has its own address space, loaded into CR3 on
  every context switch, so two tasks use the same virtual addresses (code
  `0x400000`, stack top `0x800000`) backed by DIFFERENT physical frames: real
  isolation, replacing the single identity-mapped tree every task used to share.
  Every tree has two halves. The USER half is private: 4KB pages to fresh frames,
  user bit set, holding a per-task copy of the ring-3 image and a fresh stack. The
  KERNEL half is cloned from the boot `pd_table` BY VALUE (every PD entry except
  the two user slots `pd_table[2]`/`pd_table[3]`, each carrying the identical 2MB
  huge-page kernel mapping, no user bit), so the kernel is mapped identically in
  every tree and an interrupt still lands in mapped kernel code without a CR3
  change. By value, not by reference, because MiniOS hangs the ring-3 region off
  the same `pd_table` the kernel uses, so sharing it by reference would share the
  user huge pages too and make a private `0x400000` impossible (see the ADR). The
  by-value clone rests on kernel mappings being FROZEN after boot
  (`memory_detect_and_map` fills the identity map once, nothing mutates a kernel
  PD entry afterward); a tripwire comment in `kernel/paging.c`, the reference page,
  and the ADR all warn that runtime kernel remapping (ASLR, hot-plug, higher-half)
  would make the copies go stale and force a switch to by-reference. New API:
  `paging_create_address_space()` (own PML4/PDPT/PD from `alloc_frame`, clone the
  kernel half), `paging_map_page(as, virt, phys, flags)` (4KB walk creating
  intermediate tables, user bit AND-down), and `paging_switch(as)` (load CR3,
  which also flushes the TLB since no page is `PG_GLOBAL`). `PG_*` flag bits and
  `PTE_ADDR_MASK` are shared in `kernel/paging.h`. `boot/boot.asm` now exposes
  `pml4_table` and `pdpt_table` as `global` (alongside `pd_table`) so the C clone
  can read all three boot tables. `task_t` (`kernel/scheduler.h`) gains an
  `address_space_t *aspace` and a cached `cr3`; `task_create` builds the tree
  (copying the whole linked image `_user_text_start`.._user_rodata_end into fresh
  frames at `0x400000`, mapping a fresh stack at the fixed `USER_STACK_TOP`) and
  the old user-stack bump allocator (`alloc_user_stack`, `USER_STACK_REGION_*`) is
  gone: every task's stack is now the same VA on its own frames, retiring the
  eight-stack ceiling and no-guard-page corruption from
  `docs/decisions/0011-dynamic-tasks-and-stacks.md`. `schedule()` loads the
  incoming task's CR3 after copying its register pile; `scheduler_start()` loads
  task 0's CR3 before the first drop to ring 3. Copying the read-only text per
  task is deliberate isolation, marked `TODO(shared-text)` for a future
  by-reference-text refinement. Verified under QEMU with `-d int`: three distinct
  task CR3 values, the same three user RIPs (`0x40002b`, `0x400083`, `0x4000db`)
  all at `cpl=3`, an even round-robin interleave, and zero page (`0x0E`), GP
  (`0x0D`), or double (`0x08`) faults; a temporary isolation proof (added,
  verified, removed) walked each tree and confirmed the shared VAs `0x400000` and
  the stack top page resolve to different physical frames in all three. See
  `docs/decisions/0012-per-process-paging.md`, `docs/reference/paging.md`,
  `docs/reference/scheduling.md`, and `docs/reference/memory-map.md`.
- Dynamic tasks and per-task user stacks in the scheduler (`kernel/scheduler.c`,
  `kernel/scheduler.h`). Task structs are now heap-allocated: `task_create` calls
  `kmalloc(sizeof(task_t))` and stores the pointer in `task_t *tasks[MAX_TASKS_LIMIT]`
  (a flat pointer array, cap 64, arbitrary), retiring the fixed `.bss`
  `task_t[MAX_TASKS]` array of four that existed only for lack of a heap. User
  stacks are handed out by a new bump allocator (`alloc_user_stack`) that carves
  fixed 256KB (`USER_STACK_SIZE`) slices from the one PG_USER stack region
  (`0x600000`-`0x800000`, 8 slices), replacing the two hardcoded stack tops
  (`TASK0_STACK_TOP`/`TASK1_STACK_TOP`); `task_create` no longer takes a
  `stack_top` argument. User stacks deliberately do NOT come from the kernel heap:
  `kmalloc` returns frame-pool pages with no PG_USER bit, so a ring-3 push there
  would fault. A third ring-3 program (`user_program_c`, prints "C") was added and
  all three are created in `kernel_main` to exercise the dynamic path. The
  switching logic in `schedule()` is unchanged (only indexing: `tasks[i].regs`
  became `tasks[i]->regs`, and the round-robin walk is bounded by `num_tasks`).
  The task-struct ceiling is gone; the user-stack region remains a hard ceiling
  (no guard pages, one shared region) until per-process paging, marked
  `TODO(per-process-paging)`. Verified under QEMU with `-d int`: the syscall
  vector fires from three distinct user RIPs (`0x40002b`, `0x400083`, `0x4000db`),
  each at `cpl=3` on a distinct stack, with the timer still firing and zero page
  or GP faults; the three programs interleave "ABC" on screen forever. See
  `docs/decisions/0011-dynamic-tasks-and-stacks.md`,
  `docs/reference/scheduling.md`, and `docs/reference/memory-map.md`.
- A kernel heap, `kmalloc`/`kfree` (`kernel/heap.c`, `kernel/heap.h`), ported from
  the CMSC216 p5 `el_malloc`: an explicit free list with header/footer boundary
  tags, first-fit allocation, block splitting, and coalescing with the free
  neighbours above and below. The algorithm is the original, unchanged; only the
  OS seams differ. The slab comes from a new `alloc_frames_contiguous(n)` in
  `kernel/memory.c` (a run of consecutive clear bits in the linear bitmap is
  contiguous, identity-mapped RAM) instead of `mmap`, so the p5 single-contiguous
  -slab assumption holds; `el_ctl` is a static `.bss` struct rather than an
  `mmap`'d page, and the fixed target addresses and their asserts are gone.
  `printf`/`fprintf` become the VGA `print_string` (with kernel-native decimal and
  hex printers for the stats helpers); `assert` and the `mmap`-return checks are
  dropped. `kmalloc`/`kfree` wrap their critical section in a save-and-restore
  interrupt guard (`pushfq`/`cli` ... `popfq`), because the free list is shared
  mutable state and the 100 Hz timer IRQ could otherwise land mid-relink and
  corrupt it; restore, not an unconditional `sti`, so a call from inside an
  interrupt handler stays safe. The heap builds a 16-page (64KB) initial slab in
  `heap_init()` (called from `kernel_main` after `memory_init`) and grows on
  demand; growth requires the new run to be adjacent to `heap_end` (guaranteed
  because the heap is the sole frame consumer and the bitmap is scanned
  bottom-up), and a non-adjacent run is refused and reclaimed rather than spliced
  into the single-heap boundary-tag walk. Verified under QEMU with a temporary
  self-test (removed after): allocation with sentinel readback, free-and-reuse,
  full coalesce, and the growth path all pass, with zero page/GP faults and the
  timer and syscall vectors still firing. See
  `docs/decisions/0010-kernel-heap-ported-from-p5.md` and
  `docs/reference/heap.md`.
- Reading the real amount of RAM from the Multiboot memory map and extending the
  identity map to cover it. `boot/boot.asm` now identity-maps a fixed 32MB (up
  from 8MB, 16 2MB PD entries; 4-8M stays `PG_USER`, the rest kernel-only),
  exposes `pd_table` as `global`, and forwards the Multiboot info pointer (left in
  EBX by the bootloader) to `kernel_main` in RDI. `kernel_main` now takes
  `uint64_t multiboot_info_addr`. `kernel/multiboot.h` defines the Multiboot 1
  info and mmap-entry structures (packed; the entry stride is
  `size + sizeof(size)`, since `size` does not count itself). `kernel/memory.c`
  (`memory_detect_and_map`) walks the map for the highest usable physical address,
  fills `pd_table` from 32M up to that address (rounded to 2MB, capped at 1GB, the
  single PD page's reach), and reloads CR3 to flush the TLB. The frame pool is
  sized from the same measured RAM, and every non-usable range the map reports is
  reserved by walking the map, so `alloc_frame()` now returns real, mapped,
  writable memory. This retires the invented 8MB map and 128MB pool constants and
  the "allocator returns unmapped addresses" limitation. If the map is absent
  (flags bit 6 clear), the kernel falls back to the fixed 32MB and warns rather
  than reading garbage. The boot banner prints the detected RAM. See
  `docs/decisions/0009-read-multiboot-map-extend-identity-map.md` and
  `docs/reference/memory-map.md`.
- A round-robin preemptive scheduler (`kernel/scheduler.c`, `kernel/scheduler.h`).
  The timer IRQ handler now switches between ring-3 tasks by overwriting the
  interrupt frame on the kernel stack in place: the `registers_t` the stub pushed
  is the task's whole context, so `schedule()` saves `*r` into the current task's
  slot, picks the next `TASK_READY` task round-robin, and copies its saved frame
  back over `*r`, so the stub's `iretq` resumes a different task. `task_create`
  forges a never-run task's frame (the `enter_user_mode` trick generalised: `rip`
  = entry, `user_rsp` = stack top, `cs`/`ss` = user selectors, `rflags` = 0x202
  with IF set, GPRs zero) and marks it `TASK_READY`. Tasks live in a fixed `.bss`
  array `tasks[MAX_TASKS]` (no kernel heap yet). `scheduler_start()` enters task 0
  by reusing `enter_user_mode`; a `scheduler_running` guard plus a `cli` in
  `scheduler_start` close the startup race where early timer ticks fire in kernel
  context. EOI stays before the switch (already sent by `irq_handler`), or the
  timer line would freeze after one switch.
- A second ring-3 program in `user/user_program.c`: `user_program_a` and
  `user_program_b` each loop forever calling `SYS_WRITE` with "A"/"B" and a crude
  busy-wait delay, neither calling `SYS_EXIT`, so the scheduler's switching stays
  visible. `SYS_EXIT` stays implemented but unused.
- `TASK0_STACK_TOP` (0x700000) / `TASK1_STACK_TOP` (0x800000) in
  `kernel/scheduler.h`: the single 6-8M PG_USER stack page (PD[3]) split in half,
  one 1MB stack per task (no guard page between them).
- `docs/decisions/0008-round-robin-preemptive-scheduler.md` and
  `docs/reference/scheduling.md` documenting the frame-swap switch, forging a
  never-run task, the two traps (overwrite in place, EOI before switch), task
  states, the startup race, and the stack split.
- System calls through a single `int 0x50` gate. `kernel/isr.c` installs one
  DPL 3 IDT gate at `SYSCALL_VECTOR` (0x50) with a new `GATE_USER` (0xEE) flags
  byte; every other gate stays DPL 0, so this is the only vector ring-3 code can
  raise on purpose. `kernel/isr_stubs.asm` adds `syscall_stub` (mirrors
  `ISR_NOERR`: dummy error code + vector, then a shared tail that calls the C
  dispatcher). `kernel/syscall.c` (`syscall_handler`) switches on RAX and returns
  its result in RAX: `SYS_WRITE` prints a NUL-terminated string, `SYS_EXIT` halts;
  an unknown number is reported and rejected with `-1` rather than faulting. The
  ABI numbers live in a deliberately standalone `include/syscalls.h` (numbers
  only, no kernel code) so a ring-3 program can compile against them.
- `.user_rodata` section in `linker.ld`, placed in the `:user` `PT_LOAD` segment
  (4-8M) so the ring-3 program's string literals land in user-accessible pages
  instead of `.rodata` at 1M (kernel pages).
- `docs/decisions/0007-syscalls-via-int-0x50.md` and `docs/reference/syscalls.md`
  documenting the gate, the calling convention, the two calls, and the stopgap
  pointer check.
- Ring-3 (CPL 3) user mode. `kernel/usermode.c` (`enter_user_mode`) forges the
  five-value `iretq` frame (SS, RSP, RFLAGS, CS, RIP) and returns into ring 3
  with the user selectors (`GDT_SELECTOR_USER_CODE` 0x1B, `GDT_SELECTOR_USER_DATA`
  0x23) and IF kept set. `user/user_program.c` is a self-contained ring-3 program
  in a new `.user_text` section (it calls the kernel through `int 0x50`, see the
  syscall entry above). `kernel/kernel.c` drops to it after init. This activates
  the previously inert user GDT descriptors and `tss.rsp0`.
- `GDT_SELECTOR_*` constants in `kernel/gdt.h` (kernel/user code/data, TSS) as the
  single source of truth for selector values; `kernel/gdt.c` loads the TSS by name.
- `PG_USER` (page user/US bit) in `boot/boot.asm`. `PML4[0]`/`PDPT[0]` are made
  permissive (user bit set) and the PD leaves gate real access: `PD[0]`/`PD[1]`
  stay kernel-only, `PD[2]` (4-6M, user code) and `PD[3]` (6-8M, user stack) get
  the user bit. The PD loop is unrolled into four explicit per-region writes.
- `.user_text` section in `linker.ld` at `0x400000`, placed in its own `PT_LOAD`
  segment via a new `PHDRS` block so the 1M-to-4M gap is not padded into the file
  (`minios.bin` stays ~25KB).
- `docs/decisions/0006-user-mode-with-separate-pages.md` and
  `docs/reference/user-mode.md` documenting the ring-3 drop, the page-privilege
  layout, and how the #GP / #PF results prove it.
- `include/vectors.h`, the single source of truth for every interrupt vector
  number: all 32 named CPU exceptions, `PIC_MASTER_VECTOR_BASE` (0x40) and
  `PIC_SLAVE_VECTOR_BASE` (0x48), every IRQ vector derived from the base
  (`IRQ_TIMER` = 0x40, `IRQ_KEYBOARD` = 0x41, through `IRQ_15`), and
  `SYSCALL_VECTOR` (0x50, now the live syscall gate).
- Diagnostic exception handlers in `kernel/isr.c`: `isr_handler` now decodes the
  fault instead of printing a bare number. Page faults print the faulting CR2
  address and decoded error-code bits (read/write, present, user/kernel,
  reserved-bit, instruction fetch); general protection faults decode the
  offending selector or state plainly that the error code is zero; double faults
  explain that the error code carries no information. Every exception prints its
  vector, name, and RIP/CS/RFLAGS, then halts with `cli; hlt`.
- `docs/decisions/0005-self-describing-vector-map.md` recording the vector-map
  decision and its costs.

### Fixed

- `make` with no target now builds the kernel. It had been reporting
  `user/SHELL.ELF is up to date` and building nothing else: GNU make takes the
  first explicit target in the file as its default goal, and the explicit
  `user/SHELL.ELF:` rule sits above `all:`. Fixed by declaring
  `.DEFAULT_GOAL := all` near the top of the Makefile rather than by reordering the
  rules — rule order is what broke it, and an explicit declaration cannot be broken
  again by a rule added in the wrong place.

### Removed

- The old in-kernel shell (`shell/shell.c`, `shell/shell.h`), superseded by the
  ring-3 shell above. It ran at ring 0, was driven straight from the keyboard IRQ
  (`keyboard_callback` called `shell_handle_keypress`, doing line editing and
  command dispatch inside the interrupt), and dispatched compiled-in commands
  (`help`, `clear`, `hello`, `tick`); it was also off the boot path, since
  `kernel_main` handed control to the scheduler and never called `shell_init`. Its
  only live link was the keyboard callback, now replaced by the ring buffer, so the
  files, the `shell/shell.c` entry in `C_SOURCES`, and the `#include
  "../shell/shell.h"` are all gone. `get_tick` (`kernel/timer.c`) loses its only
  caller but stays as public API.
- The compiled-in user program path, now that programs load from disk.
  `user/user_program.c` and its entry in `C_SOURCES`, the `.user_text` and
  `.user_rodata` sections and the whole `:user` PT_LOAD segment in `linker.ld`,
  the `_user_text_start` / `_user_text_end` / `_user_rodata_start` /
  `_user_rodata_end` symbols, the `build_user_space` loop that copied from them
  into each task's frames, `task_create(uint64_t entry)` itself, and the extern
  declarations of `user_program_a`/`_b`/`_c` in `kernel_main`. `minios.bin` drops
  from 45476 to 42608 bytes and from two PT_LOAD segments to one, which is the
  proof that ring-3 code is genuinely no longer inside the kernel image rather
  than merely unreferenced. A full clean rebuild also surfaced a duplicate static
  `print_hex` in `kernel/heap.c`, identical to the one added to the screen
  driver; the duplicate is gone.

### Changed
- `task_block` returns `TASK_BLOCK_INTERRUPTED` instead of always blocking. **This
  is the subtlest interaction in the signals rung.** Delivery happens on the way out
  to ring 3 and a blocked task is not on its way anywhere, so `signal_raise` readies
  it — but the re-arm that rewinds `rip` onto the `int 0x50` does not know *why* a
  task was woken, so it would re-run its read, find its event still absent (a signal
  is not data), and park again forever with the signal undelivered. `sig_interrupted`
  breaks that loop and the syscall fails with `SYSCALL_ERROR`. Every blocking call
  honours it: the console read, both pipe directions, `SYS_READKEY`, `SYS_WAIT`.
- The shell puts every job — a pipeline and a plain `run` alike — in its own process
  group, hands the keyboard over with `sys_setfg`, and **takes it back
  unconditionally** on every exit path including the failure `goto`s; a shell that
  restored the foreground only on success would lose the keyboard the first time
  anything went wrong. It also registers a `SIG_INT` handler of its own that abandons
  the half-typed line and prints a fresh prompt, so a Ctrl-C that does reach task 0
  behaves rather than killing the machine's only shell.
- `read_line` treats `SYS_FAIL` from `sys_readkey` as "a signal ran", not as a
  character. It had been storing the `-1` as a byte, so a Ctrl-C at the prompt left
  invisible `0xFF`s in the line buffer and the next command was rejected as unknown
  for no visible reason.
- `user_range_ok` moved from `kernel/syscall.c` to `kernel/memory.h`, beside the two
  region constants it tests against, so signal delivery applies the identical check
  to the frame it writes through a ring-3 stack pointer. Two spellings of one
  security check is how one of them ends up subtly weaker.
- Regenerated the reap-line examples in `docs/reference/shell.md` and
  `user/tests/README.md` from a real boot. Adding `heap used:` to the reap line in
  the previous tranche made every document quoting one stale at a stroke, and the
  free-frame numbers they carried were already wrong against the real baseline. Both
  files now say the examples are captured output that must be regenerated whenever
  the reap line's fields change.

- `kernel_main` now creates two ring-3 tasks and starts the scheduler
  (`scheduler_start`) as its last act instead of calling `enter_user_mode`
  directly; the tasks run forever, so the machine no longer halts on `SYS_EXIT`.
  `kernel/timer.c`'s `timer_callback` now calls `schedule(regs)` on every tick
  instead of only counting. `enter_user_mode` is still used, now by
  `scheduler_start` to enter task 0. `docs/architecture.md`,
  `docs/project-status.md`, and `docs/reference/memory-map.md` updated for the two
  tasks and the split stack page.
- `memory_init` now reserves the frames covering the ring-3 region (4-8M,
  `USER_REGION_START`/`USER_REGION_END`) so the frame allocator never hands out
  memory the running user program's code or stack already occupy. This resolves
  the pool/ring-3 overlap but does not make the allocator usable: the first free
  frame is now at 8M, above the 8M identity map, so `alloc_frame()` returns an
  address that page-faults on first touch until the map is extended. Both the 8M
  identity map and the 128M pool size remain invented numbers; sizing them from
  the (currently discarded) Multiboot memory map is recorded as future work.
  `docs/reference/memory-map.md` and `docs/project-status.md` updated.
- `kernel_main` now hands off to ring 3 (`enter_user_mode`) as its last act
  instead of calling `shell_init`; the shell is still compiled and working but is
  off the boot path (the ring-3 program prints via `SYS_WRITE` and then `SYS_EXIT`
  halts the machine before the idle loop). `docs/architecture.md`,
  `docs/project-status.md`, and `docs/reference/memory-map.md` updated for the
  ring-3 region (and its overlap with the frame allocator pool at 4M).
- Moved hardware IRQs off the conventional 0x20 base to a self-describing map:
  CPU exceptions at 0x00-0x1F, hardware IRQs at 0x40-0x4F, syscalls reserved at
  0x50-0x5F, so the high nibble names the category in a fault log. `pic_remap()`
  in `kernel/idt.c` now programs ICW2 to 0x40/0x48; `kernel/isr.c` installs the
  IRQ gates off the base; `kernel/isr_stubs.asm` derives IRQ vectors from a
  `PIC_MASTER_VECTOR_BASE` `equ` kept in sync with `vectors.h` by hand.
- Fixed the End-Of-Interrupt slave-PIC check in `kernel/isr.c` to compare against
  `PIC_SLAVE_VECTOR_BASE` instead of a hardcoded 40, which would otherwise stop
  acknowledging IRQ 8 to 15 (now at 0x48-0x4F).
- `kernel/timer.c` and `drivers/keyboard.c` register against `IRQ_TIMER` /
  `IRQ_KEYBOARD` from `vectors.h`, dropping their local `IRQ0_INTERRUPT` /
  `IRQ1_INTERRUPT` defines.
- `docs/reference/idt.md` updated for the new vector map and the `vectors.h`
  pointer; `learnings/03-interrupts.md` carries a note that its 32-47 numbering
  is superseded, pointing at ADR 0005.

## [0.1.0] - 2026-07-17

First booting version. MiniOS builds, links, and boots to an interactive shell
under QEMU, with the timer and keyboard driving interrupts.

### Added

- 64-bit IDT install in `kernel/idt.c`: `idt_set_entry` packs a 64-bit handler
  address across `offset_low`/`offset_mid`/`offset_high`, sets selector `0x08`,
  IST 0, and gate flags `0x8E` (present, DPL 0, interrupt gate), and `idt_init`
  loads the register with `lidt`.
- Interrupt entry points in `kernel/isr_stubs.asm`: `isr0`-`isr31` and
  `irq0`-`irq15`, with the `ISR_NOERR`/`ISR_ERR` macros normalising the error-code
  frame (real error code on vectors 8, 10, 11, 12, 13, 14, 17; dummy 0 elsewhere)
  and the common stubs that save the 15 general-purpose registers, pass a
  `registers_t*` in `RDI`, call the C dispatcher, and return with `iretq`.
- Boot path fix in the `Makefile`: link to `minios.elf` (ELF64, keeps gdb
  symbols), then repackage with `x86_64-elf-objcopy -O elf32-i386` into
  `minios.bin`, which QEMU's Multiboot `-kernel` loader accepts.
- `docs/reference/idt.md` documenting the IDT and interrupt entry path, and
  `docs/project-status.md` recording what works, what was never built, and the
  next steps.
- 32 to 64 long-mode climb in `boot/boot.asm`: three-level 2MB-page identity map
  of the first 8MB, PAE, `CR3`, EFER.LME, paging enable, a bootstrap GDT, and the
  far jump into 64-bit code that calls `kernel_main`.
- Kernel GDT and 64-bit TSS in `kernel/gdt.c`, `kernel/gdt.h`, and
  `kernel/gdt_flush.asm`: a 7-slot GDT (null, kernel code/data, user code/data,
  16-byte TSS descriptor), a dedicated ring-0 stack, and the `retfq` CS reload.
- x86-64 cross toolchain installed and pinned in the build docs
  (`x86_64-elf-gcc` 16.1.0, `x86_64-elf-binutils` 2.46.1, `nasm` 3.01,
  `qemu-system-x86_64` 11.0.0).
- Project documentation under `docs/`: architecture, building, reference pages
  (boot sequence, memory map, GDT/TSS), and architecture decision records.
- Root `CHANGELOG.md`.

### Changed

- Migrated the codebase from 32-bit i686 to x86-64: `Makefile` toolchain and
  flags (`-m64 -mno-red-zone -mcmodel=kernel`, `nasm -f elf64`), `include/types.h`
  (added 64-bit types, `size_t` widened), `linker.ld` (`elf64-x86-64` output),
  `kernel/memory.c` frame addresses widened to 64-bit, and the `registers_t` /
  `idt_entry_t` layouts updated to their 64-bit forms.
- Reorganized documentation: the former `docs/` teaching chapters and `learning.md`
  moved to `learnings/` (conceptual material), and `docs/` now holds factual
  project documentation. The two are kept strictly separate.
- Root `README.md` rewritten as a short entry point that signposts `docs/` and
  `learnings/` instead of embedding the full guide.
- `learnings/` chapters 1, 2, 3, and 5 carry a note that they describe the
  original 32-bit design, each linking to the current x86-64 reference page; the
  index states that `docs/` is the current source of truth.

### Removed

- `CONVERSION_NOTES.md` and `BOOT_NOTES.md`: ad-hoc notes whose content was
  absorbed into `docs/` (toolchain into `docs/building.md`, boot climb into
  `docs/reference/boot-sequence.md`, GDT/TSS into `docs/reference/gdt.md`), the
  ADRs, and this changelog.
