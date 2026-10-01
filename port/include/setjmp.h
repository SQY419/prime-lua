/*
 * setjmp.h -- primeLua's own setjmp header.
 *
 * Lua's error handling (ldo.c: LUAI_THROW / LUAI_TRY) is built on
 * setjmp/longjmp, so this is a load-bearing piece of the port.  The
 * implementation is primeLua's own ARM asm (plua_svc.s), AAPCS soft-float:
 * callee-saved r4-r11, sp and lr -- soft-float means no VFP registers to
 * save, which is exactly why the port targets -mfloat-abi=soft.
 */
#ifndef PLUA_SETJMP_H
#define PLUA_SETJMP_H

/* 10 words: r4-r11, sp, lr (must match plua_svc.s) */
typedef unsigned int jmp_buf[10];

int  setjmp(jmp_buf env);
void longjmp(jmp_buf env, int val);

#endif /* PLUA_SETJMP_H */
