#ifndef USERLIB_H
#define USERLIB_H

// ============================================================================
// The entire runtime a TownOS user program gets.
// ============================================================================
// A user program is now a separately compiled, statically linked ELF64 binary
// that lives on the disk image. It links against NOTHING: no host libc, no
// startup files, and no kernel code. It cannot call a kernel function even if it
// wanted to, because kernel pages are not user-accessible and the call would
// fault. The ONLY channel across the ring boundary is `int 0x50`, the syscall
// gate, and this header is the whole of the user side of it.
//
// The headers included below are the only things a user program is allowed to
// lean on, and all of them are deliberately standalone (numbers and plain types
// only, no kernel code):
//   include/syscalls.h  the syscall numbers (SYS_WRITE, SYS_EXIT, ...)
//   include/vectors.h   the vector to raise (SYSCALL_VECTOR)
//   include/signals.h   the signal numbers
//   include/usermem.h   the ring-3 address-space layout SYS_MMAP hands out from
//   include/types.h     the fixed-width integer types and size_t

#include "../include/types.h"
#include "../include/syscalls.h"
#include "../include/vectors.h"
#include "../include/signals.h"
#include "../include/usermem.h"

// The raw doorbell. `int $SYSCALL_VECTOR` traps into the kernel's DPL 3 gate.
// Convention (see include/syscalls.h): RAX = syscall number, RDI = first arg,
// return value comes back in RAX. The constraints pin each value to the register
// the ABI names: "a" is RAX, "D" is RDI. SYSCALL_VECTOR reaches the `int`
// instruction as an immediate through the "i" constraint, so the vector stays a
// named constant instead of a bare 0x50 buried in the asm string.
//
// No clobber list is needed beyond memory: the kernel's stub saves and restores
// every GPR from the frame, so on return only RAX has changed. The "memory"
// clobber keeps the compiler from reordering a string's stores after the trap,
// so the kernel sees the finished string.
//
// always_inline is still load-bearing, for a different reason than before. These
// programs build at -O0, where a plain `static inline` is emitted out of line as
// a real function. That was fatal when the helper could land in kernel pages;
// now the program is self-contained so it would merely work. Keeping the inline
// keeps every instruction the program runs inside its own mapped text, with no
// call through a symbol that the (relocation-free) loader would have to resolve.
static inline __attribute__((always_inline))
unsigned long syscall1(unsigned long number, unsigned long arg1) {
    unsigned long ret;
    __asm__ __volatile__(
        "int %[vec]"
        : "=a"(ret)
        : "a"(number), "D"(arg1), [vec] "i"(SYSCALL_VECTOR)
        : "memory");
    return ret;
}

// The zero-, two-, and three-argument forms of the same doorbell. Same ABI: RAX =
// number, then RDI, RSI, RDX in System V order ("D" = RDI, "S" = RSI, "d" = RDX),
// return in RAX. Split out by arity rather than one variadic helper so each pins
// exactly the registers the kernel reads and no others.
static inline __attribute__((always_inline))
unsigned long syscall0(unsigned long number) {
    unsigned long ret;
    __asm__ __volatile__(
        "int %[vec]"
        : "=a"(ret)
        : "a"(number), [vec] "i"(SYSCALL_VECTOR)
        : "memory");
    return ret;
}

static inline __attribute__((always_inline))
unsigned long syscall2(unsigned long number, unsigned long arg1, unsigned long arg2) {
    unsigned long ret;
    __asm__ __volatile__(
        "int %[vec]"
        : "=a"(ret)
        : "a"(number), "D"(arg1), "S"(arg2), [vec] "i"(SYSCALL_VECTOR)
        : "memory");
    return ret;
}

static inline __attribute__((always_inline))
unsigned long syscall3(unsigned long number, unsigned long arg1,
                       unsigned long arg2, unsigned long arg3) {
    unsigned long ret;
    __asm__ __volatile__(
        "int %[vec]"
        : "=a"(ret)
        : "a"(number), "D"(arg1), "S"(arg2), "d"(arg3), [vec] "i"(SYSCALL_VECTOR)
        : "memory");
    return ret;
}

// The four-argument form. RCX is the fourth argument, which is where the System V C
// ABI already puts it — unlike Linux, which has to use R10 because the `syscall`
// instruction clobbers RCX. This kernel enters through `int 0x50`, which clobbers
// nothing, so the natural register is available.
static inline __attribute__((always_inline))
unsigned long syscall4(unsigned long number, unsigned long arg1, unsigned long arg2,
                       unsigned long arg3, unsigned long arg4) {
    unsigned long ret;
    __asm__ __volatile__(
        "int %[vec]"
        : "=a"(ret)
        : "a"(number), "D"(arg1), "S"(arg2), "d"(arg3), "c"(arg4), [vec] "i"(SYSCALL_VECTOR)
        : "memory");
    return ret;
}

// SYS_WRITE: write `len` bytes of `buf` to descriptor `fd`. Returns the number of
// bytes actually written, which MAY BE LESS THAN len — a pipe accepts only what
// fits, and one call never moves more than the kernel's staging buffer. A caller
// MUST LOOP until everything is written (or a write returns <= 0, meaning the far
// end is gone): assuming one call moved the whole buffer silently drops the rest
// (B5 in docs/decisions/0022). sys_print below is that loop for the common case.
// Returns (unsigned long)-1 (as a negative long) on a bad fd, a wrong-direction fd,
// or a bad buffer. fd 1 is standard output, fd 0 standard input, by convention.
static inline __attribute__((always_inline))
long sys_write(int fd, const char *buf, unsigned long len) {
    return (long)syscall3(SYS_WRITE, (unsigned long)fd, (unsigned long)buf, len);
}

// Write all `len` bytes of `buf` to `fd`, looping until everything is written.
// THE LOOP IS THE POINT: sys_write may move fewer bytes than asked (a pipe takes
// only what fits; the console moves at most the kernel's staging buffer per call),
// so one call is never assumed to have moved everything. Returns the number of
// bytes written, short only if the descriptor went away mid-write (a write
// returning <= 0), which is the one reason to stop. sys_print and printf are both
// built on this one loop, so neither can forget it.
static inline __attribute__((always_inline))
long sys_write_all(int fd, const char *buf, unsigned long len) {
    unsigned long done = 0;
    while (done < len) {
        long w = sys_write(fd, buf + done, len - done);
        if (w <= 0) {
            return (long)done;   // far end gone or error: stop, report progress
        }
        done += (unsigned long)w;
    }
    return (long)done;
}

// Print a NUL-terminated string to fd 1 (standard output), looping until the whole
// string is written (see sys_write_all). Returns the number of bytes written. This
// is the replacement for the old single-argument sys_write, so the common "print
// this string" call sites read the same and cannot forget the loop.
static inline __attribute__((always_inline))
long sys_print(const char *s) {
    unsigned long n = 0;
    while (s[n] != '\0') {
        n++;
    }
    return sys_write_all(1, s, n);
}

// SYS_READ: read up to `len` bytes from descriptor `fd` into `buf`. Returns the
// number of bytes read, which MAY BE LESS THAN len, or 0 at END OF FILE (a pipe
// whose last writer has closed). A reader MUST LOOP: to consume a whole stream,
// keep calling until it returns 0. Returns (unsigned long)-1 (as a negative long)
// on a bad or wrong-direction fd. BLOCKING: an empty pipe with a live writer, or an
// empty console, parks the task until there is something to read; from ring 3 it
// looks like one call that took a while. fd 0 is standard input by convention.
static inline __attribute__((always_inline))
long sys_read(int fd, char *buf, unsigned long len) {
    return (long)syscall3(SYS_READ, (unsigned long)fd, (unsigned long)buf, len);
}

// SYS_CLOSE: close descriptor `fd`. Returns 0, or (unsigned long)-1 on a bad fd.
// Closing a pipe end is how the other end learns the stream is over: closing the
// last write end gives readers EOF, and closing the last read end tells a writer
// nobody is listening. A pipeline that does not close the ends it is done with hangs
// (B1/B6 in docs/decisions/0022), so close is not optional bookkeeping.
static inline __attribute__((always_inline))
long sys_close(int fd) {
    return (long)syscall1(SYS_CLOSE, (unsigned long)fd);
}

// SYS_PIPE: create a pipe. On success `fds[0]` is the read end and `fds[1]` the
// write end, both descriptors in THIS program's table, and it returns 0. Returns
// (unsigned long)-1 on failure (out of memory or no free descriptor slots). Whoever
// creates a pipe holds BOTH ends and must close the ones it does not keep, or the
// end-counts never reach zero and a reader waits forever for an EOF that cannot
// arrive (B1 in docs/decisions/0022).
static inline __attribute__((always_inline))
long sys_pipe(int fds[2]) {
    return (long)syscall1(SYS_PIPE, (unsigned long)fds);
}

// SYS_EXIT: end THIS PROGRAM with `status` (masked by the kernel to 0..255). It no
// longer halts the machine, which is what it used to mean when there was no parent
// to return to: the task leaves the scheduler for good, its memory goes back to the
// kernel's pools, and a parent blocked in sys_wait is woken with this status.
//
// Never returns, and the infinite loop below is now GENUINELY unreachable rather
// than a formality for the compiler: the kernel never schedules this task again, so
// control does not come back to the instruction after the trap.
//
// A program must call this itself at the bottom of _start. There is no crt0 and
// nothing wraps the entry point, so a program that simply falls off the end of
// _start runs into whatever bytes follow it.
static inline __attribute__((always_inline))
void sys_exit(int status) {
    syscall1(SYS_EXIT, (unsigned long)(long)status);
    for (;;) {
    }
}

// SYS_WAIT: block until any child of this program exits, and return its exit status
// (0..255). Returns (unsigned long)-1 if this program has no children at all.
//
// ANY-CHILD, NOT waitpid: a program with several children is told about whichever
// finished first and cannot ask about a particular one. BLOCKING, like sys_readkey:
// waiting costs no CPU, and from here it looks like one call that took a while to
// come back. Passes 0 for the id-out pointer, so the kernel does not report which
// child it was; sys_wait_id below is the variant that does.
static inline __attribute__((always_inline))
long sys_wait(void) {
    return (long)syscall1(SYS_WAIT, 0);
}

// Like sys_wait, but also writes the id of the child that exited through `out_id`.
// A caller reaping several children (a shell running a pipeline) uses this to match
// each reaped child against the stage it started, so it can report the last stage's
// status. Same blocking behaviour and same -1 for "no children".
static inline __attribute__((always_inline))
long sys_wait_id(unsigned long *out_id) {
    return (long)syscall1(SYS_WAIT, (unsigned long)out_id);
}

// SYS_READKEY: pop one buffered keystroke, waiting for one if none is ready.
// BLOCKING: if the buffer is empty the kernel parks this task until a key arrives,
// so the call costs no CPU while it waits and there is no reason to poll it. The
// wait is invisible from here: it looks like one call that took a while. Always
// returns a real character (1..255).
static inline __attribute__((always_inline))
unsigned long sys_readkey(void) {
    return syscall0(SYS_READKEY);
}

// SYS_LIST: fill buf with the root directory's file names, one per line, NUL-
// terminated. Returns the number of names, or (unsigned long)-1 on error.
static inline __attribute__((always_inline))
unsigned long sys_list(char *buf, unsigned long size) {
    return syscall2(SYS_LIST, (unsigned long)buf, size);
}

// The process-group requests SYS_RUN's fourth argument accepts. They are named here
// rather than written as bare numbers because 0 and -1 as a "group" read as a
// mistake at a call site.
#define SYS_RUN_GROUP_INHERIT  0UL             // the caller's own group: the default
#define SYS_RUN_GROUP_NEW      0xFFFFFFFFUL    // a new group, led by (and named after) the child

// SYS_RUN: load and start the named program; it joins the scheduler alongside this
// one. `in_fd` and `out_fd` are descriptors in THIS program's table to give the
// child as its fd 0 and fd 1, or -1 for a fresh console end. Passing pipe ends here
// is the only way to wire one program's output to another's input, since a running
// task's descriptors cannot be changed from outside. Returns the child's task id
// (>= 1) on success, (unsigned long)-1 if it could not be started (bad file, or a
// bad in_fd/out_fd). The id lets a caller running several children tell them apart
// when it waits; an ordinary run just checks for the -1 failure.
static inline __attribute__((always_inline))
unsigned long sys_run(const char *name, int in_fd, int out_fd) {
    return syscall4(SYS_RUN, (unsigned long)name,
                    (unsigned long)(long)in_fd, (unsigned long)(long)out_fd,
                    SYS_RUN_GROUP_INHERIT);
}

// Like sys_run, but places the child in a chosen PROCESS GROUP rather than the
// caller's own. Pass SYS_RUN_GROUP_NEW to start a new group led by the child — the
// returned task id IS that group's id, since a group is named after its leader — or
// an existing group id to add the child to a job already under way.
//
// This is how a shell builds a job: the first stage of a pipeline asks for a new
// group, and every later stage joins the id the first one returned. Ctrl-C is then
// addressed to that one group and reaches every stage, which is what makes a
// pipeline behave as the single thing the user typed.
//
// Returns the child's task id, or (unsigned long)-1. A group the caller is not
// allowed to join FAILS THE RUN rather than quietly falling back to inheritance: a
// half-built job group is not something a caller can detect afterwards.
static inline __attribute__((always_inline))
unsigned long sys_run_group(const char *name, int in_fd, int out_fd, unsigned long pgid) {
    return syscall4(SYS_RUN, (unsigned long)name,
                    (unsigned long)(long)in_fd, (unsigned long)(long)out_fd, pgid);
}

// SYS_SETFG: declare which process group is in the FOREGROUND — the one Ctrl-C is
// addressed to. Returns 0, or (unsigned long)-1 if this program is not allowed to
// name that group.
//
// A program may name only its own group, or a group one of its own children is in.
// That rule is what stops any program taking the keyboard and never giving it back.
// A shell hands the keyboard to a job before waiting for it and takes it back
// afterwards, unconditionally — including when the job failed or was killed, because
// a shell that only takes it back on success loses the keyboard the first time
// anything goes wrong.
static inline __attribute__((always_inline))
unsigned long sys_setfg(unsigned long pgid) {
    return syscall1(SYS_SETFG, pgid);
}

// SYS_READFILE: read the named file into buf. Returns the number of bytes read, or
// (unsigned long)-1 on error. The bytes are RAW and NOT NUL-terminated: terminate
// them yourself before printing the buffer as a string.
static inline __attribute__((always_inline))
unsigned long sys_readfile(const char *name, char *buf, unsigned long size) {
    return syscall3(SYS_READFILE, (unsigned long)name, (unsigned long)buf, size);
}

// SYS_WRITEFILE: write `len` bytes from buf to the named file, creating it or
// wholly replacing it. The bytes are raw: no terminator is written and none is
// expected in buf. Returns 0 on success, (unsigned long)-1 on error (a bad
// pointer, a name that is not 8.3, no free space, or a disk error).
static inline __attribute__((always_inline))
unsigned long sys_writefile(const char *name, const char *buf, unsigned long len) {
    return syscall3(SYS_WRITEFILE, (unsigned long)name, (unsigned long)buf, len);
}

// SYS_DELETE: delete the named file from the disk. Returns 0 on success,
// (unsigned long)-1 on error (a bad pointer, a name that is not 8.3, a missing
// file, or a disk error).
static inline __attribute__((always_inline))
unsigned long sys_delete(const char *name) {
    return syscall1(SYS_DELETE, (unsigned long)name);
}

// SYS_FREECOUNT: how many clusters on the volume are free. Returns the count
// (never an error: a number crosses the boundary by value).
static inline __attribute__((always_inline))
unsigned long sys_freecount(void) {
    return syscall0(SYS_FREECOUNT);
}

// SYS_STAT: report the size in bytes of the named file, writing it through
// `out_size`, so a caller can size a buffer before it reads. Returns 0 on success,
// (unsigned long)-1 if the file is not found (or the name is not 8.3, or names a
// directory). This is what lets `read` tell a missing file from one too big for
// its buffer, and report the size instead of a bare failure.
static inline __attribute__((always_inline))
unsigned long sys_stat(const char *name, unsigned long *out_size) {
    return syscall2(SYS_STAT, (unsigned long)name, (unsigned long)out_size);
}

// The trampoline a signal handler returns through, defined in user/trampoline.asm
// and linked into every program. A program never calls it; its address is what
// sys_signal hands the kernel, and the kernel writes it as the handler's return
// address when it forges the call frame.
extern void sigreturn_trampoline(void);

// SYS_SIGNAL: install `handler` for `sig` on this program. Pass 0 to restore the
// default action, which is to be killed with status 128 + sig.
//
// The handler is called with the signal number as its argument, on this program's
// own stack, with everything the signal interrupted saved underneath. Returning from
// it normally resumes the interrupted code exactly where it was, which is what makes
// a handled Ctrl-C a pause rather than an end.
//
// SIG_KILL CANNOT BE HANDLED and this returns -1 for it. A signal a program can
// catch is a request; there has to be one that is not, or a program could refuse to
// die.
//
// The trampoline address is the third argument on the wire and is supplied here, so
// callers never see it. It has to come from the program because there is no
// kernel-owned page mapped into a program's address space for the kernel to point a
// return address at. Returns 0, or (unsigned long)-1.
static inline __attribute__((always_inline))
unsigned long sys_signal(int sig, void (*handler)(int)) {
    return syscall3(SYS_SIGNAL, (unsigned long)sig, (unsigned long)handler,
                    (unsigned long)&sigreturn_trampoline);
}

// SYS_KILL: raise `sig` on the task with id `id`. Returns 0, or (unsigned long)-1 if
// there is no such live task or the signal is not one this kernel has.
//
// This is how a task the keyboard cannot reach is stopped: Ctrl-C goes to the
// foreground group only, so a program left running in another group — anything
// started by a parent that then exited — is unreachable from the keyboard by design.
// Pair it with sys_tasks, so the id is looked up rather than guessed.
static inline __attribute__((always_inline))
unsigned long sys_kill(unsigned long id, int sig) {
    return syscall2(SYS_KILL, id, (unsigned long)sig);
}

// SYS_MMAP: ask the kernel for `length` bytes of fresh memory. Returns the
// page-aligned address of a new region inside the heap slot (USER_HEAP_BASE up to
// USER_HEAP_LIMIT, include/usermem.h), or (unsigned long)-1. The memory is ZEROED
// and mapped in full before the call returns (nothing is lazy), and `length` is
// rounded up to whole pages, so asking for 100 bytes costs 4096. Anonymous memory
// only: no file behind it, no sharing, no protection flags.
//
// A region is placed at the lowest address above every region this program still
// holds. Space freed BELOW a live region is not reused in this rung, so a program
// that keeps one region and maps and unmaps repeatedly above it climbs the 2MB slot
// and eventually gets -1. Each program may hold at most eight regions at once. This
// is the call malloc (libc/malloc.c) is built on; most programs never call it
// directly. Does not block.
static inline __attribute__((always_inline))
unsigned long sys_mmap(unsigned long length) {
    return syscall1(SYS_MMAP, length);
}

// SYS_MUNMAP: release a region SYS_MMAP handed out, and only that: `addr` must be
// exactly the address SYS_MMAP returned and `length` the length asked of it (the
// kernel rounds it up to pages the same way). Returns 0, or (unsigned long)-1 when
// (addr, length) is not exactly a region this program holds: an address it never
// mapped, its own code or stack, part of a region, or a region it already released.
// A refused call changes nothing. There is no partial or overlapping unmap in this
// rung. Does not block.
static inline __attribute__((always_inline))
unsigned long sys_munmap(unsigned long addr, unsigned long length) {
    return syscall2(SYS_MUNMAP, addr, length);
}

// ============================================================================
// The heap: malloc, free, calloc (libc/malloc.c, linked into every program).
// ============================================================================
// A program can ask for memory at runtime instead of declaring every buffer as a
// fixed static array. The allocator is kernel/heap.c ported a second time (the
// same p5 explicit free list with boundary tags and coalescing), sitting on
// SYS_MMAP instead of the frame allocator. The first call maps a 64KB slab at the
// bottom of the heap slot; running out maps another, adjacent, slab, up to the
// kernel's eight-region cap; a slab is never given back until the program exits.
//
// Contract, and its edges:
//   - malloc(n) returns 8-byte aligned memory that holds n bytes, or NULL when the
//     kernel refuses another slab (out of frames, out of region slots, or the 2MB
//     ceiling). A fresh slab arrives zeroed from the kernel, but a REUSED block
//     holds whatever its last owner left: use calloc if zero matters.
//   - free(p) returns a block to the allocator, coalescing with its neighbours;
//     free(NULL) does nothing. Freeing anything malloc did not return, or freeing
//     twice, corrupts the heap (the allocator prints one ERROR line for the cases
//     it can detect and does nothing for the rest).
//   - calloc(count, size) is malloc(count * size) with the product checked for
//     overflow and the memory cleared.
//   - There is no realloc in this rung.
//   - Not async-signal-safe: a signal handler must not call any of these.
// See libc/malloc.c, docs/reference/user-memory.md and docs/decisions/0024.
void *malloc(size_t size);
void  free(void *ptr);
void *calloc(size_t count, size_t size);

// ============================================================================
// printf (libc/printf.c, linked into every program).
// ============================================================================
// Formatted output to fd 1. Six specifiers and nothing else: %d (int), %u
// (unsigned int), %x (unsigned int, lowercase hex, no prefix), %s (a NUL-terminated
// string), %c (a character), %% (a percent sign). No width, no precision, no
// length modifiers, no floats; more get added when something needs them. Returns
// the number of bytes written, or -1.
//
// An unrecognised specifier STOPS THE FORMAT: what came before it is written,
// nothing after it is, and the call returns -1. It is neither printed raw nor
// skipped. A %s whose pointer lies outside the ring-3 address space is refused the
// same way, before anything reads through it. Output goes through sys_write_all,
// so a long line into a full pipe waits rather than truncates.
//
// THE FORMAT ATTRIBUTE IS THE REAL DEFENCE against a format string that does not
// match its arguments, which cannot be detected at run time (printf("%s %s", one)
// reads a second argument that was never passed): the compiler checks every call
// site against the format string and warns on a mismatch. A mismatched format that
// gets past it is undefined behaviour. Two edges the compiler cannot flag, because
// they are valid for the standard printf: a length modifier (%lu, %ld) is not a
// specifier this printf knows, so it stops the format at run time; cast the value to
// unsigned int or int at the call site instead. And %p does not exist here.
int printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// A crude busy-wait so the letters do not scroll past faster than the eye can
// follow. This is NOT a timed delay, just a spin; the count was tuned by eye
// under QEMU for a readable interleave. A real system would sleep, not spin.
#define USER_DELAY_ITERATIONS  20000000

static inline __attribute__((always_inline))
void user_delay(void) {
    // volatile so -O0 (and any optimiser) keeps the loop instead of deleting it.
    for (volatile unsigned long i = 0; i < USER_DELAY_ITERATIONS; i++) {
    }
}

// ============================================================================
// next_token: a reentrant, in-place tokenizer.
// ============================================================================
// It lives here, in the user-side runtime, rather than in libc/string.c on
// purpose. The user build compiles a single freestanding translation unit and
// links no kernel objects (see the user/%.ELF rule in the Makefile), so
// libc/string.c is simply not reachable from a ring-3 program; and the kernel
// itself never tokenizes anything, so putting it there would be dead code. So it
// sits beside the syscall wrappers, the rest of the runtime the shell is allowed
// to lean on, as a standalone function the program compiles directly.
//
// It is the strtok_r style: the caller holds the current position in *pos, so
// there is NO hidden global. That is deliberate, avoiding strtok's single static
// cursor, which makes strtok non-reentrant and is a classic action-at-a-distance
// bug (a second tokenization, even in a called function, clobbers the first).
//
// IN PLACE, NO COPYING: the separator after a token is overwritten with '\0' and
// the returned pointer points INTO the caller's buffer, so THE INPUT BUFFER IS
// MODIFIED. That is what lets the shell compare a token with a plain string
// compare and still reach the untouched remainder of the line after it.
static inline char *next_token(char **pos, char sep) {
    char *p = *pos;

    // SKIP LEADING SEPARATORS. A run of separators before the token is stepped
    // over, so "run   A.ELF" (extra spaces) yields "run" then "A.ELF" and never an
    // empty token in between. This is the non-obvious part: it is also why an empty
    // or all-separator remainder returns NULL (no more tokens) instead of a
    // zero-length token.
    while (*p == sep) {
        p++;
    }

    if (*p == '\0') {
        *pos = p;
        return (char *)0;   // no more tokens (NULL is not defined in this freestanding runtime)
    }

    // The token runs from here to the next separator or the end of the string.
    char *start = p;
    while (*p != '\0' && *p != sep) {
        p++;
    }

    if (*p == sep) {
        *p = '\0';   // shred: terminate this token in place
        p++;         // step past the separator so the next call resumes after it
    }
    // else *p is already '\0' (end of string), and *pos will point at it, so the
    // next call returns NULL.

    *pos = p;
    return start;
}

#endif
