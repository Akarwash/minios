#ifndef SYSCALLS_H
#define SYSCALLS_H

// ============================================================================
// Syscall numbers: the ABI shared by the kernel and ring-3 programs.
// ============================================================================
// This header is DELIBERATELY standalone: numbers and nothing else, no types,
// no function declarations, no includes. A ring-3 program compiles against it
// without pulling in any kernel code (the kernel's pages are not user-readable,
// so a user program must not depend on anything that lives there).
//
// The convention on the wire: RAX holds the syscall number below; arguments
// follow in RDI, RSI, RDX (System V order); the return value comes back in RAX.
// The vector the program raises is SYSCALL_VECTOR (0x50) from include/vectors.h.

#define SYS_EXIT     0   // RDI = exit status (masked to 0..255); ends the calling task. Never returns.
#define SYS_WRITE    1   // RDI = fd, RSI = buffer, RDX = length; write the bytes to that descriptor, return bytes written (may be < length), or SYSCALL_ERROR
#define SYS_READKEY  2   // no args; return one buffered key in RAX, or 0 if none
#define SYS_LIST     3   // RDI = buffer, RSI = size; list root dir names, return count
#define SYS_RUN      4   // RDI = filename ptr, RSI = in_fd, RDX = out_fd (-1 for a fresh console), RCX = process group request (0 inherit, -1 new group, else join that group); load and start it, giving it those descriptors as fd 0/1; return the child's task id, or SYSCALL_ERROR
#define SYS_READFILE 5   // RDI = filename ptr, RSI = buffer, RDX = size; return bytes read
#define SYS_WAIT     6   // RDI = uint64_t *out_id or 0; block until any child exits; RAX = that child's exit status, or SYSCALL_ERROR if the caller has no children; writes the exited child's id through out_id when it is nonzero
#define SYS_WRITEFILE 7  // RDI = filename ptr, RSI = buffer, RDX = length; 0 on success, SYSCALL_ERROR on failure
#define SYS_DELETE    8  // RDI = filename ptr; 0 on success, SYSCALL_ERROR on failure
#define SYS_FREECOUNT 9  // no args; return the count of free clusters on the volume. Exposes fat32_free_count so the shell's `free` command (and the leak test) can watch it.
#define SYS_STAT     10  // RDI = name, RSI = uint64_t *out_size; 0 on success, SYSCALL_ERROR if not found. Reports a file's size without reading it, so a caller can size a buffer first. Does not block.
#define SYS_READ     11  // RDI = fd, RSI = buffer, RDX = length; read up to length bytes, return the count, 0 at EOF, SYSCALL_ERROR on a bad fd. Blocks on an empty pipe/console.
#define SYS_CLOSE    12  // RDI = fd; close the descriptor, return 0, or SYSCALL_ERROR on a bad fd. Does not block.
#define SYS_PIPE     13  // RDI = int[2] out; make a pipe, write [read_fd, write_fd], return 0 or SYSCALL_ERROR. Does not block.
#define SYS_SIGNAL   14  // RDI = signal, RSI = ring-3 handler address (0 to restore the default), RDX = trampoline address; install a handler for this task. 0 or SYSCALL_ERROR. Does not block.
#define SYS_KILL     15  // RDI = task id, RSI = signal; raise that signal on that task. 0 or SYSCALL_ERROR. Does not block.
#define SYS_SIGRETURN 16 // no args; restore the context saved when a handler was delivered. NEVER RETURNS NORMALLY: the frame it restores resumes the interrupted instruction. Only the trampoline calls it.
#define SYS_TASKS    18  // RDI = task_info_t buffer, RSI = size in bytes; fill it with one entry per live task and return the count. See include/taskinfo.h.
#define SYS_SETFG    17  // RDI = pgid; make that process group the foreground, the one Ctrl-C is addressed to. 0 or SYSCALL_ERROR. Does not block.

#endif
