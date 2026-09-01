# 0019 - Keep keyboard modifier state in the driver and put ASCII in the ring buffer

## Status

Accepted. Extended — the decision stands and now covers a third modifier.

- **Left ctrl is tracked too**, added by [0023](0023-signals.md) on exactly the
  pattern this ADR sets out for shift: a state of the hardware, set on press and
  cleared on release, resolved in the driver and never sent to ring 3. Ctrl-C and
  Ctrl-D are decoded here and produce no character at all. Right ctrl remains
  untracked for the reason this body already gives about extended scancodes.

See [reference/keyboard.md](../reference/keyboard.md) and
[reference/signals.md](../reference/signals.md) for the current state.

## Context

The keyboard driver mapped scancode 0x2A (left shift) and 0x36 (right shift) to 0,
which is the table's "unmapped, ignore this" value, and had no entry for 0x3A (caps
lock) at all. Shift was therefore not a modifier that did nothing; it was a key the
driver did not believe existed. Nothing in MiniOS could produce a capital letter or
any of `!@#$%^&*()_+{}|:"<>?~`. `return HELLO` was not a command that failed, it was
a command that could not be typed.

That had gone unnoticed because the only consumer is the shell, whose commands are
lowercase, whose filenames are matched case-insensitively by the FAT32 layer's 8.3
uppercasing, and whose only free-text command is `return`. The gap surfaces the
moment anything wants text rather than tokens.

The keyboard hardware makes the shape of the problem clear. In scancode set 1 a
byte is a press if bit 7 is clear and a release if bit 7 is set, with the key's own
code in the low seven bits. Shift is reported like any other key, twice: 0x2A on the
way down and 0xAA on the way up. So "shift is held" is not information in any single
byte — it is a state somebody has to keep, assembled from two events that can be an
arbitrary number of other keypresses apart. The only question is who keeps it.

Caps lock is reported the same way but means something different: 0x3A on press,
0xBA on release, and the *lock* is a fiction maintained entirely in software. The
hardware has no caps state to report. Whoever keeps shift state also has to invent
caps state, and has to know that shift applies to every key while caps lock applies
to letters only.

Underneath that sits a wider decision that had never been written down, because
until now nothing forced it: the ring buffer between the IRQ and `SYS_READKEY`
carries `char`, and `SYS_READKEY` returns a character. A ring-3 program has never
seen a scancode and has no way to ask for one.

## Decision

**Modifier state lives in the driver, and the ring buffer keeps carrying resolved
ASCII.** `drivers/keyboard.c` gains a second table, `scancode_to_ascii_shift[128]`,
laid out identically to the existing one; two flags, `shift_held` and `caps_on`; and
a `table_for()` helper that picks between the tables. What reaches ring 3 is still
one finished character: a program reads `'A'` and never learns that a shift key was
involved.

`keyboard_callback` becomes a sequence of early returns. A release masks bit 7 off
*first* and clears `shift_held` if what was released was a shift, then returns
regardless. A press of shift sets `shift_held` and returns. A press of caps lock
toggles `caps_on` and returns. Only what survives all of that is decoded and pushed.

The masking is load-bearing: a release arrives as 0xAA, so a check against the raw
byte matches nothing and produces a shift that turns on and never off. So is the
order — the modifier check has to happen before the release branch returns, not
after it.

**Shift and caps lock combine by XOR, for letters only.** A letter takes the shifted
table when `shift_held != caps_on`; everything else takes it when `shift_held`,
ignoring `caps_on` entirely. That is what a real keyboard does — shift means "the
other case", not "upper case" — and the alternative, OR, would make shift+`a` with
caps lock on produce `A`, and would make caps lock shift the number row.

**The rejected alternative: push raw scancodes and decode in ring 3.** A more
serious kernel does this. The driver would push the byte it read, and a user-space
library would keep the modifier state and own both tables. It is the better design
in the long run and it is the one this will eventually need, for three reasons that
are all invisible today:

- **Keys that are not characters.** Arrow keys, function keys, home and end have no
  ASCII to resolve to, so a `char` buffer cannot represent them at all. A line
  editor with history needs up and down, and there is no character to push.
- **Press and release as separate events.** ASCII collapses them. Anything that
  needs to know a key is *still down* — key repeat, a game, a chord — needs both
  edges, and the driver currently discards one of them.
- **Per-program key handling.** Every program gets one interpretation of the
  keyboard, decided in the kernel. A program that wanted a different layout, or
  wanted ctrl+C to mean something, cannot have it.

It is rejected **now** because it moves work into ring 3 without any ring-3 code
that wants the work. There is one consumer, the shell, and it wants characters; the
change would hand it a decoder to write, a state machine to keep, and two tables to
carry, in exchange for capabilities nothing has asked for. It would also break
`SYS_READKEY`'s contract and everything built on it, including the `WAIT_KEY`
blocking path, which is one rung old.

The cost of deferring is bounded and known: the tables and the state machine move
from `drivers/keyboard.c` to a user-space library largely unchanged, and
`SYS_READKEY` changes what it returns. Nothing about doing it in the driver first
makes doing it properly later harder — which is the whole reason it is safe to
defer.

**Extended scancodes are documented and not handled.** Arrow keys and the
right-hand modifiers send 0xE0 and then the real code. 0xE0 has bit 7 set, so the
release branch swallows it and the byte after it is decoded as an ordinary key.
This is harmless today by luck rather than by design: the arrows' codes fall past
the populated part of the table and decode to 0, and the two that do hit entries
(keypad enter, keypad slash) happen to produce the right character. A comment says
so at the point where the handling would go. `TODO(extended-scancodes)`.

## Consequences

- **The keyboard can now produce every printable ASCII character on a US layout.**
  `return HELLO WORLD` works, and so does `return !@#$%^&*()_+{}|:"<>?~`.

- **A modifier press pushes nothing and wakes nobody.** The early returns are what
  guarantee this, and it matters more than it looks: `scheduler_wake(WAIT_KEY)`
  readies every task blocked on a key, and a shift press that reached it would have
  them all re-issue `SYS_READKEY`, find an empty buffer, and block again. Holding
  shift down at an idle prompt would turn the machine's one wake per keypress into a
  stream of them, undoing the property [0017](0017-blocking-and-sleep.md) was built
  to get.

- **Caps lock state survives everything, including a program.** It is a driver
  global with nothing that clears it, so it persists across task switches, across a
  program starting and exiting, and across the shell being the only thing running.
  There is no LED feedback — setting the keyboard's LEDs needs a command written
  back to the controller at 0x60 with its own ACK handshake — so the only way to
  discover the state is to type a letter and look.

- **Two tables can now disagree.** They are 128 entries each, hand-written, and
  nothing checks that the second is a shifted form of the first. Keeping them
  adjacent and identically formatted, one row of eight per line with the same index
  comments, is the entire defence: a wrong entry shows up as a column that does not
  line up. A generated table, or a table of pairs, would remove the class of bug
  entirely and is worth doing if a third table (extended scancodes) ever appears.

- **Ctrl and alt are still nothing.** 0x1D and 0x38 map to 0 in both tables and no
  state is kept for them, so ctrl+C is indistinguishable from C. Since there are no
  signals and no way to kill a task ([0018](0018-process-lifecycle-exit-and-wait.md)),
  there is nothing for it to mean yet; the modifier machinery this adds is the place
  it would go.

- **The shell's command matching is still case-sensitive, and `RUN` is rejected.**
  `str_eq(cmd, "run")` compares bytes. Now that uppercase is typeable this is
  reachable for the first time, and it is left alone deliberately: `run A.ELF` and
  `run a.elf` both work, because the FAT32 layer uppercases names, so the only thing
  that breaks is shouting the command itself.

## Related

- The ring buffer, the wake, and the blocking path this must not disturb:
  [0017](0017-blocking-and-sleep.md),
  [../reference/blocking.md](../reference/blocking.md).
- The syscall whose contract the rejected alternative would change:
  [0007](0007-syscalls-via-int-0x50.md),
  [../reference/syscalls.md](../reference/syscalls.md).
- The one consumer: [0016](0016-interactive-shell.md),
  [../reference/shell.md](../reference/shell.md).
- Reference page: [../reference/keyboard.md](../reference/keyboard.md).
