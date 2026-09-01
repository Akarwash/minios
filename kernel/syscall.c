#include "syscall.h"
#include "memory.h"
#include "scheduler.h"
#include "file.h"
#include "pipe.h"
#include "heap.h"
#include "../drivers/screen.h"
#include "../drivers/keyboard.h"
#include "../fs/fat32.h"
#include "../libc/mem.h"
#include "../include/syscalls.h"
#include "signal.h"

// The most bytes one SYS_READ or SYS_WRITE moves in a single call. A counted
// buffer from ring 3 is copied through a kernel staging buffer of this size, so a
// transfer larger than this comes back as a partial count and the caller loops
// (this is the mechanism behind partial transfers, B5 in docs/decisions/0022, not
// an edge case). One static buffer is safe because a syscall runs with interrupts
// masked (the int 0x50 gate clears IF) and this kernel is single-CPU, so two
// syscalls never touch it at once.
#define SYSCALL_IO_MAX 4096
static char io_staging[SYSCALL_IO_MAX];

// The longest filename SYS_RUN and SYS_READFILE will copy in from ring 3. An 8.3
// name needs at most 12 characters plus a terminator; 16 leaves a little slack.
// The cap is what stops an unterminated user string from walking off the region.
#define SYSCALL_NAME_MAX 16

// user_range_ok moved to kernel/memory.h, beside the region constants it tests,
// so kernel/signal.c can apply the identical check to a signal frame's destination.

// Copy a NUL-terminated string from ring 3 into a kernel buffer, capping the
// length so a missing terminator cannot walk out of the region. The start pointer
// is bounds-checked; then bytes are copied until a NUL, until dst_size is reached,
// or until USER_REGION_END is reached, whichever comes first. The result is always
// NUL-terminated. Returns 0 on success, -1 if the start pointer is out of bounds or
// no terminator appears within the cap.
static int copy_user_string(uint64_t user_ptr, char *dst, uint32_t dst_size) {
    if (dst_size == 0) {
        return -1;
    }
    if (user_ptr < USER_REGION_START || user_ptr >= USER_REGION_END) {
        return -1;
    }
    for (uint32_t i = 0; i < dst_size; i++) {
        uint64_t addr = user_ptr + i;
        if (addr >= USER_REGION_END) {
            return -1;   // reached the region edge with no terminator
        }
        char c = *(const char *)addr;
        dst[i] = c;
        if (c == '\0') {
            return 0;
        }
    }
    dst[dst_size - 1] = '\0';   // cap hit: truncate rather than overflow
    return -1;                  // the name did not fit, treat as invalid
}

// SYS_WRITE: write a counted buffer to one of the caller's descriptors.
//   RDI = fd, RSI = user buffer pointer, RDX = length.
// Returns the number of bytes written, which MAY BE LESS than the length asked for
// (a pipe takes only what fits, and one call never moves more than SYSCALL_IO_MAX);
// the caller loops on the count. Returns SYSCALL_ERROR on a bad fd, a wrong-
// direction fd, or a bad buffer.
//
// VALIDATION ORDER is fd, slot, direction, then buffer: reject an fd out of range,
// then an unopened slot, then a read-end (fd 0, or a pipe's read end) that cannot be
// written, and only then bounds-check the memory. This is NOT copy_user_string: a
// counted buffer may contain zero bytes and need not be NUL-terminated, so it is
// range-checked and copied by length into the kernel staging buffer.
//
// BLOCK-AWARE, so it takes the register pile and writes rax ITSELF, and must NOT on
// the path where file_write parks the task (a full pipe with a live reader). The
// re-armed int 0x50 reads the syscall number back out of rax, so writing a return
// value there would make the woken task issue a different call. Dispatched as a bare
// statement, never `regs->rax = sys_write(...)`. See docs/reference/blocking.md and
// the long comment on sys_readkey below.
static void sys_write(registers_t *r) {
    uint64_t fd = r->rdi;
    uint64_t user_buf = r->rsi;
    uint64_t len = r->rdx;

    task_t *t = scheduler_current_task();
    if (fd >= MAX_FDS || t->fds[fd] == NULL) {
        r->rax = SYSCALL_ERROR;
        return;
    }
    file_t *f = t->fds[fd];
    if (!f->writable) {
        r->rax = SYSCALL_ERROR;   // fd 0, or a pipe's read end: not writable
        return;
    }
    if (len == 0) {
        r->rax = 0;               // nothing to write is a successful no-op
        return;
    }

    uint64_t n = len < SYSCALL_IO_MAX ? len : SYSCALL_IO_MAX;
    if (!user_range_ok(user_buf, n)) {
        r->rax = SYSCALL_ERROR;
        return;
    }
    memcpy(io_staging, (const void *)user_buf, (size_t)n);

    long w = file_write(f, io_staging, (uint32_t)n, r);
    if (w == FILE_BLOCKED) {
        return;                   // parked; leave rax holding SYS_WRITE for the re-issue
    }
    if (w < 0) {
        r->rax = SYSCALL_ERROR;
        return;
    }
    r->rax = (uint64_t)w;
}

// SYS_READ: read a counted buffer from one of the caller's descriptors.
//   RDI = fd, RSI = user buffer pointer, RDX = length.
// Returns the number of bytes read, which MAY BE LESS than the length asked for and
// MAY BE 0, which is END OF FILE (a pipe drained with no writer left). Returns
// SYSCALL_ERROR on a bad fd, a wrong-direction fd (fd 1, or a pipe's write end), or
// a bad buffer. Validation order mirrors sys_write: fd, slot, direction, buffer.
//
// BLOCK-AWARE, and it carries the same RAX discipline as sys_readkey and sys_wait:
// on the path where file_read parks the task (an empty pipe with a live writer, or
// an empty console) it writes NOTHING to rax, because the re-armed int 0x50 reads
// the syscall number back out of rax and a stray value there would make the woken
// task issue a different call (a 0 would be SYS_EXIT). Dispatched as a bare
// statement. See docs/reference/blocking.md.
static void sys_read(registers_t *r) {
    uint64_t fd = r->rdi;
    uint64_t user_buf = r->rsi;
    uint64_t len = r->rdx;

    task_t *t = scheduler_current_task();
    if (fd >= MAX_FDS || t->fds[fd] == NULL) {
        r->rax = SYSCALL_ERROR;
        return;
    }
    file_t *f = t->fds[fd];
    if (f->writable) {
        r->rax = SYSCALL_ERROR;   // fd 1, or a pipe's write end: not readable
        return;
    }
    if (len == 0) {
        r->rax = 0;
        return;
    }

    uint64_t n = len < SYSCALL_IO_MAX ? len : SYSCALL_IO_MAX;
    if (!user_range_ok(user_buf, n)) {
        r->rax = SYSCALL_ERROR;
        return;
    }

    long got = file_read(f, io_staging, (uint32_t)n, r);
    if (got == FILE_BLOCKED) {
        return;                   // parked; leave rax holding SYS_READ for the re-issue
    }
    if (got < 0) {
        r->rax = SYSCALL_ERROR;
        return;
    }
    if (got > 0) {
        memcpy((void *)user_buf, io_staging, (size_t)got);
    }
    r->rax = (uint64_t)got;       // 0 delivered as EOF
}

// SYS_CLOSE: close one of the caller's descriptors.
//   RDI = fd.
// Frees the file_t and clears the slot (and, for a pipe end, drops the pipe's
// end-count and may wake a peer or free the pipe — see close_fd). Returns 0, or
// SYSCALL_ERROR on a bad fd. Does not block, so it returns through the dispatcher
// normally with no RAX concern.
static uint64_t sys_close(uint64_t fd) {
    task_t *t = scheduler_current_task();
    if (fd >= MAX_FDS || t->fds[fd] == NULL) {
        return SYSCALL_ERROR;
    }
    close_fd(t->fds, (int)fd);
    return 0;
}

// SYS_PIPE: create a pipe and hand back its two ends in the caller's table.
//   RDI = user pointer to int[2]; on success it receives [read_fd, write_fd].
// The out pointer is a WRITE TARGET from ring 3, so its whole 2*sizeof(int) range is
// bounds-checked before the kernel writes through it (same rule as SYS_STAT's size
// pointer). Allocates one pipe_t and two file_t and takes two table slots; on any
// failure part way it unwinds what it took, so a failed SYS_PIPE consumes nothing.
// The pipe starts with exactly these two ends (readers = writers = 1, set by
// file_alloc_pipe). Does not block.
static uint64_t sys_pipe(uint64_t user_out) {
    if (!user_range_ok(user_out, 2 * sizeof(int))) {
        print_string("syscall: SYS_PIPE rejected an out-of-bounds pointer\n");
        return SYSCALL_ERROR;
    }

    task_t *t = scheduler_current_task();
    pipe_t *p = pipe_create();
    if (p == NULL) {
        return SYSCALL_ERROR;
    }

    // Read end first. If there is no free slot, the pipe has no ends yet, so a plain
    // kfree returns it cleanly.
    int rfd = alloc_fd(t->fds);
    if (rfd < 0) {
        kfree(p);
        return SYSCALL_ERROR;
    }
    file_t *rend = file_alloc_pipe(p, 0);   // read end -> readers = 1
    if (rend == NULL) {
        kfree(p);
        return SYSCALL_ERROR;
    }
    t->fds[rfd] = rend;

    // Write end. From here the pipe has one end, so a failure unwinds through
    // close_fd(rfd): that drops readers to 0, and with writers still 0 both counts
    // are zero, so file_close frees the pipe as well as the read end. No leak.
    int wfd = alloc_fd(t->fds);
    if (wfd < 0) {
        close_fd(t->fds, rfd);
        return SYSCALL_ERROR;
    }
    file_t *wend = file_alloc_pipe(p, 1);   // write end -> writers = 1
    if (wend == NULL) {
        close_fd(t->fds, rfd);
        return SYSCALL_ERROR;
    }
    t->fds[wfd] = wend;

    // Both ends are installed and counted; report the numbers. The destination was
    // range-checked above and lies in the caller's own mapped pages.
    int *out = (int *)user_out;
    out[0] = rfd;
    out[1] = wfd;
    return 0;
}

// SYS_READKEY: pop one character from the keyboard ring buffer, sleeping until one
// arrives if the buffer is empty. No pointer crosses the ring boundary here, so
// there is nothing to bounds-check: the character is returned by value in RAX.
//
// BLOCKING. On an empty buffer the caller is parked (TASK_BLOCKED, WAIT_KEY) and
// the CPU goes to another task, or idles if there is none. The keyboard IRQ wakes
// it once it has a key. The caller cannot tell: from ring 3 this is one `int 0x50`
// that took a long time to come back.
//
// THERE IS NO LOOP HERE, and that is the point. Re-checking the buffer in a loop
// inside this handler would just move the shell's old busy-wait into ring 0, where
// it is worse: the kernel would spin with the scheduler unable to run and every
// other task starved. Instead this function is entered, finds nothing, blocks, and
// ENDS. What resumes the task is a fresh entry into this same handler later.
//
// Unlike this call's own arguments, the "did a key arrive" question is answered by
// the wake itself: a WAIT_KEY task is only ever woken by a push into the buffer, so
// the retry finds a key. If a wake ever proved spurious, the retry would simply
// block again, which is one more clean yield rather than a spin.
//
// Writing to regs->rax is done HERE rather than by the dispatcher, and only on the
// path that actually has an answer. On the blocking path rax must be left holding
// SYS_READKEY, because task_block rewinds rip onto the `int 0x50` and the CPU takes
// the syscall number from rax when that instruction runs again. Overwriting it with
// a return value would make the woken task issue a DIFFERENT call: rax = 0 is
// SYS_EXIT, so a stray "return 0" here would halt the machine on the next keypress.
static void sys_readkey(registers_t *regs) {
    int c = keyboard_getchar();
    if (c != 0) {
        regs->rax = (uint64_t)c;
        return;
    }

    // A signal can cut this short rather than parking the task. task_block reports
    // that, and this call must then FAIL rather than block again — the woken task
    // would otherwise re-issue SYS_READKEY, find the buffer still empty (Ctrl-C
    // pushes no character), and park forever with the signal undelivered (S5).
    // Writing rax here is correct and required: this is a path with an answer.
    if (task_block(regs, WAIT_KEY) == TASK_BLOCK_INTERRUPTED) {
        regs->rax = SYSCALL_ERROR;
        return;
    }

    // Unreachable on the blocking path: task_block redirected the pile through
    // schedule(), so this kernel entry now belongs to another task and ends at its
    // iretq. Nothing may be added after this call, least of all a write to rax.
}

// SYS_LIST: write the root directory's file names into a ring-3 buffer, one per
// line and NUL-terminated, and return how many were written.
//   RDI = user buffer pointer, RSI = buffer size.
// The pointer is UNTRUSTED, so the WHOLE [buf, buf+size) range is bounds-checked
// against the ring-3 region before the kernel writes a single byte through it.
// Returns the number of names written, or -1 on a bad pointer or a filesystem
// error. fat32_list_names silently drops names that do not fit.
static uint64_t sys_list(uint64_t user_buf, uint64_t bufsize) {
    if (!user_range_ok(user_buf, bufsize)) {
        print_string("syscall: SYS_LIST rejected an out-of-bounds buffer\n");
        return SYSCALL_ERROR;
    }
    uint32_t count = 0;
    if (fat32_list_names((char *)user_buf, (uint32_t)bufsize, &count) != 0) {
        return SYSCALL_ERROR;
    }
    return count;
}

// SYS_RUN: load and start the program named by a ring-3 string, joining it to the
// scheduler alongside the caller.
//   RDI = user pointer to a NUL-terminated 8.3 filename.
// The name is copied into the kernel first (bounds-checked and length-capped, so a
// missing terminator cannot run off the region). task_create_from_file does the
// rest, and it already REPORTS AND SKIPS a missing or malformed program rather than
// faulting, so a bad name here costs nothing but a returned -1: the kernel is never
// taken down by what a program asks to run. Returns 0 on success, -1 on a bad
// pointer, a load failure, or a bad inherited descriptor.
//
// RSI = in_fd, RDX = out_fd: the caller's descriptors to give the child as its fd 0
// and fd 1, or -1 for a fresh console. This is THE ONLY WAY a descriptor reaches a
// child, since nothing can inject one into a running task, so a pipeline hands the
// pipe ends across here at creation. task_create_from_file validates and inherits
// them (in_fd must be a read end, out_fd a write end).
//
// RCX = the process group the child joins: 0 to inherit the caller's (the old
// behaviour, and what a program that has never heard of groups gets), TASK_PGID_NEW
// for a new group led by the child, or an existing group to join. A FOURTH REGISTER
// RATHER THAN AN OVERLOADED EXISTING ONE: the three arguments already here keep
// exactly their old meanings and their old values, so every existing call site means
// what it always did and 0 is the old behaviour. RCX (not R10) because this kernel
// enters through `int 0x50`, which unlike the `syscall` instruction does not clobber
// RCX, so the fourth argument can sit where the System V C ABI already puts it.
// task_create_from_file rejects a group the caller may not join, and the create fails
// rather than quietly inheriting.
static uint64_t sys_run(uint64_t user_name, uint64_t in_fd, uint64_t out_fd,
                       uint64_t pgid_req) {
    char name[SYSCALL_NAME_MAX];
    if (copy_user_string(user_name, name, sizeof(name)) != 0) {
        print_string("syscall: SYS_RUN rejected a bad filename pointer\n");
        return SYSCALL_ERROR;
    }
    // The caller becomes the new task's parent, which is what later lets it wait
    // for the program it started. The id has to be taken HERE, inside the syscall,
    // because `current` is only the requesting task while its own syscall is being
    // served; by the next timer tick it means somebody else.
    int id = task_create_from_file(name, scheduler_current_id(), (int)in_fd, (int)out_fd,
                                   (uint32_t)pgid_req);
    if (id < 0) {
        // REPORT THE FREE FRAME COUNT, not just the failure. A create can fail after
        // it has already built a page-table tree and mapped part of a program into
        // it, so "it did not start" and "it did not cost anything" are different
        // claims and only the first one is obvious from the screen. This number is
        // what makes the second one checkable: run a name that does not exist ten
        // times over and every count must be the same. A count that steps down each
        // time means a failure path is stranding an address space, which is what
        // this kernel did until docs/decisions/0018's teardown was wired into
        // task_create_from_file. Guarded by LIFECYCLE_DEBUG (kernel/scheduler.h)
        // alongside the reap reports, since it is the same measurement.
        print_string("run failed: ");
        print_string(name);
#if LIFECYCLE_DEBUG
        print_string(", free frames: ");
        print_int((uint32_t)frame_free_count());
#endif
        print_string("\n");
        return SYSCALL_ERROR;
    }
    // Return the child's task id (always >= 1: id 0 is the boot task). The shell uses
    // it to tell one pipeline stage from another when it reaps them; an ordinary
    // `run` ignores it and only checks for the SYSCALL_ERROR failure value.
    return (uint64_t)id;
}

// SYS_READFILE: read a whole file off the disk into a ring-3 buffer.
//   RDI = user pointer to a NUL-terminated 8.3 filename,
//   RSI = user buffer pointer, RDX = buffer size.
// BOTH pointers are untrusted and BOTH are checked before use: the filename is
// copied in (capped) and the destination [buf, buf+size) is confirmed to lie in the
// ring-3 region. fat32_read_file writes at most the file's own size and refuses
// outright if it exceeds the buffer, so nothing overruns. Returns the number of
// bytes read, or -1 on a bad pointer, a missing file, or a read error. The bytes
// are raw file contents and are NOT NUL-terminated; the caller terminates them
// before treating the buffer as a string to print.
static uint64_t sys_readfile(uint64_t user_name, uint64_t user_buf, uint64_t bufsize) {
    char name[SYSCALL_NAME_MAX];
    if (copy_user_string(user_name, name, sizeof(name)) != 0) {
        print_string("syscall: SYS_READFILE rejected a bad filename pointer\n");
        return SYSCALL_ERROR;
    }
    if (!user_range_ok(user_buf, bufsize)) {
        print_string("syscall: SYS_READFILE rejected an out-of-bounds buffer\n");
        return SYSCALL_ERROR;
    }
    uint32_t read_size = 0;
    if (fat32_read_file(name, (void *)user_buf, (uint32_t)bufsize, &read_size) != 0) {
        return SYSCALL_ERROR;
    }
    return read_size;
}

// SYS_WRITEFILE: write a whole file to the disk from a ring-3 buffer, creating it
// or wholly replacing it.
//   RDI = user pointer to a NUL-terminated 8.3 filename,
//   RSI = user buffer pointer, RDX = length in bytes.
// The mirror image of SYS_READFILE and validated identically: the filename is
// copied in (bounds-checked and length-capped) and the source range [buf, buf+len)
// is confirmed to lie inside the ring-3 region before the kernel reads a byte
// through it. fat32_write_file does the crash-safe write. Returns 0 on success, -1
// on a bad pointer, a name that is not 8.3, no free space, or a disk error.
//
// DOES NOT BLOCK, so unlike SYS_READKEY and SYS_WAIT it has no RAX-discipline
// problem: it computes an answer and returns it through the dispatcher normally,
// and rax is never left holding the call number across a reschedule.
static uint64_t sys_writefile(uint64_t user_name, uint64_t user_buf, uint64_t len) {
    char name[SYSCALL_NAME_MAX];
    if (copy_user_string(user_name, name, sizeof(name)) != 0) {
        print_string("syscall: SYS_WRITEFILE rejected a bad filename pointer\n");
        return SYSCALL_ERROR;
    }
    if (!user_range_ok(user_buf, len)) {
        print_string("syscall: SYS_WRITEFILE rejected an out-of-bounds buffer\n");
        return SYSCALL_ERROR;
    }
    if (fat32_write_file(name, (const void *)user_buf, (uint32_t)len) != 0) {
        return SYSCALL_ERROR;
    }
    return 0;
}

// SYS_DELETE: delete a file from the disk by name.
//   RDI = user pointer to a NUL-terminated 8.3 filename.
// The name is copied in and length-capped exactly like SYS_RUN's. fat32_delete
// frees the file's chain and then unpublishes its directory entry. Returns 0 on
// success, -1 on a bad pointer, a name that is not 8.3, a missing file, or a disk
// error.
//
// DOES NOT BLOCK: same as SYS_WRITEFILE, it returns through the dispatcher with no
// RAX-discipline concern.
static uint64_t sys_delete(uint64_t user_name) {
    char name[SYSCALL_NAME_MAX];
    if (copy_user_string(user_name, name, sizeof(name)) != 0) {
        print_string("syscall: SYS_DELETE rejected a bad filename pointer\n");
        return SYSCALL_ERROR;
    }
    if (fat32_delete(name) != 0) {
        return SYSCALL_ERROR;
    }
    return 0;
}

// SYS_FREECOUNT: report how many clusters on the volume are free.
//   no args; the count is returned by value in RAX.
// Nothing crosses the boundary but a number, so there is nothing to bounds-check.
// This exists so the free-cluster count is observable from ring 3: the shell's
// `free` command prints it, and the leak test built on that command watches it
// hold steady across write/delete cycles. It is the same idea as SYS_RUN reporting
// the free frame count, one layer up. Does not block. fat32_free_count walks the
// whole FAT, so this is deliberately slow and off the allocation path.
static uint64_t sys_freecount(void) {
    return (uint64_t)fat32_free_count();
}

// SYS_STAT: report the size in bytes of a named file, so a caller can size a
// buffer before it reads.
//   RDI = user pointer to a NUL-terminated 8.3 filename,
//   RSI = user pointer to a uint64_t the size is written into.
// BOTH pointers are untrusted. The filename is copied in and length-capped exactly
// like SYS_RUN's. The out_size pointer is a WRITE TARGET, not just a read: the
// kernel writes eight bytes through it, so the whole [ptr, ptr+8) range is bounds-
// checked with user_range_ok, the same check SYS_READFILE uses on its destination
// buffer. Checking only the start pointer would let a crafted pointer sitting just
// below USER_REGION_END have the kernel write a uint64_t off the end of the region
// and into kernel pages. Returns 0 on success, SYSCALL_ERROR if the file is not
// found (or the name is not 8.3, or it names a directory) — fat32_stat folds those
// into one -1, and the shell tells "no such file" apart from "too big" by the size
// it gets back, not by which of these failed.
//
// DOES NOT BLOCK, so unlike SYS_READKEY and SYS_WAIT it has no RAX-discipline
// problem: it computes an answer and returns it through the dispatcher normally,
// and rax is never left holding the call number across a reschedule.
static uint64_t sys_stat(uint64_t user_name, uint64_t user_size) {
    char name[SYSCALL_NAME_MAX];
    if (copy_user_string(user_name, name, sizeof(name)) != 0) {
        print_string("syscall: SYS_STAT rejected a bad filename pointer\n");
        return SYSCALL_ERROR;
    }
    if (!user_range_ok(user_size, sizeof(uint64_t))) {
        print_string("syscall: SYS_STAT rejected an out-of-bounds size pointer\n");
        return SYSCALL_ERROR;
    }
    uint32_t size = 0;
    if (fat32_stat(name, &size) != 0) {
        return SYSCALL_ERROR;   // not found, not 8.3, or a directory
    }
    *(uint64_t *)user_size = size;   // validated writable above; zero-extends
    return 0;
}

// SYS_EXIT: end the calling task with the status in RDI.
//   RDI = exit status, masked to 0..255 by task_exit.
// This used to halt the machine, because there was no way for a task to stop
// without stopping everything: no parent to return to and no way to reclaim what
// the task held. Now it is a real exit. The task is marked TASK_ZOMBIE and leaves
// the rotation permanently, its parent is woken if it was waiting, and its memory
// is returned to the pools later by the scheduler's sweeper, from a tick that is no
// longer standing on the dying task's page tables.
//
// Does not return: task_exit ends in schedule(), which redirects the register pile
// into a different task, so this kernel entry finishes at that task's iretq.
static void sys_exit(registers_t *regs) {
    task_exit(regs, (int)regs->rdi);
}

// SYS_WAIT: block until any child of the caller exits, and return its exit status.
// No arguments: this is any-child, not waitpid, so a parent with several children
// gets whichever finished first and cannot ask for a particular one. Returns that
// child's status (0..255), or SYSCALL_ERROR if the caller has no children at all,
// which is the one case that cannot be answered by waiting.
//
// BLOCKING, and therefore subject to the same rule as SYS_READKEY: on the waiting
// path task_wait leaves RAX alone so the re-armed `int 0x50` still reads SYS_WAIT
// out of it. RAX is written only on the two paths that have an answer. See the long
// comment on sys_readkey above, and task_wait in kernel/scheduler.c.
//
// RDI, if nonzero, is a user pointer to a uint64_t that receives the id of the child
// that exited, so a caller reaping several children (a shell running a pipeline) can
// tell which one finished. It is validated HERE, because syscall.c owns
// user_range_ok, before task_wait writes through it on the answering path; 0 means
// "do not report the id". This re-validates on every re-issue after a block, since
// RDI is preserved across the re-arm.
static void sys_wait(registers_t *regs) {
    if (regs->rdi != 0 && !user_range_ok(regs->rdi, sizeof(uint64_t))) {
        regs->rax = SYSCALL_ERROR;
        return;
    }
    task_wait(regs);
}

// SYS_SETFG: declare which process group is in the foreground — the one the
// keyboard's Ctrl-C is addressed to.
//   RDI = pgid.
// Returns 0, or SYSCALL_ERROR if the caller is not allowed to name that group.
//
// FOREGROUND IS DECLARED, NOT INFERRED, and this call is the whole of the interface.
// It cannot be derived from SYS_RUN: D.ELF starts E.ELF and exits without waiting, so
// a foreground inferred from "most recently started" would follow to E and stay there
// while the user sits at a prompt, with every Ctrl-C going to a background program.
//
// The permission rule (scheduler_set_foreground) is what stops a program taking the
// keyboard for good. Nothing crosses the ring boundary but a number, so there is
// nothing to bounds-check. Does not block.
static uint64_t sys_setfg(uint64_t pgid) {
    if (scheduler_set_foreground(scheduler_current_id(), (uint32_t)pgid) != 0) {
        return SYSCALL_ERROR;
    }
    return 0;
}

// SYS_SIGNAL: install a handler for one signal on the calling task.
//   RDI = signal number, RSI = ring-3 handler address (0 restores the default),
//   RDX = the address of this program's sigreturn trampoline.
// Returns 0, or SYSCALL_ERROR for a signal that cannot be caught (SIG_KILL) or an
// address outside the ring-3 region.
//
// THE TRAMPOLINE COMES FROM THE PROGRAM, which looks odd until you ask where else it
// could come from. A handler ends in `ret` and needs a return address, and that
// address must be executable ring-3 code. Programs are separately linked ELFs at a
// fixed 0x400000 with no kernel-owned page mapped into them, so the kernel has
// nowhere to put one. userlib.h's wrapper supplies it and hides the argument, so a
// program writes sys_signal(SIG_INT, handler) and never sees it.
//
// Does not block, so it returns through the dispatcher normally.
static uint64_t sys_signal(uint64_t sig, uint64_t handler, uint64_t trampoline) {
    if (signal_install_handler(scheduler_current_task(), (int)sig, handler, trampoline) != 0) {
        return SYSCALL_ERROR;
    }
    return 0;
}

// SYS_SIGRETURN: resume the context interrupted when a signal handler was delivered.
//   no args.
// NEVER RETURNS NORMALLY, and is not a call any program should make directly: the
// only legitimate caller is the trampoline a handler returns through. Reached at any
// other moment it kills the caller (S7) rather than restoring a frame the program
// built for itself.
//
// Writes the whole register pile itself, so it is dispatched as a bare statement and
// never as `regs->rax = ...`: a return value written afterwards would land in the
// restored context's RAX and corrupt the interrupted program's state.
static void sys_sigreturn(registers_t *r) {
    signal_sigreturn(r);
}

// Dispatch on the call number in RAX. Unknown numbers are reported and rejected
// with SYSCALL_ERROR; a bad request must never fault or halt the kernel.
void syscall_handler(registers_t *regs) {
    switch (regs->rax) {
        case SYS_EXIT:
            sys_exit(regs);   // does not return; deliberately not `regs->rax = ...`
            break;
        case SYS_WRITE:
            sys_write(regs);   // writes rax itself, and must not on the block path
            break;
        case SYS_READKEY:
            sys_readkey(regs);   // writes rax itself, and must not on the block path
            break;
        case SYS_LIST:
            regs->rax = sys_list(regs->rdi, regs->rsi);
            break;
        case SYS_RUN:
            regs->rax = sys_run(regs->rdi, regs->rsi, regs->rdx, regs->rcx);
            break;
        case SYS_READFILE:
            regs->rax = sys_readfile(regs->rdi, regs->rsi, regs->rdx);
            break;
        case SYS_WRITEFILE:
            regs->rax = sys_writefile(regs->rdi, regs->rsi, regs->rdx);
            break;
        case SYS_DELETE:
            regs->rax = sys_delete(regs->rdi);
            break;
        case SYS_FREECOUNT:
            regs->rax = sys_freecount();
            break;
        case SYS_STAT:
            regs->rax = sys_stat(regs->rdi, regs->rsi);
            break;
        case SYS_READ:
            sys_read(regs);   // writes rax itself, and must not on the block path
            break;
        case SYS_CLOSE:
            regs->rax = sys_close(regs->rdi);
            break;
        case SYS_PIPE:
            regs->rax = sys_pipe(regs->rdi);
            break;
        case SYS_WAIT:
            sys_wait(regs);   // writes rax itself, and must not on the block path
            break;
        case SYS_SETFG:
            regs->rax = sys_setfg(regs->rdi);
            break;
        case SYS_SIGNAL:
            regs->rax = sys_signal(regs->rdi, regs->rsi, regs->rdx);
            break;
        case SYS_SIGRETURN:
            sys_sigreturn(regs);   // rewrites the whole pile; deliberately not `regs->rax = ...`
            break;
        default:
            print_string("syscall: unknown number ");
            print_int((uint32_t)regs->rax);
            print_string(", rejected\n");
            regs->rax = SYSCALL_ERROR;
            break;
    }

    // Deliver on the way out of the syscall too, not only on the timer path. This is
    // what makes a signal raised BY a syscall take effect immediately: SYS_KILL would
    // otherwise set a bit and return, and the target would not notice until the next
    // tick reached it. It is also the path that delivers to a task whose blocking
    // syscall was just cut short by a signal (S5), which never sees a timer tick in
    // between.
    //
    // Safe on every path through the switch above, including the ones that did not
    // return here in any ordinary sense: sys_exit and the blocking calls end in
    // schedule(), which leaves `regs` holding the INCOMING task's frame and `current`
    // naming that task, so the two still agree and delivery goes to the right place.
    check_signals(regs);
}
