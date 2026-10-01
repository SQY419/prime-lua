/*
 * plua_libc.c -- the rest of the C library Lua 5.4 needs, on the bare
 * calculator: allocation, exit/abort, errno, locale, ctype, strtoul and a
 * couple of host-only odds and ends.
 *
 * What is NOT here is as important as what is: printf/sprintf/snprintf
 * (%f %e %g included), malloc/calloc/free, the mem-and-str core, qsort,
 * bsearch, atoi/strtol, ftoa and strtod all come from primetcc's proven
 * runtime (rt/hp_rt.c + rt/hp_string.c, compiled into plua_rt.o and
 * allocating straight from the firmware heap).  primeLua only adds the
 * pieces Lua uses that the TCC runtime never needed.
 */
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <locale.h>
#include <stdio.h>
#include <ctype.h>
#include <setjmp.h>

#include "plua_port.h"

/* ---- firmware heap wrappers (plua_svc.s) ---- */
unsigned hp_svc_malloc(unsigned n);
unsigned hp_svc_realloc(unsigned p, unsigned n);
void     hp_svc_free(unsigned p);

/* ---- errno -------------------------------------------------------------
 * The firmware has no errno; a plain global keeps liolib's error strings
 * meaningful even though the values are only "what primeLua decided".
 */
int errno = 0;

/* ---- allocation --------------------------------------------------------
 * primeLua deliberately shares primetcc's allocator header layout so the
 * two implementations interoperate: hp_rt.c's malloc() hands back an
 * 8-byte aligned pointer whose two preceding words are {raw, size}.  The
 * firmware heap itself only guarantees 4-byte alignment, and libgcc's
 * soft-float helpers use LDRD/STRD on 8-byte objects -- an unaligned access
 * faults the ARM926EJ-S and reboots the box, so the padding is not
 * optional.  realloc is the one piece hp_rt.c never provided (TCC programs
 * never needed it) and Lua's allocator uses it for everything.
 */
void *realloc(void *p, size_t n)
{
	unsigned oldn;
	void *np;

	if (p == 0)
		return malloc(n);
	if (n == 0) {
		free(p);
		return 0;
	}
	oldn = ((unsigned *)p)[-1];

	/* malloc + copy + free, NOT the firmware's realloc (svc 0x10039).
	 * primetcc's runtime deliberately avoids that service too, and it is the
	 * one allocation entry point Lua hammers: every string, table, stack and
	 * buffer growth goes through here.  Relying on the firmware to preserve
	 * the old block (and its contents) on a move is a device-behaviour
	 * dependency the emulator cannot check -- and a Lua heap corrupted by it
	 * shows up as random script errors and reboots, not as a clean failure. */
	if (n <= oldn) {			/* shrink: keep the block */
		((unsigned *)p)[-1] = (unsigned)n;
		return p;
	}
	np = malloc(n);
	if (np == 0)
		return 0;			/* the old block is untouched */
	memcpy(np, p, oldn);
	free(p);
	return np;
}

/* ---- exit / abort ------------------------------------------------------
 * There is nothing to exit to on a calculator: exit() unwinds to the jmp_buf
 * plua_main() installed so the entry sequence can still return the program
 * stack to the firmware heap and report a status to the launcher.  If exit()
 * is somehow called before that point, stop instead of returning into a
 * driver that expects not to come back.
 */
jmp_buf plua_exit_env;
int plua_exit_code = 0;
int plua_exit_armed = 0;

void exit(int status)
{
	plua_exit_code = status;
	if (plua_exit_armed)
		longjmp(plua_exit_env, 1);
	for (;;)
		;
}

void abort(void)
{
	exit(EXIT_FAILURE);
}

int atexit(void (*fn)(void))
{
	(void)fn;
	return 0;				/* exit() never runs handlers here */
}

/* ---- environment -------------------------------------------------------
 * No environment block exists on the calculator.  Returning NULL for every
 * name is what makes Lua skip LUA_INIT and liolib skip its default paths.
 */
char *getenv(const char *name)
{
	(void)name;
	return 0;
}

int system(const char *cmd)
{
	(void)cmd;
	return -1;
}

/* ---- string.h bits the runtime does not have ---- */
char *strerror(int errnum)
{
	/* The exact glibc wording: Lua's io library builds error messages as
	 * "name: " .. strerror(errno) and the differential tests compare whole
	 * messages, so a paraphrase would show up as a failure. */
	switch (errnum) {
	case 0:		return "Success";
	case EPERM:	return "Operation not permitted";
	case ENOENT:	return "No such file or directory";
	case EIO:	return "Input/output error";
	case EBADF:	return "Bad file descriptor";
	case EAGAIN:	return "Resource temporarily unavailable";
	case ENOMEM:	return "Cannot allocate memory";
	case EACCES:	return "Permission denied";
	case EEXIST:	return "File exists";
	case EINVAL:	return "Invalid argument";
	case ENOSPC:	return "No space left on device";
	case EDOM:	return "Numerical argument out of domain";
	case ERANGE:	return "Numerical result out of range";
	default:	return "Unknown error";
	}
}

/* The calculator has a single locale, so collation is byte order -- which is
 * also exactly what Lua expects before any setlocale call. */
int strcoll(const char *a, const char *b)
{
	return strcmp(a, b);
}

/* ---- stdlib bits ---- */
unsigned long strtoul(const char *s, char **endptr, int base)
{
	unsigned long v = 0;
	int neg = 0;
	const char *p = s;
	while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
		p++;
	if (*p == '+' || *p == '-')
		neg = (*p++ == '-');
	if (base == 0) {
		if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
			base = 16;
			p += 2;
		} else if (p[0] == '0') {
			base = 8;
		} else {
			base = 10;
		}
	} else if (base == 16 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
		p += 2;
	}
	for (;;) {
		int c = (unsigned char)*p, d;
		if (c >= '0' && c <= '9')
			d = c - '0';
		else if (c >= 'a' && c <= 'z')
			d = c - 'a' + 10;
		else if (c >= 'A' && c <= 'Z')
			d = c - 'A' + 10;
		else
			break;
		if (d >= base)
			break;
		v = v * (unsigned long)base + (unsigned long)d;
		p++;
	}
	if (endptr)
		*endptr = (char *)p;
	return neg ? (unsigned long)(-(long)v) : v;
}

/* ---- locale ---- */
char *setlocale(int category, const char *locale)
{
	(void)category;
	/* Lua's os.setlocale compares the result with the requested name; the
	 * only honest answer on this machine is the C locale. */
	if (locale == 0 || strcmp(locale, "C") == 0)
		return "C";
	return 0;
}

static char lconv_decimal_point[] = ".";

struct lconv *localeconv(void)
{
	static struct lconv lc;
	static int init = 0;
	if (!init) {
		lc.decimal_point = lconv_decimal_point;
		lc.thousands_sep = (char *)"";
		lc.grouping = (char *)"";
		lc.int_curr_symbol = (char *)"";
		lc.currency_symbol = (char *)"";
		lc.mon_decimal_point = (char *)"";
		lc.mon_thousands_sep = (char *)"";
		lc.mon_grouping = (char *)"";
		lc.positive_sign = (char *)"";
		lc.negative_sign = (char *)"";
		lc.int_frac_digits = 127;
		lc.frac_digits = 127;
		lc.p_cs_precedes = 127;
		lc.p_sep_by_space = 127;
		lc.n_cs_precedes = 127;
		lc.n_sep_by_space = 127;
		lc.p_sign_posn = 127;
		lc.n_sign_posn = 127;
		init = 1;
	}
	return &lc;
}

/* ---- ctype (ASCII only, which is all that reaches here) ---- */
int isdigit(int c)  { return c >= '0' && c <= '9'; }
int isupper(int c)  { return c >= 'A' && c <= 'Z'; }
int islower(int c)  { return c >= 'a' && c <= 'z'; }
int isalpha(int c)  { return isupper(c) || islower(c); }
int isalnum(int c)  { return isalpha(c) || isdigit(c); }
int isspace(int c)  { return c == ' ' || (c >= '\t' && c <= '\r'); }
int isxdigit(int c)
{
	return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
int iscntrl(int c)  { return c < 0x20 || c == 0x7f; }
int isprint(int c)  { return c >= 0x20 && c < 0x7f; }
int isgraph(int c)  { return c > 0x20 && c < 0x7f; }
int ispunct(int c)  { return isgraph(c) && !isalnum(c); }

/* ---- assert ---- */
void plua_assert_fail(const char *expr, const char *file, int line)
{
	printf("assertion failed: %s (%s:%d)\n", expr, file, line);
	abort();
}

/* ---- signal ---- */
void (*signal(int sig, void (*handler)(int)))(int)
{
	(void)sig;
	return handler;			/* remembered, never delivered */
}
