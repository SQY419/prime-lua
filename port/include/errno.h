/*
 * errno.h -- primeLua's own errno header.
 *
 * There is no errno in the firmware; Lua's liolib only reads it to build
 * error strings, so a plain global is enough.  plua_libc.c defines it.
 */
#ifndef PLUA_ERRNO_H
#define PLUA_ERRNO_H

extern int errno;

#define EPERM	1
#define ENOENT	2
#define EIO	5
#define EBADF	9
#define EAGAIN	11
#define ENOMEM	12
#define EACCES	13
#define EEXIST	17
#define EINVAL	22
#define ENOSPC	28
#define EDOM	33
#define ERANGE	34

#endif /* PLUA_ERRNO_H */
