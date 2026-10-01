/*
 * ctype.h -- primeLua's own ctype header.
 *
 * Lua 5.4 classifies characters with its own tables (lctype.h) unless
 * LUA_USE_CTYPE is defined, so these are only needed by the few library
 * corners that still use isdigit()/isspace().  plua_libc.c implements them
 * for the ASCII range, which is all the calculator will ever see.
 */
#ifndef PLUA_CTYPE_H
#define PLUA_CTYPE_H

int isalpha(int c);
int isdigit(int c);
int isalnum(int c);
int isspace(int c);
int isxdigit(int c);
int isupper(int c);
int islower(int c);
int ispunct(int c);
int iscntrl(int c);
int isprint(int c);
int isgraph(int c);
int tolower(int c);
int toupper(int c);

#endif /* PLUA_CTYPE_H */
