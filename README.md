# TownOS

A hobby x86-64 operating system kernel built from scratch in C and assembly (not for real world use, just for me to learn how an OS works and the decisions made to make it work in this way).

TownOS boots via Multiboot, climbs the CPU into 64-bit long mode, sets up the GDT
and TSS, and boots into an interrupt-driven interactive shell. It is a learning
kernel: ring-3 tasks preempted round-robin on the timer tick, each in its own
address space, running ELF64 programs read by name off a read-only FAT32 disk. The
point is to understand how an operating system works by building a small real one,
not to be a production OS.

## Status

TownOS **builds, links, and boots**. The kernel links into `townos.elf`, is
repackaged as the Multiboot-loadable `townos.bin`, and under QEMU boots into an
interactive shell that is itself a ring-3 program (`SHELL.ELF`) loaded off the disk
image. What works, what was deliberately never built, and what is next are in
[docs/project-status.md](docs/project-status.md).

## Quick start

Install the toolchain (macOS, Homebrew):

```bash
brew install x86_64-elf-gcc x86_64-elf-binutils nasm qemu
```

Build:

```bash
make
```

For Linux instructions, exact versions, how to run under QEMU, and how to debug,
see [docs/building.md](docs/building.md).

## Documentation

TownOS keeps two separate bodies of documentation:

- **[docs/](docs/README.md)** is project documentation: factual, derived from the
  source. What TownOS is, how it is built, and why the load-bearing decisions were
  made. Start here to build or hack on the kernel.
- **[learnings/](learnings/README.md)** is conceptual learning material: how
  operating systems work in general, using TownOS as the running example. Start
  here to learn the ideas.

They must not blur: `docs/` states facts about this codebase, `learnings/` teaches
concepts.

## Repository layout

```
boot/       Multiboot header and the 32 to 64 long-mode climb (assembly)
kernel/     GDT/TSS, IDT, interrupt and syscall dispatch, timer, frame allocator,
            heap, per-process paging, ELF loader, scheduler, kernel_main
drivers/    hardware drivers: screen, keyboard, ATA PIO disk, I/O ports
fs/         read-only FAT32 filesystem
libc/       minimal freestanding C library (string, mem)
user/       ring-3 programs (shell, A/B/C) and their runtime, built as standalone
            ELF64 binaries that live on the disk image, not in townos.bin
include/    shared types, the interrupt vector map, the syscall numbers
tools/      mkdisk.sh: builds the FAT32 disk image on the host
docs/       project documentation (factual)
learnings/  OS concepts and teaching material
linker.ld   section layout: kernel loads at 1M
Makefile    build system
```

See [docs/architecture.md](docs/architecture.md) for the file-by-file
responsibilities and the subsystem map.

## License

See [LICENSE](LICENSE).
