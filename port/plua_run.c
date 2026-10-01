/*
 * plua_run.c -- start a run on a firmware thread, publish its state, let the
 * launcher stream the output and stop a runaway script.
 *
 * The mechanism is PureDOOM's (see plua_run.h): the entry creates a thread
 * with the firmware's own svc 0x10000, hands the launcher the run state and
 * returns immediately, so the launcher's debug call comes back while the
 * program is still running.  DOOM does exactly this -- main() creates
 * doom_main_thread with a 512 KB stack and returns the log structure address to
 * main.py -- and it is the only way to have output DURING a run: the debug
 * interface's "call" otherwise blocks until the callee returns.
 *
 * Two things are primeLua's own:
 *
 *   * the run state (magic "PLUARUN"), which replaces DOOM's log structure and
 *     tells the launcher where the ring is, where this run's output starts,
 *     whether the run is over, and whether the run is on a thread at all;
 *   * the ABORT path.  DOOM's game polls its own input; a Lua script is
 *     arbitrary code, so the check rides on Lua's own debug hook: the launcher
 *     sets `abort` (a key press on the calculator), and every N VM instructions
 *     luaL_error("interrupted") unwinds the script.  That is the "press a key
 *     to interrupt" the README listed as future work.
 */
#include <stddef.h>

#include <stdio.h>		/* struct plua_ring, plua_ring_puts */

#include "plua_port.h"
#include "plua_input.h"		/* plua_input_remove(): hand the slot back */
#include "plua_run.h"

/* the thread's stack, in bytes.  DOOM asks for 512 KB on this machine and gets
 * it; Lua's recursive-descent parser and its C-stack use (metamethods,
 * string.format, deep pcall) want far less -- the old build malloc'd 128 KB
 * from the firmware heap for exactly this -- so a thread stack replaces that
 * malloc AND the heap churn that came with it. */
#define PLUA_THREAD_STACK	(512 * 1024)

unsigned hp_svc_create_thread(unsigned fn, unsigned arg, unsigned stack);
/* set_thread_priority(handle, prio): BOTH arguments, in that order -- see the
 * comment at the call site for what getting it wrong does on the device */
void hp_svc_set_thread_priority(unsigned handle, unsigned prio);

/* the thread entry (plua_svc.s): aligns the firmware's stack and calls
 * plua_thread_main() */
void plua_thread_entry(void);

struct plua_run plua_run __attribute__((used)) = {
	{ 'P', 'L', 'U', 'A', 'R', 'U', 'N', 0 },	/* magic */
	PLUA_RUN_IDLE,		/* state */
	0,			/* mode */
	-1,			/* exit_code */
	0,			/* abort */
	1,			/* policy: run the program on a firmware thread */
	/* interrupt: arm the abort hook (see plua_run.h).  OFF by default, and the
	 * reason is measured, not aesthetic: with a Lua hook armed the VM calls
	 * luaG_traceexec before EVERY instruction, and on this interpreter that more
	 * than doubles the work -- fib(24) in the emulator: 25,009,938 instructions
	 * with the hook off, 52,083,122 with it on (+108%).  On the device that is the
	 * difference between 3 s and 5 s for fib(30).
	 *
	 * The launcher arms it (writes 1 here) when the user asks for a full
	 * interruptible session: an empty file named "interrupt" in the app folder.
	 * Without it ON/ESC still stop a script that PRINTS, DRAWS, SLEEPS or waits
	 * for a key -- primeLua's own bindings check the flag (plua_hp.c) -- but a
	 * pure numeric loop runs to the end, at full speed. */
	0,			/* interrupt */
	0,			/* started */
	0,			/* ring */
	0,			/* ring_start */
	0			/* cfg */
};

/* The policy word is read once at the start of a run: 1 (the default) means
 * "run it on a thread and let the launcher stream it".  The launcher writes 0
 * to get the old behaviour back (marker file "nothread"), which is the escape
 * hatch if a future firmware dislikes the thread. */
int plua_run_policy(void)
{
	return plua_run.policy;
}

int plua_run_interrupt(void)
{
	return plua_run.interrupt;
}

unsigned plua_run_thread_start(unsigned cfg)
{
	struct plua_run *r = &plua_run;

	r->abort = 0;
	r->exit_code = -1;
	r->cfg = cfg;
	r->ring = (unsigned)(unsigned long)&plua_ring;
	/* WHERE THIS RUN'S OUTPUT STARTS.  The ring survives between runs (the
	 * image stays resident), so the launcher must be told where "now" is
	 * instead of guessing from a count taken before the call. */
	r->ring_start = plua_ring.count;
	r->started++;

	if (!r->policy)
		return 0;		/* inline: the caller runs plua_main() */

	r->mode = 1;
	r->state = PLUA_RUN_ACTIVE;

	/* DOOM'S TWO CALLS, ARGUMENT FOR ARGUMENT.  Both were got wrong in the
	 * first version of this file and every run rebooted the calculator: the
	 * firmware was being asked to reprioritise "thread #60".
	 *
	 *     create_thread(fn, 0, 0x80000)
	 *         r0 = the thread body, r1 = 0, r2 = the stack size in bytes.
	 *         r1 is NOT a user argument: DOOM passes 0 and its thread reads
	 *         its state from a global.  primeLua does the same -- the launch
	 *         config is in this run state (r->cfg) and the thread reads it.
	 *     set_thread_priority(handle, 60)
	 *         r0 = the handle create_thread RETURNED (still in r0, untouched
	 *         between the two calls), r1 = 60.  The priority is the SECOND
	 *         argument, not the first.
	 *
	 * The v1 code passed r1 = &config to the first and (60, <garbage>) to the
	 * second.  Every line below is checked against puredoom.elf's main(); do
	 * not "tidy" it without re-reading that. */
	{
		unsigned th = hp_svc_create_thread(
			(unsigned)(unsigned long)plua_thread_entry, 0,
			PLUA_THREAD_STACK);
		hp_svc_set_thread_priority(th, 60);
		r->thread = (int)th;
	}
	return (unsigned)(unsigned long)r;
}

void plua_run_inline_begin(unsigned cfg)
{
	struct plua_run *r = &plua_run;

	r->abort = 0;
	r->exit_code = -1;
	r->cfg = cfg;
	r->ring = (unsigned)(unsigned long)&plua_ring;
	r->ring_start = plua_ring.count;
	r->started++;
	r->mode = 0;
	r->state = PLUA_RUN_ACTIVE;
}

void plua_run_finish(int status)
{
	struct plua_run *r = &plua_run;

	/* HAND THE FIRMWARE'S INPUT BACK, FROM INSIDE THE IMAGE.
	 *
	 * A script that exits without key.remove() (or dies) leaves the OS's
	 * get_event slot pointing at the hook in this image.  The launcher has a
	 * safety net for that (calc/main.py: stand_down_hook + restore_hook_slot),
	 * but it writes through the debug channel one 32-bit store per call and
	 * cannot do the cache maintenance this side can -- and the image is the
	 * one that owns the hook, so it is also the one that should clean up.
	 * Both sides agreeing is the point: whichever runs first, the other finds
	 * nothing to do (plua_input_remove is a no-op when the slot is not ours).
	 *
	 * This is safe on the thread that ran the interpreter: no other thread of
	 * ours is alive, and the OS's input thread only ever reads the slot. */
	plua_input_remove();

	r->exit_code = status;
	r->abort = 0;
	r->state = PLUA_RUN_DONE;	/* last: the launcher polls this word */
}

/* THE THREAD BODY.  Started by plua_run_thread_start() with r0 = 0 (DOOM's own
 * convention -- see the call site), so the launch config comes from the run
 * state and this function takes no arguments.
 *
 * Runs on the firmware's thread stack: the firmware gives this thread 512 KB,
 * so the interpreter does NOT malloc a program stack here (see plua_run.h);
 * plua_begin()/plua_main() are the same two calls the old entry made. */
void plua_thread_main(void)
{
	unsigned cfg = plua_run.cfg;
	int status;

	/* FIRST, BEFORE ANYTHING ELSE: say in the durable log that the thread
	 * body was reached.  plua_diag() needs the log path, which plua_begin()
	 * normally installs, so take it from the config here -- a device that
	 * reboots inside the interpreter's startup then still shows how far it
	 * got (plua.log survives a reset; the console and the ring do not). */
	{
		const struct plua_config *c =
			(const struct plua_config *)(unsigned long)cfg;
		if (c && c->magic == PLUA_CFG_MAGIC) {
			plua_set_logpath(c->logpath);
			plua_set_homedir(c->homedir);
		}
	}
	plua_diag("thread: entered\n");
	plua_diag_hex("thread: cfg=", cfg);

	plua_begin(cfg, 0);
	/* the program's stack is the THREAD's stack in this mode (the firmware
	 * allocates it), so this is the line that says where Lua's C stack lives
	 * -- the inline path logs the malloc'd one instead (plua_stack_alloc). */
	plua_diag_hex("stack: thread bytes=", PLUA_THREAD_STACK);
	/* which way this run went -- the line a device log is read for when the
	 * firmware turns out to dislike threads ("[stream] mode=" in the launcher's
	 * log is the other half of it) */
	plua_diag_hex("run mode=", (unsigned)plua_run.mode);
	plua_diag_hex("thread handle=", (unsigned)plua_run.thread);
	status = plua_main();
	plua_diag("thread: lua returned\n");
	plua_log_ret(status);		/* RT_RET:<n> into the ring */
	plua_run_finish(status);
	plua_diag("thread: done\n");
}

int plua_run_abort_wanted(void)
{
	return plua_run.abort != 0;
}

void plua_run_abort_taken(void)
{
	plua_run.abort = 0;
}
