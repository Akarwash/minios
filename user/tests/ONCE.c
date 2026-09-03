// ONCE.ELF: read fd 0 exactly once, print how many bytes arrived, and exit 0.
//
// The downstream half of the SIGPIPE test, and the whole program is the fact that it
// does NOT drain its input. COUNT.ELF loops until EOF, which is the well-behaved
// reader; this one leaves while the writer is still writing, which is the case that
// used to hang.
//
// `run g.elf | run once.elf` is the run to look at. G writes 16384 bytes, four times
// the pipe's capacity, so it must block and resume several times to finish. ONCE
// takes one bufferful and exits, which closes the last read end. G's next write finds
// readers == 0, and the kernel raises SIG_PIPE on it; the default action kills it, so
// the pipeline ends instead of G spinning against a buffer nobody will ever drain.
//
// Before signals, that same run left G writing forever. The pipeline could not
// finish, and the only symptom was a prompt that never came back.
//
// The single read is the point and must not be tidied into a loop: a loop would make
// this COUNT.ELF, the pipe would reach EOF the ordinary way, and the case being
// tested would never arise.

#include "../userlib.h"

void _start(void) {
    char buf[64];

    long n = sys_read(0, buf, sizeof(buf));
    if (n < 0) {
        sys_print("ONCE: read failed\n");
        sys_exit(1);
    }

    printf("ONCE: read %u bytes, exiting\n", (unsigned int)n);
    sys_exit(0);
}
