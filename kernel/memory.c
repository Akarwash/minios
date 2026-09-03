#include "memory.h"
#include "multiboot.h"
#include "../libc/mem.h"
#include "../drivers/screen.h"

// Manage a region of physical RAM as fixed-size frames, tracked by a bitmap
// (one bit per frame: 1 = used, 0 = free). The pool starts safely above the
// kernel (which loads at 1MB) and the ring-3 region, and now runs up to the real
// top of RAM rather than an invented constant.
//
// Design choice: the Multiboot map parsing lives here, in memory.c, rather than
// a separate multiboot.c. Reading the map, extending the identity map, and
// sizing the frame pool are one concern (how much RAM there is and making it
// usable), so keeping them together reads better than splitting the walk across
// files. multiboot.h holds only the structure and constant definitions.
#define MEMORY_START   0x400000                 // 4MB, frame pool base

// The single pd_table is one 4KB page: 512 entries of 2MB each = 1GB. That is a
// hard ceiling on how much RAM we can identity-map without building more page
// directories, which is deliberately out of scope. RAM above 1GB is ignored.
#define TWO_MB         0x200000
#define ONE_GB         0x40000000ULL            // 1GB identity-map / pool ceiling
#define PD_ENTRIES     512
#define BOOT_MAP_TOP   0x2000000                // 32MB: what boot/boot.asm maps

// Page-directory entry flag bits. These MUST match boot/boot.asm (PG_PRESENT,
// PG_WRITABLE, PG_HUGE); the entries C writes are the same shape the boot climb
// wrote, just for higher addresses.
#define PG_PRESENT     (1 << 0)
#define PG_WRITABLE    (1 << 1)
#define PG_HUGE        (1 << 7)

// The boot page directory, defined in boot/boot.asm and exposed there as global.
// Extending the identity map is just filling more of its entries.
extern uint64_t pd_table[PD_ENTRIES];

// Largest pool the 1GB ceiling allows, used to size the static bitmap. The pool
// runs from MEMORY_START to at most 1GB, so this is the worst-case frame count.
#define MAX_FRAMES     ((ONE_GB - MEMORY_START) / FRAME_SIZE)

static uint8_t  frame_bitmap[MAX_FRAMES / 8];   // bit per frame, statically sized
static uint32_t num_frames;                     // actual frames in the pool (from real RAM)
static uint64_t pool_top;                       // top of the pool (capped at 1GB)

static void set_bit(uint32_t frame)   { frame_bitmap[frame / 8] |=  (1 << (frame % 8)); }
static void clear_bit(uint32_t frame) { frame_bitmap[frame / 8] &= ~(1 << (frame % 8)); }
static int  test_bit(uint32_t frame)  { return frame_bitmap[frame / 8] & (1 << (frame % 8)); }

// Walk the Multiboot map and return the highest usable (type 1) physical address.
// Sets *have_map to 0 if the map is absent (flags bit 6 clear), in which case the
// return value is meaningless and the caller must fall back.
static uint64_t mmap_top_of_ram(uint64_t mbi_addr, int *have_map) {
    multiboot_info_t *mbi = (multiboot_info_t *)mbi_addr;
    if (!(mbi->flags & MULTIBOOT_INFO_MEM_MAP)) {
        *have_map = 0;
        return 0;
    }
    *have_map = 1;

    uint64_t top = 0;
    uint64_t addr = mbi->mmap_addr;
    uint64_t end  = (uint64_t)mbi->mmap_addr + mbi->mmap_length;
    while (addr < end) {
        multiboot_mmap_entry_t *e = (multiboot_mmap_entry_t *)addr;
        if (e->type == MULTIBOOT_MEMORY_AVAILABLE) {
            uint64_t region_top = e->base_addr + e->length;
            if (region_top > top) {
                top = region_top;
            }
        }
        // Stride gotcha: the size field does NOT count itself, so the next entry
        // is at addr + size + sizeof(size), not addr + sizeof(entry). Getting
        // this wrong is a classic Multiboot bug that walks into garbage.
        addr += e->size + sizeof(e->size);
    }
    return top;
}

// Extend the identity map to cover [BOOT_MAP_TOP, map_top), rounded up to a 2MB
// boundary and capped at 1GB, then flush the TLB.
//
// DANGER: this edits the very page tables the CPU is walking to fetch these
// instructions. It is safe ONLY because every entry it writes is at 32MB or
// above (PD[16..]), never the low entries PD[0..15] that map the running kernel.
// Modify a low entry and the ground moves under the CPU's own instruction fetch,
// and it triple faults. Do not lower the start index.
static void extend_identity_map(uint64_t map_top) {
    uint64_t top = (map_top + (TWO_MB - 1)) & ~((uint64_t)TWO_MB - 1);  // round up to 2MB
    if (top > ONE_GB) {
        top = ONE_GB;
    }

    uint32_t first = BOOT_MAP_TOP / TWO_MB;   // 16
    uint32_t last  = (uint32_t)(top / TWO_MB);
    for (uint32_t i = first; i < last; i++) {
        pd_table[i] = ((uint64_t)i * TWO_MB) | PG_PRESENT | PG_WRITABLE | PG_HUGE;
    }

    // The CPU caches address translations in the TLB; the PD entries just written
    // will not take effect until that cache is invalidated. Reloading CR3 with
    // its own value flushes the whole TLB. This is invisible and easy to miss:
    // skip it and the new high frames read back as not-present and fault.
    __asm__ __volatile__("mov %%cr3, %%rax; mov %%rax, %%cr3" ::: "rax", "memory");
}

uint64_t memory_detect_and_map(uint64_t mbi_addr) {
    int have_map = 0;
    uint64_t top = mmap_top_of_ram(mbi_addr, &have_map);

    if (!have_map) {
        // No memory map from the bootloader: do not read garbage. Fall back to
        // the fixed window the boot climb already mapped and warn loudly.
        print_string("WARNING: no Multiboot memory map; falling back to 32MB.\n");
        top = BOOT_MAP_TOP;
    }

    pool_top = (top > ONE_GB) ? ONE_GB : top;
    extend_identity_map(pool_top);

    return have_map ? top : 0;   // 0 signals the caller that the fallback was used
}

// Mark every frame overlapping [start, end) as used, clamped to the pool.
static void reserve_range(uint64_t start, uint64_t end) {
    if (end <= MEMORY_START || start >= pool_top) {
        return;
    }
    if (start < MEMORY_START) {
        start = MEMORY_START;
    }
    if (end > pool_top) {
        end = pool_top;
    }
    uint32_t first = (uint32_t)((start - MEMORY_START) / FRAME_SIZE);
    uint32_t last  = (uint32_t)((end - MEMORY_START + FRAME_SIZE - 1) / FRAME_SIZE);  // round up
    for (uint32_t i = first; i < last && i < num_frames; i++) {
        set_bit(i);
    }
}

// Reserve every non-usable range the map reported (type != 1: BIOS/EBDA, ACPI,
// reserved holes). Walk the map, do not hardcode: the reserved layout is the
// machine's, not ours.
static void reserve_map_holes(uint64_t mbi_addr) {
    multiboot_info_t *mbi = (multiboot_info_t *)mbi_addr;
    if (!(mbi->flags & MULTIBOOT_INFO_MEM_MAP)) {
        return;
    }
    uint64_t addr = mbi->mmap_addr;
    uint64_t end  = (uint64_t)mbi->mmap_addr + mbi->mmap_length;
    while (addr < end) {
        multiboot_mmap_entry_t *e = (multiboot_mmap_entry_t *)addr;
        if (e->type != MULTIBOOT_MEMORY_AVAILABLE) {
            reserve_range(e->base_addr, e->base_addr + e->length);
        }
        addr += e->size + sizeof(e->size);   // same stride rule as the top-of-RAM walk
    }
}

void memory_init(uint64_t mbi_addr) {
    memset(frame_bitmap, 0, sizeof(frame_bitmap));

    // Size the pool from the real (capped) top of RAM that memory_detect_and_map
    // computed. Every frame in [MEMORY_START, pool_top) is now identity-mapped,
    // so no frame the allocator hands out is unmapped.
    num_frames = (pool_top > MEMORY_START)
               ? (uint32_t)((pool_top - MEMORY_START) / FRAME_SIZE)
               : 0;
    if (num_frames > MAX_FRAMES) {
        num_frames = MAX_FRAMES;
    }

    // Reserve the physical frames under the three ring-3 slots, 4-10M.
    //
    // 4-8M is historical: those frames held the live user program and its stacks
    // when the boot tree's user huge pages were the only user mapping, and the
    // reservation was never lifted. 8-10M IS NEW AND LOAD-BEARING. boot/boot.asm
    // identity-maps physical 8-10M through PD[4], and PD[4] is now every task's
    // PRIVATE heap slot (kernel/paging.c leaves it absent in the kernel clone and
    // SYS_MMAP fills it with 4KB user pages). The kernel reaches every frame it
    // owns through the identity map, and after scheduler_start the identity map
    // it walks is whichever task tree is in CR3, so a kernel pointer into physical
    // 8-10M no longer reaches physical 8-10M: it resolves through that task's heap
    // page table, or faults on an empty slot.
    //
    // WHAT BREAKS WITHOUT THIS LINE: alloc_frame hands out the lowest free frame,
    // and with only 4-8M reserved that is 0x800000 itself. The first task's PML4
    // (or the kernel heap's slab) would be placed at physical 8M, addressed as
    // 0x800000, and the moment a task tree with an absent PD[4] was loaded every
    // kernel access to it would fault; with a heap page mapped there instead, the
    // kernel would silently read and write a ring-3 program's malloc'd memory in
    // place of its own page tables. Keeping these 512 frames out of the pool is
    // what makes the private heap slot and the identity map able to coexist.
    // Regions below MEMORY_START (real-mode area, kernel image at 1M) are already
    // outside the pool and need no explicit reservation.
    reserve_range(USER_REGION_START, USER_HEAP_LIMIT);

    // Reserve everything the map flagged as non-usable that falls inside the pool.
    reserve_map_holes(mbi_addr);
}

uint64_t alloc_frame(void) {
    for (uint32_t i = 0; i < num_frames; i++) {
        if (!test_bit(i)) {
            set_bit(i);
            return MEMORY_START + (uint64_t)i * FRAME_SIZE;
        }
    }
    return 0;   // out of memory
}

// Allocate `count` physically contiguous frames and return the base address, or
// 0 if no run that long is free. The bitmap is linear and frame index i maps to
// MEMORY_START + i*FRAME_SIZE, and the whole pool is identity-mapped as one
// stretch, so a run of consecutive clear bits IS contiguous, mapped physical RAM.
// The kernel heap (kernel/heap.c) needs this: its boundary-tag block walk assumes
// one contiguous slab, which single-frame alloc_frame() cannot guarantee.
uint64_t alloc_frames_contiguous(uint32_t count) {
    if (count == 0) {
        return 0;
    }
    uint32_t run = 0;
    for (uint32_t i = 0; i < num_frames; i++) {
        if (!test_bit(i)) {
            run++;
            if (run == count) {
                uint32_t start = i - count + 1;   // run spans [start, i]
                for (uint32_t j = start; j <= i; j++) {
                    set_bit(j);
                }
                return MEMORY_START + (uint64_t)start * FRAME_SIZE;
            }
        } else {
            run = 0;   // gap: a used frame breaks the run, start over
        }
    }
    return 0;   // no contiguous run of the requested length
}

void free_frame(uint64_t addr) {
    if (addr < MEMORY_START) {
        return;
    }
    uint32_t frame = (uint32_t)((addr - MEMORY_START) / FRAME_SIZE);
    if (frame < num_frames) {
        clear_bit(frame);
    }
}

uint32_t frames_used(void) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < num_frames; i++) {
        if (test_bit(i)) {
            count++;
        }
    }
    return count;
}

// DIAGNOSTIC ONLY. How many frames are currently free, the complement of
// frames_used. Nothing in the kernel makes a decision on this number: it exists so
// that a leak can be OBSERVED rather than argued about, by printing it at a point
// where memory has just been returned (kernel/scheduler.c reports it every time a
// dead task's address space is torn down) and checking it comes back to the same
// value after the same work. A slow monotonic decrease across repeated runs is a
// leak, and without this it would be invisible until the machine ran out.
//
// It walks the whole bitmap on every call, which is fine because only debug code
// calls it. Do not put it on a hot path.
uint64_t frame_free_count(void) {
    return (uint64_t)num_frames - (uint64_t)frames_used();
}
