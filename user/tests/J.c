// J.ELF: the SYS_MMAP / SYS_MUNMAP abuse test.
//
// Every other fixture asks the kernel for something it should grant. This one asks
// for things it must REFUSE, and checks that each refusal changed nothing: a zero
// length, a length larger than the whole 2MB heap slot, a release of an address
// that was never mapped, a release of this program's own code and stack, a release
// of part of a region, a release with the wrong length, a second release of a
// region already given back, a ninth region when eight is the cap, and a region
// that would cross the ceiling of the slot. Between the abuses it does the ordinary
// thing too (map, check the memory is zero, write a pattern, read it back, release)
// so that "the kernel refused it" is distinguishable from "the kernel refuses
// everything".
//
// It is self-checking, like F.ELF: it exits 0 only if every call answered as the
// contract in include/syscalls.h says, and with a distinct non-zero status per
// failure so `run: j.elf exited with status N` names the check that failed. The
// list of statuses is below. It prints one line on success and one on failure.
//
// The two things most worth reading out of a run are the free frame count on the
// reap line, which must come back to the same value as after `run a.elf` (every
// region this program maps is released, and the largest is a few hundred frames),
// and that the program is still RUNNING after each refused call: a kernel that
// unmapped the code slot on request would take this program down at the next
// instruction fetch, and one that mapped a 3MB request would strand frames or
// climb into kernel addresses, neither of which is a return value.
//
// It talks to the syscalls directly (sys_mmap/sys_munmap) rather than through
// malloc, because malloc is built on this contract and cannot test it.

#include "../userlib.h"

// Exit statuses, one per failure mode, all distinct and none zero.
#define J_OK                        0
#define J_ZERO_ACCEPTED             1   // mmap(0) was not refused
#define J_HUGE_ACCEPTED             2   // mmap(3MB), larger than the slot, was not refused (M1)
#define J_MMAP_FAILED               3   // an ordinary, legal mmap failed
#define J_WRONG_PLACE               4   // a region was not placed where the placement rule says
#define J_NOT_ZEROED                5   // fresh memory was not zero-filled
#define J_BAD_MUNMAP_ACCEPTED       6   // munmap of an address never mapped was not refused (M4)
#define J_PARTIAL_MUNMAP_ACCEPTED   7   // munmap of part of a region, or with the wrong length, was not refused
#define J_MEMORY_CHANGED            8   // memory was damaged by a refused munmap
#define J_MUNMAP_FAILED             9   // a legal munmap failed
#define J_DOUBLE_MUNMAP_ACCEPTED   10   // releasing a region twice was not refused
#define J_SLOTS_NOT_CAPPED         11   // a ninth region was accepted
#define J_CEILING_NOT_ENFORCED     12   // a region crossing the top of the slot was accepted
#define J_GAP_REUSED               13   // a gap below a live region was reused, which this rung does not do

#define J_FAIL  ((unsigned long)-1)     // what a refused syscall returns

#define J_REGION_BYTES  8192            // two pages, so a one-page partial release is possible
#define J_PATTERN(i)    ((unsigned char)((((i) * 7) ^ 0x5A) & 0xFF))

// The program's own memory, filled at the start and checked after every refused
// release, so "changed nothing" is a claim about this program's data as well as
// about the region. Static (.bss) so it is inside the code slot, one of the two
// addresses the abuses aim at.
static unsigned char keep[4096];

static void fill(unsigned char *p, unsigned long n) {
    for (unsigned long i = 0; i < n; i++) {
        p[i] = J_PATTERN(i);
    }
}

static int intact(const unsigned char *p, unsigned long n) {
    for (unsigned long i = 0; i < n; i++) {
        if (p[i] != J_PATTERN(i)) {
            return 0;
        }
    }
    return 1;
}

static void fail(const char *what, int status) {
    sys_print("J: ");
    sys_print(what);
    sys_print("\n");
    sys_exit(status);
}

void _start(void) {
    fill(keep, sizeof(keep));

    // (1) A zero length is refused. There is no such thing as a region of no pages.
    if (sys_mmap(0) != J_FAIL) {
        fail("mmap(0) was accepted", J_ZERO_ACCEPTED);
    }

    // (2) M1: a length larger than the whole slot is refused, and this program is
    // still running afterwards. A length that would wrap when rounded up is the
    // same check from the other side.
    if (sys_mmap(3UL * 1024 * 1024) != J_FAIL) {
        fail("mmap(3MB) was accepted", J_HUGE_ACCEPTED);
    }
    if (sys_mmap((unsigned long)-1) != J_FAIL) {
        fail("mmap(-1) was accepted", J_HUGE_ACCEPTED);
    }

    // (3) The ordinary case: the first region lands at the bottom of the slot,
    // page-aligned, and reads as zero before anything is written to it.
    unsigned long r1 = sys_mmap(J_REGION_BYTES);
    if (r1 == J_FAIL) {
        fail("an ordinary mmap failed", J_MMAP_FAILED);
    }
    if (r1 != USER_HEAP_BASE) {
        fail("the first region is not at USER_HEAP_BASE", J_WRONG_PLACE);
    }
    unsigned char *p1 = (unsigned char *)r1;
    for (unsigned long i = 0; i < J_REGION_BYTES; i++) {
        if (p1[i] != 0) {
            fail("fresh memory is not zero", J_NOT_ZEROED);
        }
    }
    fill(p1, J_REGION_BYTES);

    // (4) A second region lands directly above the first, and a length that is not
    // a whole number of pages is rounded up to one.
    unsigned long r2 = sys_mmap(100);
    if (r2 == J_FAIL) {
        fail("the second mmap failed", J_MMAP_FAILED);
    }
    if (r2 != r1 + J_REGION_BYTES) {
        fail("the second region is not directly above the first", J_WRONG_PLACE);
    }

    // (5) M4: a release of anything that is not exactly a region this program holds
    // is refused and changes nothing. Five shapes of "not exactly a region": an
    // address inside the slot that was never mapped, this program's own code, its
    // own stack, the second page of a region, and a region's start with the wrong
    // length. The pattern in the region and in .bss is checked after all five.
    if (sys_munmap(USER_HEAP_BASE + 0x100000, USER_PAGE_SIZE) != J_FAIL) {
        fail("munmap of an address never mapped was accepted", J_BAD_MUNMAP_ACCEPTED);
    }
    if (sys_munmap(USER_CODE_BASE, USER_PAGE_SIZE) != J_FAIL) {
        fail("munmap of this program's code was accepted", J_BAD_MUNMAP_ACCEPTED);
    }
    if (sys_munmap(USER_STACK_LIMIT - USER_PAGE_SIZE, USER_PAGE_SIZE) != J_FAIL) {
        fail("munmap of this program's stack was accepted", J_BAD_MUNMAP_ACCEPTED);
    }
    if (sys_munmap(r1 + USER_PAGE_SIZE, USER_PAGE_SIZE) != J_FAIL) {
        fail("munmap of the second page of a region was accepted", J_PARTIAL_MUNMAP_ACCEPTED);
    }
    if (sys_munmap(r1, USER_PAGE_SIZE) != J_FAIL) {
        fail("munmap with the wrong length was accepted", J_PARTIAL_MUNMAP_ACCEPTED);
    }
    if (!intact(p1, J_REGION_BYTES) || !intact(keep, sizeof(keep))) {
        fail("memory changed after a refused munmap", J_MEMORY_CHANGED);
    }

    // (6) The real release, then the same release again, which must be refused: the
    // region is gone and the address now names nothing.
    if (sys_munmap(r1, J_REGION_BYTES) != 0) {
        fail("a legal munmap failed", J_MUNMAP_FAILED);
    }
    if (sys_munmap(r1, J_REGION_BYTES) != J_FAIL) {
        fail("releasing a region twice was accepted", J_DOUBLE_MUNMAP_ACCEPTED);
    }

    // (7) Placement: r1's gap sits BELOW the live r2, so the next region goes above
    // r2, not into the gap. Gap reuse is what a later rung would add; this one says
    // so in its ADR, and this check is what keeps that statement true.
    unsigned long r3 = sys_mmap(USER_PAGE_SIZE);
    if (r3 == J_FAIL) {
        fail("the third mmap failed", J_MMAP_FAILED);
    }
    if (r3 == r1) {
        fail("a gap below a live region was reused", J_GAP_REUSED);
    }
    if (r3 != r2 + USER_PAGE_SIZE) {
        fail("the third region is not directly above the second", J_WRONG_PLACE);
    }
    // 100 was rounded up to a page when r2 was made; releasing with 100 rounds the
    // same way and matches.
    if (sys_munmap(r2, 100) != 0 || sys_munmap(r3, USER_PAGE_SIZE) != 0) {
        fail("releasing the second or third region failed", J_MUNMAP_FAILED);
    }

    // (8) With nothing held, placement restarts at the bottom: there is no live
    // region to be above. Then the slot cap: eight regions succeed and a ninth is
    // refused, and each lands directly above the last.
    unsigned long r[8];
    for (int i = 0; i < 8; i++) {
        r[i] = sys_mmap(USER_PAGE_SIZE);
        if (r[i] == J_FAIL) {
            fail("mmap failed before the eight-region cap", J_MMAP_FAILED);
        }
        unsigned long expect = (i == 0) ? USER_HEAP_BASE : r[i - 1] + USER_PAGE_SIZE;
        if (r[i] != expect) {
            fail("a region in the run of eight is out of place", J_WRONG_PLACE);
        }
    }
    if (sys_mmap(USER_PAGE_SIZE) != J_FAIL) {
        fail("a ninth region was accepted", J_SLOTS_NOT_CAPPED);
    }
    for (int i = 0; i < 8; i++) {
        if (sys_munmap(r[i], USER_PAGE_SIZE) != 0) {
            fail("releasing one of the eight failed", J_MUNMAP_FAILED);
        }
    }

    // (9) The ceiling. Everything is released, so the next region starts at the
    // bottom of the slot: a request for the whole slot fits exactly, one page more
    // than the slot is refused (that is the M1 length check), and a request that
    // would cross the ceiling from a higher base is refused too. Mapping the whole
    // slot is 512 frames, which is also the largest single thing this fixture asks
    // for, and the reap line's free frame count says whether they all came back.
    const unsigned long slot = USER_HEAP_LIMIT - USER_HEAP_BASE;
    if (sys_mmap(slot + USER_PAGE_SIZE) != J_FAIL) {
        fail("a region larger than the slot was accepted", J_HUGE_ACCEPTED);
    }
    unsigned long whole = sys_mmap(slot);
    if (whole == J_FAIL) {
        fail("mapping the whole slot failed", J_MMAP_FAILED);
    }
    unsigned char *pw = (unsigned char *)whole;
    pw[0] = 1;
    pw[slot - 1] = 2;                    // the first and last byte of the slot are writable
    if (sys_mmap(USER_PAGE_SIZE) != J_FAIL) {
        fail("a region above a full slot was accepted", J_CEILING_NOT_ENFORCED);
    }
    if (sys_munmap(whole, slot) != 0) {
        fail("releasing the whole slot failed", J_MUNMAP_FAILED);
    }
    unsigned long half = sys_mmap(slot / 2);
    if (half == J_FAIL) {
        fail("mapping half the slot failed", J_MMAP_FAILED);
    }
    if (sys_mmap(slot / 2 + USER_PAGE_SIZE) != J_FAIL) {
        fail("a region crossing the ceiling was accepted", J_CEILING_NOT_ENFORCED);
    }
    if (sys_munmap(half, slot / 2) != 0) {
        fail("releasing the half slot failed", J_MUNMAP_FAILED);
    }

    if (!intact(keep, sizeof(keep))) {
        fail("this program's own memory changed", J_MEMORY_CHANGED);
    }

    sys_print("J: every abuse refused, every region released, memory intact\n");
    sys_exit(J_OK);
}
