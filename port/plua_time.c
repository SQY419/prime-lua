/*
 * plua_time.c -- os.time / os.date / os.clock on the calculator's clock
 *
 * THE CLOCK (measured on the device with calc/timeprobe.py, 2025-09-25)
 *     svc 0x100A5 is SDKLIB's GetSysTime and its body is literally
 *     `push {r0}; push {lr}; svc 0x100A5` -- a ONE-ARGUMENT function.  primetcc
 *     called it with r0 = 0 and the machine reset (the write went to NULL);
 *     with a real buffer it answers:
 *
 *         r0 = the year (2026)
 *         buffer, as little-endian u16s:
 *             [0] year  [1] month  [2] day-of-week (0=Sunday)  [3] day
 *             [4] hour  [5] minute [6] second  [7] ?
 *             [8..9] a u32 sub-second counter at offset 16
 *
 *     That buffer read 2026-09-25 (dow = 5 = Friday) 20:31:58, two seconds
 *     after PPL reported 20:31:56 in the same run.  time() therefore comes
 *     from the firmware itself, and os.date() formats a real calendar date.
 *     os.clock() is built from the offset-16 counter, SELF-CALIBRATED against
 *     time() so its unit (milliseconds, 1/1024 s, ...) does not have to be
 *     assumed.
 *
 * FALLBACKS
 *     If the service answers nonsense (or the machine is an emulator that
 *     models it differently) time() falls back to the epoch the launcher read
 *     from PPL -- after PLUA_FW_FAIL_MAX bad answers it stops asking, so one
 *     broken clock cannot turn every os.time() call into a firmware call.
 *     Nothing here lies: with neither source, time() is 0 (1970-01-01) and
 *     os.clock() is 0, exactly as before.
 *
 * localtime/gmtime compute a civil calendar date from the epoch arithmetic
 * (days since 1970), which is exact for any date the RTC can produce.
 */
#include <time.h>
#include <stdio.h>
#include <string.h>

#include "plua_port.h"

static long long plua_epoch = -1;	/* -1: the launcher did not say */
static struct tm plua_tm;		/* one shared breakdown, like newlib */

void plua_set_epoch(long long epoch)
{
	plua_epoch = epoch;
}

/* ---------------------------------------------------------------------------
 * the firmware clock
 * ------------------------------------------------------------------------- */
#define PLUA_FW_FAIL_MAX 6	/* stop asking after this many bad answers */

/* The firmware call, behind a seam.  On the device the argument is the 32-bit
 * address of the buffer; the host test redefines this macro so its fake gets a
 * real pointer (a 64-bit host pointer would not survive the cast). */
#ifndef PLUA_GETTIME_CALL
#define PLUA_GETTIME_CALL(buf) hp_svc_gettime((unsigned)(unsigned long)(buf))
#else
PLUA_GETTIME_DECL;		/* the test's replacement, declared by -D */
#endif

enum { FW_UNTRIED = 0, FW_WORKS = 1, FW_DEAD = 2 };

static int fw_state = FW_UNTRIED;
static int fw_fails;

static unsigned short fw_buf[16];	/* the struct svc 0x100A5 writes */

/* The 16 bytes as Unix seconds, or -1 when they are not a date at all.
 * Pure, and therefore testable on the host: tests/test_time_host.c feeds it
 * the exact structure the calculator returned. */
long long plua_systime_seconds(const unsigned short *f)
{
	struct tm tm;

	if (f[0] < 1970 || f[0] > 2200 || f[1] < 1 || f[1] > 12
	    || f[3] < 1 || f[3] > 31 || f[4] > 23 || f[5] > 59 || f[6] > 60)
		return -1;
	memset(&tm, 0, sizeof(tm));
	tm.tm_year = (int)f[0] - 1900;
	tm.tm_mon = (int)f[1] - 1;
	tm.tm_mday = (int)f[3];
	tm.tm_hour = (int)f[4];
	tm.tm_min = (int)f[5];
	tm.tm_sec = (int)f[6];
	return (long long)mktime(&tm);
}

/* One read of the firmware clock.  Returns Unix seconds or -1; *tail (when
 * given) is the raw sub-second counter.
 *
 * EVERY call goes to the firmware.  A cache keyed on the counter looks
 * tempting -- the value cannot change within one tick -- but it is wrong: the
 * buffer only changes when the service is CALLED, so comparing it against the
 * previous read always matches and the clock freezes at the first value.  The
 * call is a firmware service call, the same order of cost as malloc.
 */
long long plua_fw_now(unsigned long long *tail)
{
	unsigned short *f = fw_buf;
	long long s;
	unsigned r;

	if (fw_state == FW_DEAD)
		return -1;
	memset(f, 0, sizeof(fw_buf));
	r = PLUA_GETTIME_CALL(f);
	(void)r;
	s = plua_systime_seconds(f);
	if (s < 0) {
		if (++fw_fails >= PLUA_FW_FAIL_MAX)
			fw_state = FW_DEAD;	/* do not keep poking it */
		return -1;
	}
	fw_state = FW_WORKS;
	fw_fails = 0;
	if (tail)
		*tail = (unsigned long long)f[8]
			| ((unsigned long long)f[9] << 16);
	return s;
}

time_t time(time_t *t)
{
	time_t v;
	unsigned long long tail = 0;
	long long s = plua_fw_now(&tail);
	if (s < 0)
		s = (plua_epoch < 0 ? 0 : plua_epoch);
	v = (time_t)s;
	if (t)
		*t = v;
	return v;
}

/* ---------------------------------------------------------------------------
 * os.clock()
 *
 * Lua asks for clock()/CLOCKS_PER_SEC = seconds of CPU time since the program
 * started; on a calculator with one process, "wall clock since the interpreter
 * started" is the useful reading.  The unit of the firmware's own counter is
 * never assumed: the first time time() shows a whole-second change, the two
 * samples give ticks-per-second directly, and every later call uses that.
 * Until then the counter is taken to be milliseconds -- the 2025-09-25 run saw
 * it advance by the wall milliseconds of a 2.3 s gap.
 * ------------------------------------------------------------------------- */
static int clk_have_base;
static unsigned long long clk_base;
static unsigned long long clk_last_tail;
static long long clk_last_sec;
static long long clk_tps = 1000;	/* ticks per second, calibrated */

clock_t clock(void)
{
	unsigned long long tail = 0;
	long long s = plua_fw_now(&tail);

	if (s < 0)
		return 0;			/* no clock source: honest zero */
	if (!clk_have_base) {
		clk_have_base = 1;
		clk_base = tail;
		clk_last_tail = tail;
		clk_last_sec = s;
		return 0;
	}
	if (s != clk_last_sec) {
		long long ds = s - clk_last_sec;
		unsigned long long d = tail - clk_last_tail;
		if (ds > 0 && ds <= 60 && d > 0) {
			long long tps = (long long)(d / (unsigned long long)ds);
			if (tps >= 50 && tps <= 1000000)
				clk_tps = tps;	/* measured, not guessed */
		}
		clk_last_tail = tail;
		clk_last_sec = s;
	}
	return (clock_t)(((tail - clk_base) * 1000000ULL)
			 / (unsigned long long)clk_tps);
}

double difftime(time_t t2, time_t t1)
{
	return (double)(t2 - t1);
}

/* days since 1970-01-01 -> civil date (Howard Hinnant's algorithm) */
static void civil_from_days(long long z, int *y, int *m, int *d)
{
	long long era, doe, yoe, doy, mp;
	z += 719468;
	era = (z >= 0 ? z : z - 146096) / 146097;
	doe = z - era * 146097;
	yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	*y = (int)(yoe + era * 400);
	doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	mp = (5 * doy + 2) / 153;
	*d = (int)(doy - (153 * mp + 2) / 5 + 1);
	*m = (int)(mp < 10 ? mp + 3 : mp - 9);
	*y += (*m <= 2);
}

static const int mdays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

static int is_leap(int y)
{
	return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

/* day of year, 0-based: os.date("*t") reports it (as tm_yday) */
static int day_of_year(int y, int m, int d)
{
	int i, doy = d - 1;
	for (i = 0; i < m - 1; i++)
		doy += mdays[i] + ((i == 1 && is_leap(y)) ? 1 : 0);
	return doy;
}

static struct tm *breakdown(long long t)
{
	long long days, secs;
	int y, m, d;
	days = t / 86400;
	secs = t % 86400;
	if (secs < 0) {
		secs += 86400;
		days--;
	}
	civil_from_days(days, &y, &m, &d);
	plua_tm.tm_year = y - 1900;
	plua_tm.tm_mon = m - 1;
	plua_tm.tm_mday = d;
	plua_tm.tm_hour = (int)(secs / 3600);
	plua_tm.tm_min = (int)((secs % 3600) / 60);
	plua_tm.tm_sec = (int)(secs % 60);
	plua_tm.tm_wday = (int)((days + 4) % 7);	/* 1970-01-01 was a Thursday */
	if (plua_tm.tm_wday < 0)
		plua_tm.tm_wday += 7;
	plua_tm.tm_yday = day_of_year(y, m, d);
	plua_tm.tm_isdst = 0;
	return &plua_tm;
}

struct tm *localtime(const time_t *t)
{
	return breakdown(t ? (long long)*t : (long long)time(0));
}

struct tm *gmtime(const time_t *t)
{
	return breakdown(t ? (long long)*t : (long long)time(0));
}

/* inverse of breakdown(): os.time{year=...,month=...,day=...} and os.date
 * with an explicit time table both need it.  Months outside 1..12 are
 * normalized by carrying into the year, exactly as mktime is required to. */
time_t mktime(struct tm *tm)
{
	long long days = 0;
	int y, m, i;
	if (!tm)
		return -1;
	y = tm->tm_year + 1900;
	m = tm->tm_mon;
	y += (m >= 0 ? m : m - 11) / 12;
	m -= (m >= 0 ? m : m - 11) / 12 * 12;	/* now 0..11 */
	days += (long long)(y - 1970) * 365LL;
	for (i = 1970; i < y; i++)
		if (is_leap(i))
			days++;
	for (i = 1970; i > y; i--)
		if (is_leap(i - 1))
			days--;
	for (i = 0; i < m; i++)
		days += mdays[i] + ((i == 1 && is_leap(y)) ? 1 : 0);
	days += tm->tm_mday - 1;
	return (time_t)(days * 86400LL + (long long)tm->tm_hour * 3600
			+ (long long)tm->tm_min * 60 + tm->tm_sec);
}

static void putn(char *out, int *n, int v, int width)
{
	char tmp[8];
	int k = 0;
	if (v == 0)
		tmp[k++] = '0';
	while (v > 0) {
		tmp[k++] = (char)('0' + v % 10);
		v /= 10;
	}
	while (k < width)
		tmp[k++] = '0';
	while (k > 0)
		out[(*n)++] = tmp[--k];
}

static void put_str(char *out, int *n, const char *str, int width, int pad)
{
	int len = (int)strlen(str);
	while (len < width) {
		out[(*n)++] = (char)pad;
		width--;
	}
	memcpy(out + *n, str, (size_t)len);
	*n += len;
}

/* ---------------------------------------------------------------------------
 * strftime
 *
 * Two things about this function are not obvious and both were found the hard
 * way on the calculator:
 *
 *  1. Lua's os.date() calls it as strftime(buff, SIZETIMEFMT, buff, stm) --
 *     the format string and the destination buffer are THE SAME MEMORY.  An
 *     implementation that writes into s[] while still reading fmt[] corrupts
 *     its own format string (os.date("!%j %Y") printed "001 0371").  The
 *     result is therefore built in a local buffer and copied out at the end.
 *
 *  2. %a/%b/%h are the ABBREVIATED names and %A/%B the full ones.  Printing
 *     the long name for %a is the classic strftime mistake.
 *
 * The C locale is assumed (there is one locale on this machine).  Conversions
 * that are locale-defined -- %c, %x, %X, %Z -- follow glibc's C locale so that
 * os.date() with no arguments prints what a desktop Lua prints.
 * ------------------------------------------------------------------------- */
#define STRFTIME_LOCAL 256

size_t strftime(char *s, size_t max, const char *fmt, const struct tm *tm)
{
	static const char *abday[7] = { "Sun", "Mon", "Tue", "Wed", "Thu",
		"Fri", "Sat" };
	static const char *day[7] = { "Sunday", "Monday", "Tuesday", "Wednesday",
		"Thursday", "Friday", "Saturday" };
	static const char *abmon[12] = { "Jan", "Feb", "Mar", "Apr", "May",
		"Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
	static const char *mon[12] = { "January", "February", "March", "April",
		"May", "June", "July", "August", "September", "October",
		"November", "December" };
	char buf[STRFTIME_LOCAL];
	int n = 0;
	int wday = tm->tm_wday % 7;
	int mon_i = tm->tm_mon % 12;

	if (!s || !fmt || !tm || max == 0)
		return 0;
	if (wday < 0)
		wday += 7;
	if (mon_i < 0)
		mon_i += 12;

	for (; *fmt; fmt++) {
		if (*fmt != '%') {
			if (n < STRFTIME_LOCAL - 1)
				buf[n++] = *fmt;
			continue;
		}
		fmt++;
		switch (*fmt) {
		case 'a': put_str(buf, &n, abday[wday], 0, ' '); break;
		case 'A': put_str(buf, &n, day[wday], 0, ' '); break;
		case 'b': case 'h':
			put_str(buf, &n, abmon[mon_i], 0, ' '); break;
		case 'B': put_str(buf, &n, mon[mon_i], 0, ' '); break;
		case 'p': put_str(buf, &n, tm->tm_hour < 12 ? "AM" : "PM", 0, ' ');
			break;
		case 'Y': putn(buf, &n, tm->tm_year + 1900, 4); break;
		case 'y': putn(buf, &n, (tm->tm_year + 1900) % 100, 2); break;
		case 'C': putn(buf, &n, (tm->tm_year + 1900) / 100, 2); break;
		case 'm': putn(buf, &n, mon_i + 1, 2); break;
		case 'd': putn(buf, &n, tm->tm_mday, 2); break;
		case 'e': put_str(buf, &n, "" , 0, ' '); putn(buf, &n, tm->tm_mday, 2);
			/* %e is the space-padded day (glibc's %c uses it) */
			if (buf[n - 2] == '0')
				buf[n - 2] = ' ';
			break;
		case 'H': putn(buf, &n, tm->tm_hour, 2); break;
		case 'I': putn(buf, &n, tm->tm_hour % 12 ? tm->tm_hour % 12 : 12, 2);
			break;
		case 'M': putn(buf, &n, tm->tm_min, 2); break;
		case 'S': putn(buf, &n, tm->tm_sec, 2); break;
		case 'j': putn(buf, &n, tm->tm_yday + 1, 3); break;
		case 'u': putn(buf, &n, wday == 0 ? 7 : wday, 1); break;
		case 'w': putn(buf, &n, wday, 1); break;
		case 'n': if (n < STRFTIME_LOCAL - 1) buf[n++] = '\n'; break;
		case 't': if (n < STRFTIME_LOCAL - 1) buf[n++] = '\t'; break;
		case 'Z': put_str(buf, &n, "GMT", 0, ' '); break;
		case 'z': put_str(buf, &n, "+0000", 0, ' '); break;
		case 'c':
			{
				int k = n;
				char sub[64];
				size_t l = strftime(sub, sizeof(sub),
						    "%a %b %e %H:%M:%S %Y", tm);
				memcpy(buf + k, sub, l);
				n = k + (int)l;
			}
			break;
		case 'x':
			{
				int k = n;
				char sub[32];
				size_t l = strftime(sub, sizeof(sub), "%m/%d/%y", tm);
				memcpy(buf + k, sub, l);
				n = k + (int)l;
			}
			break;
		case 'X':
			{
				int k = n;
				char sub[32];
				size_t l = strftime(sub, sizeof(sub), "%H:%M:%S", tm);
				memcpy(buf + k, sub, l);
				n = k + (int)l;
			}
			break;
		case '%':
			if (n < STRFTIME_LOCAL - 1)
				buf[n++] = '%';
			break;
		case 0:				/* trailing '%' prints as '%' */
			if (n < STRFTIME_LOCAL - 1)
				buf[n++] = '%';
			fmt--;
			break;
		default:			/* unknown: keep it visible */
			if (n < STRFTIME_LOCAL - 2) {
				buf[n++] = '%';
				buf[n++] = *fmt;
			}
			break;
		}
		if (n >= STRFTIME_LOCAL - 1)
			break;
	}
	if ((size_t)n >= max)
		n = (int)max - 1;		/* truncate, but stay terminated */
	memcpy(s, buf, (size_t)n);
	s[n] = 0;
	return (size_t)n;
}
