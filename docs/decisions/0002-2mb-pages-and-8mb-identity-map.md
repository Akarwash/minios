# 0002 - Use 2MB pages and identity-map the first 8MB

## Status

Accepted. Partly superseded — the decision stands, one figure in the body does not.

- **The size of the boot identity map** was widened from 8MB to 32MB by
  [0009](0009-read-multiboot-map-extend-identity-map.md) (commit `0ac8f8a`).
  `boot/boot.asm` now fills sixteen PD entries, `PD[0..3]` explicitly (kernel,
  kernel, user code, user stack) and `PD[4..15]` by a loop as kernel-only spare
  RAM, so C has memory to work in before it has parsed the Multiboot map and
  extends the map from 32MB up to real RAM. The body and this file's name keep the
  original 8MB, which was the decision at the time.

2MB pages and the three-level walk are unchanged. See
[reference/boot-sequence.md](../reference/boot-sequence.md) and
[reference/memory-map.md](../reference/memory-map.md) for the current state.

## Context

Entering long mode requires paging to be enabled, and the page tables must map the
memory that holds the code executing at the moment paging turns on. The boot code
therefore needs a valid, correct set of page tables before it can enter 64-bit
mode at all. The question was how to structure that initial map with the least
that can go wrong.

## Decision

Identity-map the first 8MB using 2MB pages:

- Use 2MB pages by setting the `PS` (huge) bit on page-directory entries. This
  stops the page walk at the page directory, so only three table levels are
  needed (PML4, PDPT, PD) instead of four; there is no PT level.
- Use four PD entries covering `0x000000` to `0x7FFFFF` (8MB), identity-mapped so
  that virtual address equals physical address across the transition.

## Consequences

- No PT (fourth-level) table is needed, which means less structure to build and
  fewer places for a mistake.
- 8MB comfortably covers the VGA text buffer at `0xB8000` and the kernel loaded at
  1M, with headroom.
- Because the map is identity, the address of the executing code does not change
  at the instant `CR0.PG` is set, which is what keeps the CPU from triple-faulting
  on its own next instruction fetch.
- The page tables are explicitly zeroed in code rather than trusting the
  bootloader to zero `.bss`. A single stray present bit would be a bogus mapping
  and a silent triple fault, so all 12KB is cleared by hand.
- This is a flat, single-address-space map with no protection between regions.
  Real per-process address spaces and a proper virtual-memory layer are future
  work. The frame allocator's pool already extends past the mapped 8MB (see
  [../reference/memory-map.md](../reference/memory-map.md)).
