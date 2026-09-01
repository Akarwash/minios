#include "keyboard.h"
#include "ports.h"
#include "../kernel/isr.h"
#include "../kernel/scheduler.h"
#include "../kernel/signal.h"
#include "../include/vectors.h"

#define KEYBOARD_DATA_PORT 0x60

// Scancode set 1 packs press and release into one byte: bit 7 clear is a press,
// bit 7 set is the release of the key whose code is in the low seven bits.
#define KEY_RELEASE_MASK   0x80
#define KEY_CODE_MASK      0x7F

#define KEY_LSHIFT         0x2A
#define KEY_RSHIFT         0x36
#define KEY_CAPSLOCK       0x3A
#define KEY_LCTRL          0x1D

// The two keys that mean something when ctrl is held. Named as SCANCODES rather
// than characters because that is what this function has: the ctrl combinations are
// decided before either ASCII table is consulted, so there is no 'c' to compare
// against at that point.
#define KEY_C              0x2E
#define KEY_D              0x20

// US QWERTY scan code (set 1) to ASCII. Index = scan code, 0 = unmapped/ignored.
// Key releases (bit 7 set) are handled before either table is consulted.
//
// THE TWO TABLES ARE KEPT ADJACENT AND IDENTICALLY LAID OUT, one row of eight per
// line with the same index comments, so that a wrong entry shows up as a column
// that does not line up rather than as a character nobody can type.
static const char scancode_to_ascii[128] = {
    0,    0,   '1',  '2',  '3',  '4',  '5',  '6',   // 0x00 - 0x07
    '7',  '8', '9',  '0',  '-',  '=',  '\b', '\t',  // 0x08 - 0x0F
    'q',  'w', 'e',  'r',  't',  'y',  'u',  'i',   // 0x10 - 0x17
    'o',  'p', '[',  ']',  '\n', 0,    'a',  's',   // 0x18 - 0x1F  (0x1C enter, 0x1D ctrl)
    'd',  'f', 'g',  'h',  'j',  'k',  'l',  ';',   // 0x20 - 0x27
    '\'', '`', 0,    '\\', 'z',  'x',  'c',  'v',   // 0x28 - 0x2F  (0x2A lshift)
    'b',  'n', 'm',  ',',  '.',  '/',  0,    '*',   // 0x30 - 0x37  (0x36 rshift)
    0,    ' ', 0,    0,    0,    0,    0,    0,      // 0x38 - 0x3F  (0x38 alt, 0x39 space)
    /* remaining entries default to 0 */
};

// The same keys with shift held. Same indices, same unmapped entries: a key that
// produces nothing plain produces nothing shifted either, and the keys that are not
// characters at all (backspace, tab, enter, space, keypad *) are unchanged, because
// shift does not change what they mean.
static const char scancode_to_ascii_shift[128] = {
    0,    0,   '!',  '@',  '#',  '$',  '%',  '^',   // 0x00 - 0x07
    '&',  '*', '(',  ')',  '_',  '+',  '\b', '\t',  // 0x08 - 0x0F
    'Q',  'W', 'E',  'R',  'T',  'Y',  'U',  'I',   // 0x10 - 0x17
    'O',  'P', '{',  '}',  '\n', 0,    'A',  'S',   // 0x18 - 0x1F  (0x1C enter, 0x1D ctrl)
    'D',  'F', 'G',  'H',  'J',  'K',  'L',  ':',   // 0x20 - 0x27
    '"',  '~', 0,    '|',  'Z',  'X',  'C',  'V',   // 0x28 - 0x2F  (0x2A lshift)
    'B',  'N', 'M',  '<',  '>',  '?',  0,    '*',   // 0x30 - 0x37  (0x36 rshift)
    0,    ' ', 0,    0,    0,    0,    0,    0,      // 0x38 - 0x3F  (0x38 alt, 0x39 space)
    /* remaining entries default to 0 */
};

// ---------------------------------------------------------------------------
// Modifier state.
// ---------------------------------------------------------------------------
// Held here in the driver, not sent to ring 3. The ring buffer carries resolved
// ASCII, so a program reads 'A' and never learns that a shift key was involved.
// See docs/decisions/0019-keyboard-modifier-state-in-the-driver.md for why, and
// for what that costs (arrow keys, key repeat, per-program key handling).
//
// The difference between the two is that shift is a state of the hardware and caps
// lock is a state of the driver: shift_held tracks whether a physical key is down
// right now, so it is set on press and cleared on release, while caps_on is a flag
// the press of caps lock toggles and nothing clears.
static int shift_held = 0;
static int caps_on    = 0;

// Left ctrl, tracked exactly like shift and for the same reason: it is a state of
// the hardware, so it is set on press and cleared on release. RIGHT ctrl is NOT
// tracked, and cannot be until extended scancodes are: it arrives as the two bytes
// 0xE0 0x1D, and the 0xE0 has bit 7 set, so the release branch below swallows it and
// the 0x1D that follows is decoded as if it were the left one. That happens to work
// for right ctrl by accident. See the TODO(extended-scancodes) note further down.
static int ctrl_held  = 0;

// Set by Ctrl-D, consumed by a console read. This is the ENTIRE line discipline this
// kernel has: not a mode, not a terminal, one flag meaning "the next console read
// should report end of input".
//
// It exists because a console had no way to say EOF. file_read on an FD_CONSOLE
// blocks on WAIT_KEY and hands back characters forever, so `run count.elf` on its own
// — a program whose whole shape is "read until EOF" — could never finish, which is
// what verification 9 of the pipes rung ran into.
//
// ONE FLAG, NOT A COUNT, and it is per-machine rather than per-task, because there is
// one keyboard. Whichever task reads the console next consumes it. With a single
// foreground group that is the task the user meant; with real job control it would
// have to move onto the console descriptor itself.
static volatile int console_eof = 0;

// ---------------------------------------------------------------------------
// The keyboard ring buffer: a fixed circular queue between the keyboard IRQ
// (the producer) and SYS_READKEY (the consumer, in kernel/syscall.c).
// ---------------------------------------------------------------------------
// The IRQ runs with interrupts held off and must be short, so it does the least
// possible work: decode one scancode and drop the character here. The consumer, a
// ring-3 program calling through the syscall gate, drains the buffer at its own
// pace. Producer and consumer never run at the same instant (single CPU, and both
// the keyboard IRQ gate and the syscall gate clear IF on entry), so the two
// indices need no lock.
//
// THE TWO INDICES. write_index is the slot the next produced character goes into;
// read_index is the slot the next consumed character comes out of. They chase each
// other around a ring of KBD_BUFFER_SIZE slots, each advance stepping forward one
// slot and wrapping modulo the size.
//   write_index == read_index  means EMPTY (the consumer has caught up).
// To keep EMPTY distinguishable from FULL with that single rule, one slot is always
// left unused: the ring holds at most KBD_BUFFER_SIZE - 1 characters at once. If a
// full buffer were allowed to wrap write_index onto read_index, "full" would look
// exactly like "empty" and the buffer would appear to silently discard everything.
#define KBD_BUFFER_SIZE 128

static char     kbd_buffer[KBD_BUFFER_SIZE];
static uint32_t kbd_write_index;   // producer's cursor (next slot to fill)
static uint32_t kbd_read_index;    // consumer's cursor (next slot to drain)

// Producer helper, called only from the keyboard IRQ. Push one character unless
// the ring is full.
//
// FULL POLICY: if advancing write_index would land on read_index, the ring is full
// and the new character is DROPPED, not stored. Dropping is the deliberate choice.
// The alternative, overwriting read_index and stepping past it, would destroy input
// the consumer has already been promised and would also collapse the empty/full
// distinction above. A dropped keypress can simply be retyped; a silently mangled
// queue cannot be recovered.
static void kbd_buffer_push(char c) {
    uint32_t next = (kbd_write_index + 1) % KBD_BUFFER_SIZE;
    if (next == kbd_read_index) {
        return;   // full: drop the new key, see FULL POLICY above
    }
    kbd_buffer[kbd_write_index] = c;
    kbd_write_index = next;
}

// Consumer side, called from the SYS_READKEY handler (kernel/syscall.c). Pop one
// character, or return 0 when the ring is empty.
//
// 0 is a safe "nothing waiting" sentinel: the scancode table maps every unmapped
// key to 0 and keyboard_callback only pushes non-zero characters, so a real 0 never
// enters the ring and cannot be mistaken for "empty".
int keyboard_console_eof(void) {
    // Test-and-clear, so an EOF is delivered to exactly one read. Leaving the flag
    // set would turn one Ctrl-D into a console that reports EOF forever, and every
    // subsequent reader would see end-of-input it never asked for.
    if (!console_eof) {
        return 0;
    }
    console_eof = 0;
    return 1;
}

int keyboard_getchar(void) {
    if (kbd_read_index == kbd_write_index) {
        return 0;   // empty: the consumer has caught up to the producer
    }
    char c = kbd_buffer[kbd_read_index];
    kbd_read_index = (kbd_read_index + 1) % KBD_BUFFER_SIZE;
    return (int)(unsigned char)c;
}

// Which of the two tables decodes this scancode, given the modifier state.
//
// Shift applies to every key. Caps lock applies to letters and to nothing else,
// which is why the two are not simply OR'd together.
//
// THE XOR IS DELIBERATE AND READS LIKE A BUG. For a letter the shifted table is
// chosen when shift_held differs from caps_on, so with caps lock on, holding shift
// gives a LOWERCASE letter. That is what every real keyboard does — shift means
// "the other case", not "upper case" — and writing it as `shift_held || caps_on`
// would make shift+A with caps on produce 'A', which is wrong and which nobody
// would notice until they tried it.
//
// Whether a key is a letter is asked of the UNSHIFTED table, where letters are
// spelled a..z. The shifted table would answer the same question correctly today,
// but only because its letters are the same keys; the plain table is the one where
// "is this a letter" is a property of the key rather than a coincidence.
static const char *table_for(uint8_t scancode) {
    char plain = scancode_to_ascii[scancode];
    int is_letter = (plain >= 'a' && plain <= 'z');
    int use_shifted = is_letter ? (shift_held != caps_on) : shift_held;
    return use_shifted ? scancode_to_ascii_shift : scancode_to_ascii;
}

// The producer. An interrupt handler runs with interrupts held off (the gate is an
// interrupt gate, so the CPU cleared IF on entry), so it must be short: decode one
// scancode, drop the character into the ring buffer, and return. It must NOT echo
// and must NOT run any command. A ring-3 program drains the buffer through
// SYS_READKEY and decides what to do with each key. (Before the shell became a
// ring-3 program this called shell_handle_keypress and did real work inside the
// IRQ, which is exactly what the ring buffer now keeps out of interrupt context.)
//
// EXTENDED SCANCODES ARE NOT HANDLED, and this is where they would go. The arrow
// keys, right ctrl, right alt and a few others send 0xE0 first and the real code
// second. 0xE0 has bit 7 set, so the release branch below swallows it, and the byte
// after it is then decoded as if it were an ordinary key. That is harmless today
// only by luck: the arrows' codes (0x48, 0x4B, 0x4D, 0x50) fall past the populated
// part of the table and decode to 0, and the two that do land on entries (keypad
// enter, keypad slash) happen to produce the character they should. Handling this
// properly means a "saw 0xE0" flag and a third table, and it belongs with whatever
// first needs an arrow key. `TODO(extended-scancodes)`.
static void keyboard_callback(registers_t *regs) {
    (void)regs;
    uint8_t scancode = port_byte_in(KEYBOARD_DATA_PORT);

    // A release. MASK BIT 7 OFF BEFORE COMPARING: the release of left shift arrives
    // as 0xAA, not 0x2A, so comparing the raw byte would never match and shift would
    // turn on and stay on forever. Every other release is still ignored.
    if (scancode & KEY_RELEASE_MASK) {
        uint8_t released = scancode & KEY_CODE_MASK;
        if (released == KEY_LSHIFT || released == KEY_RSHIFT) {
            shift_held = 0;
        }
        if (released == KEY_LCTRL) {
            ctrl_held = 0;
        }
        return;
    }

    // Modifier presses change state and produce no character. Returning here is
    // what keeps them out of the ring buffer AND away from scheduler_wake below:
    // a shift press that woke every task blocked on WAIT_KEY would have them all
    // re-issue SYS_READKEY, find an empty buffer, and block again, which is exactly
    // the busy-wait that blocking exists to remove.
    if (scancode == KEY_LSHIFT || scancode == KEY_RSHIFT) {
        shift_held = 1;
        return;
    }
    if (scancode == KEY_CAPSLOCK) {
        caps_on = !caps_on;
        return;
    }
    if (scancode == KEY_LCTRL) {
        ctrl_held = 1;
        return;
    }

    // The two ctrl combinations this kernel understands. BOTH RETURN BEFORE THE
    // CHARACTER PATH BELOW, exactly as the modifier presses do, and for the same
    // two reasons: neither pushes a character (Ctrl-C is not a 'c' the shell should
    // echo, and Ctrl-D is not a 'd'), and neither may fall through to the
    // scheduler_wake(WAIT_KEY) at the bottom on a keystroke that put nothing in the
    // buffer. A wake with an empty ring hands every WAIT_KEY sleeper a wasted round
    // trip: woken, re-issue the read, find nothing, block again. Holding ctrl and
    // tapping c would spin the scheduler doing that.
    if (ctrl_held) {
        if (scancode == KEY_C) {
            // Ctrl-C: interrupt. Which task this reaches is the interesting question
            // and it is answered in the next stage, where process groups and a
            // declared foreground arrive; until then it goes to whichever task the
            // keyboard IRQ interrupted, which is enough to prove the key is decoded
            // and the bit is set, and is not yet the right answer.
            signal_raise(scheduler_current_id(), SIG_INT);
            return;
        }
        if (scancode == KEY_D) {
            // Ctrl-D: end of input on the console. Set the flag, then wake the
            // WAIT_KEY sleepers — the SAME ordering, and for the same reason, as the
            // push/wake pair at the bottom of this function. A task woken before the
            // flag was set would re-issue its read, find neither a character nor an
            // EOF, and block again, turning the keystroke into a wasted round trip.
            //
            // This wake is not the empty-ring case warned about above: there IS
            // something for a console reader to find, just not a character.
            console_eof = 1;
            scheduler_wake(WAIT_KEY);
            return;
        }
        // Any other key with ctrl held produces nothing. Falling through to the
        // character path would make Ctrl-A type a plain 'a', which is not what the
        // user asked for and would put a character in the buffer they cannot see
        // they typed.
        return;
    }

    char c = table_for(scancode)[scancode];
    if (c != 0) {
        kbd_buffer_push(c);

        // This IRQ is what CAUSES the event tasks blocked on WAIT_KEY are waiting
        // for, so waking them is its job: a sleeping task cannot notice a key
        // arriving, because it is not running. Waking only marks them READY, it does
        // not switch to them, which keeps this handler short and leaves the choice
        // of what runs next where it belongs, in the scheduler on the next tick.
        //
        // Ordering matters: push first, wake second. A task woken before the
        // character was in the buffer could be scheduled, re-issue its read, find
        // nothing, and block again, turning one keypress into a wasted round trip.
        scheduler_wake(WAIT_KEY);
    }
}

void keyboard_init(void) {
    register_interrupt_handler(IRQ_KEYBOARD, keyboard_callback);
}
