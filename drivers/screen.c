#include "screen.h"
#include "ports.h"
#include "../libc/mem.h"

static int get_cursor(void) {
    port_byte_out(0x3D4, 14);
    int offset = port_byte_in(0x3D5) << 8;
    port_byte_out(0x3D4, 15);
    offset += port_byte_in(0x3D5);
    return offset;
}

static void set_cursor(int offset) {
    port_byte_out(0x3D4, 14);
    port_byte_out(0x3D5, (uint8_t)(offset >> 8));
    port_byte_out(0x3D4, 15);
    port_byte_out(0x3D5, (uint8_t)(offset & 0xFF));
}

static void scroll(void) {
    char *video = (char *)VIDEO_ADDRESS;
    int i;

    // memmove, NOT memcpy: the source and destination overlap (every row moves up
    // over the row above it). This used to be memcpy, and it worked only because
    // the copy happens to run forward and the destination is BELOW the source, so
    // each byte was read before anything overwrote it. That is an accident of one
    // implementation, not a property of memcpy, whose contract says the ranges
    // must not overlap; a memcpy that copied backward, or by words, would have
    // scrolled garbage. memmove exists for exactly this case (libc/mem.c).
    memmove(video, video + MAX_COLS * 2, MAX_COLS * (MAX_ROWS - 1) * 2);

    char *last_row = video + (MAX_ROWS - 1) * MAX_COLS * 2;
    for (i = 0; i < MAX_COLS; i++) {
        last_row[i * 2] = ' ';
        last_row[i * 2 + 1] = WHITE_ON_BLACK;
    }
}

void screen_clear(void) {
    char *video = (char *)VIDEO_ADDRESS;
    int i;
    for (i = 0; i < MAX_ROWS * MAX_COLS; i++) {
        video[i * 2] = ' ';
        video[i * 2 + 1] = WHITE_ON_BLACK;
    }
    set_cursor(0);
}

void print_char(char c) {
    char *video = (char *)VIDEO_ADDRESS;
    int offset = get_cursor();

    if (c == '\n') {
        int row = offset / MAX_COLS;
        offset = (row + 1) * MAX_COLS;
    }
    else if (c == '\b') {
        if (offset > 0) {
            offset--;
            video[offset * 2] = ' ';
            video[offset * 2 + 1] = WHITE_ON_BLACK;
        }
        set_cursor(offset);
        return;
    }
    else {
        video[offset * 2] = c;
        video[offset * 2 + 1] = WHITE_ON_BLACK;
        offset++;
    }

    if (offset >= MAX_ROWS * MAX_COLS) {
        scroll();
        offset = (MAX_ROWS - 1) * MAX_COLS;
    }

    set_cursor(offset);
}


void print_string(char *str) {
    int i = 0;
    while (str[i] != '\0') {
        print_char(str[i]);
        i++;
    }
}

void print_int(uint32_t n) {
    char buf[11];
    int i = 0;

    if (n == 0) {
        print_char('0');
        return;
    }

    while (n > 0) {
        buf[i++] = '0' + (n % 10);
        n /= 10;
    }

    while (i > 0) {
        print_char(buf[--i]);
    }
}

// Addresses, sizes and file offsets are read and compared in hex everywhere
// else (linker scripts, readelf, the QEMU monitor, this kernel's own comments),
// so printing them in decimal makes them needlessly hard to check against the
// tools. 64-bit because an address is.
void print_hex(uint64_t n) {
    char buf[16];
    int i = 0;

    print_string("0x");
    if (n == 0) {
        print_char('0');
        return;
    }

    while (n > 0) {
        uint8_t digit = (uint8_t)(n & 0xF);
        buf[i++] = (char)(digit < 10 ? '0' + digit : 'a' + (digit - 10));
        n >>= 4;
    }

    while (i > 0) {
        print_char(buf[--i]);
    }
}

