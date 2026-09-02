// COUNT.ELF: read fd 0 until end of file, count the bytes, print the count to fd 1,
// exit 0.
//
// This is the downstream half of a pipe's "done" condition, and the reason it is a
// test fixture: piped after a writer (`run a.elf | run count.elf`) it blocks on the
// empty pipe until bytes arrive, and it only ever finishes because closing the last
// write end delivers EOF (a read returning 0). So a run that prints the right total
// and exits proves the block/wake path AND that a close is an event that unblocks an
// EOF-waiting reader (B2 in docs/decisions/0022).
//
// IT MUST LOOP ON PARTIAL READS. One sys_read moves at most the kernel's staging
// buffer, and a pipe hands over only what has been written so far, so a single read
// is never assumed to have drained the stream (B5). The loop ends only on 0 (EOF).

#include "../userlib.h"

void _start(void) {
    char buf[256];
    unsigned long total = 0;

    for (;;) {
        long n = sys_read(0, buf, sizeof(buf));
        if (n <= 0) {
            break;             // 0 = EOF (last writer closed); < 0 = read error
        }
        total += (unsigned long)n;
    }

    // The cast is because this printf has no %lu; a pipe's worth of bytes is far
    // under 4GB.
    printf("%u\n", (unsigned int)total);
    sys_exit(0);
}
