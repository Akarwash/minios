# Building, running, and debugging

This is the operational guide: what you need, how to build TownOS, how to run it,
and how to debug it. It reflects what actually works on the development machine.

## Dependencies

TownOS needs a cross toolchain that targets bare-metal x86-64 (not the host OS),
an assembler, and an emulator. The versions below are the ones actually in use on
the development machine (macOS on Apple Silicon, Homebrew):

| Tool | Version | Purpose |
|------|---------|---------|
| `x86_64-elf-gcc` | 16.1.0 | Freestanding C cross-compiler |
| `x86_64-elf-binutils` (`x86_64-elf-ld`) | 2.46.1 | Cross linker |
| `nasm` | 3.01 | Assembler (`-f elf64`) |
| `qemu-system-x86_64` | 11.0.0 | Emulator |
| `mtools` (`mformat`, `mcopy`, `mdir`) | 4.0.49 | Format the FAT32 disk image and copy files into it |

Why a cross-compiler? The host compiler produces binaries for the host OS on the
host CPU. A kernel needs code for a bare x86-64 machine with no OS underneath.
`x86_64-elf-gcc` produces exactly that, and the Makefile's freestanding flags
(`-ffreestanding -nostdlib -nodefaultlibs -fno-pie -mno-red-zone -mcmodel=kernel`)
keep it from pulling in host libc or startup code.

### Install (macOS, Homebrew)

```bash
brew install x86_64-elf-gcc x86_64-elf-binutils nasm qemu mtools
```

### Install (Linux)

There is no distro package for an `x86_64-elf` cross compiler. On an x86-64 Linux
host the native `gcc`/`ld` can build the kernel because the host architecture
already matches the target and the freestanding flags prevent host libc/startup
code from being linked in. Install the native toolchain plus the assembler and
emulator:

```bash
sudo apt install build-essential nasm qemu-system-x86 mtools
```

Alternatively, build a dedicated `x86_64-elf` cross GCC from source per the OSDev
"GCC Cross-Compiler" guide (targeting `--target=x86_64-elf`). This takes 30-plus
minutes and is only needed if the native toolchain does not work for you. The
Makefile currently hard-codes `x86_64-elf-gcc`/`x86_64-elf-ld`; on Linux with the
native toolchain, override them (`make CC=gcc LD=ld`).

## Build

```bash
make
```

This assembles every `.asm` file with `nasm -f elf64`, compiles every `.c` file
with the cross-compiler, and produces two artifacts:

- `townos.elf` — the linked ELF64 image with 64-bit symbols, used for gdb.
- `townos.bin` — the same image repackaged with `x86_64-elf-objcopy -O
  elf32-i386`, which is what QEMU's Multiboot `-kernel` loader accepts.

QEMU's built-in Multiboot loader rejects an ELF64 image ("Cannot load x86-64
image, give a 32bit one"). The entry code in `boot/boot.asm` starts in 32-bit
protected mode and climbs to long mode itself, and every address in the image
lives in low memory, so relabelling the ELF container as `elf32-i386` is accepted
by the loader and boots correctly. The code is unchanged; only the ELF header
class differs.

It also builds the user programs, which are separate binaries and not part of
`townos.bin`: `user/SHELL.ELF`, the interactive shell, plus the kernel test
fixtures `user/tests/A.ELF` through `K.ELF` and `COUNT.ELF`, `UPPER.ELF`,
`ONCE.ELF`, each linked against the ring-3 half of `libc/` (the `.user.o`
objects) and the signal trampoline. The two directories build with the
same recipe and land on the same disk; `user/` holds the program the machine is
*for* and `user/tests/` holds programs that exist only to prove a piece of the
kernel works (see `user/tests/README.md`). See
[Building a user program](#building-a-user-program) below.

To rebuild from scratch:

```bash
make clean && make
```

## Run

```bash
make run
```

This first creates and formats a disk image if one does not exist, then runs QEMU
with it attached to the primary ATA bus:

```bash
./tools/mkdisk.sh disk.img 64M               # once, if disk.img is absent
qemu-system-x86_64 -kernel townos.bin \
    -drive file=disk.img,format=raw,if=ide,index=0,media=disk
```

`disk.img` is a 64MB raw (flat, unstructured) file, created once by the `make`
rule and git-ignored. The `-drive` flags matter: `if=ide` puts the drive on the
emulated IDE/ATA controller (not virtio or AHCI), so it answers at the I/O ports
0x1F0-0x1F7 where the disk driver looks; `index=0` makes it the primary bus
master; `format=raw` means the file is a plain byte array with no qcow layering.
See [reference/disk.md](reference/disk.md).

### The disk image

`tools/mkdisk.sh` creates the image, formats it FAT32, and copies in the test
files, all with [mtools](https://www.gnu.org/software/mtools/). mtools edits the
image file directly as a bare FAT volume, so nothing needs `sudo` and nothing
needs to be mounted or attached to a loopback device. The exact commands it runs:

```bash
qemu-img create -f raw disk.img 64M
mformat -i disk.img -F ::                    # -F forces FAT32, not FAT12/FAT16
mcopy -i disk.img HELLO.TXT ::/
mdir -i disk.img ::                          # list what is on the image
```

Why 64MB and not the old 16MB: FAT32 is only legal with at least 65525 clusters.
16MB cannot reach that with a sane cluster size, so formatting tools either
refuse or silently produce FAT16 instead. 64MB clears the bar at one block per
cluster. See [reference/fat32.md](reference/fat32.md).

The image is formatted as a "superfloppy": the FAT32 volume starts at block 0,
with no partition table, which is why the kernel can read block 0 and find a boot
sector there.

The rule is idempotent. It has no prerequisites, so make skips it whenever
`disk.img` exists, and the script bails out as well. Reformatting on every
`make run` would silently destroy whatever the disk had accumulated. To add a
file to an existing image, or to inspect one:

```bash
mcopy -i disk.img somefile.txt ::/           # add (8.3 names only; see below)
mdir -i disk.img ::                          # list
mtype -i disk.img ::/HELLO.TXT               # print a file
```

To start over from a fresh image, delete `disk.img` and run `make run` again.

### What is on the disk, and which half puts it there

The image's contents come from two places, and the split is not cosmetic:

| File | Size | Written by | When |
|------|------|-----------|------|
| `HELLO.TXT` | 17 | `tools/mkdisk.sh` | once, when the image is created |
| `TEST.TXT` | 19 | `tools/mkdisk.sh` | once, when the image is created |
| `BIG.TXT` | 16384 | `tools/mkdisk.sh` | once, when the image is created |
| `HUGE.TXT` | 40981 | `make disk-testfiles` | every `make run` |
| `SHELL.ELF`, `A`–`K.ELF`, `COUNT`/`UPPER`/`ONCE.ELF` | ~22K each, the shell ~48K | `make disk-programs` | every `make run` |

`mkdisk.sh` refuses to touch an image that already exists, so **anything added to
that script reaches a freshly formatted disk and no other** — a developer with a
`disk.img` already in their tree would never see it. That is fine for the three
files the filesystem self-test knows byte for byte, which have been there since the
image format was settled. It is not fine for anything new, so `HUGE.TXT` is
generated by the Makefile as build output and re-copied on every boot, the same
treatment the program binaries get and for the same reason: a stale copy on the
disk looks exactly like a kernel bug.

`HUGE.TXT` is 640 numbered 64-byte lines followed by a marker line, deliberately
larger than the shell's 32KB file buffer. See
[reference/shell.md](reference/shell.md) for what `read HUGE.TXT` actually does,
which is not what the shell's own error message suggests.

The kernel reads 8.3 names only and skips long-filename entries, so a file copied
in as `my-long-name.text` is readable by mtools but invisible to TownOS. Use
names of at most 8 characters plus a 3 character extension.

## Building a user program

User programs are not part of the kernel. Each is compiled and linked on its own
into a static ELF64 binary that lives on the disk image, and the kernel loads it
at runtime. Changing what the machine runs does not need a kernel rebuild.

To add one:

1. Write `user/D.c` (uppercase, matching the 8.3 name it will have on the disk).
   Include `userlib.h` for the syscall wrappers, and give it a `void _start(void)`
   entry point, which is what `user/user.ld` names as the entry:

   ```c
   #include "userlib.h"

   void _start(void) {
       for (;;) {
           sys_write("D");
           user_delay();
       }
   }
   ```

2. Add `user/D.ELF` to `USER_PROGRAMS` in the `Makefile`. The pattern rule builds
   it, and `make run` copies it onto the image.
3. That is enough to run it from the shell: `make run`, then type `run D.ELF` at
   the prompt, and the loader starts it as a new task with no kernel rebuild. To
   have it launched automatically at boot instead, add `"D.ELF"` to the
   `user_programs` list in `kernel_main` (which today holds only `"SHELL.ELF"`);
   that last step is the one that needs a kernel rebuild.

The build flags are deliberate and documented in the `Makefile`. Two matter most:

- **`-mcmodel=small`, not `-mcmodel=kernel`.** The kernel model assumes symbols
  live in the top 2GB of the address space. User code links at 0x400000 and the
  kernel model produces relocation errors on it. This is the single most likely
  build failure when adding a program.
- **`-static -nostdlib -nodefaultlibs -fno-pie -no-pie`.** No host libc, no
  startup files, no relocation. The whole runtime a program gets is
  `user/userlib.h`.

To change an existing program without touching the kernel:

```bash
make user/tests/A.ELF
mcopy -o -i disk.img user/tests/A.ELF ::/
qemu-system-x86_64 -kernel townos.bin -drive file=disk.img,format=raw,if=ide,index=0,media=disk
```

`make run` does the copy step for you, on every run, so a stale program on the
image can never be what boots. See
[reference/elf-loading.md](reference/elf-loading.md).

A window opens showing the banner, the detected RAM, the disk detection line, and
then the shell prompt:

```
Welcome to TownOS!
Detected RAM: 127 MB
Disk: primary ATA master detected
Starting scheduler with 1 ring-3 tasks...
TownOS shell. type 'help'.
>
```

The timer ticks on IRQ 0 and the keyboard delivers keypresses on IRQ 1, which the
shell reads through `SYS_READKEY`. Type `help` for the command list, `list` to see
the files on the disk, `read HELLO.TXT` to print a file, `return hello` to echo
text, or `run A.ELF` to start one of the letter-printers, whose output then
interleaves with the prompt (a live demonstration of the scheduler running two
tasks). The command names are TownOS's own, not the Unix ones; see
[reference/shell.md](reference/shell.md). To quit QEMU, close the window, or press
`Ctrl-A` then `X` in the launching terminal.

## Debug

### Triple faults

A misconfigured boot climb (see
[reference/boot-sequence.md](reference/boot-sequence.md)) causes a triple fault,
where the CPU resets instead of reporting an error. These flags make it visible:

```bash
qemu-system-x86_64 -kernel townos.bin -d int -no-reboot -no-shutdown
```

- `-d int` logs every interrupt and exception the CPU takes.
- `-no-reboot` stops QEMU from rebooting on triple fault, so the log survives.
- `-no-shutdown` keeps the VM around after the fault for inspection.

Likely triple-fault causes: a missing `PS` (huge) bit on a page-directory entry,
a page table that is not 4096-aligned, a table that was not zeroed, or an identity
map that does not cover the address of the code executing when `CR0.PG` is set.

### Source-level debugging with GDB

Start QEMU with a gdb stub, halted before the first instruction:

```bash
qemu-system-x86_64 -kernel townos.bin -s -S
```

- `-s` opens a gdb server on TCP port 1234.
- `-S` freezes the CPU at startup so you can set breakpoints before anything runs.

In another terminal, point gdb at `townos.elf` (the ELF64 image keeps the 64-bit
symbols; `townos.bin` does not):

```bash
gdb townos.elf
(gdb) target remote :1234
(gdb) break kernel_main
(gdb) continue
```

### Inspecting CPU state from the monitor

Route the QEMU monitor to the terminal and query the CPU directly:

```bash
qemu-system-x86_64 -kernel townos.bin -monitor stdio
```

Then at the `(qemu)` prompt, `info registers` dumps the control registers
(CR0/CR3/CR4), segment selectors, and RIP/RSP. This is the quickest way to
confirm long mode is active and paging is on without attaching a debugger.
