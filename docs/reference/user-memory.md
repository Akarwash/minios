# User memory and the ring-3 libc

A ring-3 program can ask the kernel for memory at run time, `malloc` and `free`
it, and print with `printf`. This page documents how: the heap slot in every
task's address space, the two syscalls that fill and empty it, the per-task region
table, how a region is mapped and unmapped, how it is freed at exit, the allocator
built on top, `printf`, and how `libc/` reaches ring 3 at all. Read from
`include/usermem.h`, `kernel/syscall.c`, `kernel/paging.c`, `kernel/memory.c`,
`libc/malloc.c`, `libc/printf.c`, `user/userlib.h` and the `Makefile`. For the
rationale and the six ways it goes wrong, see
[decision 0024](../decisions/0024-user-memory-and-libc.md).

## The address space, in one place

`include/usermem.h` is the single statement of the ring-3 layout, included by the
kernel (through `kernel/memory.h`) and by every program (through `user/userlib.h`):

| Slot | Constant | Range | Holds |
|------|----------|-------|-------|
| `PD[2]` | `USER_CODE_BASE` | `0x400000`-`0x5FFFFF` | the loaded program image, mapped by the ELF loader |
| `PD[3]` | below `USER_STACK_LIMIT` | `0x600000`-`0x7FFFFF` | the stack, 256KB at the top, mapped at load |
| `PD[4]` | `USER_HEAP_BASE`..`USER_HEAP_LIMIT` | `0x800000`-`0x9FFFFF` | `SYS_MMAP` regions, lowest first; **empty at load** |

`USER_SPACE_END` (`0xA00000`) is one past the last address a program may name, and
is the bound every kernel pointer check and `printf`'s `%s` check use.

**`PD[4]` was the kernel's identity map of physical 8-10M** before this rung (see
[memory-map.md](memory-map.md)), and that range is the first the frame allocator
hands out. So `memory_init` now reserves physical 4-10M rather than 4-8M, and the
by-value kernel clone in `paging_create_address_space` leaves `PD[4]` absent along
with `PD[2]` and `PD[3]`. The reservation costs 512 frames, which is why the
free-frame baseline on the reap line dropped from 30585 to about 30072. See
[paging.md](paging.md) for the three-slot clone and teardown.

## The two syscalls

| Number | Name | Arguments | Returns |
|--------|------|-----------|---------|
| 19 | `SYS_MMAP` | RDI = length in bytes | a page-aligned address in the heap slot, or -1 |
| 20 | `SYS_MUNMAP` | RDI = address, RSI = length | 0, or -1 |

Anonymous memory only: no file behind it, no sharing, no protection flags. Neither
call blocks, so neither carries the RAX discipline that `SYS_READKEY`, `SYS_READ`,
`SYS_WRITE` and `SYS_WAIT` do ([blocking.md](blocking.md)); both compute an answer
and hand it to the dispatcher.

**`SYS_MMAP(length)`** rejects zero and anything larger than the 2MB slot (checked
*before* rounding, so the rounding cannot overflow), rounds `length` up to whole
pages, and places the region at the lowest page-aligned address above every region
the task still holds, starting at `USER_HEAP_BASE`. It rejects a region that would
cross `USER_HEAP_LIMIT`, a running total across the task's regions that would exceed
the slot, and a ninth region (see the table below). Then it allocates, **zeroes**,
and maps every page before returning: nothing is lazy, and a region reads as all
zero on its first touch. A refusal prints one `syscall: SYS_MMAP rejected ...` line,
as a rejected out-of-bounds buffer does elsewhere.

**`SYS_MUNMAP(addr, length)`** rounds `length` the same way and releases the region
whose recorded base and length match **exactly**. Anything else is refused and
changes nothing: an address inside the slot that was never mapped, the program's
own code or stack, part of a region, the wrong length, a region already released.
There is no partial and no overlapping unmap.

**Placement and gaps.** A region freed *below* a live region leaves a gap that is
never reused in this rung; only the space above the highest live region is handed
out. A program that keeps one region and maps and unmaps repeatedly above it
climbs the slot and eventually gets -1. When nothing is held, placement starts at
the bottom again. `J.ELF` checks all of this.

The ring-3 wrappers are `sys_mmap` and `sys_munmap` in `user/userlib.h`. Most
programs never call them: `malloc` does.

## The region table

```c
#define MAX_REGIONS 8
typedef struct {
    uint64_t base;        // page-aligned start, inside the heap slot
    uint64_t length;      // whole pages; 0 means the slot is unused
} region_t;
// on task_t
region_t regions[MAX_REGIONS];
```

A fixed array rather than a linked list, because a list of memory regions would
need somewhere to allocate its nodes from, in the middle of the thing that hands out
memory, and a fixed array cannot fragment its own bookkeeping. It is read for
exactly one purpose: validating `SYS_MUNMAP`. **Freeing at exit does not walk it**;
see below. `task_register` clears it; `kmalloc` does not zero, and a stale length
here would make `SYS_MUNMAP` accept an address that was never mapped.

## Mapping and unmapping a range

`paging_map_user_range(as, base, length)` (`kernel/paging.c`) is what `SYS_MMAP`
is built on. For each page it takes a frame from `alloc_frame`, zeroes it through
the identity map (a frame's last owner may have been another task's stack, and
handing that to a program would leak across the isolation this module exists for),
and installs it with `paging_map_page` as present, writable and user. **On failure
it unwinds**: every page mapped so far is unmapped and freed, and the frame that
was allocated but not yet mapped (when a page *table* was what ran out) is freed by
hand, so a `SYS_MMAP` that fails costs the task nothing. Without that, a program
retrying a failing request in a loop would strand a few dozen frames per attempt.

`paging_unmap_user_range(as, base, length)` is the inverse: for each present page
it frees the frame, clears the leaf entry, and **flushes that translation with
`invlpg`**. The flush matters because when `SYS_MUNMAP` runs, `as` is the tree in
CR3 and the CPU may have the translation cached; without the flush the program
could go on using a frame that `free_frame` has just handed to somebody else. Pages
that were never mapped are skipped, which is what lets the map unwind a partial run
through the same function. The page table under `PD[4]` stays in place until the
tree is torn down.

## Freeing at exit

`task_exit` closes descriptors and knows nothing about regions. The heap slot comes
back with the rest of the address space: `paging_destroy_address_space` frees the
user half by page-directory index, and the index list, `user_pd_slots` in
`kernel/paging.c`, is now three entries (code, stack, heap). That array is read by
both the clone and the teardown and is **the single place that has to change** if a
fourth user region is ever added. `destroy_page_table` walks the page table under
each slot and frees the leaves, not just the table, so a region a program never
released, which is every `malloc` slab, goes back to the pool when the program
exits. Ten consecutive runs of `run i.elf` printing the same free-frame count is
the test of this.

## malloc, free, calloc

`libc/malloc.c` is `kernel/heap.c` ported a second time: the CMSC216 p5 explicit
free list with boundary tags and coalescing ([heap.md](heap.md)), with `SYS_MMAP`
where `alloc_frames_contiguous` was and every function name unchanged, so the two
files can be diffed and a bug found in one can be looked for in the other. Both say
so at the top. The differences, each marked at its site:

- **Slabs come from `SYS_MMAP`.** The first `malloc` of a program maps one 64KB
  slab (the kernel heap's figure) at the bottom of the heap slot and builds the
  first free block in it. Running out maps another 64KB (or more, if one request
  needs it).
- **Slabs must be adjacent.** The ported code assumes one contiguous span: "the
  block above" is a size added to a pointer, bounded by a single `heap_end`. Growth
  therefore checks that the new slab landed exactly at `heap_end`, as the kernel
  copy does, and gives a non-adjacent slab straight back with `SYS_MUNMAP`; `malloc`
  then returns NULL rather than ever walking into a gap. It works because, when
  `malloc` is the only caller of `SYS_MMAP` in the program, the next region's
  address *is* `heap_end`. A program that maps something of its own between two
  growths breaks the adjacency and gets NULL from the next growth.
- **Slabs are never returned.** `free` puts a block on the available list; nothing
  calls `SYS_MUNMAP` on a slab. Peak usage is held until the program exits.
- **Eight regions per task** means seven growths: 512KB of heap with 64KB chunks,
  unless a single request forces a larger chunk.
- **No interrupt guard**, and nothing in its place: preemption switches to another
  task with its own heap, but a signal handler that calls `malloc` while `malloc`
  is half way through a relink corrupts the lists. `malloc` is not async-signal-safe.
- **`malloc` rounds requests to a multiple of 8**, so payloads are 8-byte aligned
  (the kernel's `kmalloc` rounds nothing). Not 16: enough for integers and
  pointers, not for aligned SSE loads.
- **`free(NULL)` is a no-op**, as C requires. Freeing anything else `malloc` did not
  return, or freeing twice, corrupts the heap; the allocator prints one `ERROR` line
  for the cases it can detect.
- **`calloc(count, size)`** checks the product for overflow and clears the block,
  always: a fresh slab arrives zeroed from the kernel, but a reused block holds its
  last owner's bytes.
- **The first call initialises the heap from inside `malloc`.** The slab path
  touches nothing that allocates (the first block's header and footer are written
  directly), and the `initialised` flag is set before the block is built, not after.
- **No `realloc`.** No stats or debug printers were ported either.

The prototypes are in `user/userlib.h`.

## printf

`int printf(const char *fmt, ...)`, in `libc/printf.c`, to fd 1. Six specifiers and
nothing else:

| Specifier | Argument | Output |
|-----------|----------|--------|
| `%d` | `int` | decimal, with a sign if negative |
| `%u` | `unsigned int` | decimal |
| `%x` | `unsigned int` | lowercase hex, no prefix |
| `%s` | `const char *` | the string |
| `%c` | `int` (a character) | the character |
| `%%` | none | `%` |

No width, no precision, no length modifiers, no floats, no `%p`. It returns the
number of bytes written, or -1.

**An unrecognised specifier stops the format.** What came before it is written,
nothing after it is, and the call returns -1; the specifier is neither printed raw
nor skipped, because either would let a mistake pass unnoticed. A `%` at the very
end of a format counts. So does `%lu`: a length modifier is valid for the standard
`printf`, so the compiler cannot flag it, and here it stops the format at run time.
Cast an `unsigned long` to `unsigned int` at the call site, which is what the shell
does for sizes and cluster counts.

**`%s` validates its pointer.** Before dereferencing, it checks the pointer lies
inside the ring-3 address space (`USER_CODE_BASE` up to `USER_SPACE_END`), and it
never walks past `USER_SPACE_END` looking for a terminator. A NULL, or any kernel
address, stops the format. An unmapped address *inside* the space is
indistinguishable from a good one here and faults as it would anywhere else in the
program.

**The compiler is the real defence.** `user/userlib.h` declares `printf` with
`__attribute__((format(printf, 1, 2)))`, so every call site's arguments are checked
against its format string at build time and a mismatch is a warning. A varargs
function cannot check this at run time: `printf("%s %s", one_thing)` reads a second
argument that was never passed. A mismatched format that gets past the compiler is
undefined; the runtime checks above bound the damage and no more. `K.ELF`'s `%q`
line has to silence the compiler's check to reach the runtime one, which is the
evidence the compile-time check is working.

**Output goes through `sys_write_all`** (`user/userlib.h`), the loop that retries a
partial `SYS_WRITE` until everything is written, factored out of `sys_print` so
both share one loop. A pipe takes only what fits and the console moves at most the
kernel's staging buffer per call, so a long line into a full pipe waits rather than
truncates. `printf` stages output in a 128-byte buffer on its own stack (it must not
allocate, and a static buffer would break under a signal handler that printed), and
flushes when it fills and at the end.

**The deletion is the point.** `user/shell.c`'s `print_uint` (ten call sites) and
the `print_ulong` copies in `COUNT.c`, `H.c` and `ONCE.c` are gone. `F.c`'s `put5`
stays: it zero-pads to five digits, which needs a field width.

## How libc reaches ring 3

A user program is a single translation unit linked against nothing, so `libc/` used
to be unreachable from it. The `Makefile` now compiles `libc/` **twice**: the kernel
objects as before (`libc/mem.o`, `libc/string.o`, with the kernel flags), and
`.user.o` objects from `USER_LIBC_SOURCES` (`mem.c`, `string.c`, `malloc.c`,
`printf.c`) with the user flags, linked into every program after the trampoline.
`malloc.c` and `printf.c` are ring-3 only. `user/userlib.h` includes `libc/string.h`
and `libc/mem.h`, so a program gets `strlen`, `strcmp`, `strcpy`, `strchr`,
`memcpy`, `memmove`, `memset` and `memcmp` along with the syscall wrappers. Programs
grew from about 13KB to about 22KB each, since nothing is garbage-collected at link.

`strchr`, `memcmp` and `memmove` were added for code that was open-coding them: the
shell's "does this line contain a pipe" test, `fs/fat32.c`'s fixed-length 8.3 name
compare, and `drivers/screen.c`'s scroll, which shifts every row up over itself and
had been using `memcpy` on overlapping ranges, working only because the copy
happens to run forward. `strlen` and `strcmp` took `const` pointers, and
`memcpy`/`memset` took the standard signatures.

## What a run looks like

`run i.elf` allocates 300 blocks, verifies them, frees them in a shuffled order,
reuses the whole heap in one 200000-byte block, and asks for 3MB, which is refused:

```
> run i.elf
run: started i.elf
syscall: SYS_MMAP rejected a length outside the 2MB heap slot
malloc: heap grow: SYS_MMAP refused another slab
I: 300 blocks allocated, verified, freed in shuffled order, and reused
reap (wait):    task 19 exited (status 0), free frames: 30072, heap used: 1448
run: i.elf exited with status 0
```

Ten consecutive runs printed `free frames: 30072` and `heap used: 1448` every time,
which is the M2 and M3 test: I exits holding four slabs it never released, and all
of them come back. `run j.elf` is the abuse test and `run k.elf` the `printf` test;
both are in [user/tests/README.md](../../user/tests/README.md) with their output.
Over the whole verification session `-d int` showed only timer (`v=40`), keyboard
(`v=41`) and syscall (`v=50`) vectors: no page fault, no general protection fault,
no double fault.

## Limitations

- No gap reuse below a live region; no partial or overlapping `SYS_MUNMAP`; no
  `realloc`; no lazy mapping; slabs never returned; eight regions per task.
- `malloc` is 8-byte aligned and not async-signal-safe.
- `printf` has no width, precision, length modifiers, floats or `%p`.
- Syscall pointer validation is region-based over 4-10M and does not walk the
  caller's page tables; an unmapped address inside the span faults in the kernel.
- A program that faults (a stray pointer, a `%s` on an unmapped address) is
  handled the way ring-3 faults always were: see [idt.md](idt.md).

## Related

- The decision and the six failure modes:
  [decision 0024](../decisions/0024-user-memory-and-libc.md).
- The three-slot clone and teardown: [paging.md](paging.md).
- The physical reservation of 8-10M: [memory-map.md](memory-map.md).
- The kernel copy of the allocator: [heap.md](heap.md).
- The twenty-one calls and the pointer checks: [syscalls.md](syscalls.md).
- The fixtures: [user/tests/README.md](../../user/tests/README.md).
