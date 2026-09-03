// paging.c: per-task address spaces (per-process paging).
//
// See paging.h and docs/reference/paging.md for the two-halves model (private
// user half, shared kernel half) and why the kernel must be mapped in every tree.
// This is the densest code in the kernel and is read as learning material, so it
// is commented heavily and deliberately.

#include "paging.h"
#include "memory.h"
#include "heap.h"
#include "../libc/mem.h"
#include "../drivers/screen.h"

// The boot page directory (boot/boot.asm). Option A clones the kernel half BY
// VALUE: a per-task tree gets its own PML4/PDPT/PD, and its PD is filled with the
// SAME huge-page entries this boot pd_table holds (identical physical kernel
// frames, kernel-only), so every task maps the identical kernel at the identical
// addresses. We do NOT point at the one boot pd_table by reference, because in
// this kernel the user region at 0x400000 lives in pd_table[2]/[3]: sharing the
// page by reference would share the user huge pages too and make a private 4KB
// mapping at 0x400000 impossible. See docs/decisions/0012 for why by-reference
// did not fit and docs/reference/paging.md for the by-value invariant.
//
// TRIPWIRE (the load-bearing invariant): by-value cloning is correct ONLY because
// kernel mappings are FROZEN after boot. memory_detect_and_map fills the identity
// map once at startup, before any task tree exists, and nothing ever mutates a
// kernel PD entry afterward, so a by-value copy can never drift out of sync with
// the boot tables. If the kernel ever remaps itself at runtime (kernel ASLR,
// memory hot-plug, a higher-half relocation), the copies in existing task trees
// would silently go stale; at that point this MUST become a by-reference share of
// the kernel tables or gain an explicit propagation step. Do not add runtime
// kernel remapping without revisiting this.
extern uint64_t pd_table[512];

// One page directory is 512 entries (512 * 2MB = 1GB), matching kernel/memory.c.
#define PD_ENTRIES  512

// One page table is likewise 512 entries (512 * 4KB = 2MB, exactly the span of the
// PD entry above it). Spelled separately from PD_ENTRIES because they are the same
// number for different reasons, and a teardown walk that confuses the two levels is
// precisely the bug worth making unspellable.
#define PT_ENTRIES  512

// The three PD slots that describe the ring-3 address space (include/usermem.h):
//   PD[2]  0x400000-0x5FFFFF : user code
//   PD[3]  0x600000-0x7FFFFF : user stack
//   PD[4]  0x800000-0x9FFFFF : user heap, filled one SYS_MMAP region at a time
// The by-value kernel clone SKIPS these three so the user branch can be overridden
// with private 4KB page tables (paging_map_page) instead of the shared huge page.
//
// PD[4] WAS NOT A FREE SLOT, and treating it as one is what the user-memory rung
// had to get right. boot/boot.asm identity-maps the first 32MB with sixteen huge
// pages, so PD[4] held the kernel's own mapping of physical 8-10M, which is also
// the range the frame allocator hands out first. Two consequences, both
// load-bearing. memory_init reserves physical 8-10M from the pool, because once a
// task tree is in CR3 the kernel can no longer reach those frames through the
// identity map (the reservation comment in kernel/memory.c says what breaks). And
// the clone below must leave PD[4] ABSENT rather than copy the boot entry: next_table
// tests only PG_PRESENT, so a copied huge-page entry would be followed as if it
// were a page table, and the first SYS_MMAP would write page-table entries straight
// into physical 0x800000, whatever lived there, with no fault at the point of the
// mistake.
#define USER_PD_INDEX_CODE   2
#define USER_PD_INDEX_STACK  3
#define USER_PD_INDEX_HEAP   4

// THE SINGLE PLACE THAT SAYS WHICH PD SLOTS ARE A TASK'S OWN. It was two entries
// (code, stack) until the user heap made it three, and it is the one thing that has
// to change if a fourth user region is ever added. Two functions read it and must
// agree: paging_create_address_space skips exactly these slots when it clones the
// kernel half, and paging_destroy_address_space frees exactly these slots and
// nothing else. A slot present in one list and not the other is either a user
// mapping that leaks at exit (M2 in docs/decisions/0024: the free frame count
// ratchets down across runs and nothing else looks wrong) or a kernel huge page
// walked as if it were a page table.
static const int user_pd_slots[] = { USER_PD_INDEX_CODE, USER_PD_INDEX_STACK, USER_PD_INDEX_HEAP };
#define USER_PD_SLOT_COUNT  ((int)(sizeof(user_pd_slots) / sizeof(user_pd_slots[0])))

static int is_user_pd_slot(int index) {
    for (int s = 0; s < USER_PD_SLOT_COUNT; s++) {
        if (user_pd_slots[s] == index) {
            return 1;
        }
    }
    return 0;
}

// Flags for a page SYS_MMAP hands to ring 3: present, writable, reachable at CPL 3.
#define USER_HEAP_PAGE_FLAGS  (PG_PRESENT | PG_WRITABLE | PG_USER)

// Extract the table index for each level from a virtual address. The x86-64 page
// walk chops the 48-bit virtual address into four 9-bit indices plus a 12-bit
// offset: PML4 (bits 47..39), PDPT (38..30), PD (29..21), PT (20..12).
#define PML4_INDEX(v)  (((v) >> 39) & 0x1FF)
#define PDPT_INDEX(v)  (((v) >> 30) & 0x1FF)
#define PD_INDEX(v)    (((v) >> 21) & 0x1FF)
#define PT_INDEX(v)    (((v) >> 12) & 0x1FF)

// Allocate a page-table frame and zero it. Page tables MUST be 4KB and 4KB-
// aligned, which is exactly what alloc_frame hands out (a whole frame), so tables
// come from the frame allocator, NOT kmalloc (kmalloc carves sub-page pieces off
// the heap slab and does not align to a frame). The returned frame is inside the
// identity-mapped pool (MEMORY_START..pool_top, capped at 1GB), so its physical
// address is also a writable virtual pointer, which is how we fill the table
// below. alloc_frame never returns a frame above the identity ceiling, so writing
// through the returned address can never fault; returns 0 only on out-of-memory.
static uint64_t *alloc_table(void) {
    uint64_t frame = alloc_frame();
    if (frame == 0) {
        return NULL;
    }
    memset((void *)frame, 0, FRAME_SIZE);
    return (uint64_t *)frame;
}

// Follow a table entry down one level, creating the next table if absent. When we
// create it, the entry is marked PRESENT | WRITABLE | USER: this is the AND-down
// rule (boot.asm relies on it too). x86 permits a ring-3 access only if the user
// bit is set at EVERY level of the walk, so the upper levels are made permissive
// and the actual privilege is gated at the leaf. Every table paging_map_page
// creates is on the user branch, so user-permissive intermediates are correct.
// Returns NULL if a new table could not be allocated.
static uint64_t *next_table(uint64_t *entry) {
    if (!(*entry & PG_PRESENT)) {
        uint64_t *table = alloc_table();
        if (table == NULL) {
            return NULL;
        }
        *entry = (uint64_t)table | PG_PRESENT | PG_WRITABLE | PG_USER;
    }
    return (uint64_t *)(*entry & PTE_ADDR_MASK);
}

address_space_t *paging_create_address_space(void) {
    // The handle is kernel-only bookkeeping (only paging/scheduler code reads it),
    // so it is safe on the kernel heap.
    address_space_t *as = (address_space_t *)kmalloc(sizeof(address_space_t));
    if (as == NULL) {
        return NULL;
    }

    // Own PML4, PDPT, and PD for the low 1GB (Option A). Page-table pages come
    // from alloc_frame (4KB, aligned), never kmalloc.
    uint64_t *pml4 = alloc_table();
    uint64_t *pdpt = alloc_table();
    uint64_t *pd   = alloc_table();
    if (pml4 == NULL || pdpt == NULL || pd == NULL) {
        // Hand any frames we did get back to the pool, BY HAND, one at a time.
        //
        // This is the one teardown in the kernel that cannot be delegated to
        // paging_destroy_address_space, and the reason is the contract this
        // function is about to fulfil: on failure it returns NULL, so the caller
        // never receives a handle and has nothing to pass to the teardown. The
        // three frames below are known to this function and to nothing else, and if
        // they are not returned here they are stranded for the life of the machine.
        //
        // The tree is also not walkable yet: pml4[0] and pdpt[0] are wired up after
        // this block, so at this moment the three frames are unlinked siblings, not
        // a tree. Handing them to a walker that follows entries would free whichever
        // of them it could reach through links that do not exist, which is none.
        if (pml4 != NULL) { free_frame((uint64_t)pml4); }
        if (pdpt != NULL) { free_frame((uint64_t)pdpt); }
        if (pd   != NULL) { free_frame((uint64_t)pd);   }
        kfree(as);
        return NULL;
    }

    // Clone the kernel half BY VALUE: copy every boot PD entry except the three
    // user slots. Present kernel entries carry the identical huge-page mapping (same
    // physical kernel frame, no user bit); absent entries copy as zero. This is
    // the copy the tripwire above governs. PD[2]/PD[3]/PD[4] are left zero (not
    // present) so paging_map_page can install private 4KB page tables there (the
    // loader and the stack mapper at create, SYS_MMAP later); a ring-3 touch of an
    // unmapped user address therefore faults, which is the guard the old
    // shared-region layout never had. See user_pd_slots above for why PD[4] must be
    // skipped and not merely overwritten later.
    for (int i = 0; i < PD_ENTRIES; i++) {
        if (is_user_pd_slot(i)) {
            continue;
        }
        pd[i] = pd_table[i];
    }

    // Wire PML4[0] -> this PDPT -> this PD. Both upper entries are user-permissive
    // (AND-down rule); the kernel leaves in the PD stay kernel-only because their
    // copied values have no user bit, so ring 3 still cannot reach kernel memory
    // even though the branch above it says "user may pass".
    pdpt[0] = (uint64_t)pd   | PG_PRESENT | PG_WRITABLE | PG_USER;
    pml4[0] = (uint64_t)pdpt | PG_PRESENT | PG_WRITABLE | PG_USER;

    // CR3 wants a PHYSICAL address; identity mapping makes the pointer's value the
    // physical address already.
    as->pml4_phys = (uint64_t)pml4;
    return as;
}

int paging_map_page(address_space_t *as, uint64_t virt, uint64_t phys, uint64_t flags) {
    uint64_t *pml4 = (uint64_t *)as->pml4_phys;

    // Walk the 4KB path, creating PDPT/PD/PT as needed. For the user branch the
    // upper two levels already exist from create (present, user); PD[2]/PD[3] and
    // the PT below are created here on first touch.
    uint64_t *pdpt = next_table(&pml4[PML4_INDEX(virt)]);
    if (pdpt == NULL) {
        return 1;
    }
    uint64_t *pd = next_table(&pdpt[PDPT_INDEX(virt)]);
    if (pd == NULL) {
        return 1;
    }
    uint64_t *pt = next_table(&pd[PD_INDEX(virt)]);
    if (pt == NULL) {
        return 1;
    }

    // Set the leaf. flags carries the real privilege of the page (the caller
    // passes PG_PRESENT | PG_WRITABLE | PG_USER for a ring-3 page).
    pt[PT_INDEX(virt)] = (phys & PTE_ADDR_MASK) | flags;
    return 0;
}

// Free every present leaf in one page table, then the table's own frame. `pt_phys`
// is the physical (== virtual, identity mapped) base of a 512-entry page table
// hanging under one of the three user PD slots, so every present entry in it is a
// 4KB frame this task privately owns: a page of its loaded program image, a page
// of its stack, or a page of a SYS_MMAP region. Nothing here is shared with any
// other task or with the kernel. THIS WALK IS WHAT FREES A REGION AT EXIT (M2): it
// frees the leaves, not just the table, so a heap page a program never released
// goes back to the pool with the rest of the tree.
//
// Freeing the leaves BEFORE the table itself matters: the table is the only record
// of where the leaves are, so returning it to the pool first loses the addresses of
// 512 frames, which then never come back. That is the leak this rung exists to
// close, so it would be a quiet way to fail the whole thing.
static void destroy_page_table(uint64_t pt_phys) {
    uint64_t *pt = (uint64_t *)pt_phys;
    for (int i = 0; i < PT_ENTRIES; i++) {
        if (pt[i] & PG_PRESENT) {
            free_frame(pt[i] & PTE_ADDR_MASK);
        }
    }
    free_frame(pt_phys);
}

void paging_destroy_address_space(address_space_t *as) {
    if (as == NULL) {
        return;
    }

    // PRECONDITION: `as` IS NOT THE TREE CURRENTLY IN CR3. See paging.h. Everything
    // below hands frames the CPU may still be translating through back to the free
    // pool, so calling this on the live tree does not fault here; it faults later,
    // somewhere unrelated, once those frames have been reissued to somebody else.
    // The scheduler upholds this by only ever destroying a tree that belongs to a
    // task other than the one running (kernel/scheduler.c: reap_sweep skips
    // `current`, and task_wait only frees a child).

    // Mirror paging_create_address_space exactly in reverse: it allocated a PML4, a
    // PDPT and a PD, wired pml4[0] -> pdpt -> pd, and left the three user PD slots
    // empty for paging_map_page to fill with private page tables. So the tree comes
    // apart leaves first, then the user page tables, then the three spine frames,
    // then the handle.
    //
    // Every level is tested for PG_PRESENT before it is followed. That is not
    // paranoia about a tree the kernel built correctly: paging_create_address_space
    // can fail partway on out-of-memory, and elf_load_file or map_user_stack can
    // fail after only some of the tree exists, so a task's tree can legitimately be
    // missing a level. Following an absent entry would take the low bits of a zero
    // as a frame address and free frame 0.
    uint64_t *pml4 = (uint64_t *)as->pml4_phys;
    if (pml4[0] & PG_PRESENT) {
        uint64_t *pdpt = (uint64_t *)(pml4[0] & PTE_ADDR_MASK);
        if (pdpt[0] & PG_PRESENT) {
            uint64_t pd_phys = pdpt[0] & PTE_ADDR_MASK;
            uint64_t *pd = (uint64_t *)pd_phys;

            // ONLY THE NAMED USER PD SLOTS, AND NOTHING ELSE. THIS IS THE MOST
            // DANGEROUS LINE IN THE KERNEL TO GET WRONG.
            //
            // The obvious implementation, "walk all 512 PD entries and free every
            // present one", is catastrophic here and gives no warning at all. Every
            // other present entry in this PD is a copy of a boot huge-page entry
            // (see the by-value clone in paging_create_address_space above) pointing
            // at SHARED KERNEL PHYSICAL MEMORY: the kernel's own code, its data, its
            // heap, the frame bitmap, this very function. They are copies of a
            // pointer, not a private allocation, and this task does not own a single
            // one of them. Freeing them marks the kernel's own frames available, the
            // allocator hands them out to the next task's page tables or heap block,
            // and the whole town is overwritten while it runs, with no message and
            // no fault at the point of the mistake.
            //
            // The PG_HUGE test below is a SECOND lock on the same door, not the
            // primary guard. The primary guard is that this loop iterates over the
            // named indices in user_pd_slots (three today: code, stack, heap) and
            // over nothing else. The heap slot is in that list because a task's
            // SYS_MMAP regions live under it: task_exit closes descriptors and knows
            // nothing about regions, so this walk is the only thing that returns a
            // region's frames when a program exits without releasing it (M2).
            for (int s = 0; s < USER_PD_SLOT_COUNT; s++) {
                int idx = user_pd_slots[s];
                uint64_t entry = pd[idx];

                // Absent: nothing was ever mapped here (a create that failed before
                // this slot was filled). Huge: this is not a page table at all but a
                // 2MB mapping, which in this kernel can only be a leftover shared
                // kernel entry, so it is not ours to free and must not be walked as
                // if it pointed at a table.
                if (!(entry & PG_PRESENT) || (entry & PG_HUGE)) {
                    continue;
                }

                destroy_page_table(entry & PTE_ADDR_MASK);
                pd[idx] = 0;   // no dangling entry in a tree being torn down
            }

            free_frame(pd_phys);
        }
        free_frame((uint64_t)pdpt);
    }
    free_frame((uint64_t)pml4);

    // The handle came from kmalloc (not the frame allocator), so it goes back to the
    // heap rather than the frame pool. Forgetting this leaks a small block per dead
    // task, which is invisible in a free-FRAME count and so is exactly the kind of
    // leak that survives the obvious test.
    kfree(as);
}

// Find the leaf entry for one user virtual address in `as`, or NULL if any level
// of the walk is absent, or the PD entry is a huge page (which in this kernel can
// only be a copied kernel entry: not a page table, and not ours to touch). Only the
// unmap below uses it; the map path goes through next_table, which creates.
static uint64_t *lookup_pte(address_space_t *as, uint64_t virt) {
    uint64_t *pml4 = (uint64_t *)as->pml4_phys;
    uint64_t entry = pml4[PML4_INDEX(virt)];
    if (!(entry & PG_PRESENT)) {
        return NULL;
    }
    uint64_t *pdpt = (uint64_t *)(entry & PTE_ADDR_MASK);
    entry = pdpt[PDPT_INDEX(virt)];
    if (!(entry & PG_PRESENT)) {
        return NULL;
    }
    uint64_t *pd = (uint64_t *)(entry & PTE_ADDR_MASK);
    entry = pd[PD_INDEX(virt)];
    if (!(entry & PG_PRESENT) || (entry & PG_HUGE)) {
        return NULL;
    }
    uint64_t *pt = (uint64_t *)(entry & PTE_ADDR_MASK);
    return &pt[PT_INDEX(virt)];
}

void paging_unmap_user_range(address_space_t *as, uint64_t base, uint64_t length) {
    for (uint64_t va = base; va < base + length; va += FRAME_SIZE) {
        uint64_t *pte = lookup_pte(as, va);
        if (pte == NULL || !(*pte & PG_PRESENT)) {
            continue;   // never mapped: a partial run being unwound, nothing to free
        }
        free_frame(*pte & PTE_ADDR_MASK);
        *pte = 0;

        // FLUSH THIS TRANSLATION FROM THE TLB. When SYS_MUNMAP calls this, `as` is
        // the tree currently in CR3 (the caller's own), and the CPU may have cached
        // the translation for `va` the last time the program touched it. Clearing
        // the entry in memory does not clear that cache. Without the invlpg the
        // program can go on reading and writing the frame it just released, through
        // the stale translation, after free_frame has handed that frame to another
        // task's page table or heap page: a silent cross-task corruption with no
        // fault anywhere near the cause. invlpg on a tree that is NOT in CR3 (the
        // unwind inside paging_map_user_range runs on the caller's tree too, so today
        // that case does not arise) is harmless: it flushes an entry for an address
        // whose current translation is simply not the one being removed.
        __asm__ __volatile__("invlpg (%0)" : : "r"(va) : "memory");
    }
}

int paging_map_user_range(address_space_t *as, uint64_t base, uint64_t length) {
    uint64_t done = 0;
    while (done < length) {
        uint64_t frame = alloc_frame();
        if (frame == 0) {
            // M3, PARTIAL MAPPING FAILURE. Page 40 of 100 found no frame. Returning
            // now would leave pages 1 to 39 mapped in a region the caller is about to
            // be told does not exist: nothing records them, SYS_MUNMAP would refuse
            // the address, and they would come back only when the task exits, if
            // it ever does. A program that retries a failing SYS_MMAP in a loop would
            // strand a few dozen frames per attempt with the free count stepping down
            // each time. So the run is unwound here, the same discipline as
            // task_create_from_file tearing down a partial address space and
            // alloc_chain in fs/fat32.c freeing a partial cluster chain: a call that
            // fails costs nothing.
            paging_unmap_user_range(as, base, done);
            return -1;
        }

        // ZERO THE FRAME. alloc_frame returns whatever the last owner left in it:
        // another task's stack, a file's bytes, a page table. Handing that to a
        // ring-3 program is an information leak across the isolation this whole
        // module exists to provide, and it is also what makes a fresh region
        // reliably all-zero, which calloc and the p5 allocator's "uninitialised is
        // zero" block state both quietly depend on. The frame is written through
        // the identity map, so this works whether or not `as` is in CR3.
        memset((void *)frame, 0, FRAME_SIZE);

        if (paging_map_page(as, base + done, frame, USER_HEAP_PAGE_FLAGS) != 0) {
            // A page TABLE could not be allocated. The frame we hold is not in the
            // tree yet, so the unwind below cannot see it; free it by hand first.
            free_frame(frame);
            paging_unmap_user_range(as, base, done);
            return -1;
        }
        done += FRAME_SIZE;
    }
    return 0;
}

// Load an address space into CR3. CR3 holds a PHYSICAL address; everything below
// 1GB is identity mapped, so as->pml4_phys is already both the physical base of
// the PML4 and a writable virtual pointer to it. Writing CR3 flushes the TLB
// (we mark no pages PG_GLOBAL, so no entries survive the write), which is exactly
// what drops the previous task's stale translations on a switch.
void paging_switch(address_space_t *as) {
    __asm__ __volatile__("mov %0, %%cr3" : : "r"(as->pml4_phys) : "memory");
}
