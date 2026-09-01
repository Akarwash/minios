# Keyboard reference

The PS/2 keyboard driver turns scancodes from IRQ1 into ASCII characters and hands
them to ring 3 one at a time. This page documents the scancode encoding, the two
translation tables, how shift and caps lock are tracked, the ring buffer between
the IRQ and the syscall, the `WAIT_KEY` wake, and the extended-scancode gap. Read
from `drivers/keyboard.c`, `drivers/keyboard.h`, and the `SYS_READKEY` handler in
`kernel/syscall.c`. For why modifier state is kept in the driver rather than
decoded in ring 3, see
[decision 0019](../decisions/0019-keyboard-modifier-state-in-the-driver.md).

## The hardware side

The 8042-compatible controller raises IRQ1 when a key event is available and the
byte is read from port 0x60 (`KEYBOARD_DATA_PORT`). The driver reads exactly one
byte per interrupt and never writes to the controller, so there is no command
handshake, no LED control, and no scancode-set negotiation: the set the controller
comes up in, set 1, is the set the tables are written for.

`keyboard_init` registers `keyboard_callback` on `IRQ_KEYBOARD` through the shared
`register_interrupt_handler` table. The gate is an interrupt gate, so the CPU
clears IF on entry and the handler runs with interrupts held off — which is the
reason it does as little as it does.

## Scancode set 1: press, release, and bit 7

One byte carries both which key and which edge:

| | |
|---|---|
| bit 7 clear | a **press**, the key's code in bits 0-6 |
| bit 7 set | a **release** of the key whose code is in bits 0-6 |

So left shift is `0x2A` down and `0xAA` up. Two constants name the halves:

```c
#define KEY_RELEASE_MASK   0x80   // bit 7: this byte is a release
#define KEY_CODE_MASK      0x7F   // bits 0-6: which key
```

**A release must be masked before it is compared.** `0xAA` is not `0x2A`, so a
check for the shift keys against the raw byte matches nothing, and shift turns on
at the press and stays on forever. The masking and the comparison happen inside the
release branch, before it returns, for the same reason.

Three keys are recognised by code rather than translated:

```c
#define KEY_LSHIFT         0x2A
#define KEY_RSHIFT         0x36
#define KEY_CAPSLOCK       0x3A
```

## The two translation tables

`scancode_to_ascii[128]` and `scancode_to_ascii_shift[128]` map a **press** code to
a character, with `0` meaning "unmapped, ignore". They are laid out identically —
one row of eight entries per line, with the same `// 0x00 - 0x07` index comments —
and kept adjacent in the file. That is the only thing checking them against each
other: a wrong entry shows up as a column that does not line up.

The shifted table differs only where shift changes the character:

| Plain | Shifted |
|-------|---------|
| `1234567890` | `!@#$%^&*()` |
| `-` `=` | `_` `+` |
| `a`-`z` | `A`-`Z` |
| `[` `]` | `{` `}` |
| `;` `'` | `:` `"` |
| `` ` `` `\` | `~` `\|` |
| `,` `.` `/` | `<` `>` `?` |

Backspace (0x0E), tab (0x0F), enter (0x1C), space (0x39) and keypad `*` (0x37) are
the same in both, because shift does not change what they mean. Unmapped entries
are `0` in both: a key that produces nothing plain produces nothing shifted.

Nothing above 0x39 is populated, so the function keys, the keypad and everything
else decode to `0` and are dropped.

## Modifier state

```c
static int shift_held = 0;   // a physical key is down right now
static int caps_on    = 0;   // a flag the driver toggles and nothing clears
```

The two differ in kind, not just in name. `shift_held` mirrors hardware: set on
press, cleared on release. `caps_on` is invented by the driver — the hardware has
no caps state and reports caps lock as an ordinary key — so it is toggled on press
and nothing ever clears it.

`caps_on` therefore survives task switches, a program starting and exiting, and
everything short of a reboot. There is no LED feedback, because lighting the
keyboard's LEDs means writing a command back to the controller and handling its
ACK, which the driver does not do. The only way to discover the state is to type a
letter and look.

### Choosing a table

```c
static const char *table_for(uint8_t scancode) {
    char plain = scancode_to_ascii[scancode];
    int is_letter = (plain >= 'a' && plain <= 'z');
    int use_shifted = is_letter ? (shift_held != caps_on) : shift_held;
    return use_shifted ? scancode_to_ascii_shift : scancode_to_ascii;
}
```

**Shift applies to every key; caps lock applies to letters and nothing else.** For
a letter the two combine by XOR, so with caps lock on, holding shift gives a
*lowercase* letter:

| caps | shift | `a` key gives |
|------|-------|---------------|
| off | off | `a` |
| off | on | `A` |
| on | off | `A` |
| on | on | `a` |

That is what a real keyboard does — shift means "the other case", not "upper case".
`shift_held \|\| caps_on` would give `A` in the last row and would also make caps
lock shift the number row into `!@#$`.

"Is this a letter" is asked of the **unshifted** table, where letters are spelled
`a`..`z`.

## The callback, in order

```c
static void keyboard_callback(registers_t *regs) {
    read the byte from 0x60

    if bit 7 set:                 // release
        mask it off
        if lshift or rshift: shift_held = 0
        return                    // every other release is ignored

    if lshift or rshift:  shift_held = 1;      return
    if capslock:          caps_on = !caps_on;  return

    c = table_for(scancode)[scancode]
    if c != 0:
        kbd_buffer_push(c)
        scheduler_wake(WAIT_KEY)
}
```

**A modifier press pushes nothing and wakes nobody.** The early returns are what
guarantee that, and it is load-bearing rather than tidy. `scheduler_wake(WAIT_KEY)`
readies *every* task blocked on a key; a shift press that reached it would have
them all re-issue `SYS_READKEY`, find an empty buffer, and block again. Holding
shift at an idle prompt would turn one wake per keypress into a stream of them,
undoing what [blocking](blocking.md) exists to achieve. Measured: holding shift
down for six seconds at an idle prompt moves the `int 0x50` count by zero.

## The ring buffer

A fixed circular queue of `KBD_BUFFER_SIZE` (128) `char`s sits between the IRQ (the
producer) and `SYS_READKEY` (the consumer, in `kernel/syscall.c`).

- `kbd_write_index` is the next slot to fill, `kbd_read_index` the next to drain.
- `write == read` means **empty**. One slot is always left unused so that full and
  empty stay distinguishable by that single rule, which caps the queue at 127
  characters.
- **Full drops the new character** rather than overwriting the oldest. Overwriting
  would destroy input the consumer has already been promised, and would collapse
  the empty/full test. A dropped keypress can be retyped.
- No lock. Single CPU, and both the IRQ gate and the syscall gate clear IF on
  entry, so producer and consumer never run at the same instant.

`keyboard_getchar()` returns `0` for empty. That is safe as a sentinel because
every unmapped key is `0` in the tables and the callback pushes only non-zero
characters, so a real `0` never enters the ring.

## What ring 3 sees

`SYS_READKEY` returns one finished character. A program reads `'A'` and cannot tell
whether shift or caps lock produced it, cannot see key releases, and cannot see any
key that has no ASCII. Everything about the keyboard other than the resulting
character stays in the driver. See
[decision 0019](../decisions/0019-keyboard-modifier-state-in-the-driver.md) for the
alternative — pushing raw scancodes and decoding in ring 3 — and why it is
deferred rather than rejected outright.

When the buffer is empty, `SYS_READKEY` blocks the caller with `WAIT_KEY` instead
of returning `0`, and this IRQ is the thing that wakes it. Push happens before the
wake: a task woken before the character was in the buffer could be scheduled,
re-issue its read, find nothing, and block again. See [blocking.md](blocking.md).

## Extended scancodes: a known gap

The arrow keys, right ctrl, right alt, keypad enter and keypad slash send **two**
bytes: `0xE0` and then the real code.

`0xE0` has bit 7 set, so the release branch swallows it — masked to `0x60`, which
is not a shift, so no state changes — and the byte that follows is then decoded as
if it were an ordinary key. Today that is harmless **by luck, not by design**:

| Key | Second byte | Decodes to | Why it does not hurt |
|-----|-------------|-----------|----------------------|
| up / left / right / down | 0x48 / 0x4B / 0x4D / 0x50 | `0` | past the populated part of the table |
| right ctrl | 0x1D | `0` | unmapped in both tables |
| right alt | 0x38 | `0` | unmapped in both tables |
| keypad enter | 0x1C | `\n` | coincidentally the right character |
| keypad slash | 0x35 | `/` | coincidentally the right character |

Handling it properly means a "saw 0xE0" flag and a way to represent keys that have
no ASCII, which a `char` ring buffer cannot do. `TODO(extended-scancodes)`, marked
in `drivers/keyboard.c` at the point where the handling would go.

## Ctrl and alt

0x1D and 0x38 map to `0` in both tables and no state is kept for them, so ctrl+C is
indistinguishable from C. Ctrl-C now raises `SIG_INT` on the foreground group (see
below); what remains unmapped is every OTHER control character, since a ctrl
combination other than C or D produces nothing at all
([decision 0018](../decisions/0018-process-lifecycle-exit-and-wait.md)), so there
is nothing for a ctrl chord to mean yet.

## Related

- The block and wake this must not disturb: [blocking.md](blocking.md),
  [decision 0017](../decisions/0017-blocking-and-sleep.md).
- The one consumer: [shell.md](shell.md),
  [decision 0016](../decisions/0016-interactive-shell.md).
- The gate the character leaves through: [syscalls.md](syscalls.md).
- The IRQ path that gets here: [idt.md](idt.md).
- The decision: [0019](../decisions/0019-keyboard-modifier-state-in-the-driver.md).

## Ctrl, Ctrl-C and Ctrl-D

Left ctrl (scancode `0x1D`) is tracked exactly as shift is — set on press, cleared on
release — because both are states of the hardware rather than of the driver. **Right
ctrl is not tracked**, and cannot be until extended scancodes are: it arrives as the
two bytes `0xE0 0x1D`, the `0xE0` has bit 7 set so the release branch swallows it,
and the `0x1D` that follows is decoded as if it were the left one. That happens to
work, by accident. See the `TODO(extended-scancodes)` note in the driver.

With ctrl held, two keys mean something:

- **`c`** raises `SIG_INT` on the foreground process group.
- **`d`** sets a `console_eof` flag rather than pushing a character.

Any other key with ctrl held produces nothing at all. Falling through to the
character path would make Ctrl-A type a plain `a`, putting a character in the buffer
the user cannot see they typed.

**Neither pushes a character, and both return before the character path's
`scheduler_wake(WAIT_KEY)`** — exactly as the modifier presses do, and for the same
reason. A wake with an empty ring hands every task blocked on `WAIT_KEY` a wasted
round trip: woken, re-issue the read, find nothing, block again. Holding ctrl and
tapping a key would turn that into a scheduler spin. (Ctrl-D's wake is *not* that
case: there is something for a console reader to find, just not a character.)

`console_eof` is the entire line discipline this kernel has. It is
**test-and-cleared** by `keyboard_console_eof()`, so one Ctrl-D ends exactly one
read; leaving it set would make the console report end-of-input forever. It is **one
flag for the whole machine**, not one per descriptor, because there is one keyboard —
with a single foreground group the task that consumes it is the one the user meant.

See [signals.md](signals.md) and [decision 0023](../decisions/0023-signals.md).
