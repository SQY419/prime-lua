/*
 * plua_file.c -- primeLua's stdio layer: the ring the calculator reads, and
 * the calculator's firmware file API.
 *
 * Two kinds of stream:
 *
 *   console streams (stdout/stderr)  -- every byte lands in the ring
 *      (struct plua_ring, magic "PLUARING") that the calculator-side
 *      launcher streams to the screen.  Unbuffered on purpose: a script
 *      that crashes or reboots the box must still have shown its output.
 *
 *   file streams -- firmware handles obtained through the wide-char path
 *      API (svc 0x1026f), the same one FileManager.hpappdir's fm.py and
 *      primetcc use.  Paths arrive from Lua as UTF-8/ASCII and are widened
 *      to UTF-16LE here.  Writes are written through (no buffering), so
 *      files survive an unclean exit.
 *
 * The pushback slot is not decoration: luaL_loadfilex() reads the first
 * character of every script to sniff the '#'/binary/BOM cases and then
 * ungetc()s it back, and it calls freopen() to re-open the file in binary
 * mode when it turns out to be bytecode.  Both must work or no script ever
 * loads.
 *
 * printf/sprintf/snprintf themselves come from primetcc's proven runtime
 * engine (rt/hp_rt.c, linked into plua_rt.o) -- it already implements
 * %d %u %x %o %c %s %p %f %e %g with flags, width and precision, which is
 * what Lua's lua_number2str ("%.14g") and string.format need.  Here we only
 * add the FILE wrappers around it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "plua_port.h"		/* plua_fw_mode(), plua_set_homedir(), ... */
#include <errno.h>

/* ---- firmware entry points (plua_svc.s, pushed-r0 SVC convention) ---- */
unsigned hp_svc_fopen(unsigned path16, unsigned mode16);
unsigned hp_svc_fclose(unsigned fd);
unsigned hp_svc_fseek(unsigned fd, unsigned off, unsigned whence);
unsigned hp_svc_ftell(unsigned fd);
unsigned hp_svc_fread(unsigned buf, unsigned nmemb, unsigned size, unsigned fd);
unsigned hp_svc_fwrite(unsigned buf, unsigned nmemb, unsigned size, unsigned fd);
unsigned hp_svc_filesize(unsigned fd);
unsigned hp_svc_remove(unsigned path16);

/* ---- the ring ----------------------------------------------------------
 * Layout matches primetcc's hp_rt.c ring (8-byte magic, 32-bit count, 32 KB
 * circular data) so the same launcher logic drains both; the magic differs
 * ("PLUARING" vs "PRIMELOG") so a launcher can never confuse the two even
 * when both are present in one image.
 */
struct plua_ring plua_ring = { "PLUARING", 0, {0} };

/* ---------------------------------------------------------------------------
 * Relative paths.
 *
 * The firmware resolves a relative name against ITS current directory, and on
 * the calculator that is not the folder the script lives in -- a script doing
 * io.open("data.txt", "w") fails with "No such file or directory" there while
 * working on a desktop, where the interpreter is started from the script's
 * own folder.  So primeLua keeps the app directory (handed over in the launch
 * config) and prepends it to any path that is not already absolute.
 *
 * Absolute paths are left alone: a script may well want C:\DATA\shared.txt.
 * ------------------------------------------------------------------------- */
#define PLUA_HOME_MAX	160
static char plua_home[PLUA_HOME_MAX];

void plua_set_homedir(const char *dir)
{
	int i;
	if (!dir) {
		plua_home[0] = 0;
		return;
	}
	for (i = 0; dir[i] && i < PLUA_HOME_MAX - 1; i++)
		plua_home[i] = dir[i];
	plua_home[i] = 0;
}

static int path_is_absolute(const char *p)
{
	if (!p || !p[0])
		return 0;
	if (p[0] == '\\' || p[0] == '/')
		return 1;
	if (p[1] == ':')			/* C:... */
		return 1;
	return 0;
}

/* resolve `path` into `out`; returns out (possibly == path) */
static const char *resolve_path(const char *path, char *out, int cap)
{
	int i = 0, j;
	if (path_is_absolute(path) || plua_home[0] == 0)
		return path;
	for (j = 0; plua_home[j] && i < cap - 2; j++)
		out[i++] = plua_home[j];
	if (i > 0 && out[i - 1] != '\\' && out[i - 1] != '/')
		out[i++] = '\\';
	for (j = 0; path[j] && i < cap - 1; j++)
		out[i++] = path[j];
	out[i] = 0;
	return out;
}

static void ring_write(const char *s, size_t n)
{
	size_t i;
	int c = plua_ring.count;
	for (i = 0; i < n; i++) {
		plua_ring.data[c & (PLUA_RING_SIZE - 1)] = s[i];
		c++;
	}
	plua_ring.count = c;
}

/* used by the entry sequence to append "RT_RET:<n>\n" without a FILE */
void plua_ring_puts(const char *s)
{
	ring_write(s, strlen(s));
}

/* primetcc's runtime and its hardware libraries log through prints(); point
 * that at primeLua's ring (the Makefile renames the runtime's own version
 * away, which also lets --gc-sections drop its 32 KB PRIMELOG buffer). */
void prints(const char *s)
{
	if (s)
		ring_write(s, strlen(s));
}

/* The same for the runtime's low-level writers (the Makefile renames hp_rt.c's
 * static ones to these names): with them pointing at primeLua's ring, the
 * runtime's own 32 KB PRIMELOG buffer has no references left and
 * --gc-sections drops it -- 32 KB of firmware heap back on a machine where
 * heap is the scarcest resource. */
void plua_hw_log_write(const char *s)
{
	if (s)
		ring_write(s, strlen(s));
}

void plua_hw_log_char(char c)
{
	ring_write(&c, 1);
}

/* ---- FILE objects ------------------------------------------------------ */
#define PLUA_PATH_MAX	256

struct plua_file {
	unsigned fd;		/* firmware handle; 0 for console streams */
	int console;		/* 1 = stdout/stderr: write into the ring */
	int eof;
	int err;
	int has_push;
	int push;		/* one-character ungetc() slot */
	char path[PLUA_PATH_MAX];
};

static struct plua_file plua_stdout_obj;
static struct plua_file plua_stderr_obj;
FILE *const plua_stdout = &plua_stdout_obj;
FILE *const plua_stderr = &plua_stderr_obj;

/* one-time console setup without a constructor (no crt to run one) */
void plua_console_init(void)
{
	plua_stdout_obj.console = 1;
	plua_stderr_obj.console = 1;
}

/* heap-allocated, so io.tmpfile() and any FILE liolib opens can be closed
 * and freed exactly like on a hosted system */
static struct plua_file *file_new(unsigned fd, int console)
{
	struct plua_file *f = (struct plua_file *)malloc(sizeof(*f));
	if (!f)
		return 0;
	f->fd = fd;
	f->console = console;
	f->eof = 0;
	f->err = 0;
	f->has_push = 0;
	f->push = 0;
	f->path[0] = 0;
	return f;
}

/* ---- path/mode widening (UTF-8 -> UTF-16LE) ---------------------------- */
static int widen(const char *s, unsigned short *out, int cap)
{
	int i;
	if (!s)
		return 0;
	for (i = 0; s[i] && i < cap - 1; i++)
		out[i] = (unsigned short)(unsigned char)s[i];
	out[i] = 0;
	return i;
}

static unsigned fw_open(const char *path, const char *mode)
{
	unsigned short p16[260], m16[8];
	widen(path, p16, 260);
	widen(mode, m16, 8);
	return hp_svc_fopen((unsigned)(unsigned long)p16,
			    (unsigned)(unsigned long)m16);
}

/* Copy a file's contents to another fd (the read-append-rewrite step).  The
 * buffer is allocated per call and released right away: this runs for a
 * handful of opens per run, not in a loop. */
static int copy_file_into(unsigned dst, const char *path, int seek_end)
{
	char *buf;
	unsigned fd, got, total = 0;
	unsigned short p16[260], m16[8];
	widen(path, p16, 260);
	widen("rb", m16, 8);
	fd = hp_svc_fopen((unsigned)(unsigned long)p16,
			  (unsigned)(unsigned long)m16);
	if (!fd)
		return 0;			/* nothing there: fine */
	buf = (char *)malloc(8192);
	if (!buf) {
		hp_svc_fclose(fd);
		return -1;
	}
	while ((got = hp_svc_fread((unsigned)(unsigned long)buf, 1, 8192, fd))
	       > 0) {
		hp_svc_fwrite((unsigned)(unsigned long)buf, 1, got, dst);
		total += got;
		if (total > 262144)		/* cap: do not chase huge files */
			break;
	}
	hp_svc_fclose(fd);
	free(buf);
	if (seek_end)
		hp_svc_fseek(dst, 0, 2 /* SEEK_END */);
	else
		hp_svc_fseek(dst, 0, 0 /* SEEK_SET */);
	return (int)total;
}

FILE *fopen(const char *path, const char *mode)
{
	unsigned fd;
	char full[PLUA_HOME_MAX + PLUA_PATH_MAX];
	struct plua_file *f;
	const char *use;
	if (!path || !mode) {
		errno = EINVAL;
		return 0;
	}
	use = resolve_path(path, full, (int)sizeof(full));
	{
		char fwm[4];
		int keep = plua_fw_mode(mode, fwm, sizeof(fwm));
		fd = fw_open(use, fwm);
		if (!fd) {
			errno = ENOENT;
			return 0;
		}
		if (keep == 2 || keep == 1) {
			/* append / update: the firmware opens "wb+" by truncating,
			 * so the old contents are written back first */
			if (copy_file_into(fd, use, keep == 2) < 0) {
				hp_svc_fclose(fd);
				errno = ENOMEM;
				return 0;
			}
		}
	}
	f = file_new(fd, 0);
	if (!f) {
		hp_svc_fclose(fd);
		errno = ENOMEM;
		return 0;
	}
	strncpy(f->path, use, PLUA_PATH_MAX - 1);
	f->path[PLUA_PATH_MAX - 1] = 0;
	return f;
}

/* luaL_loadfilex() re-opens the file in "rb" after seeing the bytecode
 * signature; filename is NULL in that call, so the saved path is used. */
FILE *freopen(const char *path, const char *mode, FILE *f)
{
	struct plua_file *pf = (struct plua_file *)f;
	unsigned fd;
	if (!pf || pf->console)
		return 0;
	if (!path)
		path = pf->path;
	if (!path || !mode)
		return 0;
	{
		char full[PLUA_HOME_MAX + PLUA_PATH_MAX];
		char fwm[4];
		path = resolve_path(path, full, (int)sizeof(full));
		plua_fw_mode(mode, fwm, sizeof(fwm));
		fd = fw_open(path, fwm);
	}
	if (!fd)
		return 0;
	hp_svc_fclose(pf->fd);
	pf->fd = fd;
	pf->eof = 0;
	pf->err = 0;
	pf->has_push = 0;
	strncpy(pf->path, path, PLUA_PATH_MAX - 1);
	pf->path[PLUA_PATH_MAX - 1] = 0;
	return f;
}

int fclose(FILE *f)
{
	struct plua_file *pf = (struct plua_file *)f;
	if (!pf)
		return EOF;
	if (pf->console)
		return 0;			/* never close a console stream */
	hp_svc_fclose(pf->fd);
	free(pf);
	return 0;
}

int fflush(FILE *f)
{
	(void)f;				/* everything is written through */
	return 0;
}

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *f)
{
	struct plua_file *pf = (struct plua_file *)f;
	size_t n = size * nmemb;
	if (!pf || !ptr || n == 0)
		return 0;
	if (pf->console) {
		ring_write((const char *)ptr, n);
		return nmemb;
	}
	{
		unsigned got = hp_svc_fwrite((unsigned)(unsigned long)ptr, 1,
					     (unsigned)n, pf->fd);
		if (got != n) {
			pf->err = 1;
			errno = EIO;
		}
		return size ? got / size : 0;
	}
}

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *f)
{
	struct plua_file *pf = (struct plua_file *)f;
	size_t n = size * nmemb, done = 0;
	if (!pf || !ptr || n == 0)
		return 0;
	if (pf->console) {			/* stdin: no input source */
		pf->eof = 1;
		return 0;
	}
	if (pf->has_push) {			/* drain the pushback first */
		((unsigned char *)ptr)[done++] = (unsigned char)pf->push;
		pf->has_push = 0;
		if (done == n)
			return size ? done / size : 0;
	}
	{
		unsigned got = hp_svc_fread((unsigned)(unsigned long)
					    ((unsigned char *)ptr + done), 1,
					    (unsigned)(n - done), pf->fd);
		done += got;
		if (done < n)
			pf->eof = 1;
		if (done == 0 && n != 0)
			errno = EIO;
		return size ? done / size : 0;
	}
}

int fseek(FILE *f, long offset, int whence)
{
	struct plua_file *pf = (struct plua_file *)f;
	if (!pf || pf->console)
		return -1;
	pf->eof = 0;
	pf->has_push = 0;
	hp_svc_fseek(pf->fd, (unsigned)offset, (unsigned)whence);
	return 0;
}

long ftell(FILE *f)
{
	struct plua_file *pf = (struct plua_file *)f;
	if (!pf || pf->console)
		return -1;
	return (long)hp_svc_ftell(pf->fd);
}

int feof(FILE *f)   { return f ? ((struct plua_file *)f)->eof : 1; }
int ferror(FILE *f) { return f ? ((struct plua_file *)f)->err : 1; }
void clearerr(FILE *f)
{
	if (f) {
		((struct plua_file *)f)->eof = 0;
		((struct plua_file *)f)->err = 0;
	}
}

int fgetc(FILE *f)
{
	struct plua_file *pf = (struct plua_file *)f;
	unsigned char c;
	if (!pf)
		return EOF;
	if (pf->has_push) {
		pf->has_push = 0;
		return pf->push;
	}
	if (fread(&c, 1, 1, f) != 1)
		return EOF;
	return (int)c;
}

int getc(FILE *f) { return fgetc(f); }

int ungetc(int c, FILE *f)
{
	struct plua_file *pf = (struct plua_file *)f;
	if (!pf || c == EOF)
		return EOF;
	pf->push = c & 0xff;
	pf->has_push = 1;
	pf->eof = 0;
	return c & 0xff;
}

int fputc(int c, FILE *f)
{
	unsigned char b = (unsigned char)c;
	if (fwrite(&b, 1, 1, f) != 1)
		return EOF;
	return (int)b;
}

int putc(int c, FILE *f) { return fputc(c, f); }
int putchar(int c)       { return fputc(c, stdout); }

char *fgets(char *buf, int size, FILE *f)
{
	int i = 0;
	if (!buf || size <= 1 || !f)
		return 0;			/* stdin has no source: EOF */
	while (i < size - 1) {
		int c = fgetc(f);
		if (c == EOF)
			break;
		buf[i++] = (char)c;
		if (c == '\n')
			break;
	}
	if (i == 0)
		return 0;
	buf[i] = 0;
	return buf;
}

int fputs(const char *s, FILE *f)
{
	size_t n = strlen(s);
	return fwrite(s, 1, n, f) == n ? 0 : EOF;
}

int puts(const char *s)
{
	if (fputs(s, stdout) == EOF)
		return EOF;
	return fputc('\n', stdout);
}

/* ---- formatted output --------------------------------------------------
 * The engine is primetcc's vsnprintf (plua_rt.o).  printf/puts live here
 * rather than in the runtime because the runtime's own versions write into
 * ITS ring, and every byte of Lua's output has to land in the one ring the
 * calculator drains.
 */
int printf(const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	int n;
	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (n < 0)
		return n;
	if ((size_t)n >= sizeof(buf))
		n = (int)sizeof(buf) - 1;
	fwrite(buf, 1, (size_t)n, stdout);
	return n;
}

int vprintf(const char *fmt, va_list ap)
{
	char buf[512];
	int n = vsnprintf(buf, sizeof(buf), fmt, ap);
	if (n < 0)
		return n;
	if ((size_t)n >= sizeof(buf))
		n = (int)sizeof(buf) - 1;
	fwrite(buf, 1, (size_t)n, stdout);
	return n;
}

/* fprintf goes through a bounded buffer: lines longer than that are
 * truncated rather than lost, which is what a 320x240 console can show
 * anyway.  Nothing in Lua produces longer single messages in practice. */
int fprintf(FILE *f, const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	int n;
	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (n < 0)
		return n;
	if ((size_t)n >= sizeof(buf))
		n = (int)sizeof(buf) - 1;
	return fwrite(buf, 1, (size_t)n, f) == (size_t)n ? n : -1;
}

int vfprintf(FILE *f, const char *fmt, va_list ap)
{
	char buf[512];
	int n = vsnprintf(buf, sizeof(buf), fmt, ap);
	if (n < 0)
		return n;
	if ((size_t)n >= sizeof(buf))
		n = (int)sizeof(buf) - 1;
	return fwrite(buf, 1, (size_t)n, f) == (size_t)n ? n : -1;
}

/* ---- misc ------------------------------------------------------------- */
int remove(const char *path)
{
	unsigned short p16[260];
	char full[PLUA_HOME_MAX + PLUA_PATH_MAX];
	if (!path) {
		errno = EINVAL;
		return -1;
	}
	widen(resolve_path(path, full, (int)sizeof(full)), p16, 260);
	hp_svc_remove((unsigned)(unsigned long)p16);
	return 0;
}

int rename(const char *oldname, const char *newname)
{
	(void)oldname; (void)newname;
	return -1;				/* no firmware rename in this port */
}

void perror(const char *s)
{
	if (s && *s)
		printf("%s: ", s);
	printf("%s\n", strerror(errno));
}

int setvbuf(FILE *f, char *buf, int mode, size_t size)
{
	(void)f; (void)buf; (void)mode; (void)size;
	return 0;				/* streams are unbuffered */
}

/* os.tmpname()/io.tmpfile(): the calculator has no /tmp, so both live in the
 * app folder under one fixed name (TMP_MAX == 1). */
char *tmpnam(char *buf)
{
	static char name[] = "C:\\DATA\\primeLua.hpappdir\\plua_tmp.lua";
	if (buf) {
		strcpy(buf, name);
		return buf;
	}
	return name;
}

FILE *tmpfile(void)
{
	return fopen("C:\\DATA\\primeLua.hpappdir\\plua_tmp.lua", "wb+");
}
