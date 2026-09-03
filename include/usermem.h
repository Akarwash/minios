#ifndef USERMEM_H
#define USERMEM_H

// ============================================================================
// The ring-3 address space, as numbers shared by the kernel and by programs.
// ============================================================================
// DELIBERATELY STANDALONE, like include/syscalls.h: constants and nothing else,
// so a ring-3 program can include it without pulling in kernel code. Both sides
// need the same picture. The kernel bounds every pointer a syscall receives
// against it (user_range_ok, kernel/memory.h) and places SYS_MMAP regions inside
// it; a program's printf validates a %s pointer against it before dereferencing,
// and its malloc rounds requests to USER_PAGE_SIZE. Two copies of these numbers
// would be two copies that drift, and the one that drifted would be the one that
// let a bad pointer through.
//
// The layout is three 2MB page-directory slots, each private to a task:
//
//   PD[2]  USER_CODE_BASE    0x400000-0x5FFFFF  the loaded program image
//   PD[3]  (below STACK_LIMIT) 0x600000-0x7FFFFF  the stack, fixed size, at the top
//   PD[4]  USER_HEAP_BASE    0x800000-0x9FFFFF  SYS_MMAP regions, lowest first
//
// Everything at USER_SPACE_END and above belongs to the kernel, and a ring-3
// pointer there is rejected before anything dereferences it. The code and stack
// slots are mapped when a program is loaded; the heap slot starts EMPTY and is
// filled one SYS_MMAP region at a time. See docs/reference/user-memory.md.

#define USER_PAGE_SIZE    4096UL        // the mapping granularity: SYS_MMAP rounds to this

#define USER_CODE_BASE    0x400000UL    // where user/user.ld links every program (PD[2])
#define USER_STACK_LIMIT  0x800000UL    // top of PD[3]: the stack grows down from here

#define USER_HEAP_BASE    0x800000UL    // bottom of PD[4]: the first SYS_MMAP region lands here
#define USER_HEAP_LIMIT   0xA00000UL    // top of PD[4]: the 2MB ceiling no region may cross

#define USER_SPACE_END    USER_HEAP_LIMIT   // one past the last address a program may name

#endif
