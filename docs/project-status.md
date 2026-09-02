# Project status

TownOS is a learning kernel: it boots x86-64 long mode, drops to ring 3, and
preempts between ring-3 tasks on the timer tick, each in its own address space
(per-process paging). It reads and writes files on a FAT32 disk by name, and its
ring-3 programs are ELF64 binaries loaded from that disk rather than code compiled
into the kernel. It boots into an interactive shell that is itself one of those ring-3
programs (`SHELL.ELF`): it reads typed commands and runs them using nothing but
syscalls, which is what proves the syscall boundary is complete. This page records
what works today, what was deliberately never built, the natural next steps, and
the known limitations. It is a factual snapshot, not a roadmap.

## What works

- The 32 to 64 long-mode climb in `boot/boot.asm`: 2MB-page identity map of the
  first 8MB, PAE, EFER.LME, paging, a bootstrap GDT, and the far jump into 64-bit
  code that calls `kernel_main`.
- The kernel GDT and 64-bit TSS (`kernel/gdt.c`, `kernel/gdt_flush.asm`).
- The IDT and the full interrupt path: 256-entry table, PIC remap to the
  self-describing vector map (hardware IRQs at 0x40-0x4F, every vector defined in
  `include/vectors.h`), all 48 entry points in `kernel/isr_stubs.asm`, and the C
  dispatch in `kernel/isr.c`. Exceptions are decoded into plain-English
  diagnostics (page-fault CR2 and error-code bits, GP-fault selector, double
  fault) rather than a bare vector number. See [reference/idt.md](reference/idt.md)
  and [decisions/0005-self-describing-vector-map.md](decisions/0005-self-describing-vector-map.md).
- The PIT timer on IRQ 0 (`kernel/timer.c`) and the PS/2 keyboard on IRQ 1
  (`drivers/keyboard.c`).
- VGA text output with scrolling and a cursor (`drivers/screen.c`).
- A bitmap physical frame allocator (`kernel/memory.c`), sized from the real RAM
  the Multiboot map reports (identity map extended to cover it, up to a 1GB cap),
  so it hands out real, mapped frames. See
  [reference/memory-map.md](reference/memory-map.md).
- A small freestanding libc (`libc/`), compiled twice: into the kernel and, with
  the user flags, into every ring-3 program. `strlen`, `strcmp`, `strcpy`,
  `strchr`, `memcpy`, `memmove`, `memset`, `memcmp`, and for ring 3 only
  `malloc`/`free`/`calloc` (`libc/malloc.c`) and `printf` (`libc/printf.c`). See
  [reference/user-memory.md](reference/user-memory.md).
- A polled ATA PIO disk driver (`drivers/disk.c`): `disk_read` and `disk_write`
  move any run of contiguous 512-byte blocks between a disk and a buffer on the
  primary ATA bus, addressed by LBA28. It polls the status port (no interrupts,
  no DMA) with bounded poll loops that time out rather than hang, and sets nIEN so
  the drive never raises IRQ14. `make run` attaches a 64MB raw `disk.img`. This is
  a raw block device, not a filesystem: it moves the exact block it is told to and
  has no names, files, or free-space tracking. A transfer freezes the machine (the
  scheduler cannot preempt mid-transfer), an accepted limitation of polled PIO. See
  [reference/disk.md](reference/disk.md) and
  [decisions/0013-ata-pio-disk-driver.md](decisions/0013-ata-pio-disk-driver.md).
- A read/write FAT32 filesystem (`fs/fat32.c`): `fat32_init` parses the boot
  sector and caches the volume geometry, `fat32_list_root` lists the root
  directory, and `fat32_read_file` reads a file by 8.3 name into a caller's
  buffer, following its cluster chain and trimming the last cluster to the size
  in the directory entry. `fat32_write_file` creates or wholly replaces a file
  (whole-file writes, no handles), `fat32_delete` removes one, and
  `fat32_free_count` reports free clusters. Writing is crash-safe by ordering:
  a new chain and its data are laid down first, a single directory-entry write is
  the commit point, and the old chain is freed only after — so a crash midway
  loses only unreferenced clusters and never damages the file on disk. Every FAT
  copy is written (only the first is read), the root directory grows when it fills,
  and FSInfo is invalidated rather than maintained. Still 8.3 names only (long
  names rejected, not mangled), the first FAT copy for reads, the root directory
  only, and no timestamps or subdirectory creation. The image is first formatted by
  the host build system (`tools/mkdisk.sh`, mtools), then read and written by the
  kernel. `fat32_stat` reports a file's size without reading it, now reachable from
  ring 3 as `SYS_STAT` so the shell's `read` can size a buffer first
  ([decisions/0021-sys-stat.md](decisions/0021-sys-stat.md)). The ELF loader
  reads every program file through this layer, so the filesystem is on the boot
  path. See [reference/fat32.md](reference/fat32.md),
  [decisions/0014-read-only-fat32.md](decisions/0014-read-only-fat32.md) (the
  original read-only scope) and
  [decisions/0020-writable-fat32.md](decisions/0020-writable-fat32.md) (writing).
- An ELF64 program loader (`kernel/elf.c`): user programs are separately
  compiled, statically linked ELF64 binaries (`user/shell.c` and the test
  fixtures in `user/tests/`, all linked with `user/user.ld` at 0x400000) that
  live on the FAT32 image and are read,
  validated, and loaded at runtime. `task_create_from_file` builds a private
  address space, maps each `PT_LOAD` segment into it (bounds-checked, with flags
  from the segment so text is mapped read-only), copies the file bytes, zeroes
  from file size up to memory size, and forges the task's frame with the entry
  point from the ELF header. Nothing ring-3 remains in `townos.bin`: changing a
  program means rebuilding one binary and copying it onto the image, with no
  kernel rebuild. A missing or malformed file is reported and skipped, costing
  only its own task. See [reference/elf-loading.md](reference/elf-loading.md) and
  [decisions/0015-elf-program-loading.md](decisions/0015-elf-program-loading.md).
- A drop to ring 3 (`kernel/usermode.c`): after init, `kernel_main` forges an
  `iretq` frame and runs a program at CPL 3 in its own user-accessible pages
  (code at 4M, stack at 6-8M), while the kernel's own pages stay ring-0-only. This activates the previously inert user GDT
  descriptors and `tss.rsp0`. See [reference/user-mode.md](reference/user-mode.md)
  and [decisions/0006-user-mode-with-separate-pages.md](decisions/0006-user-mode-with-separate-pages.md).
- System calls (`kernel/syscall.c`, `include/syscalls.h`): the ring-3 programs
  call back into the kernel through one `int 0x50` gate, the only DPL 3 gate in
  the IDT. Twenty-one calls: `SYS_WRITE` writes a counted buffer to a descriptor;
  `SYS_EXIT` ends the calling
  task with a status; `SYS_READKEY`
  pops one key from the keyboard ring buffer, sleeping the caller until one
  arrives; `SYS_LIST` writes the
  root directory's names into a caller buffer; `SYS_RUN` loads and starts a named
  program (giving it descriptors as fd 0/1); `SYS_READFILE` reads a whole file into
  a caller buffer; `SYS_WAIT`
  blocks until any child exits and returns its status; `SYS_WRITEFILE` writes a
  whole file; `SYS_DELETE` removes one; `SYS_FREECOUNT` reports the free-cluster
  count (so the shell's `free` command and the leak test can watch it);
  `SYS_STAT` reports a file's size without reading it, so the shell's `read` can
  size a buffer first and tell a missing file from one too big for it; and
  `SYS_READ`, `SYS_CLOSE`, `SYS_PIPE` are the descriptor calls (read from a fd,
  close one, make a pipe); `SYS_SIGNAL`, `SYS_KILL`, `SYS_SIGRETURN`, `SYS_SETFG`
  and `SYS_TASKS` are the signal calls; and `SYS_MMAP`/`SYS_MUNMAP` give a program
  anonymous memory in its heap slot. The
  dispatcher switches on RAX and returns its result in RAX; an unknown number is
  rejected, not fatal. Every pointer a call takes bounds the whole
  `[ptr, ptr+len)` range with `user_range_ok` (write-target pointers included,
  and the range now reaches the top of the heap slot) and
  caps copied filenames with `copy_user_string`; `SYS_WRITE` was a start-only
  stopgap until it became a counted `(fd, buf, len)` call. See
  [reference/syscalls.md](reference/syscalls.md),
  [reference/descriptors.md](reference/descriptors.md),
  [reference/pipes.md](reference/pipes.md),
  [decisions/0007-syscalls-via-int-0x50.md](decisions/0007-syscalls-via-int-0x50.md),
  [decisions/0020-writable-fat32.md](decisions/0020-writable-fat32.md),
  [decisions/0021-sys-stat.md](decisions/0021-sys-stat.md), and
  [decisions/0022-file-descriptors-and-pipes.md](decisions/0022-file-descriptors-and-pipes.md).
- An interactive shell (`user/shell.c`, booted as `SHELL.ELF`): a ring-3 program,
  loaded off the disk like any other, that reads typed commands and runs them using
  only syscalls. It reads a line a key at a time through `SYS_READKEY` (blocking, so
  it costs nothing while the user is thinking, echoing, with backspace), tokenizes
  it in place with a reentrant `next_token`, and
  dispatches the custom commands `list`, `read`, `write`, `delete`, `free`, `run`,
  `help`, `clear`, and `return` (deliberately not the Unix names). It also understands
  the `|` operator: `run A | run B | run C` runs the stages at once, connects each
  stage's output to the next through a pipe, and reports the last stage's exit status,
  closing its own copies of the pipe ends immediately so the stream ends rather than
  hangs ([reference/shell.md](reference/shell.md),
  [decisions/0022-file-descriptors-and-pipes.md](decisions/0022-file-descriptors-and-pipes.md)).
  The keyboard IRQ was reduced to a
  producer that only pushes a decoded character into a 128-slot ring buffer; the old
  in-kernel shell (`shell/shell.c`) is deleted. That a fully fenced-in program runs
  an interactive shell is the proof the syscall boundary is complete. See
  [reference/shell.md](reference/shell.md) and
  [decisions/0016-interactive-shell.md](decisions/0016-interactive-shell.md).
- A round-robin preemptive scheduler (`kernel/scheduler.c`): the timer tick
  switches between ring-3 tasks by overwriting the interrupt
  frame on the kernel stack in place and loading the next task's CR3, so the
  stub's `iretq` resumes a different task in its own address space.
  `task_create_from_file` `kmalloc`s a `task_t`, builds its private address
  space, loads a program file into it, and forges it as a never-run task (the
  ring-3 drop generalised); a pointer array tracks the heap-allocated tasks. The
  shell and whatever it has launched interleave on screen, and `run d.elf` puts
  three ring-3 tasks in the rotation at once. The old fixed four-task ceiling and
  the shared user-stack region are both gone. See
  [reference/scheduling.md](reference/scheduling.md),
  [decisions/0008-round-robin-preemptive-scheduler.md](decisions/0008-round-robin-preemptive-scheduler.md),
  and [decisions/0011-dynamic-tasks-and-stacks.md](decisions/0011-dynamic-tasks-and-stacks.md).
- Blocking and sleep (`kernel/scheduler.c`): a task with nothing to do leaves the
  rotation instead of spinning. `TASK_BLOCKED` plus a `wait_reason_t` takes it out
  of the round-robin walk; `task_block` puts it to sleep at a syscall boundary by
  rewinding its saved `rip` onto the `int 0x50`, so waking re-issues the call
  (there is no per-task kernel stack to resume into); `scheduler_wake(reason)`
  readies the sleepers and is called by whatever causes the event, today the
  keyboard IRQ. With nothing runnable, `schedule()` halts the CPU rather than
  spinning. An idle shell went from 362,648 syscalls per six seconds to three. See
  [reference/blocking.md](reference/blocking.md) and
  [decisions/0017-blocking-and-sleep.md](decisions/0017-blocking-and-sleep.md).
- Process lifecycle (`kernel/scheduler.c`, `kernel/paging.c`): a task can end.
  `SYS_EXIT` takes a status (masked to 0..255) and does paperwork only, marking the
  task `TASK_ZOMBIE` and waking its parent, because the task calling it is the one
  whose stack and CR3 the machine is standing on. Cleanup is split: a sweeper at the
  top of `schedule()` tears down the address space of any zombie that is not the
  running task (`paging_destroy_address_space`, which frees the user half only), and
  the parent frees the tombstone when it collects the status through the new
  `SYS_WAIT`. Freed slots become NULL holes, `num_tasks` is a high water mark, and
  ids are never reused. The shell's `run` now waits and reports
  `run: A.ELF exited with status 0`. See
  [reference/scheduling.md](reference/scheduling.md) and
  [decisions/0018-process-lifecycle-exit-and-wait.md](decisions/0018-process-lifecycle-exit-and-wait.md).
- Per-process paging (`kernel/paging.c`): each task has its own page-table tree,
  loaded into CR3 on every context switch, so two tasks use the same virtual
  addresses (code `0x400000`, stack top `0x800000`) backed by different physical
  frames. Each tree has a private 4KB user half (the program's own loaded
  segments and a fresh stack) and a kernel half cloned from the boot tables by value,
  kernel-only, so the kernel is mapped in every tree (interrupts land in mapped
  kernel code without a CR3 change). This is real address-space isolation: a stray
  pointer faults instead of corrupting a neighbour. The by-value kernel clone rests
  on kernel mappings being frozen after boot (a documented tripwire). See
  [reference/paging.md](reference/paging.md) and
  [decisions/0012-per-process-paging.md](decisions/0012-per-process-paging.md).
- A kernel heap (`kernel/heap.c`), `kmalloc`/`kfree`: an explicit free list with
  boundary tags and coalescing, ported from the CMSC216 p5 `el_malloc`. It draws
  its slab from `alloc_frames_contiguous` (a new multi-page frame helper in
  `kernel/memory.c`), grows on demand, and guards its critical section with a
  save-and-restore interrupt disable so the timer IRQ cannot corrupt the free
  list mid-relink. It has real callers on the boot path: the heap-allocated
  `task_t` structs and the per-process `address_space_t` handles. (`SYS_MMAP`
  sits beside it on the frame allocator directly, not on this heap; the ring-3
  `malloc` is this same allocator ported a second time.) See
  [reference/heap.md](reference/heap.md) and
  [decisions/0010-kernel-heap-ported-from-p5.md](decisions/0010-kernel-heap-ported-from-p5.md).
- User memory and a ring-3 libc (`kernel/syscall.c`, `kernel/paging.c`, `libc/`):
  a program can ask for memory at run time. `SYS_MMAP` maps fresh, zeroed,
  page-granular regions into a 2MB heap slot at `PD[4]`, placed above every region
  the task still holds and tracked in a fixed array of eight; `SYS_MUNMAP` releases
  an exact region and nothing else; a failed map unwinds what it mapped; the
  teardown frees the slot with the rest of the tree, so ten runs of a program that
  never frees its slabs leave the free frame count flat. `malloc`/`free`/`calloc`
  are `kernel/heap.c` ported again on top of it, and `printf` (`%d %u %x %s %c
  %%`) replaced every hand-rolled number printer in `user/`. `PD[4]` turned out
  to be the identity map of physical 8-10M, which is now reserved from the frame
  pool. See [reference/user-memory.md](reference/user-memory.md) and
  [decisions/0024-user-memory-and-libc.md](decisions/0024-user-memory-and-libc.md).

The kernel builds, links into `townos.elf`, is repackaged as `townos.bin`, and
boots under QEMU. In the current build `kernel_main` hands off to the scheduler as
its last act: it creates one ring-3 task from `SHELL.ELF` and enters it, and the
shell then runs the machine, launching further tasks on demand with `run`. Verified
under QEMU by a scripted key session: the prompt appears at boot, `help`/`list`/
`read`/`return`/`run`/unknown-command all behave, `run A.ELF` starts A.ELF whose
output interleaves with the live prompt, and `-d int` over the session shows only
timer, keyboard, and syscall vectors with no page fault, `#GP`, double fault, triple
fault, or disk IRQ. Left alone at the prompt the shell blocks in `SYS_READKEY` and
the machine sits in `hlt` between timer ticks, servicing three syscalls over six
idle seconds. See [building.md](building.md).

## What was never built

These are absent by design. TownOS now loads programs from files and gives them a
parent, an exit status, and a cleanup path, but they are still not processes in the
Unix sense: nothing can be passed in on the way in, and nothing can be stopped once
it is running.

- **Processes.** A program can now be started by another (`SYS_RUN`), can end with
  a status (`SYS_EXIT`), and is waited on and cleaned up (`SYS_WAIT` and the
  scheduler's sweeper). What is still missing is the rest of the model: there is no
  `fork` or `exec`, so a program cannot replace its own image or duplicate itself;
  `SYS_WAIT` is any-child rather than `waitpid`; and orphans are discarded rather
  than reparented, because there is no `init`. **Signals now exist**, so a program
  that will not finish can be stopped — see below.
- **Arguments, dynamic linking, relocation, and demand paging.** A program is
  entered with an empty stack and no argv. It must be `ET_EXEC` linked at the
  fixed 0x400000, since the loader resolves and relocates nothing, so two
  programs cannot be placed at different addresses and nothing can be shared as
  a library. The whole file is read and every segment fully populated before the
  first instruction runs. The loader also allocates fresh frames per task, so N
  tasks running the same program hold N physical copies of its read-only text; the
  cost scales with how many are running, and nothing today caps that number
  (`TODO(shared-text)`). See
  [decisions/0015-elf-program-loading.md](decisions/0015-elf-program-loading.md).
- **Demand paging, copy-on-write, and swap.** Per-process paging exists, but every
  page is mapped eagerly, at `task_create_from_file` for code and stack and at
  `SYS_MMAP` for the heap, and backed by real frames. There is no
  lazy allocation on fault, no copy-on-write sharing (the read-only user text is
  copied in full per task rather than shared, `TODO(shared-text)`), and no paging
  to disk.
- **A disk file behind a descriptor, seek, append, subdirectories, and timestamps.**
  There is now a descriptor table and pipes sit in it
  ([reference/descriptors.md](reference/descriptors.md)), but a **disk file** cannot:
  a write is still the whole file at once, so there is no `open`, no `seek`, no file
  redirect (`> OUT.TXT`), and no way to change part of a file without rewriting all of
  it (`fat32_write_file` mirrors `fat32_read_file`). Redirect is the piece that wants
  streaming file writes, which is why it is a later rung than pipes
  ([decisions/0022-file-descriptors-and-pipes.md](decisions/0022-file-descriptors-and-pipes.md)).
  The root directory can grow, but there is no `mkdir` and no `.`/`..`, so subdirectory
  creation is still absent; and a written entry's date and time fields are left zero,
  since TownOS keeps no clock. See
  [decisions/0020-writable-fat32.md](decisions/0020-writable-fat32.md).
- **Long filenames, paths, and permissions.** Long-filename directory entries are
  skipped, so a file with a long name is invisible to TownOS, and a name that will
  not fit 8.3 is rejected on write rather than mangled into a numbered alias;
  lookups are root-directory only, since the interface takes a bare name with no
  path to split; and FAT32 carries essentially no permissions, and no crash safety
  beyond the write ordering that keeps a crash from corrupting a live file.
- **Arguments to a launched program, and a shell beyond a pipeline.** The shell's
  `run` command starts a program on demand now, but it cannot pass it anything:
  `SYS_RUN` forges the same empty, argv-less frame the loader always does. The shell
  now connects programs with **pipes** (`run A | run B | run C`,
  [reference/shell.md](reference/shell.md)), but there is no redirection to or from a
  file (`> OUT.TXT`), no job control to background a stage, and no history or line
  recall (backspace editing of the current line is all there is). `run` and a pipeline
  wait for their programs and report an exit status, but they can only wait: there is
  no way to background a program, and no way to interrupt one that will not finish.

## Natural next steps

In dependency order. Each builds on the one before.

**User mode.** Done. `kernel_main` drops to ring 3 and runs a program in its own
pages; the user GDT descriptors and `tss.rsp0` are now load-bearing. See
[decisions/0006-user-mode-with-separate-pages.md](decisions/0006-user-mode-with-separate-pages.md).
The remaining steps build on it.

**System calls.** Done. Ring-3 code re-enters the kernel through one DPL 3 IDT
gate at `int 0x50` (`kernel/syscall.c`), the first and only deliberate exception
to the DPL-0-everywhere IDT policy. A call number in RAX selects a handler; there
are fourteen. See
[decisions/0007-syscalls-via-int-0x50.md](decisions/0007-syscalls-via-int-0x50.md).
What remains for a real syscall layer is safe argument validation (see the
untrusted-pointer limitation below) and more calls, both of which wait on
processes and address spaces.

**A scheduler.** Done. The timer interrupt is now a preemption point: the tick
saves the interrupted `registers_t` into the current task's slot, picks the next
runnable task round-robin, and copies its saved frame back over the on-stack frame
in place, so the ISR stub's `iretq` resumes a different task. How many tasks are in
the rotation depends entirely on what has been started: the machine boots with one
(the shell), and `run` adds another for as long as it takes to finish. The task
structs are now heap-allocated and their stacks
bump-allocated from the user region, so the fixed four-task ceiling is gone (see
[decisions/0011-dynamic-tasks-and-stacks.md](decisions/0011-dynamic-tasks-and-stacks.md)).
See also
[decisions/0008-round-robin-preemptive-scheduler.md](decisions/0008-round-robin-preemptive-scheduler.md).
What remains is everything isolation buys (below) and lifting the single-stack-
region limit, which waits on per-process address spaces.

**Per-process paging.** Done. Each task has its own page-table tree, loaded into
CR3 on every context switch (`kernel/paging.c`): a private 4KB user half on its
own frames and a kernel half cloned from the boot tables by value. Two tasks use
the same virtual addresses backed by different physical memory, so a stray or
overflowing pointer faults instead of corrupting a neighbour. See
[decisions/0012-per-process-paging.md](decisions/0012-per-process-paging.md).
What remains builds on it: handling the page fault (vector 14, which already has
a gate and a stub) for demand paging and to kill a process that touches memory it
does not own, copy-on-write to share the read-only text instead of copying it per
task, and finally loaded processes rather than compiled-in programs.

**A block device.** Done. `drivers/disk.c` is a polled ATA PIO driver that reads
and writes 512-byte blocks by LBA on the primary bus. It is the raw storage layer
a filesystem needs. See
[decisions/0013-ata-pio-disk-driver.md](decisions/0013-ata-pio-disk-driver.md).
The remaining steps build on it.

**A filesystem.** Done, for reading. `fs/fat32.c` turns names into block numbers:
it parses the boot sector, follows cluster chains through the FAT, and reads a
file by 8.3 name out of the root directory. See
[decisions/0014-read-only-fat32.md](decisions/0014-read-only-fat32.md).

**Program loading.** Done. `kernel/elf.c` validates an ELF64 file, bounds-checks
and maps each `PT_LOAD` segment into a fresh address space, zero-fills the gap
between file size and memory size, and hands the entry point to the task forge.
Every ring-3 program is now a file on the disk, and changing one needs no kernel
rebuild. See
[decisions/0015-elf-program-loading.md](decisions/0015-elf-program-loading.md).

**The interactive shell.** Done. `user/shell.c` is a ring-3 program that reads
commands and runs them through the syscalls (`SYS_READKEY`, `SYS_LIST`, `SYS_RUN`,
`SYS_READFILE`, `SYS_WRITEFILE`/`SYS_DELETE`/`SYS_FREECOUNT` behind
`write`/`delete`/`free`, and `SYS_PIPE`/`SYS_CLOSE` behind `|`) and a keyboard ring
buffer. `run` loads a named program on demand, so the fixed program list in
`kernel_main` is gone, and `run A | run B` connects two of them with a pipe. See
[decisions/0016-interactive-shell.md](decisions/0016-interactive-shell.md) and
[decisions/0022-file-descriptors-and-pipes.md](decisions/0022-file-descriptors-and-pipes.md).

**Blocking and sleep.** Done. A task can leave the rotation and be woken by the
event it waits for, so an idle machine halts rather than spins: an idle shell
dropped from 362,648 syscalls per six seconds to three. The mechanism is a block
at a syscall boundary that rewinds the saved `rip` onto the `int 0x50`, so waking
re-issues the call. See
[decisions/0017-blocking-and-sleep.md](decisions/0017-blocking-and-sleep.md).

**Process lifecycle.** Done. A task can end and be cleaned up: `SYS_EXIT` carries a
status and marks the task a zombie, a sweeper in `schedule()` frees its address
space once it is no longer the running task, and `SYS_WAIT` blocks a parent until a
child finishes and hands back the status. `run A.ELF` now prints the program's
output and then `run: A.ELF exited with status 0`, and repeated runs return the
free-frame count to the same value. See
[decisions/0018-process-lifecycle-exit-and-wait.md](decisions/0018-process-lifecycle-exit-and-wait.md).

**Filesystem writing.** Done. `fs/fat32.c` now creates, replaces, and deletes files
by name: free-cluster search (`find_free_cluster` with a persistent hint), chain
allocation and freeing, every FAT copy written (`fat32_set_entry`), directory-entry
creation and root-directory growth, and a write-before-publish ordering that makes a
crash lose only unreferenced clusters, never a live file. FSInfo is invalidated
rather than maintained. `write`/`delete`/`free` expose it from the shell. See
[decisions/0020-writable-fat32.md](decisions/0020-writable-fat32.md).

**Signals** (`kernel/signal.c`, `include/signals.h`, `user/trampoline.asm`). An
interrupt one layer up, with no hardware behind it: a per-task pending set, delivery
only on the way out to ring 3, default actions (kill with status 128 + signal), and
catchable handlers reached through a hand-forged ring-3 call frame and a
program-supplied trampoline, with `SYS_SIGRETURN` restoring the interrupted context.
Process groups make Ctrl-C address a job rather than a task, and the foreground group
is declared through `SYS_SETFG` rather than inferred. `SYS_KILL` and `SYS_TASKS` (with
`ps` and `kill`) reach tasks the keyboard cannot, `SIG_PIPE` ends a writer whose
reader has gone, and Ctrl-D gives the console an end of file. See
[decisions/0023-signals.md](decisions/0023-signals.md) and
[reference/signals.md](reference/signals.md).

**User memory and a libc** (`SYS_MMAP`/`SYS_MUNMAP`, `libc/malloc.c`,
`libc/printf.c`). Done. A program can ask for memory at run time, `malloc` and
`free` it, and print with `printf`; `libc/` is compiled into ring 3 as well as the
kernel, and the hand-rolled number printers are gone. See
[decisions/0024-user-memory-and-libc.md](decisions/0024-user-memory-and-libc.md)
and [reference/user-memory.md](reference/user-memory.md).

**Phase 1 is complete.** Every rung it set out — boot, memory, interrupts, user
mode, syscalls, a scheduler, per-process paging, a disk, a filesystem, program
loading, a shell, blocking, a process lifecycle, writing, descriptors and pipes,
signals, and now user memory with a libc — is built, documented and tested.

**Next.** The remaining process work the shell makes concrete: argv on the new stack
so `run` can pass arguments, and `waitpid` so a parent with several children can name
one (the pipeline shell reaches for it, working around it by matching the reaped
child's id). Demand paging is the natural successor to eager `SYS_MMAP` (and would
give growable stacks), and it changes what a page fault means, which is why it was
kept out of the libc rung. Subdirectories and paths are the filesystem's own next
rung, and a file redirect (`> OUT.TXT`) wants streaming file writes. **Pipes are
done**
([decisions/0022-file-descriptors-and-pipes.md](decisions/0022-file-descriptors-and-pipes.md)),
added exactly as predicted — a new pair of `wait_reason_t` and a waker in the right
place, no change to the block/wake mechanism. **Signals are done too.** Waiting on
the disk is the same shape as a pipe's block and still a seam.

## Known limitations

- **Syscall pointer validation is region-based, not per-process.** Every call that
  takes a pointer from ring 3 (untrusted — the confused-deputy problem) now bounds
  the **whole** `[ptr, ptr+len)` range with `user_range_ok`, including the
  write-target pointers of `SYS_STAT`, `SYS_PIPE`, and `SYS_WAIT`, and caps copied
  filenames with `copy_user_string`; `SYS_WRITE` was a start-pointer-only stopgap
  until it became a counted `(fd, buf, len)` call and moved onto `user_range_ok`
  like the rest. What is still missing is that the check tests virtual addresses
  against the **fixed region constants** (`USER_REGION_START`..`USER_SPACE_END`,
  4-10M, which since the libc rung includes the heap slot that starts empty)
  rather than walking the caller's own page tables to confirm each page is mapped
  and user-accessible — which is now possible (every task has a private tree) but
  not implemented, so an unmapped address inside the span passes the check and
  faults in the kernel when dereferenced. Recorded as a TODO in `kernel/syscall.c`.
  See [reference/syscalls.md](reference/syscalls.md).
- **User memory is eager, region-based, and small.** `SYS_MMAP` maps every page
  before it returns (no lazy mapping), a task holds at most eight regions, a gap
  below a live region is never reused, there is no partial or overlapping
  `SYS_MUNMAP` and no `realloc`, and `malloc` never returns a slab to the kernel,
  gives 8-byte (not 16-byte) alignment, and is not async-signal-safe. `printf` has
  no width, precision, length modifiers or floats, and a mismatched format string
  is undefined beyond the bounds the runtime places on it. See
  [decisions/0024-user-memory-and-libc.md](decisions/0024-user-memory-and-libc.md).
- **Blocking is narrow, and signals are a small subset of the real thing.** Task structs
  are heap-allocated and each task now has its own address space with a private
  stack (per-process paging), so the old fixed four-task ceiling and the shared
  user-stack region are both gone: a stack overflow faults in the offending task
  instead of corrupting a neighbour. Blocking exists, but only at a syscall entry
  point and only for a handler whose work can be redone from the top, because a
  block rewinds the caller onto its `int 0x50` rather than resuming mid-handler;
  blocking part way through a multi-block disk transfer would need
  `TODO(per-task-kernel-stack)`. There is also no timed sleep, so nothing can ask
  to be woken after a duration and a blocking call has no timeout
  (`TODO(timed-sleep)`), and wakeup is a linear scan rather than a per-reason wait
  queue. Task exit and reclamation now exist, but with sharp edges of their own:
  a task can now be stopped (Ctrl-C, or `kill`), but signals are a small subset of
  the real thing: **no per-signal masks** (one `sig_active` flag blocks everything
  for a handler's duration), no `sigaction`, no `SIGCHLD`, no `SIGSTOP`/`SIGCONT`,
  and no job control beyond a single foreground group; `SYS_WAIT` is any-child, so a
  parent cannot name a particular one; orphaned zombies are discarded rather than reparented, as
  there is no `init`; and with no crt0, a program that falls off the end of `_start`
  runs into whatever bytes follow it. Memory is also still used wastefully: the
  read-only user text is copied in full per task rather than shared
  (`TODO(shared-text)`). See
  [reference/scheduling.md](reference/scheduling.md),
  [reference/blocking.md](reference/blocking.md),
  [reference/paging.md](reference/paging.md),
  [decisions/0012-per-process-paging.md](decisions/0012-per-process-paging.md),
  [decisions/0017-blocking-and-sleep.md](decisions/0017-blocking-and-sleep.md), and
  [decisions/0018-process-lifecycle-exit-and-wait.md](decisions/0018-process-lifecycle-exit-and-wait.md).
- **Disk transfers freeze the machine.** The disk driver polls, so the CPU spins
  in the wait loops for the whole transfer and nothing else runs, including the
  scheduler: the timer tick cannot preempt a task while a block is moving. This is
  slow and blocking, an accepted limitation of polled PIO. The fix is
  interrupt-driven transfer (IRQ14 when a block is ready) and then DMA, recorded as
  future work in
  [decisions/0013-ata-pio-disk-driver.md](decisions/0013-ata-pio-disk-driver.md).
  The filesystem inherits this: it caches nothing, so every FAT lookup reads a
  full 512-byte block through the polled driver, and reading a large file freezes
  the machine for the duration.
- **No SMP.** TownOS assumes a single CPU. It uses the legacy 8259 PIC, not the
  APIC/IO-APIC, and has no per-core state or locking.
- **1GB identity-map ceiling.** The boot climb (`boot/boot.asm`) maps a fixed
  32MB, then `kernel/memory.c` reads the Multiboot map and extends the identity
  map to cover real RAM, capped at 1GB. The cap is real: the single `pd_table` is
  one 4KB page (512 x 2MB = 1GB), so physical RAM above 1GB is not mapped and is
  ignored. Lifting it needs more page directories, which is out of scope. The
  frame pool is sized from the same detected RAM, so `alloc_frame()` now returns
  real, mapped, writable memory (the old "invented sizes" and "allocator returns
  unmapped addresses" problems are gone). See
  [reference/memory-map.md](reference/memory-map.md) and
  [decisions/0009-read-multiboot-map-extend-identity-map.md](decisions/0009-read-multiboot-map-extend-identity-map.md).
- **QEMU only.** The kernel has been built and booted under
  `qemu-system-x86_64`. It has not been run on real hardware or other emulators,
  and the `townos.bin` boot path relies on QEMU's built-in Multiboot `-kernel`
  loader.
