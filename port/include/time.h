/*
 * time.h -- primeLua's own time header.
 *
 * os.time / os.date / os.clock have no firmware time source yet (see
 * plua_time.c): the wall clock is seeded from the calculator at launch
 * (main.py reads it and puts it in the launch config), and os.clock
 * measures elapsed time from that same seed.  Precision is therefore
 * whatever the launcher can read -- documented in PRIMELUA_README.md as a
 * known limitation.
 */
#ifndef PLUA_TIME_H
#define PLUA_TIME_H

#include <stddef.h>

typedef long long time_t;	/* 64-bit: os.time{year=2100} must not wrap */
typedef long clock_t;

#define CLOCKS_PER_SEC	1000000L

struct tm {
	int tm_sec;
	int tm_min;
	int tm_hour;
	int tm_mday;
	int tm_mon;
	int tm_year;	/* years since 1900 */
	int tm_wday;
	int tm_yday;
	int tm_isdst;
};

time_t  time(time_t *t);
clock_t clock(void);
double  difftime(time_t t2, time_t t1);
time_t  mktime(struct tm *tm);
struct tm *localtime(const time_t *t);
struct tm *gmtime(const time_t *t);
size_t  strftime(char *s, size_t max, const char *fmt, const struct tm *tm);

#endif /* PLUA_TIME_H */
