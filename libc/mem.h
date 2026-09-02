#ifndef MEM_H
#define MEM_H

#include "../include/types.h"

// ============================================================================
// Memory functions, compiled twice: into the kernel and into every ring-3
// program (see the USER_LIBC_SOURCES rule in the Makefile).
// ============================================================================
// Standard signatures, so code written against the C library's memcpy and memset
// compiles here unchanged and a caller may use the returned pointer.

// Copy n bytes from src to dest. THE RANGES MUST NOT OVERLAP: this copies forward,
// byte by byte, and an overlap where dest is above src overwrites bytes before
// they are read. Use memmove for anything that might overlap.
void *memcpy(void *dest, const void *src, size_t n);

// Copy n bytes from src to dest, correctly even when the ranges overlap: forward
// when dest is below src, backward when it is above, so no byte is overwritten
// before it has been read. Added for the screen driver's scroll, which shifts
// every row up by one over itself (drivers/screen.c).
void *memmove(void *dest, const void *src, size_t n);

// Set n bytes at dest to (unsigned char)val.
void *memset(void *dest, int val, size_t n);

// Compare n bytes as unsigned char: 0 if equal, otherwise negative or positive
// according to the first differing byte. This is the compare for fixed-length,
// unterminated fields, which is what an 8.3 directory name is (fs/fat32.c).
int memcmp(const void *a, const void *b, size_t n);

#endif
