# The kernel heap

This is the factual description of TownOS's kernel heap, read from
`kernel/heap.c`, `kernel/heap.h`, and the `alloc_frames_contiguous` helper in
`kernel/memory.c`. It is an explicit-free-list allocator with boundary tags and
coalescing, ported from the CMSC216 p5 `el_malloc`. For why it was ported rather
than rewritten, see
[decisions/0010](../decisions/0010-kernel-heap-ported-from-p5.md).

The heap is a layer on top of the frame allocator. The frame allocator
(`kernel/memory.c`) hands out whole 4KB frames; the heap carves those frames into
arbitrary-size blocks. Keep the two straight: frames are pages, `kmalloc` is
bytes on pages.

**Ported twice.** `libc/malloc.c` is this allocator again, in ring 3, with
`SYS_MMAP` where `alloc_frames_contiguous` is here and no interrupt guard; the block
layout, the two lists, the split, the coalesce and every function name are
identical, so the two files can be diffed and a bug found in one can be looked for
in the other by name. Both say so at the top, and the differences are enumerated at
the top of `libc/malloc.c`. See [user-memory.md](user-memory.md) and
[decisions/0024](../decisions/0024-user-memory-and-libc.md).

## The block layout: header, user data, footer

Every block is a header, then the user's bytes, then a footer:

```
+----------------+-------------------------+------------+
| el_blockhead_t |     user data (size)    | el_blockfoot_t |
| size,state,    |  <- kmalloc returns here |   size      |
| next,prev      |                         | (repeat)    |
+----------------+-------------------------+------------+
```

- The **header** (`el_blockhead_t`) holds the block's `size`, its `state`
  (available or used), and the `next`/`prev` pointers that thread it onto one of
  the free-list chains. `kmalloc` returns the address just past the header.
- The **footer** (`el_blockfoot_t`) repeats the size. It is a *boundary tag*.

`EL_BLOCK_OVERHEAD` is the header plus footer (40 bytes on this build: a 32-byte
header, an 8-byte footer). Every block pays it.

## Why the footer exists: walking backward

To coalesce a freed block with the free block *below* it in memory, you have to
find that lower block's header. Without help, that means scanning the whole heap
from the start, because block sizes vary and you cannot guess where the previous
header begins. The footer solves it in O(1): step back from your header by
`sizeof(el_blockfoot_t)` to land on the previous block's footer, read the size it
stored there, and jump back that far to reach its header. That is exactly what
`el_block_below` does. Walking *up* (`el_block_above`) needs no footer, just add
your own size plus overhead, but walking *down* is impossible without it. The
footer is the price paid on every block to make downward coalescing cheap.

## Split and coalesce are inverses

Two operations reshape the heap, and they are mirror images.

- **Split** (`el_split_block`): when `kmalloc` finds a free block bigger than the
  request, it cuts a header/footer pair into the middle. The low part shrinks to
  the requested size and is returned; the high part becomes a new, smaller free
  block. One block becomes two, adding one header/footer pair.
- **Coalesce** (`el_merge_block_with_above`): when a block is freed, if the block
  above it is also free, the two merge into one. The lower block absorbs the
  upper's size plus the overhead of the header/footer pair that used to divide
  them. Two blocks become one, removing one header/footer pair.

Split adds a divider; coalesce removes one. `el_free` coalesces in both
directions: it merges with the block above, then finds the block below (via the
footer) and merges that into the freed block too, so a run of adjacent frees
collapses back into one large free block rather than leaving fragments.

## The two lists

There are two doubly-linked lists, `avail` (free blocks) and `used`, each with
dummy `beg`/`end` nodes. The dummies mean insertion and removal never special-case
the ends: there is always a node before and after, so the pointer updates are
uniform. `kmalloc` first-fits down `avail` (`el_find_first_avail`), moves the
chosen block to `used`, and puts any split-off remainder back on `avail`. `kfree`
does the reverse and then coalesces.

## The frame-allocator seam

The p5 original got its slab from `mmap` and grew it with more `mmap` at a
requested address. There is no `mmap` here. The seam is `alloc_frames_contiguous`
(`kernel/memory.c`):

- `heap_init()` asks for a 16-page (64KB) contiguous run and builds the first
  free block spanning it.
- When `el_malloc` finds nothing large enough, the `kmalloc` wrapper grows the
  heap: it asks for enough contiguous pages to cover the request (at least a
  16-page chunk), appends them, and retries.

`alloc_frames_contiguous(n)` works because the frame bitmap is linear and the
whole pool is identity-mapped: `n` consecutive clear bits are `n` contiguous,
mapped, writable physical frames. That is what lets the ported code keep its
assumption that the heap is one contiguous slab bounded by a single
`heap_start`/`heap_end` pair.

**Growth must stay adjacent.** The boundary-tag walk steps through memory
linearly and is bounded by that single `heap_end`. A growth run is required to
sit immediately above the current `heap_end`; because the heap is the only
consumer of the frame allocator after `memory_init` and the bitmap is scanned
bottom-up, the run always comes back adjacent. A non-adjacent run is refused and
its frames reclaimed, not spliced in: a disjoint second region would leave a gap
that `el_block_above`/`el_block_below` would walk straight into, reading garbage.
Keeping the heap one contiguous slab is the invariant that makes the O(1)
boundary-tag walk safe.

## Interrupt safety

The free lists are shared mutable state, and TownOS preempts. The timer fires
100 times a second, and its handler (or the scheduler it drives) could call
`kmalloc` while another `kmalloc` is halfway through relinking a list. That
corrupts the list.

`kmalloc` and `kfree` therefore bracket their critical section with a
save-and-restore interrupt guard (`irq_save`/`irq_restore` in `kernel/heap.c`):
read RFLAGS with `pushfq`, `cli` to disable interrupts, do the work, then restore
the *saved* flags. It is not an unconditional `sti`: `kmalloc` may be called from
inside an interrupt handler where interrupts are already off, and forcing them
back on there would re-enter the very code the guard protects. Save the previous
state, restore exactly that.

## Interface

- `void heap_init(void)` builds the initial slab. Call it once from
  `kernel_main`, after `memory_init()` (the heap draws its pages from the frame
  allocator, which must be up first).
- `void *kmalloc(size_t size)` / `void kfree(void *ptr)` are the public
  allocation calls.
- `heap_print_stats()`, `heap_avail_count()`, `heap_total_bytes()` are debug and
  introspection helpers kept from the p5 source and made kernel-native.

## Known limitations

- **First-fit fragmentation.** Accepted by design; it is the p5 algorithm
  unchanged.
- **A global allocation lock in spirit.** The interrupt guard serialises all
  allocation. Fine at 100 Hz with short critical sections, but it would need
  revisiting under SMP (which TownOS does not have).
- **One shared address space.** The heap allocates; it does not isolate. It sits
  on the same single identity-mapped space as everything else. See
  [memory-map.md](memory-map.md).

## Related

- The frame allocator underneath: [memory-map.md](memory-map.md).
- The decision record: [decisions/0010](../decisions/0010-kernel-heap-ported-from-p5.md).
- The second port, as the ring-3 `malloc`: [user-memory.md](user-memory.md).
- The fixed task table this unblocks: [scheduling.md](scheduling.md).
