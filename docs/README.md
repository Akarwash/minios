# TownOS project documentation

This is the factual documentation for TownOS: what it is, how it is put together,
how to build and run it, and why the load-bearing decisions were made. It is
derived from the source, not from concepts. For the conceptual "how operating
systems work" material, see [`../learnings/`](../learnings/README.md) instead.
The two are kept separate on purpose: `docs/` states facts about this codebase,
`learnings/` teaches ideas.

## A note on the name

The project was renamed from **MiniOS** to **TownOS**. That rename is deliberately
finished in one direction only, and this note is here so the next person does not
"finish" it the wrong way.

**Live documentation carries the new name.** Everything whose job is to describe the
project as it is now — this `docs/` tree, the top-level `README.md`, the code
comments, and the strings the kernel prints — says TownOS, and should be corrected
to TownOS the day any of it goes stale.

**The records still say MiniOS, on purpose.** The architecture decision records in
[`decisions/`](decisions/) and the entries in [`../CHANGELOG.md`](../CHANGELOG.md)
keep the old name. Their whole value is in being a record of a moment: an ADR says
what was decided *then*, under the name the project carried *then*, and a changelog
entry says what shipped in a given release. Rewrite either to say TownOS and it now
claims to have been written about a project that did not yet have that name — it
forges the past into agreement with the present, which is the exact failure
[`decisions/README.md`](decisions/README.md) freezes ADR bodies to avoid. A record
that reads MiniOS under a dated heading is honest and datable; one retrofitted to
TownOS is a claim nobody can check.

So the split is not an unfinished chore. Correct the live pages forward; leave every
ADR body and changelog entry exactly as it was written.

## Pages

| Page | What it covers |
|------|----------------|
| [architecture.md](architecture.md) | What TownOS is, the directory layout, the subsystem map, and control flow from `_start` to the event loop |
| [building.md](building.md) | Toolchain and versions, install commands, how to build and run, and how to debug |
| [reference/boot-sequence.md](reference/boot-sequence.md) | The 32 to 64 long-mode climb in `boot/boot.asm`, step by step |
| [reference/memory-map.md](reference/memory-map.md) | Physical memory layout: load address, VGA buffer, identity-mapped region, page tables, stacks |
| [reference/paging.md](reference/paging.md) | Per-process paging: the private-user/shared-kernel two-halves model, the by-value kernel clone, the CR3 switch, and the frozen-mappings invariant |
| [reference/gdt.md](reference/gdt.md) | The kernel GDT and TSS: selector table, descriptor layouts, and the bootstrap-vs-kernel GDT split |
| [reference/idt.md](reference/idt.md) | The IDT and interrupt entry path: gate format, PIC remap, the 48 stubs, dispatch, and EOI |
| [reference/user-mode.md](reference/user-mode.md) | The drop to ring 3: the forged `iretq` frame, the ring-3 selectors and stack, the user bit ANDed down the page walk, and how the isolation is proven |
| [reference/syscalls.md](reference/syscalls.md) | The `int 0x50` syscall gate: the single DPL 3 doorway, the register calling convention, the twenty-one calls, and the untrusted-pointer checks |
| [reference/descriptors.md](reference/descriptors.md) | File descriptors: the per-task table, the console/pipe kinds, the 0-input/1-output convention, and the read/write/close/pipe calls |
| [reference/pipes.md](reference/pipes.md) | Pipes: the ring buffer, the reader/writer counts, the block/wake rules, and how end-of-file works (why empty is not EOF, and why closing wakes) |
| [reference/signals.md](reference/signals.md) | Signals: raising vs delivering, the forged handler frame and the trampoline, `sigreturn`, process groups and the declared foreground, Ctrl-C and Ctrl-D |
| [reference/scheduling.md](reference/scheduling.md) | The round-robin preemptive scheduler: the interrupt frame as the task, the forged frame for a never-run task, the in-place frame overwrite and CR3 load, the task states, the zombie sweeper, and the startup race |
| [reference/heap.md](reference/heap.md) | The kernel heap: header/footer boundary tags, split/coalesce, the frame-allocator seam, and interrupt safety |
| [reference/user-memory.md](reference/user-memory.md) | User memory and the ring-3 libc: the heap slot, `SYS_MMAP`/`SYS_MUNMAP` and the per-task region table, mapping and unmapping with the unwind and the TLB flush, freeing at exit, the second port of the allocator as `malloc`, `printf`, and how `libc/` reaches ring 3 |
| [reference/disk.md](reference/disk.md) | The polled ATA PIO disk driver: the 512-byte block model, the port layout, the read and write flows, and the driver-vs-filesystem layering |
| [reference/fat32.md](reference/fat32.md) | The read/write FAT32 filesystem: the on-disk layout, the boot sector fields, cluster-to-block arithmetic, FAT chains and the 28-bit mask, directory entries and 8.3 names, the read path, and the write path (chain alloc/free, directory growth, both FAT copies, the commit ordering, FSInfo) |
| [reference/elf-loading.md](reference/elf-loading.md) | The ELF64 program loader: the manifest, the header and program header fields used, validation and the segment bounds check, the load loop and the zero-fill, and the separate user build |
| [reference/shell.md](reference/shell.md) | The interactive shell (a ring-3 program): the read-match-do loop, the keyboard ring buffer, the shell syscalls and their pointer checks, the tokenizer, and the command table |
| [reference/blocking.md](reference/blocking.md) | Blocking and sleep: the blocked state and wait reason, the syscall re-arm that makes a block possible on one shared kernel stack, the `hlt` idle path, and the block/wake pairing rule |
| [reference/keyboard.md](reference/keyboard.md) | The PS/2 keyboard driver: scancode set 1 and the release bit, the two translation tables, shift and caps-lock state and the XOR that combines them, the ring buffer, the `WAIT_KEY` wake, and the extended-scancode gap |
| [project-status.md](project-status.md) | What works, what was never built, and the natural next steps |
| [decisions/](decisions/) | Architecture decision records (ADRs) for the load-bearing choices |

## Decisions

- [0001 — Target x86-64 rather than i686](decisions/0001-target-x86-64.md) — Build for 64-bit long mode from the start rather than 32-bit i686.
- [0002 — Use 2MB pages and identity-map the first 8MB](decisions/0002-2mb-pages-and-8mb-identity-map.md) — Identity-map the first 8MB with 2MB pages, so only three page-table levels are needed (no PT).
- [0003 — Bootstrap GDT in boot.asm is separate from the kernel GDT](decisions/0003-bootstrap-gdt-separate-from-kernel-gdt.md) — A throwaway bootstrap GDT makes the far jump legal; C installs the real kernel GDT later, and the two stay separate.
- [0004 — Build the TSS now, before user mode exists](decisions/0004-build-tss-before-user-mode.md) — Build the TSS (descriptor, ring-0 stack, `ltr`) now, before user mode needs it.
- [0005 — A self-describing interrupt vector map](decisions/0005-self-describing-vector-map.md) — Assign vectors by category (exceptions 0x00-0x1F, IRQs 0x40-0x4F, syscalls 0x50-0x5F), with `include/vectors.h` the single source of truth.
- [0006 — Enter ring 3 with a separate user page region](decisions/0006-user-mode-with-separate-pages.md) — Drop to ring 3 through a forged `iretq` frame, marking the 4-8M pages user while kernel pages stay ring-0-only.
- [0007 — System calls via a single `int 0x50` gate](decisions/0007-syscalls-via-int-0x50.md) — Route every syscall through one DPL 3 `int 0x50` gate (the only user-reachable gate); RAX carries the call number.
- [0008 — A round-robin preemptive scheduler](decisions/0008-round-robin-preemptive-scheduler.md) — Preempt on the timer tick by overwriting the interrupt frame in place, with a fixed `.bss` task table of four.
- [0009 — Read the Multiboot memory map and extend the identity map](decisions/0009-read-multiboot-map-extend-identity-map.md) — Read the Multiboot map, extend the identity map to real RAM (capped at 1GB), and size the frame pool from it.
- [0010 — Port the p5 explicit-free-list allocator as the kernel heap](decisions/0010-kernel-heap-ported-from-p5.md) — Port the CMSC216 p5 `el_malloc` (boundary tags, coalescing) as `kmalloc`/`kfree`, swapping `mmap` for `alloc_frames_contiguous` and adding an interrupt guard.
- [0011 — Heap-allocate task structs and bump-allocate user stacks](decisions/0011-dynamic-tasks-and-stacks.md) — Retire the fixed four-task array (`kmalloc` each `task_t`) and the two hardcoded stacks (bump-allocate 256KB slices of the user region); the stack ceiling remains until per-process paging.
- [0012 — Per-process paging: a private address space per task](decisions/0012-per-process-paging.md) — Give each task its own page-table tree (private 4KB user half, kernel half cloned by value), switch CR3 on context switch, so tasks share virtual addresses but not physical memory.
- [0013 — A polled ATA PIO disk driver](decisions/0013-ata-pio-disk-driver.md) — Read and write 512-byte LBA28 blocks on the primary ATA bus by polling (no interrupts, no DMA); the simplest correct block device, which freezes the machine during a transfer and unblocks a filesystem.
- [0014 — A read-only FAT32 filesystem](decisions/0014-read-only-fat32.md) — Give the raw blocks names: parse the boot sector, follow FAT cluster chains, and read a file by 8.3 name, read-only (first FAT copy, root directory, no long filenames), with the image formatted by the host build system.
- [0015 — Load programs from disk as ELF64 binaries](decisions/0015-elf-program-loading.md) — User programs become separately linked static ELF64 files on the FAT32 image, loaded at runtime by an in-kernel loader that validates every field and bounds-checks every segment address; changing a program no longer means rebuilding the kernel.
- [0016 — An interactive shell as a ring-3 program](decisions/0016-interactive-shell.md) — The shell becomes a fenced-in ring-3 program (`SHELL.ELF`) that reads commands and runs them using four new syscalls (`SYS_READKEY`, `SYS_LIST`, `SYS_RUN`, `SYS_READFILE`) and a keyboard ring buffer, proving the syscall boundary is complete; the old in-kernel shell is removed.
- [0017 — Blocking and sleep, by re-arming the syscall](decisions/0017-blocking-and-sleep.md) — Give a task a blocked state and a wait reason, skip blocked tasks in the rotation, and let a task sleep at a syscall boundary by rewinding its saved `rip` onto the `int 0x50` so waking re-issues the call; an idle shell drops from 362,648 syscalls per six seconds to three.
- [0018 — Process lifecycle: exit, wait, and two-phase death](decisions/0018-process-lifecycle-exit-and-wait.md) — Let a task end: `SYS_EXIT` takes a status and does paperwork only (mark `TASK_ZOMBIE`, wake the parent), a sweeper in `schedule()` frees the address space of any zombie that is not the running task, and the parent frees the tombstone at `SYS_WAIT`; `run` now waits and reports an exit status.
- [0019 — Keyboard modifier state in the driver](decisions/0019-keyboard-modifier-state-in-the-driver.md) — Track shift and caps lock in `drivers/keyboard.c` and keep resolved ASCII in the ring buffer, rather than pushing raw scancodes for ring 3 to decode; uppercase and the shifted symbols become typeable, and a modifier press pushes nothing and wakes nobody.
- [0020 — A writable FAT32 filesystem](decisions/0020-writable-fat32.md) — Make `fs/fat32.c` writable: whole-file writes with no handles, replace-not-overwrite with a strict write-before-publish ordering (new chain and data first, one directory-entry write as the commit point, old chain freed last), delete in the same rung, every FAT copy written, FSInfo invalidated rather than maintained, and non-8.3 names rejected rather than mangled.
- [0021 — Expose `fat32_stat` to ring 3 as `SYS_STAT`](decisions/0021-sys-stat.md) — Add an eleventh syscall that reports a file's size without reading it, so the shell's `read` can stat first and tell a missing file (`read: no such file: X`) from one too big (`read: X is N bytes, the buffer holds M`) from a disk error; removes the unreachable truncation notice, and takes no partial reads and no offset (that would change `SYS_READFILE`).
- [0023 — Signals](decisions/0023-signals.md) — An interrupt one layer up, with no hardware behind it: a pending set, delivery only on the way out to ring 3, default actions and catchable handlers with a hand-forged call frame and a program-supplied trampoline, process groups so Ctrl-C addresses a job, a declared (never inferred) foreground, `SYS_KILL`/`SYS_TASKS` for tasks the keyboard cannot reach, `SIGPIPE`, and Ctrl-D. Includes a catalogue of the eight ways this goes wrong (S1–S8), six of them silently.
- [0022 — File descriptors and pipes](decisions/0022-file-descriptors-and-pipes.md) — Give every task a fixed table of open destinations, make pipes one kind of entry in it, reshape `SYS_WRITE` to `(fd, buf, len)` and add `SYS_READ`/`SYS_CLOSE`/`SYS_PIPE`, pass ends to children through `SYS_RUN`, and teach the shell `|`. Includes the EOF argument (empty is not finished; closing must wake) and a catalogue of the six silent ways pipes hang or corrupt (B1–B6).
- [0024 — User memory and a fuller libc](decisions/0024-user-memory-and-libc.md) — Anonymous, eager `SYS_MMAP`/`SYS_MUNMAP` into a 2MB heap slot at `PD[4]` (which was the identity map of physical 8-10M, now reserved), a fixed array of eight regions per task, the kernel heap ported a second time as a ring-3 `malloc` on top, a six-specifier `printf` whose real defence is the compiler's format attribute, `libc/` compiled twice so it reaches ring 3, and the six ways this goes wrong (M1–M6), four of them silently. Phase 1 ends here.

## Status

TownOS **builds, links, and boots to an interactive shell** under QEMU, reads and
writes files by name on a FAT32 disk, and loads and runs its ring-3 programs from
that disk as ELF64 binaries. The shell itself is one of those ring-3 programs.

- All C sources compile cleanly under `-Wall -Wextra`.
- All assembly sources assemble cleanly with `nasm -f elf64`.
- The kernel links into `townos.elf` and is repackaged as a Multiboot-loadable
  `townos.bin`. `make run` boots it under QEMU: the banner appears, the timer ticks
  on IRQ 0, the keyboard delivers keypresses on IRQ 1, and `SHELL.ELF` runs at
  ring 3, dispatching `list`, `read`, `write`, `delete`, `free`, `run`, `help`,
  `clear`, `return`, `ps` and `kill` through the syscall gate, and printing with
  `printf`. A program can `malloc` at run time. With nobody typing, the shell
  sleeps and the CPU halts between timer ticks rather than spinning.

The full feature list, the things that are still deliberately absent (argv, dynamic
linking, demand paging, subdirectories), and the natural next steps are in
[project-status.md](project-status.md).

For the exact build, run, and debug commands, see [building.md](building.md).
