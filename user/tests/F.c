// User program F: the multi-cluster write test.
//
// The shell's `write` can only ever produce a single-cluster file, because a
// typed line is far shorter than one 512-byte cluster. So the write path's real
// work — allocating a chain of several clusters, linking them, and reading them
// back in order — is never exercised from the keyboard. F is what exercises it: it
// writes a 16KB file, which at 512 bytes per cluster is a 32-cluster chain, a real
// chain rather than a lucky single cluster that broken chain logic could still get
// right.
//
// It is self-checking, so a human never has to eyeball 16KB. It fills a buffer with
// a recognisable pattern of numbered, fixed-width lines (the same idea as
// HUGE.TXT), writes it as FTEST.TXT, reads it back into a second buffer, and
// compares byte for byte AND on length. It exits 0 only on an exact match, and with
// a distinct non-zero status per failure mode so the shell's `run: ... exited with
// status N` says which check failed. It prints one line saying the same.
//
// 16KB, not larger, on purpose: it clears one cluster many times over yet stays
// under the shell's 32KB read buffer, so a human can `read FTEST.TXT` afterwards
// and spot-check the numbered lines by eye.

#include "../userlib.h"

// 256 lines of 64 bytes is 16384 bytes exactly: 32 clusters at 512 bytes each.
#define F_LINE   64
#define F_LINES  256
#define F_SIZE   (F_LINE * F_LINES)

// Exit statuses, one per failure mode, all distinct and none zero.
#define F_OK              0
#define F_WRITE_FAILED    1
#define F_READ_FAILED     2
#define F_LENGTH_MISMATCH 3
#define F_CONTENT_MISMATCH 4

// Static (.bss, inside the ring-3 region), not on the stack: two 16KB buffers are
// far too big for the user stack.
static char wbuf[F_SIZE];
static char rbuf[F_SIZE];

// Write a 5-digit zero-padded decimal of n into dst[0..4]. No libc, so the digits
// are placed by hand, least significant last.
static void put5(char *dst, unsigned int n) {
    for (int i = 4; i >= 0; i--) {
        dst[i] = (char)('0' + (n % 10));
        n /= 10;
    }
}

// Build one 64-byte line: "FTEST.TXT line NNNNN " then dots, then a newline as the
// 64th byte. Fixed width so a human reading the file can see exactly which line is
// which, and so the total is an exact multiple of the cluster size. put5 stays
// hand-rolled: printf has no field width, so it cannot zero-pad to five digits.
static void fill_line(char *p, unsigned int lineno) {
    const char *prefix = "FTEST.TXT line ";
    unsigned long i = strlen(prefix);
    memcpy(p, prefix, i);
    put5(p + i, lineno);
    i += 5;
    p[i++] = ' ';
    memset(p + i, '.', (F_LINE - 1) - i);
    p[F_LINE - 1] = '\n';
}

void _start(void) {
    for (unsigned int L = 0; L < F_LINES; L++) {
        fill_line(wbuf + L * F_LINE, L);
    }

    if (sys_writefile("FTEST.TXT", wbuf, F_SIZE) != 0) {
        sys_print("F: write failed\n");
        sys_exit(F_WRITE_FAILED);
    }

    unsigned long n = sys_readfile("FTEST.TXT", rbuf, F_SIZE);
    if (n == (unsigned long)-1) {
        sys_print("F: read failed\n");
        sys_exit(F_READ_FAILED);
    }
    if (n != F_SIZE) {
        sys_print("F: length mismatch\n");
        sys_exit(F_LENGTH_MISMATCH);
    }
    for (unsigned long i = 0; i < F_SIZE; i++) {
        if (rbuf[i] != wbuf[i]) {
            sys_print("F: content mismatch\n");
            sys_exit(F_CONTENT_MISMATCH);
        }
    }

    sys_print("F: FTEST.TXT 16384 bytes written and verified\n");
    sys_exit(F_OK);
}
