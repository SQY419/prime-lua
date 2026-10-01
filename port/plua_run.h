/*
 * plua_run.h -- how a run is started, watched and finished.
 *
 * WHY THIS EXISTS: STREAMING OUTPUT
 * ---------------------------------
 * primeLua used to run the interpreter on the launcher's own thread: the
 * debug interface's "call" did not return until Lua had finished, so the
 * launcher could only drain the output ring afterwards.  A program that prints
 * in a loop showed nothing at all until it exited, and a program that hung
 * showed nothing ever.
 *
 * A run therefore happens on a FIRMWARE THREAD, exactly the way PureDOOM
 * starts its game (puredoom.elf's main(): sys_create_thread(doom_main_thread,
 * 0, 0x80000) + sys_set_thread_priority(), then return).  The entry hands the
 * launcher the address of the run state below and returns at once; the
 * launcher pumps the ring while the script runs -- which is what DOOM's
 * main.py LogReader does -- and can abort a runaway script by setting `abort`.
 *
 * THE STATE IS A CONTRACT WITH calc/main.py (offsets, all 4-byte words after
 * the magic):
 *      0  magic[8]      "PLUARUN" -- how the launcher finds it
 *      8  state         0 idle, 1 running, 2 finished
 *     12  mode          1 = firmware thread, 0 = inline (no thread)
 *     16  exit_code     the interpreter's status once state == 2
 *     20  abort         the LAUNCHER writes 1: stop at the next hook check
 *     24  policy        the LAUNCHER writes 0 to run inline (default 1)
 *     28  interrupt     the LAUNCHER writes 1: install the abort hook
 *     32  started       runs this image has served
 *     36  thread        what create_thread returned (diagnostics only)
 *     40  ring          &plua_ring -- where the output goes
 *     44  ring_start    plua_ring.count when this run began
 *     48  cfg           the launch config of THIS run
 * Changing the order means changing calc/main.py's RUN_* constants in the
 * same commit.
 */
#ifndef PLUA_RUN_H
#define PLUA_RUN_H

#define PLUA_RUN_MAGIC		"PLUARUN"

#define PLUA_RUN_IDLE		0
#define PLUA_RUN_ACTIVE		1
#define PLUA_RUN_DONE		2

struct plua_run {
	char magic[8];
	volatile int state;
	volatile int mode;
	volatile int exit_code;
	volatile int abort;
	volatile int policy;
	volatile int interrupt;
	volatile int started;
	volatile int thread;	/* diagnostics: create_thread's return value */
	unsigned ring;
	unsigned ring_start;
	unsigned cfg;
};

extern struct plua_run plua_run;

/* Start the run on a firmware thread.  Returns the run state address (what the
 * entry hands back to the launcher) when a thread took the job, 0 when the
 * caller must run the program itself (policy says inline, or the firmware has
 * no thread to give us). */
unsigned plua_run_thread_start(unsigned cfg);

/* The inline path's bookkeeping: same state, mode 0.  The launcher cannot
 * stream such a run -- it only gets control back when the program is over --
 * but it must still find a sane state and exit code there. */
void plua_run_inline_begin(unsigned cfg);

/* The thread's body (plua_thread_entry in plua_svc.s calls this).  Takes no
 * argument: the config comes from the run state, exactly as DOOM's thread body
 * takes none -- see the call site in plua_run.c. */
void plua_thread_main(void);

/* state = 2, exit_code = status.  Called by both paths when the interpreter
 * has returned, BEFORE the launcher's teardown reads it. */
void plua_run_finish(int status);

/* the abort flag, as the Lua debug hook sees it (plua_hp.c) */
int plua_run_abort_wanted(void);
void plua_run_abort_taken(void);

/* the policy word, cached for the C side (plua_config has no room for it) */
int plua_run_policy(void);

/* 1 when the launcher asked for the keyboard abort to be armed.  The abort
 * check is a Lua debug hook, and a hook is VISIBLE to scripts (debug.gethook()
 * reports it), so it is only installed when somebody is there to press the key
 * -- which also keeps primeLua's output byte-for-byte identical to the desktop
 * interpreter when nothing is watching. */
int plua_run_interrupt(void);

#endif /* PLUA_RUN_H */
