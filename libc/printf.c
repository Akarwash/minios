// printf.c: a printf for ring 3, on top of SYS_WRITE.
//
// Until this existed every program that wanted to print a number built the digits
// by hand: user/shell.c had a print_uint with ten call sites, and COUNT.c, H.c and
// ONCE.c each had a print_ulong, all the same dozen lines written over again. The
// point of this file is that those are gone. See docs/decisions/0024 and
// docs/reference/user-memory.md.
//
// Six specifiers and nothing else: %d %u %x %s %c %%. No width, no precision, no
// length modifiers, no floats. More get added when something needs them. An
// unrecognised specifier STOPS THE FORMAT: whatever was formatted before it is
// written, nothing after it is, and the call returns -1. It is neither printed raw
// nor skipped, because both would let a mistake in a format string pass unnoticed.
//
// M5, READING PAST THE ARGUMENTS. A variadic function cannot know how many
// arguments it was given: printf("%s %s", one_thing) reads a second argument that
// was never passed, and gets whatever was in the register, which is a fault or
// garbage depending on the specifier. That is not fixable here and this file does
// not pretend to fix it. What it does is bound the damage. %s checks that the
// pointer lies inside the ring-3 address space before dereferencing it and never
// walks past the end of that space looking for a terminator, so a garbage pointer
// stops the format rather than reading kernel memory or running off the top of the
// heap slot; and the first unrecognised specifier stops the format, as above. The
// real defence is at compile time: user/userlib.h declares this function with
// __attribute__((format(printf, 1, 2))), so the compiler checks every call site's
// arguments against its format string and a mismatch is a warning in the build, not
// a fault at run time. A mismatched format that gets past the compiler (a pointer
// cast to the right type, a format built at run time) is undefined, full stop.
//
// Output goes through sys_write_all (user/userlib.h), the loop that retries a
// partial SYS_WRITE until everything is written. A pipe takes only what fits and
// the console moves at most the kernel's staging buffer per call, so one SYS_WRITE
// is never assumed to have moved a whole formatted string; without the loop a long
// line into a full pipe would be silently truncated at the pipe's capacity.

#include <stdarg.h>
#include "../user/userlib.h"

// Output is staged in a small buffer and flushed when it fills and at the end, so
// a call makes a handful of syscalls rather than one per character. The buffer is
// on the stack: printf must not allocate (a program's very first output may well
// be from inside malloc's error path), and a static buffer would break the moment
// a signal handler called printf while the interrupted code was inside it.
#define PRINTF_STAGE  128

typedef struct {
    char buf[PRINTF_STAGE];
    unsigned long len;      // bytes staged, not yet written
    unsigned long total;    // bytes written so far
    int failed;             // a write came up short: the far end is gone
} out_t;

static void flush(out_t *o) {
    if (o->len == 0) {
        return;
    }
    long w = sys_write_all(1, o->buf, o->len);
    if (w < 0 || (unsigned long)w < o->len) {
        o->failed = 1;
    }
    if (w > 0) {
        o->total += (unsigned long)w;
    }
    o->len = 0;
}

static void put(out_t *o, char c) {
    if (o->len == PRINTF_STAGE) {
        flush(o);
    }
    o->buf[o->len++] = c;
}

// Emit `v` in `base` (10 or 16), least-significant digit first into a scratch
// buffer, then reversed. 2^64 is 20 decimal digits, so 21 bytes is enough for any
// value; the do/while prints a single 0 for zero rather than nothing, which is the
// single most common value there is.
static void put_unsigned(out_t *o, unsigned long v, unsigned int base) {
    char tmp[21];
    int i = 0;
    do {
        unsigned int d = (unsigned int)(v % base);
        tmp[i++] = (char)(d < 10 ? '0' + d : 'a' + (d - 10));
        v /= base;
    } while (v != 0);
    while (i > 0) {
        put(o, tmp[--i]);
    }
}

// Is `s` somewhere a ring-3 pointer is allowed to point? The address-space bounds
// are the same ones the kernel applies to a pointer it is handed (include/usermem.h),
// which is the most this side can check: an address inside the space that is not
// mapped (the empty middle of the code slot, an unmapped part of the heap slot) is
// indistinguishable from a good one here, and touching it is the program's own
// page fault, as it would be anywhere else in the program.
static int user_pointer_ok(const char *s) {
    unsigned long a = (unsigned long)s;
    return a >= USER_CODE_BASE && a < USER_SPACE_END;
}

int printf(const char *fmt, ...) {
    out_t o;
    o.len = 0;
    o.total = 0;
    o.failed = 0;

    int stopped = 0;
    va_list ap;
    va_start(ap, fmt);

    for (const char *p = fmt; *p != '\0' && !stopped; p++) {
        if (*p != '%') {
            put(&o, *p);
            continue;
        }
        p++;
        switch (*p) {
            case 'd': {
                int v = va_arg(ap, int);
                if (v < 0) {
                    put(&o, '-');
                    // Negate in a wider type: -INT_MIN does not fit in an int.
                    put_unsigned(&o, (unsigned long)(-(long)v), 10);
                } else {
                    put_unsigned(&o, (unsigned long)v, 10);
                }
                break;
            }
            case 'u':
                put_unsigned(&o, (unsigned long)va_arg(ap, unsigned int), 10);
                break;
            case 'x':
                put_unsigned(&o, (unsigned long)va_arg(ap, unsigned int), 16);
                break;
            case 'c':
                // char is promoted to int through the ellipsis; read it back as one.
                put(&o, (char)va_arg(ap, int));
                break;
            case 's': {
                const char *s = va_arg(ap, const char *);
                if (!user_pointer_ok(s)) {
                    stopped = 1;   // M5: not a pointer this program may read through
                    break;
                }
                while (*s != '\0') {
                    put(&o, *s);
                    s++;
                    if ((unsigned long)s >= USER_SPACE_END) {
                        stopped = 1;   // ran to the top of the address space with no NUL
                        break;
                    }
                }
                break;
            }
            case '%':
                put(&o, '%');
                break;
            default:
                // Anything else, including a '%' at the very end of the format (*p
                // is then the terminating NUL): stop, do not print it, do not skip it.
                stopped = 1;
                break;
        }
    }

    va_end(ap);
    flush(&o);
    if (o.failed || stopped) {
        return -1;
    }
    return (int)o.total;
}
