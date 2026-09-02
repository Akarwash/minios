# Architecture

TownOS is a small x86-64 hobby kernel that boots via Multiboot, climbs into
64-bit long mode, and boots into an interactive shell. It is a learning kernel
with a read-only filesystem: files on the disk can be listed and read by name, but
nothing can be written back. Its user programs are separately compiled ELF64
binaries that live on that disk and are loaded at runtime, not code welded into the
kernel image. The shell is one of them: a fenced-in ring-3 program (`SHELL.ELF`)
that reads typed commands and runs them using nothing but syscalls, which is what
proves the syscall boundary is complete. TownOS drops to ring 3 (CPL 3) to run
those programs in their own user-accessible pages, and those programs call back
into the kernel through a single `int 0x50` syscall gate (`SYS_WRITE`, `SYS_EXIT`,
`SYS_READKEY`, `SYS_LIST`, `SYS_RUN`, `SYS_READFILE`, `SYS_WAIT`) rather than
faulting. A
round-robin preemptive scheduler switches between the ring-3 tasks on every timer
tick by overwriting the interrupt frame in place, so they run concurrently: the
shell boots alone, and `run A.ELF` starts another task that interleaves with it.
Each task is a heap-allocated `task_t` with its OWN page-table tree (per-process
paging): the scheduler loads that task's CR3 on every switch, so two tasks share
virtual addresses but not physical memory. That is real address-space isolation,
not a single shared space. A task with nothing to do leaves the rotation entirely
rather than spinning: it blocks at a syscall boundary, and whatever causes the
event it waits for wakes it, so an idle machine halts the CPU instead of burning
it. See [reference/user-mode.md](reference/user-mode.md),
[reference/syscalls.md](reference/syscalls.md),
[reference/shell.md](reference/shell.md),
[reference/scheduling.md](reference/scheduling.md),
[reference/blocking.md](reference/blocking.md), and
[reference/paging.md](reference/paging.md).

This page is a map, not a tutorial. For the concepts behind each subsystem, see
[`../learnings/`](../learnings/README.md) and follow the cross-links.

## Directory layout

| Directory | Responsibility |
|-----------|----------------|
| `boot/` | Multiboot header and the hand-written 32 to 64 long-mode climb (assembly). |
| `kernel/` | Core kernel: GDT/TSS, IDT, interrupt dispatch, syscall dispatch, timer, physical frame allocator, the kernel heap, per-process paging, ring-3 entry, the scheduler, and `kernel_main`. |
| `drivers/` | Hardware drivers: VGA text screen, PS/2 keyboard, the polled ATA PIO disk driver, port I/O helpers. |
| `fs/` | The filesystem layer, above the disk driver: read-only FAT32 (list the root directory, read a file by name). |
| `libc/` | Freestanding C library, compiled twice: into the kernel (`mem`, `string`) and into every ring-3 program (`mem`, `string`, `malloc`, `printf`). |
| `user/` | The ring-3 programs, built as standalone static ELF64 binaries (linked with `user/user.ld`) that live on the disk image and are loaded at runtime, not part of `townos.bin`. Holds the interactive shell (`user/shell.c`, booted as `SHELL.ELF`), which is the program the machine is for, and the runtime everything compiles against (`user/userlib.h`, `user/user.ld`). |
| `user/tests/` | Kernel test fixtures: ring-3 programs that exist to prove a piece of the kernel works and would be pointless on a machine anybody used (`A.c` through `K.c`, plus `COUNT.c`, `UPPER.c` and `ONCE.c`). They build identically to the shell and land on the same disk; the split is about what a reader should conclude when one of them looks strange. See `user/tests/README.md`. |
| `include/` | Shared definitions (`types.h`, the vector map, the syscall ABI numbers). |

## Source files

| File | Responsibility | State |
|------|----------------|-------|
| `boot/boot.asm` | Multiboot header, page tables, PAE/EFER/paging, bootstrap GDT, far jump to 64-bit, call `kernel_main`. | Implemented |
| `kernel/kernel.c` | `kernel_main`: the init sequence, then loads `SHELL.ELF` from disk as a task and starts the scheduler. | Implemented |
| `kernel/gdt.c`, `kernel/gdt.h` | Kernel GDT (null, kernel code/data, user code/data) and 64-bit TSS; selector constants. | Implemented |
| `kernel/usermode.c`, `kernel/usermode.h` | `enter_user_mode`: forge the `iretq` frame and drop to ring 3. | Implemented |
| `kernel/syscall.c`, `kernel/syscall.h` | Syscall dispatcher: `syscall_handler` switches on RAX (`SYS_WRITE`, `SYS_EXIT`, `SYS_READKEY`, `SYS_LIST`, `SYS_RUN`, `SYS_READFILE`, `SYS_WAIT`), with `user_range_ok`/`copy_user_string` bounding the untrusted pointers. | Implemented |
| `kernel/scheduler.c`, `kernel/scheduler.h` | Round-robin scheduler: `task_create_from_file` heap-allocates and forges a task, builds its private address space and loads a program file into it, `schedule` swaps the interrupt frame and loads the next task's CR3, `scheduler_start` enters task 0. `task_block` puts a task to sleep at a syscall boundary and `scheduler_wake` readies the sleepers on a reason; with nothing runnable, `schedule` parks the CPU in `hlt`. `task_exit` marks a task `TASK_ZOMBIE` and wakes its parent, `reap_sweep` frees a zombie's address space (never the running task's), and `task_wait` blocks a parent until a child exits and then frees the tombstone. | Implemented |
| `kernel/paging.c`, `kernel/paging.h` | Per-process paging: `paging_create_address_space` (private tree, kernel half cloned by value), `paging_map_page` (4KB user mappings), `paging_switch` (load CR3), `paging_destroy_address_space` (free the user half only, never the tree in CR3). | Implemented |
| `kernel/elf.c`, `kernel/elf.h` | ELF64 program loader: validate a file, bounds-check and map each `PT_LOAD` segment, copy and zero-fill it, report the entry point. | Implemented |
| `user/shell.c` | The interactive shell: a ring-3 program (`SHELL.ELF`) that reads a line via `SYS_READKEY`, tokenizes it, and dispatches `list`/`read`/`run`/`help`/`clear`/`return` through syscalls. | Implemented |
| `user/tests/A.c`, `B.c`, `C.c` | Kernel test fixtures, each a separate static ELF64 binary on the disk image, launched on demand with the shell's `run`. A prints 20 letters and exits 0 (the ordinary case, with a real `.bss`), B prints 60 (a second binary of a visibly different length), C prints 40 and exits **3** (proving a non-zero status survives the trip back to the prompt). | Implemented |
| `user/tests/D.c`, `E.c` | The sweeper fixtures. D starts E and exits **without** waiting, which orphans E; E is then the only kind of zombie `reap_sweep`'s free path and `parent_alive`'s "no" branch can ever see. `run d.elf` is the only test that reaches them. | Implemented |
| `user/userlib.h` | The whole runtime a user program gets: `always_inline` inline-asm syscall wrappers, the delay loop, the tokenizer, and the `malloc`/`printf` prototypes; it includes the two libc headers. | Implemented |
| `user/user.ld` | User program linker script: entry `_start`, load address 0x400000, every loadable segment page-aligned (a contract with the loader). | Implemented |
| `kernel/gdt_flush.asm` | `lgdt`, reload data segments, reload CS via far return, `ltr`. | Implemented |
| `kernel/idt.c`, `kernel/idt.h` | IDT table, `idt_set_entry`, PIC remap, IDT zeroing, `lidt`. | Implemented |
| `kernel/isr.c`, `kernel/isr.h` | C-side interrupt dispatch: `isr_install`, `isr_handler`, `irq_handler`, handler registration. | Implemented |
| `kernel/isr_stubs.asm` | `isr0`-`isr31`, `irq0`-`irq15` entry points and the common save/restore stubs. | Implemented |
| `kernel/timer.c`, `kernel/timer.h` | PIT driver: program channel 0, count ticks on IRQ 0, call `schedule` each tick. | Implemented |
| `kernel/memory.c`, `kernel/memory.h` | Bitmap physical frame allocator, plus `alloc_frames_contiguous` for multi-page runs. | Implemented |
| `kernel/heap.c`, `kernel/heap.h` | Kernel heap (`kmalloc`/`kfree`): explicit free list with boundary tags and coalescing, ported from p5, on top of the frame allocator. | Implemented |
| `drivers/screen.c`, `drivers/screen.h` | VGA text output: `print_char`/`print_string`/`print_int`/`print_hex`, scrolling, cursor. | Implemented |
| `drivers/keyboard.c`, `drivers/keyboard.h` | PS/2 keyboard driver on IRQ 1: scancode to ASCII, pushed into a ring buffer the IRQ fills and `SYS_READKEY` drains, then `scheduler_wake(WAIT_KEY)` to rouse a sleeping reader. | Implemented |
| `drivers/disk.c`, `drivers/disk.h` | Polled ATA PIO disk driver: `disk_init`/`disk_read`/`disk_write` move 512-byte LBA28 blocks on the primary bus. | Implemented |
| `drivers/ports.c`, `drivers/ports.h` | `in`/`out` port I/O wrappers (byte and word, in and out). | Implemented |
| `fs/fat32.c`, `fs/fat32.h` | Read-only FAT32: `fat32_init` parses the boot sector, `fat32_list_root` lists the root directory, `fat32_read_file` reads a file by 8.3 name, `fat32_stat` reports a size without reading. | Implemented (read-only; the ELF loader reads programs through it) |
| `libc/string.c`, `libc/string.h` | `strlen`, `strcmp`, `strcpy`, `strchr`. Compiled into the kernel and into ring 3. | Implemented |
| `libc/mem.c`, `libc/mem.h` | `memcpy`, `memmove`, `memset`, `memcmp`. Compiled into the kernel and into ring 3. | Implemented |
| `libc/malloc.c` | Ring-3 only: `malloc`, `free`, `calloc`, the kernel heap's allocator ported a second time on `SYS_MMAP`. | Implemented |
| `libc/printf.c` | Ring-3 only: `printf` with `%d %u %x %s %c %%`, on `sys_write_all`. | Implemented |
| `include/types.h` | Fixed-width integer types and `NULL`. | Implemented |
| `include/vectors.h` | Single source of truth for every interrupt vector, including `SYSCALL_VECTOR`. | Implemented |
| `include/syscalls.h` | Standalone syscall ABI numbers (`SYS_EXIT`, `SYS_WRITE`, `SYS_WAIT`, ...), no kernel code. | Implemented |
| `linker.ld` | Section layout: the kernel at 1M in a single `PT_LOAD` segment (nothing ring-3 is in the image any more). | Implemented |
| `Makefile` | Build rules, toolchain, flags. | Implemented |

## Subsystem map

The boot-to-shell chain:

```
boot (boot.asm) ............ long-mode climb, hands off to kernel_main
  -> GDT/TSS (gdt.c) ....... segment descriptors + TSS, ltr
  -> IDT (idt.c) ........... PIC remap, IDT zero, set_entry, lidt
  -> ISR stubs (isr_stubs)   isr0-31 / irq0-15 entry points, common save/restore
  -> drivers .............. screen, keyboard, timer, ports
  -> heap_init ............ build the kernel heap (heap.c)
  -> disk_init ............ probe the primary ATA bus, silence IRQ14 (disk.c)
  -> fat32_init ........... parse the boot sector, mount the FAT32 volume (fat32.c)
  -> task_create_from_file  read SHELL.ELF off the disk, validate and load its
                          segments into a fresh private address space, forge the task
                          (elf.c, scheduler.c, paging.c)
  -> scheduler_start ...... load task 0's CR3, enter task 0 via enter_user_mode (usermode.c)
  -> SHELL.ELF ............ runs at CPL 3 in its own tree, reads keys and runs commands
                          via int 0x50 (SYS_READKEY/LIST/RUN/READFILE/WRITE)
  -> keyboard IRQ ......... pushes each key into the ring buffer (keyboard.c)
  -> timer tick ........... schedule() swaps the interrupt frame and loads the next task's CR3
  -> syscall_handler ...... dispatches the call and writes the result to RAX (syscall.c)
```

The kernel links into `townos.elf`, is repackaged as a Multiboot-loadable
`townos.bin`, and boots under QEMU. See [building.md](building.md) for the build
and run steps, [reference/idt.md](reference/idt.md) for the interrupt path, and
[reference/user-mode.md](reference/user-mode.md) for the ring-3 drop.

## Control flow

From power-on to the idle loop:

1. GRUB/QEMU reads the Multiboot header in `boot/boot.asm` and hands control to
   `_start` in 32-bit protected mode.
2. `_start` performs the 32 to 64 long-mode climb (page tables, PAE, EFER.LME,
   enable paging), loads a bootstrap GDT, and far-jumps into 64-bit code. See
   [reference/boot-sequence.md](reference/boot-sequence.md).
3. The 64-bit entry sets `RSP = stack_top` and calls `kernel_main`.
4. `kernel_main` (`kernel/kernel.c`) runs the init sequence in order:
   `gdt_init()`, `isr_install()`, `timer_init(100)`, `keyboard_init()`, clears
   the screen and prints the banner, then `memory_detect_and_map()` (reads the
   Multiboot map, extends the identity map to real RAM, flushes the TLB) and
   `memory_init()` (sizes the frame pool from the detected RAM). The banner
   reports the detected RAM. See
   [reference/memory-map.md](reference/memory-map.md).
5. `kernel_main` calls `heap_init()`, then `disk_init()` (which probes the
   primary ATA bus, prints whether a disk was detected, and sets nIEN so the
   polled driver's IRQ14 stays silent), then `fat32_init()` to mount the volume,
   then `task_create_from_file("SHELL.ELF")` (which
   `kmalloc`s a `task_t`, builds a private page-table tree, reads and validates
   the program file and maps its segments into that tree, maps a private stack,
   and forges its ring-3 `iretq` frame with the entry point from the ELF header).
   A file that is missing or malformed costs only its own task: the loader prints
   the reason, and if nothing loaded the kernel idles rather than scheduling. Then
   `scheduler_start()`, which loads task 0's CR3 and
   enters task 0 via `enter_user_mode`. From here the timer tick is a preemption
   point: `schedule()` saves the interrupted task's register frame, copies the
   next task's frame over it in place, and loads the next task's CR3, so `iretq`
   resumes a different task in its own address space. The shell loops, reading keys
   through `SYS_READKEY` and running commands through the `int 0x50` gate; a `run
   A.ELF` command calls `SYS_RUN`, which loads a second task, and then `SYS_WAIT`,
   which blocks the shell until that task calls `SYS_EXIT`. The exiting task is
   marked a zombie; the scheduler's sweeper frees its address space on a later tick
   and the shell frees the tombstone when it collects the status. With nobody typing,
   the shell is blocked inside `SYS_READKEY` and, if it is the only task, nothing is
   runnable, so `schedule()` halts the CPU until the keyboard IRQ wakes it.
   `scheduler_start` does not return, so the `hlt` idle loop below it is unreachable
   in this build. See
   [reference/scheduling.md](reference/scheduling.md),
   [reference/blocking.md](reference/blocking.md),
   [reference/paging.md](reference/paging.md),
   [reference/user-mode.md](reference/user-mode.md), and
   [reference/syscalls.md](reference/syscalls.md).

`kernel_main` takes the Multiboot info pointer (`boot/boot.asm` forwards EBX in
RDI) and is not expected to return. Interrupts stay enabled across the drop (each
task's forged RFLAGS keeps IF set), which is what lets the timer preempt a running
task and drive the switch.

## Where to read more

- Boot climb: [reference/boot-sequence.md](reference/boot-sequence.md)
- GDT/TSS: [reference/gdt.md](reference/gdt.md)
- Interrupts and the IDT: [reference/idt.md](reference/idt.md)
- Ring 3 and syscalls: [reference/user-mode.md](reference/user-mode.md), [reference/syscalls.md](reference/syscalls.md)
- The scheduler: [reference/scheduling.md](reference/scheduling.md)
- Blocking and sleep: [reference/blocking.md](reference/blocking.md)
- Per-process paging: [reference/paging.md](reference/paging.md)
- Memory layout: [reference/memory-map.md](reference/memory-map.md)
- The kernel heap: [reference/heap.md](reference/heap.md)
- The disk driver: [reference/disk.md](reference/disk.md)
- The filesystem: [reference/fat32.md](reference/fat32.md)
- Program loading: [reference/elf-loading.md](reference/elf-loading.md)
- The interactive shell: [reference/shell.md](reference/shell.md)
- Concepts (the why): [`../learnings/`](../learnings/README.md)
