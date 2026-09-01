#ifndef KEYBOARD_H
#define KEYBOARD_H

void keyboard_init(void);

// Pop one character from the keyboard ring buffer, or return 0 if none is waiting.
// The consumer end of the producer/consumer pair driven by the keyboard IRQ; the
// SYS_READKEY handler (kernel/syscall.c) is its only caller. See the ring buffer
// in drivers/keyboard.c for the empty sentinel and the full-drop policy.
int keyboard_getchar(void);

// Has Ctrl-D been pressed since the last time this was asked? Returns 1 exactly
// once per Ctrl-D and clears the flag, so one keystroke ends one console read.
// Called by file_read on an FD_CONSOLE (kernel/file.c), which reports it as a
// zero-byte read — the end-of-file convention every reader already understands.
int keyboard_console_eof(void);

#endif
