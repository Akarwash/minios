#include "file.h"
#include "pipe.h"                // pipe_read/pipe_write, and the pipe_t innards close_fd touches
#include "heap.h"
#include "scheduler.h"          // task_block, scheduler_wake, WAIT_KEY/WAIT_PIPE_*
#include "../drivers/screen.h"
#include "../drivers/keyboard.h" // keyboard_getchar, the console read source

// ============================================================================
// File descriptors: the read/write interface behind a task's fd table.
// ============================================================================
// A descriptor hides whether bytes are going to the screen or into a pipe. Stage 1
// knows only the console; stage 3 adds the pipe branch to file_write/file_read and
// the pipe-end bookkeeping to close_fd. See docs/decisions/0022-file-descriptors-
// and-pipes.md.

file_t *file_alloc_console(int writable) {
    file_t *f = (file_t *)kmalloc(sizeof(file_t));
    if (f == NULL) {
        return NULL;
    }
    f->kind = FD_CONSOLE;
    f->pipe = NULL;
    f->writable = writable;
    return f;
}

long file_write(file_t *f, const char *buf, uint32_t len, registers_t *r) {
    // The console is always ready, never blocks, and accepts everything it is
    // handed, so `r` (the block path's pile) is unused here.
    (void)r;

    if (f->kind == FD_CONSOLE) {
        // Print the counted buffer BYTE BY BYTE, not as a string. It is a counted
        // buffer that may contain no terminator and may legitimately contain zero
        // bytes, so print_string (which stops at the first NUL) would be wrong.
        for (uint32_t i = 0; i < len; i++) {
            print_char(buf[i]);
        }
        return (long)len;
    }

    // A pipe: hand the bytes to the ring buffer, which may take only some of them or
    // park the task if it is full and a reader is still alive.
    return pipe_write(f->pipe, buf, len, r);
}

long file_read(file_t *f, char *buf, uint32_t len, registers_t *r) {
    if (f->kind == FD_CONSOLE) {
        // Drain up to `len` characters from the keyboard ring. If none is waiting,
        // park the task on WAIT_KEY exactly as sys_readkey does, and the keyboard IRQ
        // wakes it when a key arrives.
        //
        // A CONSOLE NOW HAS AN END OF FILE, which it did not when ADR 0022 was
        // written: Ctrl-D sets a flag in the keyboard driver and this reports it as a
        // zero-byte read. That is the whole of the line discipline. Without it a
        // program shaped as "read fd 0 until EOF" — COUNT.ELF is exactly that — could
        // never finish when run on its own rather than downstream of a pipe.
        uint32_t n = 0;
        while (n < len) {
            int c = keyboard_getchar();
            if (c == 0) {
                break;
            }
            buf[n++] = (char)c;
        }
        if (n > 0) {
            return (long)n;
        }

        // ORDER MATTERS: characters first, then EOF, then block. Buffered characters
        // are delivered before the end of input is reported, so typing "abc" and then
        // Ctrl-D gives a reader "abc" and then 0, rather than dropping the three
        // characters that were already typed. Checking EOF before draining the ring
        // would lose them.
        if (keyboard_console_eof()) {
            return 0;             // end of input: the reader's loop terminates
        }

        task_block(r, WAIT_KEY);
        return FILE_BLOCKED;      // parked; the caller must not touch rax
    }

    // A pipe: drain the ring buffer, or block for a writer, or return EOF.
    return pipe_read(f->pipe, buf, len, r);
}

file_t *file_alloc_pipe(struct pipe *p, int writable) {
    file_t *f = (file_t *)kmalloc(sizeof(file_t));
    if (f == NULL) {
        return NULL;             // counted nothing: the caller frees the pipe if empty
    }
    f->kind = FD_PIPE;
    f->pipe = p;
    f->writable = writable;
    // Count this end. The pipe's readers/writers ARE the count of live ends, so the
    // moment an end exists it must be counted (and file_close uncounts it). This is
    // the single place a pipe end is tallied; there is deliberately no second count
    // on the file_t (B4).
    if (writable) {
        p->writers++;
    } else {
        p->readers++;
    }
    return f;
}

file_t *file_dup(file_t *src) {
    file_t *f = (file_t *)kmalloc(sizeof(file_t));
    if (f == NULL) {
        return NULL;
    }
    *f = *src;                   // same kind, same pipe pointer, same direction
    if (f->kind == FD_PIPE) {
        // A new END exists now, so count it — that is what makes the child's copy a
        // real, independently-closable end and not a mere alias. The shared pipe_t is
        // the entire connection between the two tasks.
        if (f->writable) {
            f->pipe->writers++;
        } else {
            f->pipe->readers++;
        }
    }
    return f;
}

void file_close(file_t *f) {
    if (f->kind == FD_PIPE) {
        pipe_t *p = f->pipe;

        // Drop this end, and if it was the LAST of its kind, WAKE the peers blocked
        // on the opposite condition. THIS WAKE IS THE MOST IMPORTANT DOZEN LINES IN
        // THE RUNG. A reader parked on an empty pipe is only ever woken by a write;
        // if the last writer CLOSES instead of writing, nothing here would wake it and
        // it would wait forever for an EOF it can never observe (observing it needs it
        // to run). Closing is an event, and the pairing rule — whoever causes the
        // condition wakes the waiters — applies to it exactly as to a write (B2 in
        // ADR 0022, and docs/reference/blocking.md).
        if (f->writable) {
            p->writers--;
            if (p->writers == 0) {
                scheduler_wake(WAIT_PIPE_READ);   // readers now see EOF
            }
        } else {
            p->readers--;
            if (p->readers == 0) {
                scheduler_wake(WAIT_PIPE_WRITE);  // writers now see a broken pipe
            }
        }

        // The pipe is nobody's now: free it. The counts are the number of live ends,
        // so both at zero means no end anywhere can still reach this buffer.
        if (p->readers == 0 && p->writers == 0) {
            kfree(p);
        }
    }
    kfree(f);
}

void close_fd(file_t **fds, int fd) {
    file_t *f = fds[fd];
    if (f == NULL) {
        return;                  // already closed, or never opened: nothing to do
    }
    fds[fd] = NULL;              // clear the slot BEFORE freeing, so it is never a dangling pointer
    file_close(f);
}

int alloc_fd(file_t **fds) {
    // The lowest free slot, so descriptor numbers stay small and predictable. The
    // number is CHOSEN BY THE KERNEL and reported back to the caller; it is an index
    // into THIS task's table only and names nothing in any other task's.
    for (int i = 0; i < MAX_FDS; i++) {
        if (fds[i] == NULL) {
            return i;
        }
    }
    return -1;                   // table full
}
