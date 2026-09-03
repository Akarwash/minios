# 0024 - User memory and a fuller libc

## Status

Accepted.

## Context

A ring-3 program's address space was exactly what its ELF file declared, plus a
fixed 256KB stack: code and data at `PD[2]`, the stack at the top of `PD[3]`, and
nothing a program could ask for after it started. Every fixture in `user/tests/`
used fixed static buffers because there was no alternative; `F.c` wrote exactly
16384 bytes and not a number it worked out at run time; a program that wanted to
hold a list of unknown length could not be written.

The library side was thinner still. `libc/` held `memcpy`, `memset`, `strlen`,
`strcmp` and `strcpy`, compiled into the kernel and **unreachable from ring 3**,
because a user program was a single translation unit linked against nothing. So
`user/shell.c` grew a hand-rolled `print_uint` with ten call sites and its own
`str_eq` and `str_len`, and `COUNT.c`, `H.c` and `ONCE.c` each grew a `print_ulong`
that was the same dozen lines again. That is what a missing library function looks
like from the inside: not an absence, but the same code appearing repeatedly in
slightly different forms, where a bug in one copy is not a bug in the others and
none of them is found.

The allocator was not the problem. `kernel/heap.c`, the CMSC216 p5 `el_malloc`
ported once already ([0010](0010-kernel-heap-ported-from-p5.md)), does all the real
work and has run as the kernel heap for a dozen rungs. What it cannot do in ring 3
is **create the memory it hands out**: in the kernel it took a 64KB slab from
`alloc_frames_contiguous`, and a program has no privilege, no frame allocator and
no way to map a page. So the question was narrower than "write a malloc": how does
a program ask the kernel for more address space, and how does the kernel give it?

This is the last rung of phase 1, on top of the process lifecycle
([0018](0018-process-lifecycle-exit-and-wait.md)), per-process paging
([0012](0012-per-process-paging.md)) and signals ([0023](0023-signals.md)).

## Decision

Six decisions, each taken as stated, and one premise that turned out to be wrong
and is corrected below rather than papered over.

1. **Anonymous `mmap` only.** `SYS_MMAP(length)` returns a page-aligned address;
   `SYS_MUNMAP(addr, length)` releases it. No file mapping, no `MAP_SHARED`, no
   protection flags. That is the subset `malloc` uses, and the rest of `mmap`'s
   surface is a different subject wearing the same name.

2. **`mmap` rather than `brk`**, despite being about sixty lines larger. A `brk`
   heap is one run that only moves at one end, so memory can never be returned from
   the middle; with regions, guard pages and shared memory stay possible later
   without a redesign; and the p5 allocator was originally written against `mmap`,
   so the port is closer to its original shape than a `brk` shim would be.

3. **Eager mapping.** `SYS_MMAP` allocates, zeroes and maps every page before it
   returns. Lazy mapping is demand paging: a page fault handler that responds to a
   fault by mapping a frame rather than reporting a bug, which is a genuinely new
   capability (it would also give growable stacks) and one that changes what a page
   fault *means* in this kernel. It deserves a rung where it is the subject rather
   than arriving as a side effect of wanting `malloc`.

4. **The heap region is `PD[4]`, `0x800000`, with a 2MB ceiling.** The next slot
   above code (`PD[2]`) and stack (`PD[3]`); nothing collides with it because the
   stack is fixed size and fully mapped at load. **The premise that `PD[4]` was
   unused was wrong**, and the correction is part of this decision. `boot/boot.asm`
   identity-maps the first 32MB through sixteen huge pages, so `PD[4]` held the
   kernel's own mapping of physical 8-10M, and that range is the first thing the
   frame allocator hands out (the shell's page tables lived at `0x810000`). Two
   things follow. `memory_init` now reserves physical 8-10M from the frame pool, the
   same way 4-8M already was, because once a task tree is in CR3 the kernel cannot
   reach those frames through the identity map any more; that costs 512 frames and
   moves the free-frame baseline from 30585 to about 30072. And the by-value kernel
   clone leaves `PD[4]` absent rather than copying the boot entry, because
   `next_table` tests only `PG_PRESENT` and would have followed a copied huge-page
   entry as if it were a page table, writing the first region's page-table entries
   into physical `0x800000` with no fault at the point of the mistake. The layout
   is spelled out once, in `include/usermem.h`, shared by the kernel and by programs.

5. **A fixed array of eight regions per task**, `task_t.regions`, rather than a
   linked list. A list of memory regions would need somewhere to allocate its nodes
   from, in the middle of the thing that hands out memory; a fixed array cannot
   fragment its own bookkeeping. Eight is plenty for a `malloc` that grows its slab
   a few times. The array is read for exactly one purpose, validating `SYS_MUNMAP`;
   cleanup at exit does not read it at all, because the teardown frees the whole
   slot by page-directory index. Placement is the lowest page-aligned address above
   every region the task still holds, starting at the bottom of the slot. **A gap
   below a live region is never reused in this rung**; space above the highest live
   region is. Reusing gaps is what a real allocator's region list does and what a
   later rung would add.

6. **`printf` supports `%d %u %x %s %c %%` and nothing else.** No width, no
   precision, no length modifiers, no floats, no `%p`. More get added when
   something needs them. An unrecognised specifier stops the format and returns -1
   rather than being printed raw or skipped, so a mistake in a format string is
   visible.

Two smaller decisions rode along. `libc/` is compiled **twice**, into the kernel as
before and, with the user flags, into `.user.o` objects linked into every program,
so a function added to `libc/` reaches both sides and neither has to re-implement
it. And `kernel/heap.c` was ported a second time rather than shared: the two copies
(`kernel/heap.c`, `libc/malloc.c`) are the same code with the memory source
swapped, and both say so at the top, so a bug found in one can be looked for in the
other by name.

### The port, and the assumption it carried

The ported allocator assumes its blocks live in **one contiguous span**: "the block
above" is computed by adding a size to a pointer and bounded by a single
`heap_end`. `alloc_frames_contiguous` guaranteed that in the kernel; `SYS_MMAP`
promises only that a region is placed at the lowest address above every live one.
The port keeps the assumption and keeps the kernel copy's adjacency check: a new
slab that does not land exactly at `heap_end` is given straight back with
`SYS_MUNMAP` and the growth fails, so `malloc` returns NULL rather than ever walking
into a gap. Growth works in practice because, when `malloc` is the only thing in a
program calling `SYS_MMAP`, the next region's address *is* `heap_end`. That is the
"keep one span and fail when it is exhausted" choice, with the failure made loud.

## Six ways this goes wrong

Four of the six fail silently. Each has the fix, a comment at the site saying what
breaks without it, and a fixture that reaches it.

### M1. Unbounded length or address

- **Symptom.** A program asks for a huge length and the kernel maps over the top
  of the heap slot into kernel-only addresses, or runs the frame allocator dry and
  takes down the next task's page-table allocation.
- **Cause.** `length` and `addr` come from ring 3. The confused deputy of
  [0007](0007-syscalls-via-int-0x50.md), except that here the deputy maps pages on
  request.
- **Fix** (`sys_mmap`, `kernel/syscall.c`). Reject zero; reject any length larger
  than the 2MB slot *before* rounding, so the rounding cannot overflow; round to a
  page; reject a region that would cross `USER_HEAP_LIMIT`; reject a total across
  the task's regions that exceeds the slot. For `SYS_MUNMAP`, accept only the exact
  start of a region the task owns (M4). `J.ELF` asks for 3MB, for -1, and for one
  page more than the slot, and checks it is still running afterwards.

### M2. Regions leaked on task exit

- **Symptom.** The free frame count ratchets down across repeated runs of a program
  that allocates. Nothing else looks wrong. Silent.
- **Cause.** `task_exit` closes descriptors ([0022](0022-file-descriptors-and-pipes.md))
  and knows nothing about regions, and `malloc` never gives a slab back, so every
  program that ever called `malloc` exits holding memory.
- **Fix** (`kernel/paging.c`). `paging_destroy_address_space` already freed the
  user half by page-directory index; `USER_PD_INDEX_HEAP` joins that list. The
  list is now a single array, `user_pd_slots`, read by both the clone and the
  teardown, and it is the one place that has to change if a fourth region is ever
  added. Confirmed by reading that `destroy_page_table` walks the page table under
  each slot and frees the leaves, not just the table. Ten consecutive `run i.elf`
  print the same free-frame count.

### M3. Partial mapping failure

- **Symptom.** A `SYS_MMAP` that runs out of frames half way returns an error, and
  the frames it already mapped are never seen again. Silent.
- **Cause.** The mapping loop fails on page 40 of 100 and returns without undoing
  pages 1 to 39. Nothing records them, `SYS_MUNMAP` refuses the address, and they
  come back only when the task exits.
- **Fix** (`paging_map_user_range`, `kernel/paging.c`). Unwind on failure: unmap
  and free every page mapped so far (and the frame that was allocated but not yet
  mapped, when a page table was what ran out), then return -1. The same discipline
  as `task_create_from_file` tearing down a partial address space and `alloc_chain`
  freeing a partial cluster chain: a call that fails costs nothing.

### M4. `munmap` of something never mapped

- **Symptom.** A program frees an address it invented, and the kernel unmaps and
  frees frames belonging to its code or its stack, or frames that belong to nobody
  yet, which are then handed to another task and overwritten. Silent until later.
- **Cause.** `munmap` trusting its address argument.
- **Fix** (`sys_munmap`, `kernel/syscall.c`). Look `(addr, length)` up in the task's
  region array; unless a region starts exactly there with exactly that length,
  return `SYSCALL_ERROR` and change nothing. There is no partial and no overlapping
  unmap in this rung: each is refused. `paging_unmap_user_range` also flushes each
  unmapped page from the TLB, because the caller's own tree is in CR3 and a stale
  translation would let the program keep using a frame that is about to be
  somebody else's. `J.ELF` tries an address never mapped, its own code, its own
  stack, the second page of a region, the wrong length and a double release, and
  checks its memory afterwards.

### M5. `printf` reading past its arguments

- **Symptom.** Garbage output, or a fault, in a program whose format string does
  not match its arguments.
- **Cause.** Varargs cannot be checked at run time. `printf("%s %s", one_thing)`
  reads a second argument that was never passed and gets whatever was in the
  register.
- **Fix**, which is a bound, not a fix, and is documented as such. `%s` checks that
  its pointer lies inside the ring-3 address space (`include/usermem.h`) before
  dereferencing it and never walks past the end of that space looking for a
  terminator; the first unrecognised specifier stops the whole format. **The real
  defence is the compiler**: `user/userlib.h` declares `printf` with
  `__attribute__((format(printf, 1, 2)))`, so every call site is checked at build
  time. A mismatched format that gets past the compiler is undefined. `K.ELF`
  prints every specifier, then a `%q`, a NULL and a low pointer, and checks each
  return value.

### M6. `malloc`'s first call reentering itself

- **Symptom.** Infinite recursion, or a stack overflow off the bottom of `PD[3]`, on
  the very first `malloc` in a program.
- **Cause.** `malloc` finds no free block, calls `SYS_MMAP` for a slab, and the slab
  initialisation calls something that allocates.
- **Fix** (`el_init` and `malloc`, `libc/malloc.c`). The slab path touches nothing
  that allocates: the first block's header and footer are written directly, field
  by field, and the lists are initialised by assignment. A static `initialised`
  flag is set inside the slab path *before* the first block is built, not after,
  so any re-entry sees a heap rather than starting a second first slab. `I.ELF`'s
  first `malloc` is the test; every program's is.

## Alternatives considered

**`brk`.** About thirty lines against ninety, and one number of kernel state per
task instead of a list. Rejected for the structural reason in decision 2: a `brk`
heap cannot return memory from the middle, ever.

**Lazy mapping.** Strictly more efficient (a program that asks for 1MB and touches
one page gets one frame), and the same machinery as growable stacks. Deferred for
the reason in decision 3: today a page fault means something is wrong, and that is
a property worth keeping until a rung is about changing it.

**A different slot for the heap**, above the identity map (a second PDPT entry at
1GB, guaranteed unused since the identity map is capped there). Would have avoided
reserving physical 8-10M. Rejected because the decision to use `PD[4]` had already
been taken and written up, the reservation costs 2MB on a machine with 127MB, and
the correction is one line and one comment where a new PDPT branch would have been a
new mechanism.

**A slab-aware walk** in the ported allocator, so slabs need not be adjacent.
Rejected for this rung: it changes the algorithm the two copies share, which is the
thing the second port was trying not to do, and the adjacency check gives growth in
the only case that arises today.

**Sharing `kernel/heap.c` by compiling one file twice** with the memory source
behind a macro. Rejected: the two copies differ in more than the slab call (no
interrupt guard, no print helpers, `free(NULL)`, alignment), and a file full of
`#ifdef` would be harder to diff than two files that are meant to be diffed.

**Width and precision in `printf`.** `F.c`'s `put5` would have gone. Rejected as
decision 6 says: nothing else needs them, and `put5` is four lines.

## Consequences

What this buys: a program can `malloc` and `free` at run time; a program that
allocates and frees in a loop leaves the free frame count flat; `printf` with six
specifiers, checked at every call site by the compiler; `strchr`, `memcmp` and
`memmove` where the code was open-coding them; and no hand-rolled number printer
anywhere in `user/`.

What it does not buy, all deliberate:

- **No gap reuse in the region list.** A gap below a live region is never handed
  out again, so a program that keeps one region and maps and unmaps repeatedly above
  it climbs the 2MB slot and eventually gets `SYSCALL_ERROR`.
- **No partial or overlapping `munmap`.** A region is released exactly as it was
  handed out, or not at all.
- **No `realloc`.**
- **No lazy mapping.** Asking for memory costs frames whether or not it is touched,
  and asking for the whole slot costs 512 of them.
- **Slabs are never returned to the kernel.** Peak usage is held until the program
  exits.
- **Eight regions per task**, so with a 64KB growth chunk `malloc` can grow seven
  times, 512KB in all, unless a single request forces a larger chunk.
- **`malloc` returns 8-byte alignment**, not the 16 that `max_align_t` asks for:
  enough for every integer and pointer, not for aligned SSE loads.
- **`malloc` is not async-signal-safe.** There is no interrupt guard in ring 3 and
  nothing else in its place; a signal handler must not call it.
- **`printf` has no width or precision**, no length modifiers, no floats, no `%p`.
  A `%lu` is not a mismatch the compiler can flag, because it is valid for the
  standard `printf`; here it stops the format at run time. Cast to `unsigned int`.
- **A mismatched format string is undefined.** The runtime bounds the damage; the
  compiler's format attribute is the defence.
- **Syscall pointer validation is wider and still region-based.** `user_range_ok`
  now bounds a pointer against the top of the heap slot (4-10M), so a malloc'd
  buffer can be handed to a syscall, but it still does not walk the caller's page
  tables, and the heap slot starts empty: an unmapped address inside the span passes
  the check and faults in the kernel when dereferenced, as the unmapped middle of
  the code slot always did.
- **The free-frame baseline moved** by 512 frames, and every transcript that quotes
  one was regenerated.
- **`SYS_MMAP` and `SYS_MUNMAP` print a line when they refuse**, like the other
  calls that reject a bad pointer, so `J.ELF` is noisy by design.

## References

- [`docs/reference/user-memory.md`](../reference/user-memory.md) — how the finished thing works.
- [0010](0010-kernel-heap-ported-from-p5.md) — the first port of the same allocator.
- [0012](0012-per-process-paging.md) — the private user half the heap slot joins.
- [0018](0018-process-lifecycle-exit-and-wait.md) — the teardown that now frees the heap slot.
- [0023](0023-signals.md) — why a signal handler must not call `malloc`.
- `include/usermem.h` — the layout, in one place.
