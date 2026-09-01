#include "../drivers/screen.h"
#include "../drivers/keyboard.h"
#include "../drivers/disk.h"
#include "../fs/fat32.h"
#include "elf.h"
#include "gdt.h"
#include "isr.h"
#include "timer.h"
#include "memory.h"
#include "heap.h"
#include "scheduler.h"

// The program the machine boots into: the interactive shell, a ring-3 binary on
// the disk (SHELL.ELF, built from user/shell.c). It is launched alone rather than
// alongside the letter-printers (A/B/C.ELF), which would interleave their output
// with the prompt and make the shell awkward to type against. Those programs still
// live on the disk, so the shell can start them on demand with `run A.ELF`, which
// is a cleaner demonstration of the scheduler than booting them automatically.
// Changing what the machine runs means rebuilding one binary and copying it onto
// the image, with no kernel rebuild.
static char *user_programs[] = { "SHELL.ELF" };
#define USER_PROGRAM_COUNT (sizeof(user_programs) / sizeof(user_programs[0]))

void kernel_main(uint64_t multiboot_info_addr) {
    gdt_init();          // describe memory (flat segments) + load the TSS
    isr_install();       // install interrupt handlers, remap PIC, enable interrupts
    timer_init(100);     // 100 Hz heartbeat on IRQ0
    keyboard_init();     // listen for keypresses on IRQ1

    screen_clear();
    print_string("Welcome to TownOS!\n");

    // Read the real amount of RAM from the Multiboot map, extend the identity map
    // to cover it (capped at 1GB), and flush the TLB. This must happen before
    // memory_init so every frame the allocator manages is actually mapped.
    uint64_t top_of_ram = memory_detect_and_map(multiboot_info_addr);
    print_string("Detected RAM: ");
    print_int((uint32_t)(top_of_ram >> 20));   // 0 means the no-map fallback ran
    print_string(" MB\n");
    memory_init(multiboot_info_addr);          // size the frame pool from real RAM

    heap_init();        // build the kernel heap on top of the frame allocator

    disk_init();        // probe the primary ATA bus and silence its IRQ line

    if (fat32_init() != 0) {
        print_string("FAT32: mount failed, programs cannot be loaded\n");
    }

    print_string("Loading ring-3 programs from disk...\n");

    // Create one ring-3 task per program file. Each read, parse, and load can
    // fail on its own (a missing file, a corrupt one, a program that asks to be
    // put somewhere it is not allowed to go), and a failure must cost only that
    // task: the loader reports the reason and this skips it, leaving the others
    // to run. A bad file on the disk is not a reason to take the kernel down.
    uint32_t started = 0;
    for (uint32_t i = 0; i < USER_PROGRAM_COUNT; i++) {
        // TASK_NO_PARENT: kernel_main started this, not another task. There is no
        // task in existence yet that could be its parent, so nothing will ever
        // call SYS_WAIT on it and its tombstone is dropped by the sweeper rather
        // than collected. See task_exit and reap_sweep in kernel/scheduler.c.
        // -1, -1: a fresh console on fd 0 and fd 1. The boot task inherits no pipe
        // ends (there is no caller to inherit from), so it reads the keyboard and
        // writes the screen like any ordinary `run` with no pipeline around it.
        if (task_create_from_file(user_programs[i], TASK_NO_PARENT, -1, -1,
                                  TASK_PGID_INHERIT) >= 0) {
            started++;
        }
    }

    if (started == 0) {
        print_string("No programs loaded, nothing to schedule.\n");
        while (1) {
            __asm__ __volatile__("hlt");
        }
    }

    print_string("Starting scheduler with ");
    print_int(started);
    print_string(" ring-3 tasks...\n");

    // Hand off to the scheduler. This is the LAST thing kernel_main does:
    // scheduler_start enters task 0 and never returns; from here on the timer
    // interrupt switches between the tasks. If we ever reach the loop below, the
    // handoff silently failed.
    scheduler_start();

    while (1) {
        __asm__ __volatile__("hlt");
    }
}
