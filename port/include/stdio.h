/*
 * stdio.h -- primeLua's own stdio header (shadows newlib's).
 *
 * Lua 5.4 is compiled against THIS header.  There is no newlib stdio here:
 * FILE is primeLua's own object (see plua_file.c), stdout/stderr are the
 * PRIMELOG ring the calculator-side main.py streams to the screen, and file
 * descriptors are firmware handles opened through the wide-char path API
 * (svc 0x1026f), exactly like the DOOM / primetcc ports do.
 */
#ifndef PLUA_STDIO_H
#define PLUA_STDIO_H

#include <stddef.h>
#include <stdarg.h>

#define EOF		(-1)
#define BUFSIZ		512
#define FOPEN_MAX	16
#define FILENAME_MAX	256
#define L_tmpnam	64
#define TMP_MAX		1
#define SEEK_SET	0
#define SEEK_CUR	1
#define SEEK_END	2

/* liolib's io.setvbuf() maps names to these; primeLua's streams are always
 * unbuffered, so the mapping only has to exist, not to mean anything. */
#define _IOFBF		0
#define _IOLBF		1
#define _IONBF		2

/* opaque to Lua: liolib only ever holds a FILE * */
struct plua_file;
typedef struct plua_file FILE;

/* the two console streams (plua_file.c): writes land in the PRIMELOG ring */
extern FILE *const plua_stdout;
extern FILE *const plua_stderr;

#define stdin	((FILE *)0)
#define stdout	(plua_stdout)
#define stderr	(plua_stderr)

FILE  *fopen(const char *path, const char *mode);
FILE  *freopen(const char *path, const char *mode, FILE *f);
int    fclose(FILE *f);
int    fflush(FILE *f);
size_t fread(void *ptr, size_t size, size_t nmemb, FILE *f);
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *f);
int    fseek(FILE *f, long offset, int whence);
long   ftell(FILE *f);
int    feof(FILE *f);
int    ferror(FILE *f);
void   clearerr(FILE *f);
int    fgetc(FILE *f);
int    ungetc(int c, FILE *f);
int    fputc(int c, FILE *f);
int    putc(int c, FILE *f);
int    getc(FILE *f);
char  *fgets(char *buf, int size, FILE *f);
int    fputs(const char *s, FILE *f);
int    puts(const char *s);
int    putchar(int c);
FILE  *tmpfile(void);
char  *tmpnam(char *buf);
int    remove(const char *path);
int    rename(const char *oldname, const char *newname);
void   perror(const char *s);
int    setvbuf(FILE *f, char *buf, int mode, size_t size);

int    printf(const char *fmt, ...);
int    fprintf(FILE *f, const char *fmt, ...);
int    sprintf(char *buf, const char *fmt, ...);
int    snprintf(char *buf, size_t n, const char *fmt, ...);
int    vprintf(const char *fmt, va_list ap);
int    vfprintf(FILE *f, const char *fmt, va_list ap);
int    vsprintf(char *buf, const char *fmt, va_list ap);
int    vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);

/* the ring the calculator-side loader scans for ("PLUARING" magic) */
#define PLUA_RING_SIZE	32768
struct plua_ring {
	char magic[8];
	volatile int count;
	char data[PLUA_RING_SIZE];
};
extern struct plua_ring plua_ring;
void plua_ring_puts(const char *s);
void plua_console_init(void);

#endif /* PLUA_STDIO_H */
