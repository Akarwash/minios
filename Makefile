# ============================================================================
# Cross toolchain install (x86_64-elf) — this repo builds a freestanding
# x86-64 kernel and needs a cross compiler that targets bare metal, not the host.
#
# This machine (macOS / Apple Silicon, Homebrew) — the following was run and works:
#   brew install x86_64-elf-gcc x86_64-elf-binutils nasm qemu
#   # verified: x86_64-elf-gcc 16.1.0, x86_64-elf-ld (binutils) 2.46.1,
#   #           nasm 3.01, qemu 11.0.0
#
# Debian / Ubuntu (reference — not run here):
#   sudo apt install nasm qemu-system-x86
#   # No distro package ships an x86_64-elf cross gcc/binutils; build from source
#   # (see https://wiki.osdev.org/GCC_Cross-Compiler) targeting --target=x86_64-elf,
#   # or on 64-bit hosts the native gcc/ld can be used with the flags below.
#
# The kernel links and boots. `make` produces two artifacts:
#   townos.elf  the linked ELF64 image, with 64-bit symbols for gdb
#   townos.bin  the same image repackaged as a 32-bit ELF, which is what QEMU's
#               Multiboot -kernel loader accepts (see the townos.bin rule below)
# ============================================================================

# The target `make` with no arguments builds. GNU make otherwise picks the FIRST
# explicit target in the file, which for a long time was `user/SHELL.ELF` — a bare
# `make` reported "user/SHELL.ELF is up to date" and built no kernel at all. Naming
# the goal here fixes that in a way that reordering the rules would not: a rule added
# above `all:` later cannot silently take the default back.
.DEFAULT_GOAL := all

# Compilers and tools
CC = x86_64-elf-gcc
LD = x86_64-elf-ld
ASM = nasm
OBJCOPY = x86_64-elf-objcopy
QEMU = qemu-system-x86_64

# Compiler flags
#   -m64            build 64-bit code
#   -mno-red-zone   the red zone is unsafe once interrupts run in kernel mode
#   -mcmodel=kernel code/data live in the negative 2GB; required for a 64-bit kernel
CFLAGS = -ffreestanding -m64 -mno-red-zone -mcmodel=kernel -fno-pie -nostdlib -nodefaultlibs -Wall -Wextra
ASMFLAGS = -f elf64
LDFLAGS = -T linker.ld -nostdlib

# Source files
C_SOURCES = kernel/kernel.c kernel/gdt.c kernel/idt.c kernel/isr.c kernel/timer.c kernel/memory.c \
            kernel/usermode.c kernel/syscall.c kernel/scheduler.c kernel/heap.c kernel/paging.c \
            kernel/elf.c kernel/file.c kernel/pipe.c kernel/signal.c \
            drivers/screen.c drivers/ports.c drivers/keyboard.c drivers/disk.c \
            fs/fat32.c \
            libc/mem.c libc/string.c
ASM_SOURCES = boot/boot.asm kernel/gdt_flush.asm kernel/isr_stubs.asm

# Object files (replace .c with .o and .asm with .o)
C_OBJECTS = $(C_SOURCES:.c=.o)
ASM_OBJECTS = $(ASM_SOURCES:.asm=.o)

ALL_OBJECTS = $(ASM_OBJECTS) $(C_OBJECTS)

# ---------------------------------------------------------------------------
# User programs: separate binaries, not part of the kernel image
# ---------------------------------------------------------------------------
# Each user program is compiled and linked on its own into a static ELF64 file
# that lands on the disk image. The kernel reads and loads it at runtime, so
# changing a program means rebuilding one small binary and copying it onto the
# image, not rebuilding the kernel.
#
#   -mcmodel=small  NOT -mcmodel=kernel. The kernel model assumes every symbol
#                   lives in the top 2GB of the address space; user code links at
#                   0x400000, nowhere near that, and the kernel model produces
#                   relocation errors on it.
#   -static         no dynamic linking; the loader resolves nothing at runtime.
#   -nostdlib       no host libc and no startup files. The entire runtime a
#                   program gets is user/userlib.h.
#   -fno-pie -no-pie  position DEPENDENT. The loader does not relocate, so the
#                   program must be linked at the exact address it loads at.
USER_CFLAGS = -ffreestanding -m64 -mno-red-zone -mcmodel=small -fno-pie -no-pie \
              -nostdlib -nodefaultlibs -static -Wall -Wextra
USER_LD_SCRIPT = user/user.ld

# -z max-page-size=4096 keeps the linker from padding segments out to its default
# 2MB alignment, which would bloat each binary enormously for no benefit here.
USER_LDFLAGS = -T $(USER_LD_SCRIPT) -Wl,-z,max-page-size=4096 -Wl,--build-id=none

# 8.3 uppercase names because the filesystem reads 8.3 names only, and the source
# file names match the on-disk names so the mapping needs no explaining. SHELL.ELF
# is the exception: its source is user/shell.c (lowercase), so it needs the explicit
# rule below rather than the pattern rule, which would look for user/SHELL.c and
# only resolve to shell.c on a case-insensitive filesystem.
#
# TWO KINDS OF PROGRAM, IN TWO DIRECTORIES. user/ holds programs the machine is for
# (today just the shell); user/tests/ holds fixtures that exist to prove a piece of
# the kernel works and would be pointless on a machine anybody used. They build
# identically and land on the same disk; the split is about what a reader should
# conclude when one of them looks strange. See user/tests/README.md.
USER_PROGRAMS = user/tests/A.ELF user/tests/B.ELF user/tests/C.ELF \
                user/tests/D.ELF user/tests/E.ELF user/tests/F.ELF user/tests/G.ELF \
                user/tests/COUNT.ELF user/tests/UPPER.ELF user/SHELL.ELF

user/%.ELF: user/%.c user/userlib.h $(USER_LD_SCRIPT) include/syscalls.h include/vectors.h
	$(CC) $(USER_CFLAGS) $(USER_LDFLAGS) -o $@ $<

# The kernel test fixtures. Same recipe; a separate rule because the source lives a
# directory down and the prerequisite path has to say so.
#
# This rule and the one above BOTH match user/tests/X.ELF (the one above with the
# stem "tests/X"). Make resolves that by preferring the shorter stem, so this rule
# wins, which is what we want: it is the one whose prerequisites name the right
# source file.
user/tests/%.ELF: user/tests/%.c user/userlib.h $(USER_LD_SCRIPT) include/syscalls.h include/vectors.h
	$(CC) $(USER_CFLAGS) $(USER_LDFLAGS) -o $@ $<

# The interactive shell. Same recipe as the pattern rule, but the target and source
# names differ in case, so it is spelled out explicitly and portably.
user/SHELL.ELF: user/shell.c user/userlib.h $(USER_LD_SCRIPT) include/syscalls.h include/vectors.h
	$(CC) $(USER_CFLAGS) $(USER_LDFLAGS) -o $@ $<

# Default target
all: townos.bin $(USER_PROGRAMS)

# Link everything into the ELF64 image. This keeps the 64-bit symbols gdb needs.
townos.elf: $(ALL_OBJECTS)
	$(LD) $(LDFLAGS) -o $@ $^

# Repackage the ELF64 as a 32-bit ELF for booting.
#   QEMU's built-in Multiboot -kernel loader rejects an ELF64 image ("Cannot load
#   x86-64 image, give a 32bit one"). Our entry code (boot/boot.asm) starts in
#   32-bit protected mode and climbs to long mode itself, and every address in the
#   image lives in low memory, so relabelling the container as elf32-i386 is
#   accepted by the loader and boots correctly. The code is unchanged; only the
#   ELF header class differs. gdb should point at townos.elf for symbols.
townos.bin: townos.elf
	$(OBJCOPY) -O elf32-i386 $< $@

# Compile C files
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Assemble ASM files
%.o: %.asm
	$(ASM) $(ASMFLAGS) $< -o $@

# Disk image for the ATA driver and the FAT32 filesystem, created once if absent.
#
# 64MB, not 16MB: FAT32 is only legal with at least 65525 clusters, and 16MB
# cannot reach that with a sane cluster size (it would need 256-byte clusters,
# which is smaller than a block). Formatting tools refuse a 16MB FAT32 volume or
# silently hand back FAT16 instead. 64MB clears the bar comfortably at one block
# per cluster.
#
# tools/mkdisk.sh formats the image and copies in the test files with mtools (no
# sudo, no mounting). The rule has no prerequisites, so make skips it whenever
# disk.img already exists, and the script bails out too: reformatting on every
# `make run` would silently destroy the disk's contents.
DISK_IMG = disk.img
DISK_SIZE = 64M

$(DISK_IMG):
	./tools/mkdisk.sh $(DISK_IMG) $(DISK_SIZE)

# Copy the user programs onto the image. Deliberately PHONY, so it runs on every
# `make run` even when the image already exists.
#
# The image itself must be created once and then left alone (reformatting would
# destroy its contents), but the program binaries are build output and must never
# be stale: running an old A.ELF because the image was not refreshed looks exactly
# like a loader bug and costs an afternoon. mcopy -o overwrites without asking.
#
# EVERYTHING GOES IN THE ROOT DIRECTORY, `::/`, AND MUST KEEP DOING SO, even though
# the sources now sit in two directories. fs/fat32.c looks a name up in the ROOT
# DIRECTORY ONLY: it takes a bare 8.3 name, does no path parsing, and has no
# directory traversal, so the root is the only directory the kernel can see. Copying
# the fixtures into a `TEST/` directory on the image would make them unreachable,
# not tidy: `run a.elf` would report the file as missing. A source-tree folder is the
# only kind of folder this project has today, and giving the disk one needs
# subdirectory support in the filesystem, which is a rung of its own.
.PHONY: disk-programs
disk-programs: $(DISK_IMG) $(USER_PROGRAMS)
	mcopy -o -i $(DISK_IMG) $(USER_PROGRAMS) ::/

# ---------------------------------------------------------------------------
# Generated test files for the disk
# ---------------------------------------------------------------------------
# HUGE.TXT exists to exercise one path: what happens when a file is bigger than the
# buffer the caller offered. Until it existed the largest thing on the disk was
# BIG.TXT at 16KB, well under the shell's 32KB buffer, so that path had never run
# once. 40960 bytes clears the buffer with room to spare.
#
# WHAT IT ACTUALLY SHOWS, which is not what the shell's own comment expects:
# fat32_read_file refuses a file larger than the buffer outright rather than
# filling the buffer and stopping, so `read HUGE.TXT` prints `read: cannot read
# huge.txt` and no file contents at all. The shell's "showing the first N bytes"
# notice is therefore unreachable for a large file, and reachable only for one of
# exactly 32767 bytes, where it would be a false alarm on a complete file. Having
# the file on the disk is what makes that visible; it is not fixed here.
#
# IT IS GENERATED HERE AND NOT IN tools/mkdisk.sh, which is where HELLO.TXT,
# TEST.TXT and BIG.TXT come from. That script formats an image and refuses to run
# against one that already exists, so a file added there arrives on a freshly
# formatted disk and on no other: every developer with a disk.img already in their
# tree would never see it. Generating this as build output and copying it on every
# `make run` is the same reasoning that already makes disk-programs phony. The
# three files mkdisk.sh writes stay where they are; there is no second copy of
# their contents anywhere, and moving them would mean writing one.
#
# The lines are numbered and padded to a fixed width so that a human reading
# truncated output can see exactly which byte the read stopped at. The last line is
# a marker: if END OF FILE is on screen, the file was not truncated.
DISK_TESTFILES = HUGE.TXT

# 640 lines of 64 bytes is 40960 exactly, before the marker line.
HUGE_LINES = 640
HUGE_PAD = 43

HUGE.TXT:
	awk -v n=$(HUGE_LINES) -v w=$(HUGE_PAD) 'BEGIN { \
	    pad = sprintf("%" w "s", ""); gsub(/ /, ".", pad); \
	    for (i = 1; i <= n; i++) printf "HUGE.TXT line %05d %s\n", i, pad; \
	    print "HUGE.TXT END OF FILE"; \
	}' > $@

# Copy them onto the image, for the same reason and in the same way as the
# programs above: the image is created once and left alone, but anything that is
# build output must be refreshed on every boot or a stale copy will be what runs.
.PHONY: disk-testfiles
disk-testfiles: $(DISK_IMG) $(DISK_TESTFILES)
	mcopy -o -i $(DISK_IMG) $(DISK_TESTFILES) ::/

# Run in QEMU with the disk attached to the primary ATA bus.
#   if=ide      put the drive on the emulated IDE/ATA controller (NOT virtio or
#               AHCI), so it answers at I/O ports 0x1F0-0x1F7 where the driver looks
#   index=0     first drive on that controller = primary bus master
#   format=raw  the file is a flat byte array, no qcow layering
run: townos.bin disk-programs disk-testfiles
	$(QEMU) -kernel townos.bin -drive file=$(DISK_IMG),format=raw,if=ide,index=0,media=disk

# Clean build files. The disk image is NOT removed: it is not build output, it is
# the machine's disk, and the test files on it were put there by hand.
#
# The user binaries are removed through $(USER_PROGRAMS) rather than by a wildcard,
# so this list can never fall behind the list that is built: adding a program in one
# place adds it in both. The stale user/*.ELF line covers binaries left behind by a
# build from before the fixtures moved into user/tests/, which `make clean` would
# otherwise never touch again and which would sit on the disk image looking current.
#
# $(DISK_TESTFILES) goes too: it is generated, so removing it is free and the next
# `make run` writes it again. The COPY of it already on the disk image stays, along
# with everything else there, because the image is not build output.
clean:
	rm -f $(ALL_OBJECTS) townos.bin townos.elf $(USER_PROGRAMS) $(DISK_TESTFILES)
	rm -f user/A.ELF user/B.ELF user/C.ELF user/user_program.o