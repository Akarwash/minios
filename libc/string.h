#ifndef STRING_H
#define STRING_H

#include "../include/types.h"

// ============================================================================
// String functions, compiled twice: into the kernel and into every ring-3
// program (see the USER_LIBC_SOURCES rule in the Makefile).
// ============================================================================
// These take const pointers, as the C library's do, so a string literal or a
// const buffer can be passed without a cast. Everything here is NUL-terminated
// strings; fixed-length fields are memcmp/memcpy's business (libc/mem.h).

// Length of a NUL-terminated string, not counting the terminator.
size_t strlen(const char *str);

// Compare two NUL-terminated strings: 0 if equal, otherwise the difference of
// the first pair of bytes that differ (negative or positive).
int strcmp(const char *s1, const char *s2);

// Copy src, terminator included, to dest, and return dest. No bound: dest must
// have room for strlen(src) + 1 bytes.
char *strcpy(char *dest, const char *src);

// The first occurrence of c in s, or NULL if there is none. As in C, looking for
// '\0' finds the terminator. Added for the shell's "does this line contain a
// pipe" test (user/shell.c).
char *strchr(const char *s, int c);

#endif
