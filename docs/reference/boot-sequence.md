# Boot sequence: the 32 to 64 long-mode climb

`boot/boot.asm` receives control from GRUB/QEMU in 32-bit protected mode with
paging off. Long mode requires paging on, so the climb builds valid page tables
first and only then enables paging. All code before the far jump runs under
`[bits 32]`; everything after runs under `[bits 64]`.

The order below is the order in the source, and the order matters: several steps
are preconditions for later ones.

## The climb, step by step

1. **Zero the tables.** Clear all 12 KB (PML4 + PDPT + PD) by hand. The
   bootloader is not trusted to have zeroed `.bss`, and a single stray byte that
   sets a present bit is a bogus mapping and a silent triple fault. The three
   tables are contiguous in `.bss`, so one `rep stosd` clears all of them.
2. **PML4[0] points at the PDPT**, marked present and writable. Only entry 0 is
   used: the whole identity map lives in the low region the first PML4 entry
   covers.
3. **PDPT[0] points at the PD**, present and writable.
4. **PD[0..15] identity-map the first 32MB** with 2MB pages. Entry N maps virtual
   `N * 0x200000` to the same physical address (fake address equals real
   address). The `PS` (huge) bit is mandatory: it tells the CPU this entry is a
   2MB page and to stop the walk here, instead of chasing a fourth-level page
   table that does not exist. The first four entries are written explicitly, one
   per line, because their privilege differs: `PD[0]` and `PD[1]` (`0x000000` to
   `0x3FFFFF`, the kernel, the VGA text buffer at `0xB8000`, the kernel at 1M)
   are kernel-only, and `PD[2]` and `PD[3]` (`0x400000` to `0x7FFFFF`) carry the
   user bit for the ring-3 code and stack regions. The remaining twelve,
   `PD[4..15]` (`0x800000` to `0x1FFFFFF`), are filled by a loop as kernel-only
   spare RAM: the memory C works in before it has parsed the Multiboot map, and
   where the frame allocator's pool begins. The map is 32MB rather than the 8MB
   originally decided because C cannot learn how much RAM there is until it runs,
   and it cannot run until this map exists; C only ever adds entries at 32MB and
   up ([memory-map.md](memory-map.md)).
5. **Enable PAE** by setting CR4 bit 5. Physical Address Extension is required
   for long mode. Without it, enabling paging in the next steps would just turn
   on 32-bit paging.
6. **Load CR3** with the physical base of the PML4. Because the tables are
   identity-mapped, the label's address is already its physical address.
7. **Set EFER.LME.** Read MSR `0xC0000080` with `rdmsr`, set bit 8 (Long Mode
   Enable), write it back with `wrmsr`. This only arms long mode; it does not
   activate it.
8. **Enable paging (CR0.PG).** Setting bit 31 is the moment long mode activates
   and the moment address translation starts applying to the CPU's own
   instruction fetches. This is why step 4 had to be correct first: the very next
   instruction is fetched through the identity map just installed.
9. **Load the bootstrap GDT and far-jump into 64-bit code.** The CPU is now in
   long mode but still in a 32-bit (compatibility) code segment. A far jump that
   reloads CS with a 64-bit code selector is the only way to reach true 64-bit
   execution. The bootstrap GDT is local to `boot.asm` and exists only to make
   this jump legal (see [gdt.md](gdt.md) and
   [decision 0003](../decisions/0003-bootstrap-gdt-separate-from-kernel-gdt.md)).
10. **In 64-bit code**, load the flat data selector into the segment registers,
    set `RSP = stack_top`, and `call kernel_main` (which takes no arguments).
11. **Halt.** `kernel_main` should never return; if it does, the code falls into
    a `cli` + `hlt` loop.

## Why identity mapping is required at the moment CR0.PG is set

Before paging, the CPU fetches instructions using physical addresses. The instant
`CR0.PG` is set (step 8), every address, including the address of the next
instruction to fetch, goes through the page tables. If those tables do not map the
currently-executing code to itself, the next fetch lands somewhere invalid and the
CPU triple faults on its own instruction stream. Identity mapping the region that
holds the running code (the first 32MB, which covers the kernel at 1M) guarantees
the address does not change across the transition. That is the whole reason the
tables are built before paging is enabled rather than after.

## Related

- The bootstrap GDT and the kernel GDT that replaces it: [gdt.md](gdt.md).
- Where the page tables and stacks live: [memory-map.md](memory-map.md).
- Why 2MB pages, and the original 8MB identity map:
  [decision 0002](../decisions/0002-2mb-pages-and-8mb-identity-map.md). The map
  was widened to 32MB when the Multiboot map began extending it
  ([decision 0009](../decisions/0009-read-multiboot-map-extend-identity-map.md));
  the ADR keeps its name and its body, and its Status says so.
- The concept behind paging and long mode:
  [`../../learnings/`](../../learnings/README.md).
