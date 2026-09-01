#ifndef TASKINFO_H
#define TASKINFO_H

#include "types.h"

// ============================================================================
// What SYS_TASKS reports: one of these per live task.
// ============================================================================
// This is the kernel's first debugging tool, and the first time the task table
// has been visible from outside the kernel at all. It exists because SYS_KILL
// needs an id, and an id the user has to guess is not an interface: `ps` and
// `kill` are a pair, and neither is much use alone.
//
// It has a header of its own rather than living in include/syscalls.h, because
// that header is deliberately numbers-only — no types, no declarations — so that
// a ring-3 program can include it and be certain it has pulled in nothing else.
// A struct crossing the ring boundary is a type, so it goes here.
//
// THIS STRUCT IS AN ABI. The kernel writes it straight into a ring-3 buffer, so
// its layout is a promise to every program compiled against it. Adding a field
// in the middle silently misaligns every existing binary; add at the end, or
// change the syscall.

// Task states, as they cross the ring boundary.
//
// DELIBERATELY SEPARATE FROM the kernel's own task_state_t (kernel/scheduler.h)
// rather than the same enum exported. The kernel's states are an internal matter
// and should stay free to change — splitting TASK_BLOCKED by reason, say —
// without silently changing what a compiled `ps` prints. The kernel maps one to
// the other explicitly, so a new internal state has to be considered here rather
// than leaking out with whatever number it happened to get.
#define TASK_INFO_READY    1   // runnable, waiting for a slice
#define TASK_INFO_RUNNING  2   // on the CPU right now
#define TASK_INFO_BLOCKED  3   // waiting for an event, off the rotation
#define TASK_INFO_ZOMBIE   4   // exited; kept only until its status is collected

typedef struct {
    uint32_t id;           // this task's id; what SYS_KILL takes
    uint32_t parent_id;    // who started it, or 0xFFFFFFFF for a task nobody started
    uint32_t pgid;         // its process group: what Ctrl-C is addressed to
    uint32_t state;        // one of the TASK_INFO_* values above
    int32_t  exit_status;  // MEANINGFUL ONLY when state == TASK_INFO_ZOMBIE, else 0
} task_info_t;

#endif
