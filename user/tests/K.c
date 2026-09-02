// K.ELF: the printf test.
//
// One line per supported specifier, each with values chosen so the output can be
// checked against this source by eye: that is the half a human reads. The other
// half is self-checking. printf returns the number of bytes it wrote, or -1 when
// the format was stopped, and every return value here is compared with the length
// the line must have if every specifier did its job; a wrong digit, a missing
// character, or a format that stopped early all show up as the wrong count. It
// exits 0 only if every count matches, and otherwise with 1 + the index of the
// first line that did not, so `run: k.elf exited with status N` says which.
//
// The last three lines are the ones that matter most, because they are the
// failures printf is DESIGNED to have. `%q` is not a specifier this printf knows,
// so the format stops there: "stop here: " is printed, " never printed" is not, and
// the call returns -1 rather than printing the `%q` raw or skipping over it. The
// two bad %s pointers (a NULL, and an address below the ring-3 region) are refused
// before anything dereferences them, and stop the format the same way. That is M5
// in docs/decisions/0024: a varargs mismatch cannot be caught at run time, and what
// printf can do is bound the damage.
//
// The `%q` line is a deliberate misuse of the format string, and the compiler says
// so: user/userlib.h declares printf with __attribute__((format(printf, 1, 2))),
// which is the REAL defence against a mismatched format, and it flags this line at
// build time exactly as it should. The warning is silenced for this one line only,
// because the line exists to prove the run-time check, and the pragma is the
// evidence that the compile-time one is working.

#include "../userlib.h"

// Each case is a printf call and the byte count it must return.
static int check(int index, int got, int want) {
    if (got != want) {
        printf("K: line %d returned %d, expected %d\n", index, got, want);
        sys_exit(1 + index);
    }
    return index + 1;
}

void _start(void) {
    int i = 0;

    // A string longer than printf's 128-byte staging buffer, so the flush path in
    // the middle of a line is exercised, not just the flush at the end.
    static const char long_line[] =
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
        "0123456789abcdef";   // 144 characters

    i = check(i, printf("%%d: %d %d %d\n", 0, 42, -42), 13);
    i = check(i, printf("%%u: %u %u\n", 0u, 4294967295u), 17);
    i = check(i, printf("%%x: %x %x %x\n", 0u, 255u, 0xdeadbeefu), 18);
    i = check(i, printf("%%s: %s %s\n", "hello", ""), 11);
    i = check(i, printf("%%c: %c%c%c\n", 'T', 'o', 'S'), 8);
    i = check(i, printf("100%% done\n"), 10);
    i = check(i, printf("mixed: %s=%d, %x, %c%%\n", "answer", 42, 42u, '!'), 25);
    i = check(i, printf("%s\n", long_line), 145);

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat"
    // Deliberately wrong, on purpose: see the comment at the top of this file.
    i = check(i, printf("stop here: %q never printed\n"), -1);
#pragma GCC diagnostic pop
    printf("\n");   // the stopped line has no newline of its own

    // The bad pointers are read from volatiles so the compiler cannot see the
    // constants: a literal NULL here is a build warning (-Wformat-overflow), and
    // rightly so, but the check being tested is the one at RUN time, on a value
    // that arrived the way a bad pointer does in a real program.
    volatile unsigned long null_ptr = 0;
    volatile unsigned long low_ptr = 16;
    i = check(i, printf("bad %%s pointers: %s never printed\n", (const char *)null_ptr), -1);
    printf("\n");
    i = check(i, printf("bad %%s pointers: %s never printed\n", (const char *)low_ptr), -1);
    printf("\n");

    printf("K: %d lines checked\n", i);
    sys_exit(0);
}
