# Per-process paging reference

Each task in TownOS runs in its own page-table tree, loaded into CR3 on every
context switch, so two tasks can use the same virtual address for different
physical memory. This page documents how a tree is built, why the kernel is
mapped in every one, the 4KB/2MB split, and the one invariant the whole scheme
rests on. Read from `kernel/paging.c`, `kernel/paging.h`, `kernel/scheduler.c`,
and `boot/boot.asm`. For the rationale and trade-offs see
[decision 0012](../decisions/0012-per-process-paging.md).

## The problem it solves

Before this, everything shared the single boot tree (`boot/boot.asm`): one PML4,
one PDPT, one PD, all 2MB huge pages, identity mapped. Identity mapping means
virtual address equals physical address for everyone, so two tasks could not use
the same virtual address for different memory. That made tasks really threads:
their stacks had to sit at different addresses (a bump allocator split one shared
region, see [decision 0011](../decisions/0011-dynamic-tasks-and-stacks.md)), and
any task could read any other's memory with nothing to fault on.

Per-process paging gives each task its OWN tree. Every program runs its code at
`0x400000` and puts its stack top at `0x800000`, but lands on DIFFERENT physical
frames. That difference is the isolation.

## Two halves: private user, shared kernel

Every tree is split the way every real kernel splits an address space.

**User half, PRIVATE per task.** 4KB pages to freshly allocated frames, user bit
set. This is what makes `0x400000` mean different physical memory in different
trees.

**Kernel half, SHARED (identical) in every tree, kernel-only.** The kernel must
be mapped in every tree, because when an interrupt fires the CPU jumps into
kernel code WITHOUT changing CR3: the first kernel instruction is fetched through
whatever task tree is currently loaded. If the kernel were not mapped there, the
CR3 switch itself would triple-fault on the very next instruction. So the IDT,
the GDT, the TSS `rsp0` stack, the interrupt stub, the kernel stack the interrupt
frame sits on, and all kernel code and data live in the kernel half of every
tree.

## Why the user half MUST be 4KB pages

A 2MB huge-page identity mapping forces virtual==physical: the page IS its own
physical address. That cannot give two tasks different physical memory at the
same virtual address. So the private user region must use 4KB pages, walked all
the way down to a PT whose leaf points at an arbitrary frame. The kernel region
stays 2MB huge pages (it is shared and identity mapped, so huge pages are fine
and cheaper). Each tree is therefore deliberately MIXED page sizes: 4KB user,
2MB kernel.

## Building a tree: clone the kernel half by value

`paging_create_address_space()` (`kernel/paging.c`) builds one tree:

1. Allocate an `address_space_t` handle on the kernel heap (kernel-only
   bookkeeping, safe there) and its own PML4, PDPT, and PD from `alloc_frame`
   (page tables must be 4KB and 4KB-aligned, which `alloc_frame` guarantees and
   `kmalloc` does not).
2. Fill the PD by COPYING every boot `pd_table` entry EXCEPT the three user slots
   (`pd_table[2]` = user code, `pd_table[3]` = user stack, `pd_table[4]` = user
   heap), which are left absent so the user branch can be overridden with private
   4KB page tables. Each copied kernel entry carries the identical 2MB huge-page
   mapping (same physical kernel frame, no user bit), so every tree maps the
   identical kernel, kernel-only. The three indices live in one array,
   `user_pd_slots`, which the teardown reads too (see below). `PD[4]` was the
   boot tree's identity map of physical 8-10M, which is why those frames are now
   reserved from the pool ([memory-map.md](memory-map.md)) and why the slot must
   be left absent rather than copied: `next_table` would follow a copied huge-page
   entry as if it were a page table.
3. Wire `PDPT[0] -> this PD` and `PML4[0] -> this PDPT`, both user-permissive.

`paging_map_page(as, virt, phys, flags)` then installs the private user mappings,
walking `PML4 -> PDPT -> PD -> PT` and creating intermediate tables on first
touch. `paging_switch(as)` loads `as->pml4_phys` into CR3.

### By value, not by reference: why

The obvious approach is to share the kernel half BY REFERENCE (point every tree's
`PML4[0]` at the one boot `pdpt_table`/`pd_table`). It does not fit TownOS's
layout. Everything hangs off a single branch, `PML4[0] -> PDPT[0] -> pd_table`,
and the ring-3 region lives INSIDE that same `pd_table` (user code is
`pd_table[2]`, user stack `pd_table[3]`). Sharing `pd_table` by reference would
share the user huge pages too, making a private 4KB mapping at `0x400000`
impossible: the whole point. So the kernel half is cloned by value, entry by
entry, skipping the three user slots.

## The load-bearing invariant: kernel mappings are frozen after boot

By-value cloning is correct ONLY because the kernel's mappings never change after
boot. `memory_detect_and_map` (`kernel/memory.c`) fills the identity map once at
startup, BEFORE any task tree exists, and nothing ever mutates a kernel PD entry
afterward. So a by-value copy, taken by `paging_create_address_space` on the way
through `task_create_from_file`, can never drift out of sync with the boot tables.

This is a tripwire, not a footnote:

> If the kernel ever remaps itself at runtime (kernel ASLR, memory hot-plug, a
> higher-half relocation), the by-value copies already living in every existing
> task tree would silently go stale. The kernel would then see different mappings
> depending on which task happened to be current. At that point the by-value
> clone MUST become a by-reference share of the kernel tables, or gain an
> explicit step that propagates the change into every existing tree.

The same warning is at the `extern pd_table` clone site in `kernel/paging.c` and
in [decision 0012](../decisions/0012-per-process-paging.md). Do not add runtime
kernel remapping without revisiting it.

## The user bit at every level (AND-down)

x86 permits a ring-3 access only if the user bit (US, `PG_USER`) is set at EVERY
level of the walk: PML4, PDPT, PD, and PT. `paging_map_page` makes the
intermediate tables it creates user-permissive and lets the LEAF PTE gate the
real privilege. This is safe because every table it creates is on the user
branch. The cloned kernel leaves have NO user bit, so even though the branch
above them (`PML4[0]`/`PDPT[0]`) says "user may pass", ring 3 still cannot reach
kernel memory: the AND of the levels is kernel-only at the leaf.

## What a task's user half holds

`task_create_from_file` (`kernel/scheduler.c`) fills the private user half in two
steps, from two different places, and a third slot is filled later, on request:

- **Program image, by the ELF loader.** `elf_load_file` (`kernel/elf.c`) reads
  ONE program's file off the disk and walks its `PT_LOAD` segments. For each
  segment it allocates fresh frames, zeroes them, copies the segment's `p_filesz`
  bytes of file content in, and maps them at the segment's own `p_vaddr` with
  flags derived from the segment's own permissions — so a read-only text segment
  is mapped without `PG_WRITABLE`. The tail from `p_filesz` to `p_memsz` is left
  as the zeroes it was allocated with; that is the `.bss`. Only that one
  program's bytes are mapped, and only the pages it declares.
- **Stack, by the scheduler.** `map_user_stack` maps fresh frames at a FIXED
  virtual address, top `0x800000` (`USER_STACK_TOP`) growing down
  `USER_STACK_SIZE` bytes, `PG_PRESENT | PG_WRITABLE | PG_USER`. No copy: the
  program writes its own stack as it runs. Every task reuses the same VA on its
  own frames, which is exactly what per-process paging makes possible and what
  retires the old shared-stack-region ceiling.
- **Heap, by `SYS_MMAP`, at run time.** `PD[4]` (`0x800000` up to `0xA00000`) is
  empty when the task starts. `paging_map_user_range` fills it one region at a
  time with fresh, zeroed frames, `PG_PRESENT | PG_WRITABLE | PG_USER`, unwinding
  every page it mapped if a frame or page table runs out, and
  `paging_unmap_user_range` empties a region again, flushing each page from the
  TLB. See [user-memory.md](user-memory.md).

**This used to be one step from one source, and the difference matters.** Before
per-file loading ([decision 0015](../decisions/0015-elf-program-loading.md)) a function
called `build_user_space` copied the whole linked ring-3 image — `_user_text_start`
to `_user_rodata_end`, every program at once — into every task's tree, reading it
through the boot identity map. Neither that function nor those linker symbols
exist any more. A task's user half now holds the one program it is running rather
than all of them, its size follows that program's segments rather than being the
same for every task, and text can be mapped read-only because the loader knows
which segment is text. The old note about duplicating the read-only text per task
is obsolete in its particulars: what is duplicated now is one program's text, not
the whole image, though sharing it by reference between two tasks running the same
file is still not done.

Because programs are linked at `0x400000` and loaded there, nothing is relocated:
the entry address in the ELF header is the address the code was linked for, and
the forged `rip` is that value unchanged.

## Switching CR3: where, and why it is safe

The switch happens in two places (`kernel/scheduler.c`):

- **`schedule()`** loads the incoming task's CR3 (`mov %cr3`, using the cached
  `task_t.cr3`) AFTER copying its register pile over the live interrupt frame,
  and before the stub's `iretq`.
- **`scheduler_start()`** loads task 0's CR3 (`paging_switch`) BEFORE the first
  drop to ring 3, since until then the boot tables are active and task 0's code
  and stack live in ITS tree.

This is safe mid-interrupt only because everything the CPU still needs on the way
out lives in the kernel half, cloned identically into every tree:

- the register pile `r` is on the KERNEL stack,
- the `tasks[]` array and scheduler code are kernel `.data`/`.text`,
- when the timer next fires, the IDT, GDT, TSS `rsp0` stack, and interrupt stub
  are all reached through the same kernel mappings.

So the switch changes only the user half; the kernel never disappears out from
under itself. If any of those lived in the user half, the switch would
triple-fault on the next instruction. The ordering trap: the CR3 write MUST come
after the scheduler is done reading its own state and before `iretq` returns to
ring 3.

## Tearing a tree down

`paging_destroy_address_space(address_space_t *as)` (`kernel/paging.c`) is the
inverse of everything the section above builds: `paging_create_address_space`, the
loader's segment mappings, and the stack. The scheduler calls it when a finished
task is cleaned up (see [scheduling.md](scheduling.md)), and `task_create_from_file`
calls it on every failure path after the create succeeded, so a program that fails
to load costs nothing.

**It frees the USER half only.** It walks `pml4[0] → pdpt[0] → pd` and then
descends into exactly three PD entries — `USER_PD_INDEX_CODE` (2),
`USER_PD_INDEX_STACK` (3) and `USER_PD_INDEX_HEAP` (4), the `user_pd_slots` array
that the clone also reads — freeing the present leaf pages of each page table, then
the page table itself, then the PD, the PDPT, and the PML4, and finally `kfree`s
the handle. The heap slot being in that list is what returns a program's
`SYS_MMAP` regions at exit: `task_exit` knows nothing about regions, and `malloc`
never releases a slab, so this walk is the only thing that frees them.

The list of three indices is the whole safety of the function, and it is why it is
written as an explicit list rather than a loop over present entries. It is also the
single place that has to change if a fourth user region is ever added. **A generic
"free everything present in this tree" walk would be catastrophic**: the kernel
half is cloned *by value* into every tree, so those entries point at the shared
kernel mappings, and freeing them returns the live kernel's memory to the frame
allocator. Nothing faults at that moment. The frames are simply marked free, handed
out later to somebody else, and written over, and the machine dies somewhere
unrelated, long afterwards, with no message that points anywhere near here. The
`PG_HUGE` skip inside the descent is a *second* lock (kernel-half entries are 2MB
pages and have no page table to walk), not the primary guard.

**Precondition: `as` MUST NOT be the address space currently loaded in CR3.** The
CPU is walking that tree to fetch the instructions doing the freeing. There is no
check for it and no fault when it is violated; the same delayed, misattributed
death applies. The scheduler upholds this by never sweeping the *current* task.

What it deliberately does **not** free: the kernel half (shared, not owned by this
task), the physical frames of the original ring-3 image at `0x400000` (the task's
copies are freed, the source image is not), and the `task_t` itself (that belongs
to the scheduler, which may need to keep it as a tombstone).

## CR3 holds a physical address, and the write flushes the TLB

CR3 takes a PHYSICAL address. Everything below 1GB is identity mapped, so
`as->pml4_phys` doubles as both the physical base of the PML4 and a writable
virtual pointer to it. Writing CR3 also flushes the TLB, because TownOS marks no
page `PG_GLOBAL`, so no entries survive the write. That is exactly what drops the
outgoing task's stale user translations on a switch, for free, with no explicit
`invlpg`.

## What a run looks like

**One task exists at boot now.** `kernel/kernel.c` starts `SHELL.ELF` and nothing
else ([decision 0016](../decisions/0016-interactive-shell.md)); the second and third trees
only come into being when somebody types `run`. So the thing to watch is a tree
appearing and disappearing, not three of them starting together.

Booted under QEMU with `-d int`, sitting at the prompt, then `run a.elf`, the CR3
column reads (counts are consecutive runs of interrupts at that CR3; regenerated
after the user-memory rung, which is why the addresses differ from older
transcripts):

```
    2 CR3=0000000000111000     ticks on the boot tree, before scheduler_start
  336 CR3=0000000000a10000     the shell alone, blocked in readkey
    2 CR3=0000000000a68000     A's tree exists; the two interleave
    5 CR3=0000000000a10000
  136 CR3=0000000000a68000
  141 CR3=0000000000a10000     A has exited and been reaped; the shell alone again
```

and the vectors over the same run:

```
  537 v=40      timer, IRQ 0
   65 v=50      syscalls
   20 v=41      keyboard, IRQ 1 — one per key of "run a.elf" plus the newline
```

Read four things out of that. There are exactly two task CR3 values because there
were exactly two tasks, and the boot CR3 appears only before the first switch.
The middle stretch alternates between the two trees, which is the round-robin.
The long runs at `0xa10000` at each end are the shell alone with nothing to switch
to; the shell's PML4 is at `0xa10000` rather than the `0x810000` of older
transcripts because physical 8-10M is now reserved and the first free frame is
above 10M. And there is **no `v=0e`, `v=0d` or `v=08`** anywhere: no page fault,
no general protection fault, no double fault. The `cpl=3` RIPs are all inside
`0x40009f`–`0x4071b7`, the loaded image's own range (larger than it was, since
every program now links the ring-3 libc), in both trees.

A temporary isolation proof (added, verified, then removed) walked each task's
tree and confirmed the shared VAs `0x400000` and the stack top page resolve to
DIFFERENT physical frames in every tree. Same virtual address, different physical
memory: the isolation, demonstrated.

Failure modes to recognise: a triple fault (`0x08`) right after the first switch
means something the kernel needs is not in the cloned kernel half. A page fault
at a user RIP means the private user mapping for that task is missing or wrong.

## Related

- The decision and its trade-offs:
  [decision 0012](../decisions/0012-per-process-paging.md).
- The scheduler that loads CR3 on switch, and calls the teardown:
  [scheduling.md](scheduling.md) and
  [decision 0018](../decisions/0018-process-lifecycle-exit-and-wait.md).
- The physical layout and the fixed user virtual addresses:
  [memory-map.md](memory-map.md).
- The heap slot, `SYS_MMAP`/`SYS_MUNMAP`, and the map/unmap helpers:
  [user-memory.md](user-memory.md) and
  [decision 0024](../decisions/0024-user-memory-and-libc.md).
- The shared-region layout this replaces:
  [decision 0011](../decisions/0011-dynamic-tasks-and-stacks.md).
- Concepts behind virtual memory and page tables:
  [`../../learnings/05-memory-management.md`](../../learnings/05-memory-management.md).
</content>
