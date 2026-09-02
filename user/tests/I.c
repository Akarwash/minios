// I.ELF: the malloc test.
//
// Until this rung a ring-3 program had exactly the memory it declared at compile
// time, plus a stack, which is why every fixture here uses fixed static buffers.
// I is the first program that asks for memory at runtime, and it is written to
// catch the ways an allocator fails quietly: blocks that overlap, blocks that come
// back with somebody else's bytes in them, a free list that loses blocks, and
// growth that does not join up. It is self-checking, like F.ELF: it exits 0 only if
// every check passes, with a distinct non-zero status per failure mode so
// `run: i.elf exited with status N` names the check, and prints one line either way.
//
// What it does, in order:
//   1. malloc a few hundred blocks of varying sizes (1 to 1400 bytes, total about
//      210KB, so the heap has to grow past its first 64KB slab several times),
//      check each is 8-byte aligned, and write a pattern into each that depends on
//      the block AND the byte offset.
//   2. Verify every pattern, only after every block exists: a block that overlaps
//      another has had its pattern overwritten by the time this runs, which is
//      what a per-block check straight after the write would miss.
//   3. free them in a shuffled order (a fixed permutation, so a failure is
//      reproducible), which exercises coalescing in both directions and the case of
//      freeing a block whose neighbours are still in use.
//   4. malloc again, a single block larger than any slab. It must succeed, and it
//      must come back at the address of the very first block: everything was freed
//      and coalesced back into one span, across the slab boundaries, so the whole
//      heap is one free block again and first-fit returns its start. That is the
//      reuse check and the "slabs joined up" check in one.
//   5. calloc, and check it is zero: a REUSED block holds its last owner's bytes,
//      so this is calloc's memset being tested, not the kernel's zeroing.
//   6. malloc something larger than the whole 2MB heap slot, which must fail with
//      NULL rather than anything louder.
//
// The kernel's reap line is the other half of the test. Ten consecutive runs must
// print the same free frame count and the same heap-used figure every time: the
// slabs are never given back by malloc, so if the address space teardown did not
// free the heap slot with the rest (M2 in docs/decisions/0024) the count would
// step down by this program's slabs on every run.

#include "../userlib.h"

#define I_BLOCKS 300

// Exit statuses, one per failure mode, all distinct and none zero.
#define I_OK                 0
#define I_MALLOC_FAILED      1   // an allocation in the first round returned NULL
#define I_MISALIGNED         2   // a block was not 8-byte aligned
#define I_PATTERN_MISMATCH   3   // a block's pattern was damaged (overlap, or a lost byte)
#define I_REUSE_FAILED       4   // the big allocation after freeing everything failed
#define I_NOT_COALESCED      5   // it succeeded but not at the first block's address
#define I_CALLOC_NOT_ZERO    6   // calloc returned memory that was not all zero
#define I_HUGE_NOT_NULL      7   // an impossible request did not return NULL

// Sizes from 1 to 1400 bytes in a fixed, non-monotonic sequence. 97 is coprime with
// 1400, so the sequence walks the whole range before repeating.
#define I_SIZE(i)        (1 + (((i) * 97) % 1400))

// The pattern byte for byte j of block i: depends on both, so a byte copied from
// the wrong block, or the wrong offset, is wrong.
#define I_PATTERN(i, j)  ((unsigned char)((((i) * 31) + ((j) * 7)) ^ 0xA5))

// The shuffled free order: index k of the permutation is block (k * 7 + 3) % 300.
// 7 is coprime with 300, so every block is visited exactly once.
#define I_ORDER(k)       ((((k) * 7) + 3) % I_BLOCKS)

static unsigned char *blocks[I_BLOCKS];

static void done(const char *what, int status) {
    sys_print("I: ");
    sys_print(what);
    sys_print("\n");
    sys_exit(status);
}

void _start(void) {
    // (1) Allocate and fill.
    for (int i = 0; i < I_BLOCKS; i++) {
        unsigned long n = I_SIZE(i);
        blocks[i] = (unsigned char *)malloc(n);
        if (blocks[i] == (unsigned char *)0) {
            done("malloc returned NULL in the first round", I_MALLOC_FAILED);
        }
        if (((unsigned long)blocks[i] & 7) != 0) {
            done("a block is not 8-byte aligned", I_MISALIGNED);
        }
        for (unsigned long j = 0; j < n; j++) {
            blocks[i][j] = I_PATTERN(i, j);
        }
    }

    // (2) Verify every pattern, now that every block exists.
    for (int i = 0; i < I_BLOCKS; i++) {
        unsigned long n = I_SIZE(i);
        for (unsigned long j = 0; j < n; j++) {
            if (blocks[i][j] != I_PATTERN(i, j)) {
                done("a block's pattern was damaged", I_PATTERN_MISMATCH);
            }
        }
    }

    // (3) Free in a shuffled order.
    for (int k = 0; k < I_BLOCKS; k++) {
        free(blocks[I_ORDER(k)]);
    }

    // (4) Reuse: one block bigger than any slab, which only fits if every freed
    // block coalesced back into one span across the slab boundaries. First-fit then
    // returns the start of the heap, which is where block 0 was.
    unsigned char *big = (unsigned char *)malloc(200000);
    if (big == (unsigned char *)0) {
        done("a large allocation after freeing everything failed", I_REUSE_FAILED);
    }
    if (big != blocks[0]) {
        done("the freed blocks did not coalesce back to the start of the heap", I_NOT_COALESCED);
    }
    for (unsigned long j = 0; j < 200000; j++) {
        big[j] = (unsigned char)(j & 0xFF);
    }
    free(big);

    // (5) calloc on reused memory must be zero.
    unsigned char *z = (unsigned char *)calloc(64, 128);
    if (z == (unsigned char *)0) {
        done("calloc returned NULL", I_REUSE_FAILED);
    }
    for (unsigned long j = 0; j < 64 * 128; j++) {
        if (z[j] != 0) {
            done("calloc returned memory that was not zero", I_CALLOC_NOT_ZERO);
        }
    }
    free(z);

    // (6) An impossible request fails cleanly: bigger than the 2MB heap slot.
    if (malloc(3UL * 1024 * 1024) != (void *)0) {
        done("a 3MB request did not return NULL", I_HUGE_NOT_NULL);
    }

    done("300 blocks allocated, verified, freed in shuffled order, and reused", I_OK);
}
