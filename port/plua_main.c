/*
 * plua_main.c -- primeLua's entry sequence: the calculator-side launch
 * contract, the program stack, and the call into Lua's own driver.
 *
 * The interpreter itself is upstream Lua: lua.c is compiled unchanged with
 * its main() renamed (-Dmain=plua_lua_standalone_main), so primeLua runs
 * the same argument handling, the same REPL and the same libraries as the
 * desktop `lua` binary.  This file only supplies what a calculator has to
 * do differently:
 *
 *   - no argv: the launcher passes argc/argv through struct plua_config
 *     (see plua_port.h), reached through the r0 handshake in plua_svc.s;
 *   - no exit(): exit() unwinds to the jmp_buf armed here, so the entry
 *     sequence can hand the program stack back to the firmware heap;
 *   - no 16 KB stack: Lua's recursive-descent parser needs far more, and an
 *     overflow would silently corrupt the heap instead of faulting.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>

#include "plua_port.h"
#include "plua_input.h"     /* &plua_in_state, published in plua.log */

/* upstream lua.c, compiled with -Dmain=plua_lua_standalone_main */
int plua_lua_standalone_main(int argc, char **argv);

/* ---------------------------------------------------------------------------
 * Program stack.
 *
 * The loader calls plua_entry on the debug interface's stack, which is far
 * too shallow for Lua (the parser recurses once per nesting level, and every
 * metamethod / string.format / pcall adds C frames).  A 256 KB stack is
 * malloc'd from the firmware heap for the duration of the run and given back
 * afterwards, so repeated runs do not leak -- exactly the discipline
 * primetcc's runtime arrived at after its own "calculator reboots after
 * using doubles" bug hunt.
 *
 * The frame base (r7 at entry) is parked in memory because the interpreter
 * clobbers r7, and the scratch word next to it holds the raw malloc pointer
 * that has to be freed at the end.
 * ------------------------------------------------------------------------- */
/* The interpreter's C stack.  First choice is generous, then it halves: the
 * calculator's free heap is shared with the 350 KB image and with Lua's own
 * heap, and a malloc that returns 0 used to leave the program running on the
 * debug interface's tiny stack instead -- which corrupts memory and reboots
 * the box rather than failing cleanly.  The static fallback guarantees that
 * SOMETHING is always available, so the failure mode is "small stack" and not
 * "writes through the floor". */
#define PLUA_STACK_MAX		(128 * 1024)
#define PLUA_STACK_MIN		(32 * 1024)
#define PLUA_STACK_STATIC	(24 * 1024)

static char plua_static_stack[PLUA_STACK_STATIC];

static unsigned plua_a8;		/* frame base, saved by plua_entry */
static unsigned plua_cfg;		/* struct plua_config * from r0 */

void plua_save_a8(unsigned a8)
{
	plua_a8 = a8;
}

unsigned plua_get_a8(void)
{
	return plua_a8;
}

/* r0 = scratch slot reserved by plua_entry; returns the stack top (0 = keep
 * the caller's stack, which still runs Lua, just with little headroom). */
unsigned plua_stack_alloc(unsigned *scratch)
{
	unsigned size = PLUA_STACK_MAX;
	unsigned char *p = 0;
	unsigned top;

	while (size >= PLUA_STACK_MIN) {
		p = (unsigned char *)malloc(size + 64);
		if (p)
			break;
		plua_diag_hex("stack: malloc failed at ", size);
		size >>= 1;
	}
	if (!p) {
		/* last resort: a static buffer inside the image.  Small, but it is
		 * a real stack with a known end rather than the debug stack. */
		plua_diag("stack: falling back to the static buffer\n");
		scratch[0] = 0;			/* nothing to free later */
		top = ((unsigned)plua_static_stack + 7) & ~7u;
		return top + PLUA_STACK_STATIC - 16;
	}
	plua_diag_hex("stack: bytes=", size);
	/* +64, not +32: the frame base is stashed at [top] and the allocator
	 * only guarantees 4-byte alignment, so the alignment window can push
	 * the top of the frame past the end of the block. */
	top = ((unsigned)p + 31) & ~7u;
	scratch[0] = (unsigned)p;
	return top + size;
}

void plua_stack_free(unsigned *scratch)
{
	if (scratch && scratch[0]) {
		free((void *)scratch[0]);
		scratch[0] = 0;
	}
}

void plua_begin(unsigned cfg, unsigned dbg_sp)
{
	/* The config pointer arrives in r0; the debug stack pointer (r1) is
	 * only needed for diagnostics, which primeLua leaves to the launcher. */
	(void)dbg_sp;
	plua_cfg = cfg;
	plua_console_init();
	if (cfg) {
		const struct plua_config *c = (const struct plua_config *)cfg;
		if (c->magic == PLUA_CFG_MAGIC) {
			plua_set_epoch(c->epoch);
			plua_set_logpath(c->logpath);
			plua_set_homedir(c->homedir);
			plua_diag("---- run ----\n");
			plua_diag_hex("cfg=", cfg);
			plua_diag_hex("heap_free=", c->heap_free);
			plua_diag_hex("dbg_sp=", dbg_sp);
			plua_diag_hex("argc=", (unsigned)c->argc);
			/* WHERE OUR INPUT STATE IS, said out loud.
			 *
			 * The launcher has to be able to disarm the input hook and
			 * put the firmware's get_event bytes back after a program
			 * exits -- including a program that never called
			 * key.remove().  It used to find the state itself by
			 * scanning lua.elf through the firmware file API for the
			 * "PRIMEIN" magic, and on the real calculator that scan
			 * FAILS (the file API does not walk 400+ KB reliably), so
			 * the safety net silently did nothing: the device log reads
			 *
			 *     [hook] stand-down: no PRIMEIN in the image
			 *     [hook] NOT disarmed after clock.lua
			 *
			 * With the address in plua.log -- a line the launcher reads
			 * anyway -- no scan is needed: it reads armed/saved[] and
			 * writes armed=0 through the debug channel, which is exact.
			 */
			plua_diag_hex("in_state=", (unsigned)(unsigned long)&plua_in_state);
		}
	}
}

/* "RT_RET:<n>\n" into the ring: the firmware debug interface's return value
 * is unreliable on this hardware (primetcc measured it reporting 2 whatever
 * main() returns), so the launcher parses the exit code out of the stream. */
void plua_log_ret(unsigned v)
{
	char b[20], d[10];
	int i = 0, j = 0;
	const char *p = "RT_RET:";
	while (*p)
		b[i++] = *p++;
	do {
		d[j++] = (char)('0' + v % 10);
		v /= 10;
	} while (v);
	while (j)
		b[i++] = d[--j];
	b[i++] = '\n';
	b[i] = 0;
	plua_ring_puts(b);
	plua_diag_hex("exit code=", v);
}

/* ---------------------------------------------------------------------------
 * The program: hand upstream lua.c its argv and unwind cleanly on exit().
 * ------------------------------------------------------------------------- */
static char *plua_fallback_argv[] = { (char *)"lua", 0 };

int plua_main(void)
{
	const struct plua_config *cfg =
		(const struct plua_config *)plua_cfg;
	int argc = 1;
	char **argv = plua_fallback_argv;
	int status;

	if (cfg && cfg->magic == PLUA_CFG_MAGIC && cfg->argc > 0 && cfg->argv) {
		argc = cfg->argc;
		argv = cfg->argv;
	}

	/* the API version of the interpreter that is actually running: a script
	 * that needs a newer binding than the lua.elf on the device is then
	 * obvious from the log, instead of failing as "attempt to call a nil
	 * value" */
	plua_diag("interp: " PLUA_VERSION "\n");
	plua_diag("lua: starting\n");
	if (setjmp(plua_exit_env) == 0) {
		plua_exit_armed = 1;
		status = plua_lua_standalone_main(argc, argv);
	} else {
		status = plua_exit_code;	/* exit() was called from Lua */
	}
	plua_exit_armed = 0;
	plua_diag_hex("lua: returned status=", (unsigned)status);
	return status;
}
