#ifndef MEMORY_H
#define MEMORY_H

#include "../include/types.h"

#define FRAME_SIZE 4096

// The ring-3 program's code (4-6M) and stack (6-8M) physically occupy the low
// end of the frame pool. See boot/boot.asm (PD[2]/PD[3]) and
// user/user_program.c. memory_init() reserves these frames so the allocator
// never hands out memory the running user program already lives on. They are
// exposed here (not kept file-local to memory.c) so the syscall layer can reuse
// them to bound-check untrusted pointers from ring 3.
#define USER_REGION_START  0x400000         // 4 MB, ring-3 code (PD[2])
#define USER_REGION_END    0x800000         // 8 MB, top of ring-3 stack (PD[3])

// Is the whole range [ptr, ptr + len) inside the single ring-3 region? THE
// security boundary for every kernel path handed a pointer that came from ring 3,
// whether to read a string out of it or to write a buffer through it. It bounds the
// ENTIRE range rather than just the start, and it is careful about overflow: ptr +
// len can wrap on a crafted length and a wrapped sum compares as comfortably small,
// so len is checked against the room above ptr rather than by forming ptr + len.
//
// It lives here, beside the two constants it tests against, rather than in the
// syscall layer that used to own it, because kernel/signal.c needs the same check
// on the same region: signal delivery writes a whole register frame through a stack
// pointer that came from ring 3 (S4 in docs/decisions/0023-signals.md). Two
// spellings of one security check is how one of them ends up subtly weaker.
static inline int user_range_ok(uint64_t ptr, uint64_t len) {
    if (ptr < USER_REGION_START || ptr >= USER_REGION_END) {
        return 0;
    }
    return len <= USER_REGION_END - ptr;
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
