# Chapter 8: Long Mode and Paging

> Read chapter 3 (interrupts) first if you have not. This chapter is about what
> happens before any of that, in the first few hundred instructions the kernel
> ever runs.

## Where we are

GRUB hands you control. You have a 64-bit CPU, and it is running in 32-bit mode
with paging switched off.

You did not ask for that. It is what you get. The most capable processor you own
starts up pretending to be a machine from the early 1990s, and before it will do
anything modern you have to walk it up there yourself, in a specific order, with
several steps that fail silently and take the whole machine down if you get them
wrong.

This chapter is about why that is, and about the thing you have to build before the
CPU will agree to be 64-bit: a map.

## Rule 1: the CPU boots as its own ancestor

Every x86 processor made in the last forty years starts in **real mode**, a 16-bit
mode from 1978. Software walks it up to 32-bit protected mode, and then, if it
wants, up again to 64-bit long mode. GRUB does the first climb for you and hands
you a 32-bit machine. The second climb is yours.

There is no design intention here. Nobody sat down and decided this was a good
architecture. It is an accumulation: each generation kept the previous one's
starting state so that the previous generation's software would still boot, and
forty years of that produces a processor that has to be talked into being itself.

The cost is this chapter. The benefit is that software written in 1985 still runs
on hardware from 2026, which is not nothing, and is the reason x86 is still here.

## Rule 2: what an address actually is

This is the concept the rest of the chapter hangs on, so it is worth being slow
about.

With paging off, an address is a **location**. Box 4000 is box 4000. The CPU puts
4000 on the wires and the memory chips hand back whatever is in box 4000. There is
no interpretation, no lookup, no possibility of the answer being different at
different times.

With paging on, an address is a **question**. The CPU takes the address the program
used, walks a table you built, and comes back with an actual physical location. The
program says "box 4000" and the hardware says "let me check what box 4000 means
right now."

That is the whole idea. Every address a program touches is a *virtual* address, and
the map decides what it means.

Once you can ask that question, you can give different answers.

## Rule 3: what different answers buy you

Three things, and they are the reason every serious operating system since the
1960s has paging.

**Isolation.** Two programs can both use address 0x400000 and get completely
different physical memory, because each has its own map. Neither can see the
other's, neither can reach the other's, and neither knows the other exists. That is
chapter 14 and it is the big one.

**Lying about layout.** A program can be compiled to believe it lives at
0x400000 and be loaded anywhere in physical RAM, because the map makes the lie
true. Without this, every program would have to be built knowing where it would end
up, or be relocated at load time.

**Enforcement.** A map entry carries bits as well as an address. Present or not.
Writable or read-only. Reachable from ring 3 or not. So an address can be made to
simply not work, and touching it becomes a **page fault**: a trap the kernel
handles, instead of a silent read of somebody else's memory.

That third one changes the character of every bug in the system. Without paging, a
stray pointer quietly corrupts something and the program dies twenty seconds later
somewhere unrelated. With paging, the stray pointer stops the program at the
instruction that did it.

## Rule 4: the map is a tree, not a list

The obvious way to build the map is a flat table: one entry per page, look up the
page number, get the physical address.

Do the arithmetic. A 64-bit address space with 4KB pages has about 2^52 pages. A
flat table would need four quadrillion entries, for one program, most of them
describing memory that does not exist. The table would be larger than the memory it
describes by an enormous margin.

So the map is a **tree**. On x86-64 it is four levels deep, and TownOS uses the
names the manuals use: PML4 at the top, then PDPT, then PD, then PT at the bottom.
Each level has 512 entries and each entry points at the next level down.

The trick is that **a branch that is not there costs nothing**. If a program uses no
memory in some region, the entry covering that region is simply absent, and the
entire subtree beneath it does not exist. So a program using a few megabytes,
scattered across a 64-bit space, is described in a few kilobytes of tables.

The cost is that a translation is now four memory lookups instead of zero, which
would be ruinous if it happened on every access. It does not: the CPU caches recent
translations in the **TLB** (translation lookaside buffer), and the walk only
happens on a miss. That is why changing the map means telling the CPU to forget its
cache, and why forgetting to do so produces bugs where the machine is using a map
you deleted.

## Rule 5: long mode requires paging

Here is the constraint that decides the shape of the boot code.

You cannot run in 64-bit mode without paging on. It is not optional and there is no
mode that gives you 64-bit flat physical addressing. The map is mandatory.

Which means the order is forced: **build valid page tables first, then enable
paging, and the act of enabling paging is the moment long mode activates.** You
cannot climb first and map afterwards.

## Rule 6: changing the wheels while driving

This is the best part of the chapter and it is the thing to remember if you remember
one thing.

At the instant you set the paging bit in CR0, address translation starts applying to
*everything*. Not just to data the program touches. To the CPU's own instruction
fetches. Including the fetch of the very next instruction after the one that turned
paging on.

So the code that enables paging has to already be mapped, by the tables it just
built, to the exact address it is currently executing at. If it is not, the next
fetch goes somewhere invalid, there is no handler installed to catch it, and the
CPU triple faults on its own instruction stream and resets.

The solution is **identity mapping**: build the tables so that virtual address N
maps to physical address N. Fake address equals real address. Then nothing moves
under your feet at the moment of the switch. The instruction after the one that
enabled translation is fetched through the map, and the map returns exactly where
it already was.

You are changing the wheels while the car is moving, and identity mapping is the
trick that makes the car not notice.

TownOS identity-maps the first 32MB. Sixteen entries of 2MB each, covering the VGA
text buffer at 0xB8000 and the kernel loaded at 1MB with a great deal of room to
spare. Enough to survive the transition, and simple enough to build by hand in
assembly before any C exists.

The first four entries are written out one per line, because their privilege
differs and it is worth being able to read it off:

    PD[0]  0x000000-0x1FFFFF   kernel: kernel code and data, VGA, the kernel stack
    PD[1]  0x200000-0x3FFFFF   kernel
    PD[2]  0x400000-0x5FFFFF   USER: where a ring-3 program's code will live
    PD[3]  0x600000-0x7FFFFF   USER: where its stack will live

The remaining twelve are filled by a loop, because they are uniform: 8MB to 32MB,
kernel only, no user bit. That range is not decoration. It is the memory C works in
before it has parsed the Multiboot map, and it is where the frame allocator's pool
begins.

## Rule 7: the identity map is scaffolding

Worth saying explicitly, because it is easy to mistake the scaffolding for the
building.

Identity mapping is not a design. It is a device for surviving one instant. It is
the answer to "how do I turn on translation without the ground moving", and nothing
more. Everything built on top of it eventually stops being identity-mapped, and by
chapter 14 each program has its own map where addresses mean whatever the kernel
decided they should mean.

Some of the low identity map survives, because the kernel finds it convenient to
have physical memory reachable at its own address. That is convenience, not
architecture.

## The climb, without the register detail

The order in `boot/boot.asm`, and every step is a precondition for a later one:

1. **Zero the tables by hand.** All 12KB of them. The bootloader is not trusted to
   have cleared `.bss`, and one stray byte that happens to set a present bit is a
   bogus mapping and a silent triple fault.
2. **Link the levels.** PML4[0] points at the PDPT, PDPT[0] points at the PD. Only
   entry 0 of each, because the whole identity map lives in the low region entry 0
   covers.
3. **Identity-map 32MB** with sixteen 2MB pages in the PD. Four written explicitly,
   twelve in a loop.
4. **Enable PAE.** Physical Address Extension is required for long mode. Turn on
   paging without it and you get 32-bit paging instead, which is not an error, just
   silently the wrong thing.
5. **Load CR3** with the address of the PML4. Because the tables are identity
   mapped, their virtual address is already their physical address, which is
   convenient and is another thing the identity map is quietly buying.
6. **Arm long mode** by setting a bit in a model-specific register. This only arms
   it. Nothing happens yet.
7. **Enable paging.** This is the moment. Translation starts, long mode activates,
   and step 3 had better have been right.
8. **Far jump into 64-bit code.** Explained below.

## Rule 8: why the far jump, and why it needs a table

After step 7 the CPU is in long mode but is still executing in a *compatibility*
code segment, which is 64-bit mode still behaving as though it were 32-bit. To
reach true 64-bit execution, the code segment register has to be reloaded with a
descriptor marked as 64-bit.

Descriptors live in a table. So the boot code has to install one before it can
finish the climb, and that table is chapter 9.

That is a satisfying place for this chapter to end, because it is exactly the shape
of the whole climb: every step exists to make the next one legal.

## Why 2MB pages, and why 32MB

Two decisions worth understanding rather than accepting.

**2MB pages** instead of 4KB ones mean the tree stops at the third level. A PD
entry with the huge bit set says "this entry is a 2MB page, stop walking, there is
no fourth table below me." That removes an entire level of tables from the boot
code, which matters a great deal when you are writing it in assembly with no
allocator and no error handling.

**32MB** because the boot map has to be built with no knowledge of how much memory
the machine actually has. The Multiboot map that answers that question is parsed
later, in C, and C cannot run until this map exists. So the boot code picks a
figure that is certainly safe and comfortably larger than the kernel needs, and
leaves C room to work in before it extends the map to cover real RAM.

C only ever adds entries at 32MB and up. It never touches these low sixteen, which
map the running kernel. That division is what makes the boot map a fixed, known
quantity rather than something that shifts underneath the code that is standing
on it.

### A fossil worth noticing

The ADR for this is called
`docs/decisions/0002-2mb-pages-and-8mb-identity-map.md`, and the figure in its
filename is not the figure in the code.

That is not a mistake. The original decision genuinely was 8MB, which was the
smallest round number covering the VGA buffer and the kernel. The map was widened
later, and the ADR keeps its name because an ADR records what was decided at a
moment, not what is true now. Its Status field is where the widening belongs.

It is worth knowing this one is there, because it is exactly the kind of stale
number that propagates: it was copied into the reference page, and from there into
an earlier draft of this chapter, and from there into chapter 24, before anybody
checked it against `boot.asm`. When a figure appears in three documents and one
source file, the source file is the one that is right.

## What this still is not

- **No per-process maps.** There is one map and everybody shares it. Isolation is
  chapter 14 and it is the payoff this chapter sets up.
- **No demand paging.** Nothing is mapped lazily. If a page is in the map it is
  backed by real memory right now.
- **No swapping, ever.** The other famous use of the "an address is a question"
  trick is answering "it is on disk, hold on", faulting, fetching it, and resuming.
  TownOS will never do this, and it is worth knowing that the machinery for it is
  the same machinery you already have.

## Exercises

1. A program reads address 0x400000. Describe the physical lookup with paging off,
   and then with paging on, counting memory accesses in each case.
2. Why does the boot code zero the page tables by hand instead of relying on `.bss`
   being clear? Describe the failure if it did not, and say why it would be hard to
   diagnose.
3. Suppose the identity map covered 0 to 4MB and the kernel were loaded at 5MB.
   Name the exact instruction at which the machine dies and say what the symptom
   looks like from outside.
4. A flat page table for a 64-bit space with 4KB pages would have roughly 2^52
   entries. At 8 bytes each, how much memory is that, and how does it compare to
   the memory it is describing?
5. Enabling PAE is step 4 and enabling paging is step 7. What happens if you swap
   them? Note that this is not a crash, which is what makes it interesting.
6. The TLB caches translations. Describe a bug that could only happen because of the
   TLB, and say what instruction fixes it.
7. Identity mapping is described here as scaffolding. Name one thing in the kernel
   today that still depends on the low identity map existing, and say what would
   break if it were removed.
