#include "scheduler.h"
#include "syscall.h"     // SYSCALL_ERROR: the value SYS_WAIT returns to a childless caller
#include "gdt.h"
#include "usermode.h"
#include "heap.h"
#include "paging.h"
#include "memory.h"
#include "elf.h"
#include "../libc/mem.h"
#include "../drivers/screen.h"   // the LIFECYCLE_DEBUG reap report below

// Pointers to the heap-allocated tasks, in creation order (ids 0, 1, ...). Each
// task_t is kmalloc'd in task_register. A flat pointer array (rather than a linked
// list) keeps schedule()'s O(1) round-robin indexing a mechanical change from the
// old fixed array.
//
// SLOTS 0..num_tasks-1 MAY CONTAIN NULL. This used to be a dense array where a
// non-NULL pointer was guaranteed, and that assumption is now wrong: a reaped task
// has its struct kfree'd and its slot set back to NULL, leaving a HOLE. The id is
// never reused (a hole stays a hole forever, see num_tasks below), so an id remains
// a permanent, unambiguous name for one task and a stale id can never silently
// address a different one. The cost is that EVERY loop over this array must skip
// NULL: a missing skip dereferences a freed pointer and page-faults at a low
// address, tens of seconds after the task that made the hole exited, which reads
// like a fault with no cause. All of them are marked below.
//
// A task_t is kernel-only bookkeeping (only the code here reads it), never touched
// by ring-3 code, so it is safe on kernel (non-PG_USER) heap pages. That is why
// the STRUCT can go on the heap but the STACK below cannot.
static task_t *tasks[MAX_TASKS_LIMIT];

// Flags for a ring-3 stack page: present, writable, reachable at CPL 3. Program
// pages are mapped by the ELF loader instead, which derives their flags from the
// segment's own (kernel/elf.c), so a text segment can be mapped read-only.
#define USER_PAGE_FLAGS  (PG_PRESENT | PG_WRITABLE | PG_USER)

// Map a fresh stack into `as` at the fixed stack VA (USER_STACK_BASE up to
// USER_STACK_TOP, both in usermode.h). Fresh frames, no copy: the program writes
// its own stack as it runs. Every task's stack is at the SAME virtual address on
// its OWN physical frames, which is what per-process paging buys and what
// replaced the old bump allocator that split PD[3] by hand.
//
// Returns 0 on success, -1 if a frame or page-table allocation fails. On failure
// it LEAVES THE PARTIAL MAPPING IN PLACE and does not unwind it, which is not a
// leak because the caller destroys the whole tree on any failure (see
// task_create_from_file below). That division is deliberate: unwinding here would
// mean a second, near-duplicate teardown walk that only ever sees stacks, and two
// teardown walks that must agree about what a task owns is exactly how one of them
// drifts and starts freeing memory the task does not own. There is one teardown in
// this kernel, paging_destroy_address_space, and it takes whole trees.
static int map_user_stack(address_space_t *as) {
    for (uint64_t va = USER_STACK_BASE; va < USER_STACK_TOP; va += FRAME_SIZE) {
        uint64_t frame = alloc_frame();
        if (frame == 0) {
            return -1;
        }
        if (paging_map_page(as, va, frame, USER_PAGE_FLAGS) != 0) {
            return -1;
        }
    }
    return 0;
}

// Index of the task currently on the CPU.
static uint32_t current = 0;

// Forward declaration: task_create_from_file checks a group request long before the
// permission rule itself is defined, and the rule wants to sit beside the other two
// group functions rather than at the top of the file.
static int may_use_group(uint32_t caller_id, uint32_t pgid);

// The process group the keyboard talks to. Group 0 is task 0's — the shell's — so
// at boot Ctrl-C is addressed to the shell, which is the right default: the shell is
// the only thing running, and it is the thing the person is typing at.
//
// It is DECLARED by a task calling SYS_SETFG, never inferred from what was started
// most recently. See scheduler_set_foreground below and the note in scheduler.h.
static uint32_t foreground_pgid = 0;

// A HIGH WATER MARK, not a count of live tasks. Tasks are handed ids 0, 1, ... in
// creation order and this only ever grows: reaping a task frees its struct and
// NULLs its slot, but does NOT decrement this, because doing so would let the next
// creation hand out an id that is already the name of a dead task. A parent holding
// the id of a child it has not waited for would then be pointed at a stranger. So
// the walks below are bounded by this and skip the holes, and the array fills up
// over the life of the machine even though the live task count may not grow at all.
static uint32_t num_tasks = 0;

// Set while schedule() is parked in its all-blocked idle loop (idle_until_runnable
// below). The idle loop runs with interrupts ENABLED, which is the whole point, so
// timer ticks keep arriving and keep calling schedule() while we sit there. Those
// nested calls must do nothing and return: if a nested call parked in the idle loop
// too, every tick would nest one level deeper on the single shared kernel stack and
// a long idle would overflow it. With this flag the nesting depth stays at one, the
// nested tick unwinds back into the hlt loop, and the OUTER call (which owns the
// live pile we are going to overwrite) is the one that performs the switch.
static int scheduler_idling = 0;

// Guard against a startup race. The timer starts ticking the instant isr_install
// runs sti (long before scheduler_start), and each tick calls schedule(). Until
// task 0 has actually been entered, those early ticks fire in kernel (CPL 0)
// context and there is nothing to switch: schedule() must NOT save that kernel
// pile over a forged task or copy a forged task onto the kernel stack. This flag
// stays 0 until scheduler_start arms it, so schedule() is a no-op before then.
static int scheduler_running = 0;

// Register a task whose address space is already built, and forge its saved pile
// so iretq will "return" into a program that never ran.
//
// Both creation paths end here, so the forge exists in exactly one place. The
// only thing that differs between a compiled-in program and one loaded from a
// file is where `entry` came from: a linker symbol, or an ELF header.
//
// `parent_id` is stamped on the new task and never changes. It is recorded at
// creation because it cannot be recovered afterwards, and two things read it: the
// parent's SYS_WAIT, to find its children, and the reap sweeper, to decide whether
// a zombie still has anybody who might read its exit status.
//
// Returns the task id, or -1 if the heap is full or the task table is.
static int task_register(address_space_t *as, uint64_t entry, uint32_t parent_id,
                         uint32_t pgid_req) {
    if (num_tasks >= MAX_TASKS_LIMIT) {
        return -1;                          // bookkeeping array full (arbitrary cap)
    }

    // The task_t is kernel-only bookkeeping, so it is safe on the kernel heap.
    // This is what removes the old fixed-4 ceiling on task structs.
    task_t *t = (task_t *)kmalloc(sizeof(task_t));
    if (t == NULL) {
        return -1;                          // out of heap: same contract as old "table full"
    }

    // Every task is born with a console on fd 0 and fd 1: fd 0 an INPUT end that
    // reads the keyboard, fd 1 an OUTPUT end that writes the screen. Allocate them
    // BEFORE consuming an id below, so running out of heap here fails as cleanly as a
    // full table — no id is spent and nothing is published to tasks[]. That 0 = input
    // and 1 = output is a CONVENTION shared by the kernel, every program, and the
    // shell; nothing in the hardware or the dispatcher enforces it, and a program
    // that writes fd 0 or reads fd 1 is simply rejected. A pipeline child has one or
    // both of these replaced by an inherited pipe end (see task_create_from_file).
    file_t *fd_in = file_alloc_console(0);
    file_t *fd_out = file_alloc_console(1);
    if (fd_in == NULL || fd_out == NULL) {
        if (fd_in != NULL) kfree(fd_in);
        if (fd_out != NULL) kfree(fd_out);
        kfree(t);
        return -1;
    }

    uint32_t id = num_tasks++;
    tasks[id] = t;

    t->aspace = as;
    t->cr3 = as->pml4_phys;                 // cached so schedule() need not deref

    // Forge the pile so iretq will "return" into a program that never ran. This
    // is the exact trick enter_user_mode uses (usermode.c), generalised: instead
    // of pushing the five iretq values by hand and running iretq now, we fill the
    // registers_t the scheduler will later copy onto the stack, and let the
    // timer's iretq consume it.
    memset(&t->regs, 0, sizeof(t->regs));   // all GPRs start at 0
    t->regs.rip = entry;                    // first instruction (0x400000 region)
    t->regs.user_rsp = USER_STACK_TOP;      // fixed stack top, same VA in every task
    t->regs.cs = GDT_SELECTOR_USER_CODE;    // 0x1B: ring-3 code, RPL 3
    t->regs.ss = GDT_SELECTOR_USER_DATA;    // 0x23: ring-3 data, RPL 3

    // rflags bit 9 (IF) MUST be set. If IF is clear the task runs with interrupts
    // masked, the timer never fires, and this task is never preempted: it owns
    // the machine forever and no other task ever runs. USER_MODE_RFLAGS (0x202)
    // has bit 1 (reserved, always 1) and bit 9 (IF) set.
    t->regs.rflags = USER_MODE_RFLAGS;

    t->id = id;
    t->state = TASK_READY;
    t->wait_reason = WAIT_NONE;   // only meaningful once the task blocks
    t->parent_id = parent_id;     // who to wake and who may read the status below
    t->exit_status = 0;           // only meaningful once the task is a TASK_ZOMBIE
    t->sig_pending = 0;           // nothing raised on a task that has not run yet

    // The group. TASK_PGID_NEW names the group after this task, which is both the
    // Unix convention and, here, what makes a fresh group id unique without a
    // counter: ids are never reused, so no live group can already be called this.
    // The caller has already checked that any OTHER request is one it is allowed to
    // make (see task_create_from_file), so by this point pgid_req is either a
    // sentinel or a permitted group.
    if (pgid_req == TASK_PGID_NEW) {
        t->pgid = id;
    } else if (pgid_req == TASK_PGID_INHERIT) {
        t->pgid = (parent_id == TASK_NO_PARENT) ? 0u : tasks[parent_id]->pgid;
    } else {
        t->pgid = pgid_req;
    }

    // Install the console descriptors allocated above and clear the rest of the
    // table. From here the task can read fd 0 and write fd 1 the instant it runs.
    for (int i = 0; i < MAX_FDS; i++) {
        t->fds[i] = NULL;
    }
    t->fds[0] = fd_in;
    t->fds[1] = fd_out;
    return (int)id;
}

int task_create_from_file(const char *name, uint32_t parent_id, int in_fd, int out_fd,
                          uint32_t pgid_req) {
    // (0a) A request to join a NAMED existing group is checked FIRST, before any
    // resource is taken, under the same rule that governs the foreground. A request
    // the caller may not make fails the create outright rather than quietly falling
    // back to inheritance: a shell that asked for a job group and got its own would
    // build a job whose stages Ctrl-C reaches individually, and would have no way to
    // find out. The two sentinels need no check — inheriting is always allowed, and a
    // new group named after a task that does not exist yet cannot collide with
    // anything.
    if (pgid_req != TASK_PGID_INHERIT && pgid_req != TASK_PGID_NEW &&
        !may_use_group(parent_id, pgid_req)) {
        return -1;
    }

    // (0) Resolve any inherited descriptors against the CALLER's table FIRST, before
    // building anything, so a bad fd fails with nothing to undo. -1 means "fresh
    // console" (the default fds task_register makes), so only a real fd is resolved,
    // and this never touches the caller's table for kernel_main's boot task, which
    // passes -1/-1 before any task exists. in_fd must be a READ end and out_fd a
    // WRITE end: the child reads its fd 0 and writes its fd 1.
    file_t *in_src = NULL;
    file_t *out_src = NULL;
    if (in_fd != -1) {
        file_t **caller = scheduler_current_task()->fds;
        if (in_fd < 0 || in_fd >= MAX_FDS || caller[in_fd] == NULL || caller[in_fd]->writable) {
            return -1;                      // no such fd, or it is a write end
        }
        in_src = caller[in_fd];
    }
    if (out_fd != -1) {
        file_t **caller = scheduler_current_task()->fds;
        if (out_fd < 0 || out_fd >= MAX_FDS || caller[out_fd] == NULL || !caller[out_fd]->writable) {
            return -1;                      // no such fd, or it is a read end
        }
        out_src = caller[out_fd];
    }

    // Four steps, in this order: private address space, program loaded into it,
    // stack mapped, task registered and its frame forged. This is the only
    // creation path there is. It replaced one that copied a ring-3 image linked
    // into the kernel and took its entry from a linker symbol; the page tree,
    // the stack, the forge, the scheduler and the CR3 switch are all unchanged
    // from that, and only the source of the bytes and of `entry` differ.
    address_space_t *as = paging_create_address_space();
    if (as == NULL) {
        // NOTHING TO DESTROY HERE, and calling the teardown would be a mistake. A
        // failed create cleans up after itself internally (kernel/paging.c) and
        // hands back NULL rather than a half-built handle, so there is no tree and
        // no handle to pass on. This early return is the one failure path in this
        // function that must NOT be given a paging_destroy_address_space call.
        return -1;
    }

    // EVERY FAILURE FROM HERE ON MUST DESTROY `as`. By this point the tree exists
    // and owns real memory: three page-table frames from the create, plus whatever
    // the loader and the stack mapper managed to map before they gave up. Returning
    // without tearing it down strands all of it for the lifetime of the machine,
    // and it is invisible: a failed `run` prints an error and the town carries on
    // looking healthy while the free-frame count quietly steps down each time.
    //
    // This used to be excused with "tasks are never destroyed once built", which was
    // true before there was a teardown at all. There is one now
    // (docs/decisions/0018), it is PG_PRESENT-checked at every level, and it is
    // therefore safe on a tree that is only partly built: a create that failed
    // before the loader mapped anything leaves both user PD slots absent, and a
    // stack mapping that failed halfway leaves a page table with only some leaves
    // present. Both come apart correctly.
    uint64_t entry = 0;
    if (elf_load_file(name, as, &entry) != 0) {
        paging_destroy_address_space(as);
        return -1;      // elf_load_file has already said what was wrong with it
    }

    if (map_user_stack(as) != 0) {
        paging_destroy_address_space(as);
        return -1;
    }

    // Pre-allocate the inherited descriptors BEFORE registering the task, counting
    // their pipe ends now. Doing the last fallible allocation before the task becomes
    // schedulable means the steps after task_register cannot fail, so we never have
    // to tear down a live, already-registered task — only this not-yet-registered
    // address space and any dup already made (undone with file_close, which drops the
    // pipe count it took).
    file_t *in_dup = NULL;
    file_t *out_dup = NULL;
    if (in_src != NULL) {
        in_dup = file_dup(in_src);
        if (in_dup == NULL) {
            paging_destroy_address_space(as);
            return -1;
        }
    }
    if (out_src != NULL) {
        out_dup = file_dup(out_src);
        if (out_dup == NULL) {
            if (in_dup != NULL) file_close(in_dup);   // undo the count in_dup took
            paging_destroy_address_space(as);
            return -1;
        }
    }

    int id = task_register(as, entry, parent_id, pgid_req);
    if (id < 0) {
        // Out of kernel heap, or the bookkeeping array is full. The tree was built
        // perfectly; there is simply nowhere to record the task that would own it,
        // so nobody will ever free it later and it has to go back now. Undo the
        // inherited-end counts too, or the pipe would never be freed.
        if (in_dup != NULL) file_close(in_dup);
        if (out_dup != NULL) file_close(out_dup);
        paging_destroy_address_space(as);
        return -1;
    }

    // The task is registered with a fresh console on fd 0 and fd 1. Swap in whichever
    // ends are inherited, freeing the default console each displaces (close_fd frees a
    // console file_t outright — it has no pipe count). No allocation happens here, so
    // nothing below can fail. It is safe against a timer tick scheduling the child
    // early: this whole function runs inside sys_run with interrupts masked, so the
    // child cannot run until the syscall returns.
    if (in_dup != NULL) {
        close_fd(tasks[id]->fds, 0);
        tasks[id]->fds[0] = in_dup;
    }
    if (out_dup != NULL) {
        close_fd(tasks[id]->fds, 1);
        tasks[id]->fds[1] = out_dup;
    }
    return id;
}

uint32_t scheduler_current_id(void) {
    // `current` is file-static on purpose: nothing outside this file may index the
    // task table. A syscall handler still needs to know WHO is asking (SYS_RUN
    // stamps the new task's parent_id with it), so the id is exported and the table
    // is not.
    return current;
}

// May `caller_id` use `pgid` — as the foreground group, or as the group to put a
// child into? This is decision 3 of the signals rung, and it is the rule that keeps
// the keyboard reachable.
//
// A task may name its OWN group, or a group at least one of its OWN CHILDREN is
// already in. Nothing else. WITHOUT THIS RULE ANY PROGRAM COULD TAKE THE KEYBOARD
// AND NEVER GIVE IT BACK: it would name some other group as the foreground, every
// Ctrl-C from then on would be addressed to tasks the user is not looking at, and no
// key would reach the shell to undo it. There is no privileged task here that could
// take it back, so the rule has to hold at the moment the request is made.
//
// The child clause is what a shell needs and is the only reason this is not simply
// "your own group": a shell puts a pipeline in a group of its children and then has
// to name that group, which is not its own.
static int may_use_group(uint32_t caller_id, uint32_t pgid) {
    task_t *caller = (caller_id < num_tasks) ? tasks[caller_id] : NULL;
    if (caller == NULL) {
        return 0;
    }
    if (caller->pgid == pgid) {
        return 1;                   // your own group is always yours to name
    }
    for (uint32_t i = 0; i < num_tasks; i++) {
        task_t *t = tasks[i];
        if (t == NULL || t->state == TASK_ZOMBIE) {
            continue;               // a reaped hole, or a task that has already exited
        }
        if (t->parent_id == caller_id && t->pgid == pgid) {
            return 1;               // a group one of your children is in
        }
    }
    return 0;
}

uint32_t scheduler_foreground_pgid(void) {
    return foreground_pgid;
}

int scheduler_set_foreground(uint32_t caller_id, uint32_t pgid) {
    if (!may_use_group(caller_id, pgid)) {
        return -1;
    }
    foreground_pgid = pgid;
    return 0;
}

uint32_t scheduler_task_count(void) {
    // A HIGH-WATER MARK, NOT A LIVE COUNT: ids are never reused and a reaped task
    // leaves a permanent NULL hole, so this bounds a scan over tasks[] and says
    // nothing about how many tasks exist. Every caller must skip the holes.
    return num_tasks;
}

task_t *scheduler_task_by_id(uint32_t id) {
    // num_tasks is a high-water mark, not a live count: ids are never reused, so a
    // reaped task leaves a permanent NULL hole that this must step over rather than
    // dereference. See the tasks[] declaration above.
    if (id >= num_tasks) {
        return NULL;
    }
    return tasks[id];
}

task_t *scheduler_current_task(void) {
    // The one deliberate window into the table, for the descriptor syscalls that
    // must read and edit the CALLER's own fd table (SYS_READ/WRITE/CLOSE/PIPE). It
    // returns the running task, which during a syscall is the one that made the
    // call. Everything else about the table stays private to this file.
    return tasks[current];
}

void scheduler_start(void) {
    // Close the last sliver of the startup race. Between arming scheduler_running
    // and the iretq inside enter_user_mode there are a handful of instructions; a
    // timer tick landing there would try to switch while we are mid-handoff. cli
    // masks interrupts for those few instructions. It is safe because the first
    // thing that happens on entering task 0 is iretq restoring rflags = 0x202,
    // which sets IF again, so the timer resumes the moment ring-3 code runs.
    __asm__ __volatile__("cli");

    current = 0;
    tasks[0]->state = TASK_RUNNING;
    scheduler_running = 1;

    // Load task 0's address space BEFORE dropping to ring 3. Until now we have run
    // on the boot tables; task 0's user code and stack live in ITS tree, not the
    // boot tree, so entering ring 3 without this switch would fetch the first user
    // instruction through a mapping that no longer describes this task. This is
    // safe because the kernel half is cloned identically into task 0's tree, so
    // enter_user_mode (kernel code) and the task_t it reads (kernel heap) stay
    // mapped across the switch; only the user half changes. Every LATER switch is
    // done by schedule(); this is just the first one, which schedule() never runs.
    paging_switch(tasks[0]->aspace);

    // Reuse the proven ring-3 entry path rather than hand-rolling a second iretq.
    // Task 0's forged GPRs (all zero) do not matter on this first entry: a fresh
    // program sets up its own registers before it reads any. Every LATER entry
    // into task 0 goes through schedule(), which restores its full saved pile.
    enter_user_mode(tasks[0]->regs.rip, tasks[0]->regs.user_rsp);
}

// Is any task runnable at all? Used only by the idle loop, which asks about the
// whole table rather than about a position in the rotation.
static int any_task_ready(void) {
    for (uint32_t i = 0; i < num_tasks; i++) {
        if (tasks[i] == NULL) {
            continue;               // a reaped task's hole (see tasks[] above)
        }
        if (tasks[i]->state == TASK_READY) {
            return 1;
        }
    }
    return 0;
}

// The round-robin pick. Walk forward from the task after `from`, wrapping, and
// return the first TASK_READY slot, or -1 if there is none.
//
// Testing for TASK_READY (rather than "not UNUSED") is what makes a blocked task
// invisible to the rotation: it is stepped straight over, however many times the
// cursor comes round, until whatever it waits for marks it READY again. Candidate
// num_tasks is `from` itself, so a task that is still runnable keeps the CPU when
// nothing else wants it, and a task that has just BLOCKED itself does not match
// and so cannot be handed back the CPU it just gave up. The same test also makes a
// TASK_ZOMBIE unreachable: a dead task is never READY, so the rotation can never
// hand the CPU back to one, and no separate check for it is needed here.
//
// The walk is bounded by num_tasks, which is a HIGH WATER MARK and no longer the
// count of live tasks: slots 0..num_tasks-1 used to be exactly the live tasks, and
// since reaping began that is false. The cursor therefore steps over NULL holes as
// well as blocked tasks, and the array is scanned in full even when few tasks live.
static int find_next_ready(uint32_t from) {
    for (uint32_t i = 1; i <= num_tasks; i++) {
        uint32_t cand = (from + i) % num_tasks;
        if (tasks[cand] == NULL) {
            continue;               // a reaped task's hole (see tasks[] above)
        }
        if (tasks[cand]->state == TASK_READY) {
            return (int)cand;
        }
    }
    return -1;
}

// Everyone is blocked. Park the CPU until an interrupt makes someone runnable.
//
// INTERRUPTS MUST BE ENABLED HERE. This is the one place in the kernel where that
// is not just preferable but load-bearing: the ONLY thing that can produce a READY
// task is an interrupt handler (today the keyboard IRQ waking a WAIT_KEY task), so
// halting with interrupts masked would mean nothing could ever wake anyone and the
// machine would be dead, not idle. That is the deadlock to avoid.
//
// `hlt` with interrupts enabled is what makes "everyone asleep" cost ZERO CPU
// rather than spin. The CPU stops executing entirely and draws no power until a
// hardware interrupt arrives, instead of whirling through a loop that re-reads a
// variable nothing in this thread can change. Spinning here would be the same
// mistake as the busy-wait this whole change exists to remove, just moved into the
// scheduler.
//
// `sti; hlt` in one breath is deliberate and must stay adjacent. sti takes effect
// only AFTER the following instruction, precisely so this pair is atomic: an
// interrupt cannot slip into the gap, find nothing to wake, and leave us halted
// forever with the wakeup already spent. The condition is re-read with interrupts
// off (we enter with IF clear from the interrupt gate, and cli again after each
// wake), so a wakeup cannot be missed between the test and the halt.
static void idle_until_runnable(void) {
    scheduler_idling = 1;
    while (!any_task_ready()) {
        __asm__ __volatile__("sti; hlt; cli");
    }
    scheduler_idling = 0;
}

// Length in bytes of the `int 0x50` instruction that brings a task into a syscall.
// The opcode is CD ib: one byte of opcode, one immediate byte carrying the vector.
// Named, because a bare 2 buried in pointer arithmetic on a saved rip is unreadable
// and unsearchable.
#define INT_INSTR_LEN 2

void task_block(registers_t *r, wait_reason_t reason) {
    // (1) Take this task out of the rotation and record what it is waiting for, so
    // the waker for that event can find it again (see scheduler_wake).
    tasks[current]->state = TASK_BLOCKED;
    tasks[current]->wait_reason = reason;

    // (2) RE-ARM THE SYSCALL: resume ON the int, not after it, so the woken task
    // re-issues the syscall.
    //
    // This is the heart of the design and the part worth understanding. We cannot
    // freeze this task where it stands, half way through a C function in the
    // kernel, and thaw it here later. Two things in this kernel forbid it. The
    // saved pile holds the rip the CPU pushed on the ring-3 to ring-0 transition,
    // so it is always a RING-3 address, never a kernel one: restoring a pile can
    // only ever resume user code. And there is a single kernel stack shared by
    // every task (tss.rsp0, see gdt.c), so the C frames we are standing on right
    // now are abandoned the instant we switch away, and the next task to enter the
    // kernel writes over them.
    //
    // So instead of resuming in the middle, we resume at the beginning. The CPU
    // pushed the address of the instruction AFTER `int 0x50`; winding it back by
    // the length of that instruction points it at the int itself. When this task is
    // eventually woken and rescheduled, its iretq lands on the int, the syscall is
    // issued again from scratch, and this time it finds what it was waiting for
    // (that is precisely what being woken means) and returns normally.
    //
    // This only makes sense because the caller entered through `int 0x50`. A task
    // that reached here any other way would have its rip wound back into the middle
    // of whatever instruction happens to precede it, which is garbage. That
    // invariant is enforced by convention, not by the type system: see the header.
    r->rip -= INT_INSTR_LEN;

    // (3) Switch away through the SAME routine the timer uses. schedule() saves the
    // (now rewound) pile into this task, picks a READY task, overwrites the live
    // pile and loads the new CR3, so the iretq at the end of syscall_common_stub
    // returns into a different task. The only difference from the timer path is
    // what prompted the call, which is why one routine serves both: involuntary
    // preemption and a voluntary block are the same switch.
    //
    // No EOI concern here, unlike the timer path: `int 0x50` is a software
    // interrupt, so the PIC has no in-service bit to acknowledge.
    //
    // Interrupt-flag discipline matches the timer path exactly. Both handlers are
    // reached through interrupt gates, so IF is clear throughout, and the iretq
    // restores the incoming task's own rflags (IF set) as it returns to ring 3.
    // Nothing here leaves interrupts disabled across the yield, which matters: the
    // keyboard IRQ that will wake this task has to be able to fire.
    schedule(r);

    // Unreachable on the blocking path: schedule() redirected the pile, so this
    // kernel entry now belongs to another task and ends at that iretq.
}

void scheduler_wake(wait_reason_t reason) {
    // The block and the wake are a matched pair, and the pairing rule is that
    // whoever CAUSES an event wakes the tasks waiting on it. A blocked task cannot
    // wake itself: it is not running, so it cannot notice anything. That is the
    // whole point, and it is why this lives here and is called from the driver that
    // produced the event rather than from anything the sleeper does.
    //
    // A linear scan over every task, which is fine at this scale (a handful of
    // tasks) and is the honest simple thing. A kernel with many blocked tasks would
    // keep a per-reason wait queue instead and wake off the head in constant time.
    for (uint32_t i = 0; i < num_tasks; i++) {
        if (tasks[i] == NULL) {
            continue;               // a reaped task's hole (see tasks[] above)
        }
        if (tasks[i]->state == TASK_BLOCKED && tasks[i]->wait_reason == reason) {
            tasks[i]->state = TASK_READY;
            tasks[i]->wait_reason = WAIT_NONE;
        }
    }

    // Deliberately no context switch here. This runs in interrupt context, where
    // the live pile belongs to whatever was interrupted, not to the task we just
    // woke, so switching would be both wrong and unnecessary: the woken task is
    // back in the rotation and the next tick's schedule() will reach it.
}

// The two labels a reap report can carry, WHICH PATH DID THE FREEING. They are not
// cosmetic. Two different pieces of code can tear a dead task's address space down
// (see task_wait for why there are two), and in every ordinary test it is the
// parent's SYS_WAIT, every single time: the sweeper's own free path is reached only
// when a zombie's parent is gone, which needs a fixture built for it
// (user/tests/D.c). Without the label the two are indistinguishable on screen, so
// "the sweeper freed it" and "something else got there first" look identical and
// the only test of that code cannot tell whether it passed.
//
// Padded to the same width so the numbers after them line up in a column. A free
// frame count that drifts is the failure this reporting exists to catch, and a
// drift is obvious in an aligned column and easy to miss in a ragged one.
#define REAP_BY_WAIT     "reap (wait):    "
#define REAP_BY_SWEEPER  "reap (sweeper): "

// Called at the moment a dead task's address space has just gone back to the pools,
// by whichever of the two teardown paths got there first. It is a function rather
// than a line in each path so the two cannot drift into reporting different things.
//
// CALL IT ONLY WHERE THE FREE ACTUALLY HAPPENED. Both paths guard on
// `aspace != NULL`, so for any one task exactly one of them does real work and the
// other finds nothing to do; a report from the second would name a path that freed
// nothing and print a free frame count that says so, which is worse than silence.
static void lifecycle_report_reap(char *by, uint32_t id, int32_t status) {
#if LIFECYCLE_DEBUG
    print_string(by);
    print_string("task ");
    print_int(id);
    print_string(" exited (status ");
    print_int((uint32_t)status);
    print_string("), free frames: ");
    print_int((uint32_t)frame_free_count());
    // Heap bytes in use, ALONGSIDE the free frame count, so every leak test gets
    // small-object coverage for free: frame_free_count sees a leaked address space
    // but not a leaked file_t (~24 bytes), and heap_used_bytes sees the file_t. A
    // reap line whose two numbers both come back to baseline is a stronger all-clear.
    print_string(", heap used: ");
    print_int((uint32_t)heap_used_bytes());
    print_string("\n");
#else
    (void)by;
    (void)id;
    (void)status;
#endif
}

// Is there still somebody who could read a zombie's exit status?
//
// This is not simply "does the slot exist". A ZOMBIE PARENT DOES NOT COUNT. It has
// exited itself, so it will never issue another SYS_WAIT, and treating it as a live
// reader would keep its dead children's tombstones alive forever waiting on a call
// that cannot come: a permanent leak of one task_t per orphan, invisible in a free
// frame count because a task_t is a heap block rather than a frame.
static int parent_alive(uint32_t parent_id) {
    if (parent_id == TASK_NO_PARENT) {
        return 0;              // kernel_main started this task; nobody can wait on it
    }
    if (parent_id >= num_tasks) {
        return 0;              // an id never handed out (defensive: cannot happen)
    }
    if (tasks[parent_id] == NULL) {
        return 0;              // the parent has itself been reaped
    }
    return tasks[parent_id]->state != TASK_ZOMBIE;
}

// Free what the dead are still holding. Called from schedule() only, once per tick.
//
// The split of who frees what is the heart of this design. THE HEAVY RESOURCES (the
// user frames, the page tables, the address_space_t) ARE FREED HERE; the light ones
// (the task_t and its slot) are freed by the parent at wait time, because they hold
// the exit status the parent has not read yet. Splitting them is what lets the
// memory come back promptly without losing the answer to "how did it end".
//
// HOW TO ACTUALLY REACH THE FREE BELOW: `run d.elf`. In every ordinary case the
// parent's SYS_WAIT gets here first (task_wait explains why it nearly always wins
// the race), so this loop finds the zombie already stripped and does nothing, and a
// test that only runs normal programs never executes a line of it. D.ELF exists to
// break that: it starts E.ELF and exits without waiting, leaving E with a dead
// parent and nobody who can ever collect it. Watch for `reap (sweeper):` rather
// than `reap (wait):` in the output. See user/tests/README.md.
static void reap_sweep(void) {
    for (uint32_t i = 0; i < num_tasks; i++) {
        task_t *t = tasks[i];
        if (t == NULL || t->state != TASK_ZOMBIE) {
            continue;
        }

        // THE SAFETY ARGUMENT OF THE ENTIRE RUNG IS THIS ONE LINE. On the tick where
        // a task exits, `current` is STILL that task and ITS page tables are STILL
        // loaded in CR3: task_exit does paperwork and calls schedule(), and the CR3
        // write that moves off this tree happens at the very end of schedule(),
        // below. Destroying the tree here would hand the frames the CPU is at this
        // moment translating through back to the free pool. Nothing would fault now;
        // the frames would be reissued to the next task's page tables and the
        // machine would die later, somewhere unrelated. Skipping `current` means a
        // task is only ever swept on a LATER tick, by which time CR3 has moved on.
        if (i == current) {
            continue;
        }

        if (t->aspace != NULL) {
            paging_destroy_address_space(t->aspace);
            t->aspace = NULL;   // the tree is gone: never load or free it twice
            t->cr3 = 0;         // and never let this stale value reach CR3
            lifecycle_report_reap(REAP_BY_SWEEPER, t->id, t->exit_status);
        }

        // An orphan loses its tombstone. With nobody left to call SYS_WAIT, the
        // exit status has no reader, so keeping the struct would keep a fact nobody
        // will ever ask for. The id is NOT recycled: the slot stays NULL forever
        // (see num_tasks above), so nothing can be confused about who this was.
        if (!parent_alive(t->parent_id)) {
            kfree(t);
            tasks[i] = NULL;
        }
    }
}

void task_exit(registers_t *r, int status) {
    task_t *t = tasks[current];

    // (0) Close every descriptor this task holds, BEFORE it becomes a zombie and
    // before schedule() below, so the wakes happen while this task is still `current`
    // and the scheduler has not moved on. A task that exits without closing anything
    // is the normal case, and THIS LOOP IS WHAT MAKES EOF WORK AT ALL: closing a pipe
    // write end here drops writers to zero and wakes a downstream reader parked on an
    // empty pipe (close_fd -> file_close, the B2 wake), which is how `a.elf | count`
    // ends rather than hanging. Closing frees the file_t's now (the heap they sit on
    // is reclaimed immediately, unlike the address space, which the sweeper frees on a
    // later tick).
    for (int i = 0; i < MAX_FDS; i++) {
        if (t->fds[i] != NULL) {
            close_fd(t->fds, i);
        }
    }

    // (1) Record how it ended, MASKED TO 0..255. The mask is not cosmetic: SYS_WAIT
    // returns the status in RAX and returns SYSCALL_ERROR (-1) when the caller has
    // no children at all, so an unmasked status of -1 would be indistinguishable
    // from "you have no children" and a parent would mis-report a child that exited
    // perfectly normally. Masking makes the two value ranges disjoint by
    // construction rather than by hoping no program picks the wrong number.
    t->exit_status = status & 0xFF;

    // (2) Off the rotation for good. TASK_ZOMBIE is not TASK_BLOCKED: nothing will
    // ever wake it, and find_next_ready only ever returns TASK_READY slots, so the
    // CPU can never be handed back to it. wait_reason is cleared because it is only
    // meaningful while BLOCKED, and a zombie carrying a stale reason would be woken
    // by the next scheduler_wake for that reason and marked READY again.
    t->state = TASK_ZOMBIE;
    t->wait_reason = WAIT_NONE;

    // (3) Wake the parent if it is asleep in SYS_WAIT, BY ID, not by reason.
    //
    // scheduler_wake(WAIT_CHILD) would be wrong here, and would look right. It
    // readies EVERY task blocked on WAIT_CHILD, town-wide; the other waiting parents'
    // children have not exited, so each would be scheduled, re-issue its wait, find
    // no zombie among its own children, and block again. Worse, the wake is what
    // publishes "your child is ready to be reaped", so broadcasting it announces
    // something untrue to everyone but one task. WAIT_CHILD is the first wait reason
    // whose event belongs to ONE specific sleeper rather than to whoever is
    // listening, which is why it is the first one woken by id.
    //
    // THIS MUST HAPPEN BEFORE schedule(). If the parent is still BLOCKED when
    // schedule() runs and it is the only other task in the town, find_next_ready
    // returns -1, schedule() parks in idle_until_runnable, and nothing can ever make
    // anyone ready again: the only task that could have woken the parent is this one,
    // and it is already dead. The machine sits in `hlt` forever with no message. That
    // is a deadlock built out of two individually correct pieces, so the ordering is
    // load-bearing and not merely tidy.
    if (parent_alive(t->parent_id)) {
        task_t *p = tasks[t->parent_id];
        if (p->state == TASK_BLOCKED && p->wait_reason == WAIT_CHILD) {
            p->state = TASK_READY;
            p->wait_reason = WAIT_NONE;
        }
    }

    // (4) Switch away, through the same routine the timer drives.
    //
    // NOTE WHAT IS NOT HERE. There is no rip rewind: that is task_block's trick, for
    // a task that will run again and re-issue its syscall, and this task must never
    // run again. Nothing is freed: this task's page tables are still in CR3 right
    // now, so the frames go back on a later tick, from the sweeper above, which is
    // the only context that is guaranteed not to be standing on them. And rax is not
    // written, because there is nobody left to return a value to.
    schedule(r);

    // Unreachable: schedule() redirected the pile into another task, and no path
    // will ever pick a TASK_ZOMBIE again.
}

void task_wait(registers_t *r) {
    int have_child = 0;

    for (uint32_t i = 0; i < num_tasks; i++) {
        task_t *t = tasks[i];
        if (t == NULL || t->parent_id != current) {
            continue;
        }
        have_child = 1;

        if (t->state != TASK_ZOMBIE) {
            continue;           // alive: keep looking, it may not be the only child
        }

        // A child has already exited. Take its status AND its id (read the id before
        // the struct is freed below), then take it apart.
        int32_t status = t->exit_status;
        uint32_t child_id = t->id;

        // Freeing the address space HERE as well as in the sweeper is DELIBERATE and
        // is not a duplicated responsibility to be tidied away. A parent can call
        // SYS_WAIT on the very tick its child exited, before the sweeper has had a
        // chance to run, and this path is about to free the task_t: without this the
        // struct (and with it the only pointer to the tree) would be gone and the
        // whole address space would leak. Both paths guard on aspace != NULL, so
        // whichever arrives first does the work and the other does nothing.
        //
        // In practice THIS path usually wins, which is worth knowing before reading
        // the sweeper as the normal case. task_exit wakes the parent and switches
        // straight to it, the parent's iretq lands on its rewound `int 0x50`, and it
        // re-enters this function without a timer tick in between, so the sweeper
        // has had no opportunity to run. The sweeper's copy is what collects a child
        // whose parent is slow to wait, or never waits at all.
        //
        // It is safe here for the same reason the sweeper's skip of `current` makes
        // it safe there: this is a CHILD of the caller, and a child is by definition
        // not the task running right now, so this tree is not the one in CR3.
        if (t->aspace != NULL) {
            paging_destroy_address_space(t->aspace);
            t->aspace = NULL;
            t->cr3 = 0;
            lifecycle_report_reap(REAP_BY_WAIT, t->id, status);
        }

        kfree(t);
        tasks[i] = NULL;        // a permanent hole; the id is never reused

        // Report WHICH child, if the caller asked. r->rdi is a user pointer the
        // sys_wait wrapper already validated (or zero, meaning "do not report"). This
        // is what lets a shell running a pipeline match the reaped child against its
        // last stage and report that stage's status (docs/reference/shell.md).
        if (r->rdi != 0) {
            *(uint64_t *)r->rdi = (uint64_t)child_id;
        }
        r->rax = (uint64_t)(int64_t)status;
        return;
    }

    if (!have_child) {
        // Nothing to wait for, and nothing that could ever arrive. Blocking here
        // would be an unbreakable sleep: only task_exit wakes a WAIT_CHILD sleeper,
        // and with no children nothing can ever call it on this task's behalf. Fail
        // loudly-in-RAX instead. SYSCALL_ERROR is safe to write because this path
        // does NOT block (see below).
        r->rax = SYSCALL_ERROR;
        return;
    }

    // Children exist but none has exited yet. Sleep until one does.
    //
    // NOTHING MAY WRITE r->rax ON THIS PATH, and this is the second syscall to carry
    // that rule (the first is sys_readkey, kernel/syscall.c, where it is documented
    // at length). task_block rewinds rip onto the `int 0x50`, so when the woken task
    // re-executes that instruction the CPU reads the SYSCALL NUMBER out of RAX.
    // Leaving a return value there makes the woken task issue a DIFFERENT syscall,
    // and here the accident is spectacular rather than subtle: SYS_EXIT is 0, so
    // writing 0 (a perfectly ordinary "child exited successfully" status) would make
    // the woken parent kill itself instead of reporting the result. The shell is
    // usually that parent.
    task_block(r, WAIT_CHILD);
}

void schedule(registers_t *r) {
    // Ignore ticks that fire before task 0 has been entered (see the flag above).
    if (!scheduler_running) {
        return;
    }

    // A tick that landed while the outer call is parked in idle_until_runnable has
    // nothing useful to do and must not park as well (see scheduler_idling above).
    // Returning here unwinds it straight back into that hlt loop, which re-checks.
    if (scheduler_idling) {
        return;
    }

    // Collect what the dead are still holding, before anything else happens.
    //
    // THE PLACEMENT IS THE DESIGN, not a convenience. It has to be INSIDE schedule()
    // because schedule() is the only code in the kernel that runs after a CR3 switch
    // has moved off a dead task's tree; anywhere else (in task_exit, in the syscall
    // dispatcher, in a timer callback) would still be standing on the address space
    // it is trying to free. And it has to skip `current`, which it does, because on
    // the tick where a task exits, `current` is still that very task.
    //
    // It runs after the two guards above so it never fires before the scheduler is
    // armed or while a nested tick is unwinding out of the idle loop, and before the
    // save below so a freed task's memory is out of the way before this tick starts
    // shuffling piles around.
    reap_sweep();

    // DEFENSIVE, AND IT SHOULD BE UNREACHABLE. Every other loop over tasks[] skips
    // NULL because a reaped task leaves a hole, but the CURRENT slot is different:
    // it should never be a hole by construction. Only two things free a task_t, and
    // neither can free this one. The sweeper below explicitly refuses to touch
    // `current`, and task_wait only ever frees a CHILD of the caller, which by
    // definition is not the caller. If this ever fires, one of those two invariants
    // has been broken and the alternative is dereferencing a freed pointer as the
    // very first act of the switch, so it returns rather than reads.
    if (tasks[current] == NULL) {
        return;
    }

    // EOI ORDERING: the End-Of-Interrupt to the PIC is already sent, by
    // irq_handler (kernel/isr.c), BEFORE it calls this handler. That order is
    // load-bearing here: once we overwrite the pile below and the stub runs iretq
    // into the NEXT task, this handler invocation never returns, so any EOI we
    // tried to send AFTER the switch would never run, the timer line would stay
    // masked, and no further ticks would arrive. The machine would freeze after
    // exactly one switch. Because irq_handler already acked the PIC, the timer
    // keeps firing across the switch. Do not move the EOI after the switch.

    // (1) Save the pile the timer interrupted into the current task's slot.
    tasks[current]->regs = *r;

    // Put the outgoing task back into the rotation ONLY if it was actually running.
    // This used to be an unconditional TASK_READY, which is now wrong: task_block
    // marks the current task TASK_BLOCKED and then drives this very function to
    // switch away, so clobbering the state here would put the task straight back
    // into the rotation and undo the block on the spot, and the "blocked" task
    // would be handed the CPU again a tick later having waited for nothing.
    if (tasks[current]->state == TASK_RUNNING) {
        tasks[current]->state = TASK_READY;
    }

    // (2) Pick the next TASK_READY slot, round-robin, wrapping around. num_tasks
    // (not the MAX_TASKS_LIMIT cap) bounds the walk: it is the high water mark of
    // ids ever handed out, so every task that exists is inside it, and the walk
    // steps over the NULL holes reaping leaves behind.
    int picked = find_next_ready(current);

    // (2a) Nothing is runnable: every task is blocked waiting for something. Do not
    // spin, and do not fall back on a blocked task, which would resume a program in
    // the middle of a wait it has not finished. Sleep the CPU until an interrupt
    // makes someone ready, then ask again, which now succeeds.
    if (picked < 0) {
        idle_until_runnable();
        picked = find_next_ready(current);
    }

    uint32_t next = (uint32_t)picked;
    tasks[next]->state = TASK_RUNNING;

    // If the only ready task is the one we interrupted, there is nothing to switch
    // to. Leave the pile untouched so the stub returns to the same program.
    if (next == current) {
        return;
    }

    current = next;

    // (3) THE SWITCH. Copy the next task's saved pile OVER the live pile in place.
    // This is the whole point and the easiest thing to get wrong: iretq pops its
    // five values (and the stub pops the GPRs) from the STACK, not from this
    // array. Writing tasks[next].regs into a local, or anywhere but through r,
    // would leave the on-stack pile unchanged and iretq would return to the SAME
    // program. It must be written through r, which points at the live stack.
    *r = tasks[next]->regs;

    // (4) SWITCH ADDRESS SPACES. Load the next task's CR3 so its private user half
    // becomes active: the identical VAs (code at 0x400000, stack top 0x800000) now
    // resolve to THIS task's own frames, which is the isolation. Writing CR3 also
    // flushes the TLB (we use no global pages), dropping the previous task's stale
    // user translations for free.
    //
    // ORDERING TRAP: this MUST come after we are done touching the outgoing task's
    // world and before iretq returns to ring 3, and it is only safe mid-interrupt
    // because EVERYTHING the CPU still needs on the way out lives in the KERNEL
    // half, which is cloned identically into every tree: the register pile `r` is
    // on the kernel stack, the tasks[] array and this code are kernel .data/.text,
    // and when the timer next fires the IDT, GDT, TSS rsp0 stack and interrupt
    // stub are all reached through the same kernel mappings. So the switch changes
    // only the user half; the kernel never disappears out from under itself. If
    // any of those lived in the user half this would triple-fault on the next
    // instruction. Use the cached cr3 to avoid a double dereference on this path.
    __asm__ __volatile__("mov %0, %%cr3" : : "r"(tasks[next]->cr3) : "memory");
}
