/*
 * stdlib.h -- primeLua's own stdlib header (shadows newlib's).
 *
 * malloc/calloc/free and qsort/bsearch/abs/labs come from primetcc's
 * runtime (rt/hp_rt.c, compiled into plua_rt.o) and allocate from the
 * calculator's firmware heap.  realloc is primeLua's (plua_libc.c).
 */
#ifndef PLUA_STDLIB_H
#define PLUA_STDLIB_H

#include <stddef.h>

#define EXIT_SUCCESS	0
#define EXIT_FAILURE	1
#define RAND_MAX	2147483647

void  *malloc(size_t n);
void  *calloc(size_t n, size_t size);
void  *realloc(void *p, size_t n);
void   free(void *p);
void   abort(void);
void   exit(int status);
int    atexit(void (*fn)(void));
char  *getenv(const char *name);
int    system(const char *cmd);
int    abs(int v);
long   labs(long v);
int    atoi(const char *s);
long   atol(const char *s);
long   strtol(const char *s, char **endptr, int base);
unsigned long strtoul(const char *s, char **endptr, int base);
double strtod(const char *s, char **endptr);
double atof(const char *s);
void   qsort(void *base, size_t n, size_t size,
	     int (*cmp)(const void *, const void *));
void  *bsearch(const void *key, const void *base, size_t n, size_t size,
	       int (*cmp)(const void *, const void *));
int    rand(void);
void   srand(unsigned seed);

#endif /* PLUA_STDLIB_H */
