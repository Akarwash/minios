#include "pipe.h"
#include "file.h"               // FILE_BLOCKED, FILE_ERR
#include "scheduler.h"          // task_block, scheduler_wake, WAIT_PIPE_*
#include "heap.h"

// ============================================================================
// Pipes: the ring buffer, and the block/wake/EOF rules.
// ============================================================================
// The buffer arithmetic is the keyboard driver's, one slot left unused so empty and
// full stay distinct. The interesting part is what a pipe adds: blocking when there
// is nothing to do and waking whoever can make progress, and treating a closed end
// as an event. See docs/decisions/0022-file-descriptors-and-pipes.md.

pipe_t *pipe_create(void) {
    pipe_t *p = (pipe_t *)kmalloc(sizeof(pipe_t));
    if (p == NULL) {
        return NULL;
    }
    p->read_index = 0;
    p->write_index = 0;
    p->readers = 0;   // no ends yet; file_alloc_pipe/file_dup count each as it appears
    p->writers = 0;
    return p;
}

// Bytes currently buffered, and free slots (minus the one always left unused so
// full is distinguishable from empty).
static uint32_t pipe_used(const pipe_t *p) {
    return (p->write_index + PIPE_SIZE - p->read_index) % PIPE_SIZE;
}
static uint32_t pipe_space(const pipe_t *p) {
    return (PIPE_SIZE - 1) - pipe_used(p);
}

long pipe_read(pipe_t *p, char *buf, uint32_t len, registers_t *r) {
    uint32_t used = pipe_used(p);
    if (used > 0) {
        uint32_t n = used < len ? used : len;
        for (uint32_t i = 0; i < n; i++) {
            buf[i] = p->buf[p->read_index];
            p->read_index = (p->read_index + 1) % PIPE_SIZE;
        }
        // Draining made room, so a writer parked on a full pipe can now proceed.
        // CHANGE THE BUFFER FIRST (done), WAKE SECOND: a writer woken before the room
        // existed would run, still find the pipe full, and re-block, wasting a round
        // trip. The wake is coarse (every WAIT_PIPE_WRITE task); extra wakers simply
        // re-block. See docs/reference/blocking.md.
        scheduler_wake(WAIT_PIPE_WRITE);
        return (long)n;
    }

    // Empty. EMPTY IS NOT FINISHED; empty with no writer is finished. If a writer is
    // still open, the bytes may yet come, so wait for them. If not, no byte can ever
    // arrive, and that is EOF.
    if (p->writers > 0) {
        // A signal can cut this short instead of parking us: task_block reports that
        // rather than blocking, and the read fails so the signal can be delivered on
        // the way out. Without it, Ctrl-C on a task blocked reading a pipe would wake
        // it, have it re-issue the read, find the pipe still empty — a signal is not
        // data — and block again, forever (S5).
        if (task_block(r, WAIT_PIPE_READ) == TASK_BLOCK_INTERRUPTED) {
            return FILE_ERR;
        }
        return FILE_BLOCKED;      // parked; the caller must not touch rax
    }
    return 0;                     // EOF
}

long pipe_write(pipe_t *p, const char *buf, uint32_t len, registers_t *r) {
    // NO READER CAN EVER DRAIN THIS PIPE. Blocking would be forever and buffering
    // would be pointless, so fail. Unix raises SIGPIPE here; an error return is
    // enough for a kernel with no signals.
    if (p->readers == 0) {
        return FILE_ERR;
    }

    uint32_t space = pipe_space(p);
    if (space == 0) {
        // Full, but a reader is alive to drain it: park until it does (backpressure).
        // Interruptible for the same reason the read above is (S5).
        if (task_block(r, WAIT_PIPE_WRITE) == TASK_BLOCK_INTERRUPTED) {
            return FILE_ERR;
        }
        return FILE_BLOCKED;
    }

    uint32_t n = space < len ? space : len;
    for (uint32_t i = 0; i < n; i++) {
        p->buf[p->write_index] = buf[i];
        p->write_index = (p->write_index + 1) % PIPE_SIZE;
    }
    // Bytes are in the buffer now, so a reader parked on empty can proceed. Publish
    // FIRST, wake SECOND, same ordering rule as above.
    scheduler_wake(WAIT_PIPE_READ);
    return (long)n;
}
