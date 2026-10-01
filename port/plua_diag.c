/*
 * plua_diag.c -- durable diagnostics for primeLua.
 *
 * Why this exists: the interesting failures happen on the calculator and only
 * there (memory pressure, firmware behaviour, a reboot).  A reboot wipes the
 * console AND stops MicroPython from flushing anything it had open, so the
 * interpreter's own output ring dies with it.  Every diagnostic line is
 * therefore appended to a FILE and the file is closed before returning, which
 * is what makes the trail survive an instant reset.
 *
 * Writing happens through the firmware's wide-char file API (the same one the
 * launcher and primetcc use), with a read-append-rewrite cycle rather than
 * "ab" mode: the firmware's append support is not something we can rely on,
 * and the file is capped so it cannot grow without bound.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "plua_port.h"

unsigned hp_svc_fopen(unsigned path16, unsigned mode16);
unsigned hp_svc_fclose(unsigned fd);
unsigned hp_svc_fread(unsigned buf, unsigned nmemb, unsigned size, unsigned fd);
unsigned hp_svc_fwrite(unsigned buf, unsigned nmemb, unsigned size, unsigned fd);

#define DIAG_CAP	4096

static char diag_path[192];		/* ASCII device path, or empty */
static char diag_buf[DIAG_CAP];

static void widen(const char *s, unsigned short *out, int cap)
{
	int i;
	for (i = 0; s[i] && i < cap - 1; i++)
		out[i] = (unsigned short)(unsigned char)s[i];
	out[i] = 0;
}

void plua_set_logpath(const char *path)
{
	int i;
	if (!path) {
		diag_path[0] = 0;
		return;
	}
	for (i = 0; path[i] && i < (int)sizeof(diag_path) - 1; i++)
		diag_path[i] = path[i];
	diag_path[i] = 0;
}

void plua_diag(const char *msg)
{
	unsigned short p16[200], m16[8];
	unsigned have = 0, n = 0, i, drop, fd;

	if (diag_path[0] == 0 || msg == 0)
		return;

	/* read what is there */
	widen(diag_path, p16, 200);
	widen("rb", m16, 8);
	fd = hp_svc_fopen((unsigned)(unsigned long)p16,
			  (unsigned)(unsigned long)m16);
	if (fd) {
		have = hp_svc_fread((unsigned)(unsigned long)diag_buf, 1,
				    DIAG_CAP - 256, fd);
		if (have > DIAG_CAP - 256)
			have = DIAG_CAP - 256;
		hp_svc_fclose(fd);
	}
	while (msg[n])
		n++;
	drop = 0;
	if (have + n > DIAG_CAP - 256)
		drop = have + n - (DIAG_CAP - 256);
	if (drop >= have) {
		have = 0;
	} else {
		for (i = 0; i < have - drop; i++)
			diag_buf[i] = diag_buf[i + drop];
		have -= drop;
	}
	for (i = 0; i < n; i++)
		diag_buf[have + i] = msg[i];
	have += n;

	widen("wb+", m16, 8);
	fd = hp_svc_fopen((unsigned)(unsigned long)p16,
			  (unsigned)(unsigned long)m16);
	if (fd) {
		hp_svc_fwrite((unsigned)(unsigned long)diag_buf, 1, have, fd);
		hp_svc_fclose(fd);	/* closed == durable across a reset */
	}
}

void plua_diag_hex(const char *tag, unsigned v)
{
	char line[96];
	int i = 0, j;
	while (tag && tag[i] && i < 72)
		line[i] = tag[i], i++;
	for (j = 28; j >= 0; j -= 4)
		line[i++] = "0123456789abcdef"[(v >> j) & 0xF];
	line[i++] = '\n';
	line[i] = 0;
	plua_diag(line);
}

/*
 * The hardware libraries (hp_input.c) log through diag_log().  primetcc's
 * version writes C:\DATA\primetcc.hpappdir\crash.log; primeLua redirects the
 * symbol here (-Ddiag_log=hp_diag_log_redirect in the Makefile... see below)
 * so their diagnostics land in primeLua's own log with the rest.
 */
void diag_log(const char *s)
{
	plua_diag("hw: ");
	plua_diag(s);
}
