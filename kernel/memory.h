#ifndef MEMORY_H
#define MEMORY_H

#include "../include/types.h"

#define FRAME_SIZE 4096

// The ring-3 address space is three 2MB page-directory slots: code (PD[2], 4-6M),
// stack (PD[3], 6-8M) and, since the user-memory rung, the heap (PD[4], 8-10M)
// that SYS_MMAP fills one region at a time. The numbers live in include/usermem.h
// because ring-3 code needs the same ones; these two are the kernel's historical
// names for the code-and-stack span, kept because the ELF loader and the docs
// reason in terms of them. memory_init() reserves the PHYSICAL frames under all
// three slots (see the comment there for why the heap slot's frames must be
// reserved too), and the syscall layer bounds untrusted ring-3 pointers against
// the whole span with user_range_ok below.
#include "../include/usermem.h"
#define USER_REGION_START  USER_CODE_BASE    // 4 MB, ring-3 code (PD[2])
#define USER_REGION_END    USER_STACK_LIMIT  // 8 MB, top of ring-3 stack (PD[3])

// Is the whole range [ptr, ptr + len) inside the ring-3 address space? THE
// security boundary for every kernel path handed a pointer that came from ring 3,
// whether to read a string out of it or to write a buffer through it. It bounds the
// ENTIRE range rather than just the start, and it is careful about overflow: ptr +
// len can wrap on a crafted length and a wrapped sum compares as comfortably small,
// so len is checked against the room above ptr rather than by forming ptr + len.
//
// The upper bound is USER_SPACE_END, the top of the HEAP slot, not the top of the
// stack: a buffer a program got from malloc has to be usable as a syscall buffer,
// or malloc is only good for memory the program never shows the kernel. The check
// is still region-based rather than per-page (it does not walk the caller's page
// tables), so an address inside the span that is not mapped passes it and faults
// in the kernel when dereferenced; that was already true of the unmapped middle of
// the code slot, and the heap slot, which starts empty, makes such addresses more
// common. See the limitation in docs/project-status.md.
//
// It lives here, beside the constants it tests against, rather than in the
// syscall layer that used to own it, because kernel/signal.c needs the same check
// on the same region: signal delivery writes a whole register frame through a stack
// pointer that came from ring 3 (S4 in docs/decisions/0023-signals.md). Two
// spellings of one security check is how one of them ends up subtly weaker.
static inline int user_range_ok(uint64_t ptr, uint64_t len) {
    if (ptr < USER_REGION_START || ptr >= USER_SPACE_END) {
        return 0;
    }
    return len <= USER_SPACE_END - ptr;
}

// Read the Multiboot memory map, extend the identity map to cover real RAM (up
// to a 1GB ceiling), and flush the TLB. Returns the detected top-of-RAM in bytes
// (uncapped, for reporting); 0 means no map was provided and the safe fixed
// fallback was used. Must run before memory_init so the frames it manages are
// mapped. mbi_addr is the physical Multiboot info pointer boot.asm passed in.
uint64_t memory_detect_and_map(uint64_t mbi_addr);

// Size the frame pool from the detected RAM and reserve the ranges that are not
// free (the ring-3 region and every non-usable range the map reported).
void memory_init(uint64_t mbi_addr);

uint64_t alloc_frame(void);        // returns a free physical frame address, or 0 if none

// Allocate `count` physically contiguous frames; returns the base address, or 0
// if no run that long is free. Used by the kernel heap, which needs a single
// contiguous slab for its boundary-tag block walk (see kernel/heap.c).
uint64_t alloc_frames_contiguous(uint32_t count);

void free_frame(uint64_t addr);
uint32_t frames_used(void);

// DIAGNOSTIC ONLY: how many frames are free right now. Nothing decides anything on
// this; it exists so a leak can be observed, by printing it where memory has just
// been returned and checking it comes back to the same value. See memory.c.
uint64_t frame_free_count(void);

#endif
