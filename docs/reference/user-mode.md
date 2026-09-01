# User mode (ring 3)

This page documents how TownOS drops to CPL 3 and how it proves the drop is
real. Read from `kernel/usermode.c`, `user/shell.c`, `boot/boot.asm`,
`linker.ld`, and `kernel/gdt.c`. For the rationale and trade-offs see
[decision 0006](../decisions/0006-user-mode-with-separate-pages.md).

## What runs where

| Piece | Location | Source |
|-------|----------|--------|
| Ring-3 program code | `0x400000` (loaded from an ELF file, PD[2]) | `user/shell.c`, `user/tests/*.c`, `user/user.ld`, `kernel/elf.c` |
| Ring-3 stack top | `0x800000` (top of PD[3]) | `USER_STACK_TOP` in `kernel/usermode.h` |
| Ring-0 stack on entry | `tss.rsp0` (top of `tss_stack`) | `kernel/gdt.c` |
| Ring-3 code selector | `0x1B` (`GDT_SELECTOR_USER_CODE`, RPL 3) | `kernel/gdt.h` |
| Ring-3 data selector | `0x23` (`GDT_SELECTOR_USER_DATA`, RPL 3) | `kernel/gdt.h` |

## The transition

There is no instruction that raises code into ring 3. x86 only *lowers*
privilege by returning into less-privileged code, so `enter_user_mode`
(`kernel/usermode.c`) forges the frame an interrupt-return consumes and executes
it. In 64-bit mode `iretq` pops five 8-byte values in this order:

```
RIP, CS, RFLAGS, RSP, SS
```

so the function pushes them in reverse (SS first, RIP last), then runs `iretq`:

```
push  SS      = 0x23   (ring-3 data selector)
push  RSP     = 0x800000
push  RFLAGS  = 0x202  (reserved bit 1 + IF)
push  CS      = 0x1B   (ring-3 code selector, RPL 3)
push  RIP     = the program's ELF entry point
iretq
```

Because the popped CS has RPL 3, the CPU performs a privilege change: it loads
`SS:RSP`, sets RFLAGS, and begins executing at `CS:RIP` as ring-3 code. The data
segment registers (DS/ES/FS/GS) are pointed at the ring-3 data selector first;
long mode largely ignores them for addressing, but leaving ring-0 selectors
loaded across the drop is untidy.

`RFLAGS = 0x202` keeps the interrupt flag set. This is not optional: if ring 3 ran
with interrupts masked the timer and keyboard would go dead, and (now that a
scheduler exists) the running task would never be preempted, owning the machine
forever. The same 0x202 is what `task_register` forges into every task's saved
frame. See [scheduling.md](scheduling.md).

`enter_user_mode` does not return. It is still the path into ring 3, now invoked
by `scheduler_start` (`kernel/scheduler.c`) to enter task 0; every later entry
into a task goes through the scheduler restoring its saved frame instead.

## Why the isolation holds

The x86 page walk grants ring-3 access only if the user (US) bit is set at
*every* level from PML4 to the leaf — the bits are ANDed down the walk. TownOS
uses that: the two upper levels are permissive and the PD leaves decide access.

```
PML4[0], PDPT[0]          user bit SET     (permissive: a user branch may pass)
PD[0]  0x000000-0x1FFFFF  user bit CLEAR   kernel code, data, VGA, stack
PD[1]  0x200000-0x3FFFFF  user bit CLEAR   kernel
PD[2]  0x400000-0x5FFFFF  user bit SET     ring-3 code
PD[3]  0x600000-0x7FFFFF  user bit SET     ring-3 stack
```

Setting the user bit high on PML4[0]/PDPT[0] does not expose the kernel: the
PD[0]/PD[1] leaves still withhold it, and a walk that hits a clear user bit at
any level denies ring-3 access. The leaf is the real gate.

## How it is proven

The drop was first proven by making the ring-3 program execute `cli`, a
CPL-0-only instruction, and observing the #GP it raises at `cpl=3` with
`CS=0x1B`; an isolation cross-check pointing a ring-3 write at the kernel page
`0x100000` produced a #PF with the user error-code bit set and `CR2=0x100000`.
That verification, with the exact QEMU `-d int` output, is recorded in
[decision 0006](../decisions/0006-user-mode-with-separate-pages.md).

Nothing faults on purpose any more. The shipped programs — the shell
(`user/shell.c`) and the kernel test fixtures (`user/tests/`), all loaded from
disk as ELF files — demonstrate the drop the constructive way instead: they run
at CPL 3 and call the kernel through `int 0x50` in a loop, and the scheduler
switches between them on the timer tick. Each task runs in its OWN page-table
tree (per-process paging), so they share the same user virtual addresses but not
the same physical memory. Under QEMU with `-d int`, vector `0x50` fires from each
live task, each at `cpl=3`, each under its own `CR3`, with no #GP and no #PF. The
letters interleave on screen, printed by the kernel on the ring-3 programs'
behalf. That a ring-3 pointer into a loaded program's rodata (4-8M) is accepted
while a kernel address is rejected is the same leaf-level user-bit boundary, now
exercised through the syscall path instead of a fault. See
[syscalls.md](syscalls.md), [scheduling.md](scheduling.md), and
[paging.md](paging.md).

## What this is not

- **Not full multitasking.** Programs are now loaded from the filesystem rather
  than compiled into the kernel image, and they are created, preempted, exited and
  reaped at runtime (see [scheduling.md](scheduling.md) and
  [elf-loading.md](elf-loading.md)). What is still missing is the rest of the
  process abstraction: no `fork`, no `exec`, no `sigaction` (signals exist, but as a
  small subset — see [signals.md](signals.md)), and
  no file descriptors — a program's only handle on the world is the fixed set of
  syscalls in `include/syscalls.h`.
- **Not demand paging.** Each task has real per-process isolation: its own
  page-table tree and a `CR3` switch on every context switch, so tasks share
  virtual addresses but not physical frames (see [paging.md](paging.md)). What is
  still missing is laziness: every page a program declares is mapped eagerly by
  `task_create_from_file`, with no page-fault-driven demand paging, copy-on-write,
  or swapping.

## Related

- Decision and trade-offs: [../decisions/0006-user-mode-with-separate-pages.md](../decisions/0006-user-mode-with-separate-pages.md).
- The descriptors and TSS used: [gdt.md](gdt.md).
- The page tables re-privileged: [boot-sequence.md](boot-sequence.md).
- The faults that report success: [idt.md](idt.md).
- Memory layout and the 4-8MB overlap caveat: [memory-map.md](memory-map.md).
