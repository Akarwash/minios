# Chapter 24: User Memory and a Real Library

> Read chapter 13 (the heap) and chapter 14 (per-process paging) first. This
> chapter ports chapter 13's allocator a second time, and the reason the port is
> interesting is entirely chapter 14's fault.

## Where we are

A ring-3 program's address space is two regions:

```
PD[2]   0x400000-0x5FFFFF   code, data, bss   (loaded from the ELF)
PD[3]   0x600000-0x7FFFFF   stack
```

`PD[4]` through `PD[511]` are unmapped and unused.

So a program has exactly the memory it declared at compile time, plus a stack. That
is why every fixture in `user/tests/` uses fixed static buffers, why `F.c` writes
exactly 16384 bytes and not a number it worked out at runtime, and why a program
that wanted to hold a list of unknown length simply could not be written.

## Rule 1: the allocator is not the problem

Worth separating immediately, because the instinct is to think "we need to write a
malloc" and you already have one.

Chapter 13's allocator does all the real work: headers, footers, splitting,
coalescing, free lists, boundary tags. That code exists, it has been debugged
against a test suite, and it has been running as the kernel heap for a dozen
rungs. You could compile it into a ring-3 program tomorrow.

What it cannot do is **create the memory it hands out**. In the kernel it called
`alloc_frames_contiguous` for a 64KB slab. A ring-3 program cannot: it has no
privilege, it cannot touch the frame allocator, and it cannot map a page (chapter
10).

So the allocator is solved and the question is narrower than it looks: **how does a
program ask for more address space, and how does the kernel give it?**

## Rule 2: two historical answers

**`brk`.** One boundary. The program says "move my heap's end to X" and the kernel
maps pages from the old end to the new one. Kernel state is a single number per
process.

**`mmap`.** "Give me N pages, wherever, and tell me where." Independent regions,
each released independently. Kernel state is a list of regions per process.

`brk` is the older answer and the smaller one, perhaps thirty lines against ninety.
Its limitation is structural: a `brk` heap is one contiguous run that only moves at
one end, so freeing a large block in the middle leaves it mapped forever. There is
no way to punch a hole.

TownOS uses anonymous `mmap`, and the reasoning is worth recording honestly.
Returning memory from the middle is a real capability that `brk` cannot ever have,
guard pages and shared memory stay possible later without a redesign, and the p5
allocator was originally written against `mmap` in userspace, so the port is closer
to its original shape than a `brk` shim would be.

"Anonymous" means memory and nothing else. No file mapping, no `MAP_SHARED`, no
protection flags. That is the subset `malloc` actually uses, and the rest of
`mmap`'s surface is a different subject wearing the same name.

## Rule 3: the textbook collision does not happen here

Every diagram of a Unix process shows the heap growing up from low addresses and
the stack growing down from high, meeting somewhere in the middle. That collision
is the thing `brk` implementations spend their care on.

Your layout does not have it. The stack is at `PD[3]`, a fixed 2MB, fully mapped
when the task is created, and it never grows. So the heap simply takes the next
slot:

```
PD[2]   0x400000   code
PD[3]   0x600000   stack     fixed, never grows
PD[4]   0x800000   heap      grows up, 2MB ceiling
```

Nothing to collide with. The bound is the top of the slot and the check is one
comparison.

That simplicity is borrowed, though. It exists because your stacks cannot grow,
which is itself a limitation. A kernel with growable stacks gets the collision
back, and has to decide what happens when the two regions approach each other.

## Rule 4: eager or lazy, and why lazy is a different chapter

**Eager.** `mmap` maps every page before returning. Ask for 1MB and 256 frames are
allocated and mapped, whether or not you touch them.

**Lazy.** Record that the region exists, map nothing. When the program touches a
page, it faults, and the page fault handler notices the address falls inside a
declared region and maps a frame right there.

Lazy is better, and not by a small margin. A program that asks for 1MB and touches
one page gets one frame. It is also **demand paging**, which chapter 8 listed
explicitly as something TownOS does not do, and it is the same machinery that would
give you growable stacks.

It is not in this chapter for one specific reason: it requires the page fault
handler to start doing *work* rather than reporting. Right now a page fault means
something is wrong, and that is a valuable property. The moment the handler can
respond to a fault by mapping a frame, every genuine bug that would have crashed
loudly has a chance of being silently absorbed instead.

That is worth doing carefully, in a rung where it is the subject, rather than
arriving as a side effect of wanting `malloc`.

## Rule 5: the kernel has to remember what it gave out

`brk` needs one number. `mmap` needs a list, because the whole point is that
regions are independent and can be released in any order.

```c
#define MAX_REGIONS 8

typedef struct {
    uint64_t base;
    uint64_t length;      // 0 means unused
} region_t;

// on task_t
region_t regions[MAX_REGIONS];
```

A fixed array rather than a linked list, and the reason is a small joke worth
noticing: a linked list of memory regions would need somewhere to allocate its
nodes from, and you are in the middle of building the thing that allocates. A fixed
array cannot fragment its own bookkeeping.

Eight regions is generous for a `malloc` that grows its slab a few times.

### Two things that list is for

**Validating `munmap`.** Without the list, `munmap` would have to trust the address
a ring-3 program hands it, and a program could unmap its own code, or its stack, or
ask the kernel to free frames it never owned. With the list, the answer is a lookup:
if no region starts exactly there with exactly that length, refuse and change
nothing.

**Cleaning up a dead task.** `paging_destroy_address_space` frees the user slots by
index, because chapter 14 established that a generic "free everything present" walk
would return the shared kernel mapping to the frame pool and kill the machine. That
index list is now three entries instead of two, and it is the single place that has
to change if a fourth region is ever added.

## Rule 6: the port, and the assumption it carries

Porting an allocator is the second-cheapest way to get one, and the cheapest is
using one somebody else already ported. You have done this before: chapter 13 took
the CMSC216 p5 allocator, changed where it got memory from, and changed nothing
else.

This is the same operation again, from the kernel into `libc/`, with `SYS_MMAP`
where `alloc_frames_contiguous` used to be.

But there is an assumption riding along, and it is exactly the kind that survives a
port unnoticed.

`kernel/heap.c` assumes **its blocks live in one contiguous span of addresses**,
because "the block above" is computed by adding a size to a pointer. That was
guaranteed by `alloc_frames_contiguous`, which is contiguous by name.

`mmap` guarantees no such thing. Two slabs obtained by two calls can sit anywhere,
with unmapped space between them. So a walk that runs off the end of slab one lands
in nothing, and the failure is a page fault in the allocator, which is the worst
place in a program for a page fault to be.

Two honest ways out: keep one slab and fail when it is exhausted, or make the walk
slab-aware so it knows where each span ends. The first is smaller and is fine while
programs are small.

The general lesson is the one worth keeping: **a ported component brings its
environment's guarantees with it as invisible assumptions.** The code does not
change, so nothing draws your attention to them. They surface as faults in the
component you trusted most.

## Rule 7: what `printf` is actually for

The user-facing half of this rung looks like convenience and mostly is. But look at
what its absence has already cost.

`user/shell.c` grew a hand-rolled `print_uint` with ten call sites.
`user/tests/count.c` grew its own. Both exist because there was no way to print a
number, and both are the same twelve lines written twice, so a bug in one would not
be a bug in the other and neither would be found.

That is what a missing library function looks like from the inside: not an absence,
but the same code appearing repeatedly in slightly different forms.

So the real deliverable of a `printf` stage is the **deletion**. If `printf` lands
and both `print_uint`s survive, nothing has been gained and something has been
added.

### And one thing that cannot be fixed

`printf` is variadic, so nothing at runtime can check that the arguments match the
format. `printf("%s %s", one_thing)` reads a second argument that was never passed,
and that is a fault or garbage depending on what was in the register.

There is no runtime fix and pretending otherwise is worse than admitting it. What
you can do is bound the damage: validate a `%s` pointer before dereferencing it,
and stop the whole format at the first unrecognised specifier rather than silently
skipping it. The real defence is the compiler:

```c
int printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
```

Which checks every call site at build time, for free. Worth knowing that this is
one of the rare cases where the correct answer to a class of runtime bug is a
compiler attribute.

## What this still is not

- **No gap reuse.** A `munmap`'d region's address range is not handed out again, so
  a program that maps and unmaps repeatedly will exhaust the 2MB slot even though
  almost none of it is in use. A real region list coalesces and reuses. This one
  does not.
- **No partial `munmap`.** You release exactly the region you were given, or
  nothing.
- **No `realloc`.**
- **Slabs are never returned.** `free` puts a block back on the allocator's free
  list; nothing ever hands a slab back to the kernel. Peak usage is held until the
  program exits.
- **No lazy mapping**, so asking for memory costs frames whether or not you touch
  it.
- **No growable stack.** Still a fixed 2MB, and a program that recurses too deeply
  runs off the end of it into unmapped space.
- **`printf` has no width or precision**, no floats, and no `%p`.

## Exercises

1. The allocator was already written and debugged. Explain precisely what a ring-3
   program cannot do that made this a kernel rung anyway.
2. `brk` cannot return memory from the middle of the heap. Draw the allocation
   pattern that makes this expensive, and say why it does not matter for a program
   that runs for a second and exits.
3. The heap goes at `PD[4]` and cannot collide with the stack. Name the limitation
   that buys that simplicity, and describe what would have to change if it were
   lifted.
4. Lazy mapping would be strictly more efficient. Give the argument against adding
   it in this rung, in terms of what a page fault currently means.
5. Region tracking uses a fixed array rather than a linked list. Give the reason
   that is about this specific rung rather than about performance.
6. `munmap` takes an address from ring 3. Describe what a malicious program would
   pass, and what the region list does about it.
7. The ported allocator assumes one contiguous span. Trace exactly what happens on
   the first allocation that walks off the end of slab one, and say why the symptom
   would be so misleading.
8. Two programs each hand-rolled a number printer. Beyond duplication, name the
   specific risk that creates, and say how you would find such pairs elsewhere in a
   codebase.
9. `printf("%s %s", one_thing)` cannot be caught at runtime. Explain why, and name
   the thing that does catch it.
