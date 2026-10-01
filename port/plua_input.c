/*
 * plua_input.c -- primeLua's keyboard/touch backend, the DOOM way.
 *
 * WHERE THIS CODE COMES FROM
 * ---------------------------
 * This is a from-scratch implementation of the mechanism PureDOOM uses on this
 * same calculator (PureDOOM-master/hp_puredoom.c: install_input_hack +
 * my_get_event_hook + aggressive_memcpy), reconstructed from puredoom.elf and
 * re-verified here.  It replaces two earlier attempts:
 *
 *   1. primetcc's shared rt/hp_input.c + rt/hp_input_svc.c;
 *   2. primeLua's own "naked entry + hand-built trampoline" version.
 *
 * Both worked in the emulator and killed the machine's input path on the real
 * calculator: after a script that installed the hook, a touch left keys and
 * touch dead until a reset (bisect_hookentry.lua reproduces it, and the device
 * logs in README §7 record it).  DOOM does the same job -- it patches the same
 * firmware slot, from the same kind of loaded image -- and its input works, so
 * the mechanism below is DOOM's, not ours:
 *
 *   * the hook is an ordinary C function, entered by a BRANCH from the patched
 *     slot (ldr pc, [pc, #-4]) and returning with the LR it was called with;
 *   * it does NOT blx into a copy of the firmware's stub.  It calls primeLua's
 *     own svc 0x1003F wrapper instead (plua_svc.s: hp_svc_get_event, three
 *     instructions: push {r0} / push {lr} / svc 0x1003F -- the kernel takes the
 *     SVC's return address off the stack, so this returns straight to the hook);
 *   * the patch is EIGHT bytes: the trap word and the hook address.  The
 *     firmware's own stub is left otherwise untouched;
 *   * the write is done with DOOM's aggressive_memcpy ritual: IRQs masked, the
 *     translation table base poisoned to quiesce the core, the write, then
 *     clean+invalidate D-cache / invalidate I-cache / drain the write buffer,
 *     then the TTBR put back.  This is the part the failed versions got wrong
 *     in both directions (a whole-D-cache invalidate WITHOUT the quiesce reset
 *     the machine -- see README §7);
 *   * DECODING HAPPENS HERE, on the OS's input thread, exactly as DOOM does it.
 *     The earlier primeLua version copied the raw 128-byte event into a ring
 *     and parsed it later on the Lua thread, because "merely passing through
 *     our code" was believed to be fatal.  DOOM's hook does real work --
 *     loops, global writes, C calls -- on that thread and the machine is fine,
 *     so that belief was wrong, and the split it justified is gone.
 *
 * WHAT IS PRIMELUA'S OWN
 * ----------------------
 * The queue the Lua side reads (key.event/getkey/gettouch), the counters
 * key.hookstats() reports, the state layout the launcher uses to stand the hook
 * down after a run, and the touch bookkeeping DOOM keeps for its mouse
 * emulation (DOOM turns touch into relative mouse motion; primeLua hands the
 * contact to Lua as absolute pixels plus that motion).
 *
 * THE STATE LAYOUT IS A CONTRACT WITH calc/main.py
 * -------------------------------------------------
 *     0   magic[8]        "PRIMEIN" -- the launcher finds the state by it
 *     8   hook_fn         absolute address of our entry (R_ARM_RELATIVE)
 *     12  saved[4]        the firmware's own slot bytes (16), for restore
 *     28  saved_valid     1 once a trustworthy original was captured
 *     32  armed           1 while a Lua program is listening
 *     36  allow_hook      the launcher's policy word (0 = leave input alone)
 *     40+ primeLua's own counters (the launcher does not read past allow_hook)
 * Changing the order means changing calc/main.py's ST_* constants in the same
 * commit.
 */

#include <stddef.h>

#include "plua_input.h"

/* firmware get_event API slot: the first word of its stub (see README §7) */
#define PLUA_FW_EVENT_SLOT	0x307FBFA0u
#define PLUA_SLOT_TRAP		0xE51FF004u	/* ldr pc, [pc, #-4] */

/* The firmware's own stub, as this machine has it (already popped once by the
 * kernel: the SVC's return address comes off the stack).  Used when the slot
 * holds OUR trap but no trustworthy original was ever captured -- a session
 * that died before it could restore.  Writing these back gives a WORKING
 * get_event, where NOPs would break input for every app until the next reboot. */
#define PLUA_STUB0	0xE52D0004u	/* push {r0} */
#define PLUA_STUB1	0xE52DE004u	/* push {lr} */
#define PLUA_STUB2	0xEF01003Fu	/* svc  0x1003F */
#define PLUA_STUB3	0xE49D0004u	/* pop  {r0} */

/* the raw get_event frame, measured on the device (primetcc rt/hp_input.h) */
#define EV_TYPE		4
#define EV_TOUCH_N	24
#define EV_ITEM		28
#define EV_ITEM_SZ	12
#define EV_KEY_FLAG	28
#define EV_KEY_ID	34
#define EV_TYPE_KEY	0x00100010u
#define EV_TYPE_TOUCH	15u
#define EV_KEY_PRESS	16u
#define EV_KEY_RELEASE	0x100000u
#define EV_ACT_PRESS	1u
#define EV_ACT_MOVE	2u
#define EV_ACT_RELEASE	8u

/* DOOM's touch jitter filter: a move whose x and y are both within one pixel
 * of the last accepted position is noise, not a gesture. */
#define EV_MOVE_SLOP	1

#define PLUA_Q_SIZE	64

/* the shared state: field order is the launcher's contract (see above) */
struct plua_in_state {
	char magic[8];
	unsigned hook_fn;
	unsigned saved[4];
	int saved_valid;
	volatile int armed;
	volatile int allow_hook;
	/* primeLua's own counters (the launcher does not touch these) */
	volatile int hook_calls;
	volatile int skipped;
	volatile int bad_type;
	volatile int key_press, key_release;
	volatile int touch_press, touch_release, touch_moves, touch_jitter;
	volatile int touches;
	volatile int q_full;
	volatile int install_count, remove_count;
	volatile int restore_writes, restore_verified, restore_failed;
	/* the live contact, DOOM's g_is_touching / g_last_touch_x / g_last_touch_y:
	 * the last ACCEPTED position, which is also what a move's delta is
	 * measured against. */
	volatile int touching;
	volatile int last_x, last_y;
	/* ---- the item ring (single producer: the OS input thread) ---------- */
	volatile int head, tail;
	struct plua_item q[PLUA_Q_SIZE];
};

/* THE LAYOUT IS A CONTRACT WITH calc/main.py.  These asserts turn a silent
 * drift into a build error -- the launcher writes armed/allow_hook and reads
 * saved[]/saved_valid by hard-coded offset, and a mismatch would mean writing
 * into the wrong field of a live input path. */
_Static_assert(offsetof(struct plua_in_state, saved) == 12,
	       "saved[] moved: calc/main.py's ST_SAVED is 12");
_Static_assert(offsetof(struct plua_in_state, saved_valid) == 28,
	       "saved_valid moved: calc/main.py's ST_SAVED_VALID is 28");
_Static_assert(offsetof(struct plua_in_state, armed) == 32,
	       "armed moved: calc/main.py's ST_ARMED is 32");
_Static_assert(offsetof(struct plua_in_state, allow_hook) == 36,
	       "allow_hook moved: calc/main.py's ST_ALLOW_HOOK is 36");

struct plua_in_state plua_in_state __attribute__((used)) = {
	/* designated initializers: the fields this file cares about are named,
	 * everything else is zero -- a positional list here had one value too
	 * many and silently shifted the item ring's initializer (GCC said so;
	 * the old version of this file got away with it only because the struct
	 * was smaller).  allow_hook is the one non-zero default: interception is
	 * allowed unless the launcher's policy word says otherwise. */
	.magic = { 'P', 'R', 'I', 'M', 'E', 'I', 'N', 0 },
	.allow_hook = PLUA_IN_ALLOW_HOOK_ON,
	.q = { { 0 } },
};

/* our own svc 0x1003F wrapper (plua_svc.s).  Calling it from the hook is the
 * whole trick: the kernel's return address comes off the stack the wrapper
 * pushes, so this behaves like an ordinary call that fills `ev`. */
unsigned hp_svc_get_event(unsigned ev);

/* ---------------------------------------------------------------------------
 * the slot: write it the way DOOM writes it.
 *
 * Every CP15 operation here is copied from puredoom.elf's aggressive_memcpy,
 * including the TTBR poison: on this core a write to the firmware's code
 * followed by a whole-D-cache invalidate WITHOUT it is a reset, and with it is
 * what PureDOOM has been doing on this machine all along.
 * ------------------------------------------------------------------------- */
static void slot_copy(volatile unsigned *dst, const unsigned *src, int nwords)
{
	unsigned zero = 0;

	__asm__ __volatile__(
		"mrs	r4, cpsr\n\t"
		"orr	r5, r4, #0x80\n\t"	/* mask IRQ: the patch is two words
						   and an interrupt between them
						   runs the trap half-written */
		"msr	cpsr_c, r5\n\t"
		"mrc	p15, 0, r6, c3, c0, 0\n\t"	/* save TTBR */
		"mvn	r5, #0\n\t"
		"mcr	p15, 0, r5, c3, c0, 0\n\t"	/* quiesce (DOOM's poison) */
		"1:\n\t"
		"ldr	r5, [%1], #4\n\t"
		"str	r5, [%0], #4\n\t"
		"subs	%2, %2, #1\n\t"
		"bne	1b\n\t"
		"mcr	p15, 0, %3, c7, c10, 1\n\t"	/* clean+invalidate D-cache */
		"mcr	p15, 0, %3, c7, c5, 0\n\t"	/* invalidate I-cache */
		"mcr	p15, 0, %3, c7, c10, 4\n\t"	/* drain write buffer */
		"mcr	p15, 0, r6, c3, c0, 0\n\t"	/* TTBR back */
		"msr	cpsr_c, r4\n\t"
		: "+r"(dst), "+r"(src), "+r"(nwords)
		: "r"(zero)
		: "r4", "r5", "r6", "memory");
}

static int slot_is_trap(void)
{
	return *(volatile unsigned *)PLUA_FW_EVENT_SLOT == PLUA_SLOT_TRAP;
}

static void slot_restore_defaults(struct plua_in_state *s)
{
	s->saved[0] = PLUA_STUB0;
	s->saved[1] = PLUA_STUB1;
	s->saved[2] = PLUA_STUB2;
	s->saved[3] = PLUA_STUB3;
	s->saved_valid = 1;
}

/* ---------------------------------------------------------------------------
 * the item ring (producer: the OS input thread, consumer: Lua)
 * ------------------------------------------------------------------------- */
static void push_item(struct plua_in_state *s, unsigned char type,
		      unsigned char down, unsigned char act, unsigned short key,
		      unsigned short x, unsigned short y, short dx, short dy)
{
	int h = s->head;
	int n = (h + 1) & (PLUA_Q_SIZE - 1);

	if (n == s->tail) {
		s->q_full++;
		return;				/* ring full: drop the newest */
	}
	s->q[h].type = type;
	s->q[h].down = down;
	s->q[h].action = act;
	s->q[h].key = key;
	s->q[h].x = x;
	s->q[h].y = y;
	s->q[h].dx = dx;
	s->q[h].dy = dy;
	s->head = n;
}

/* ---------------------------------------------------------------------------
 * THE HOOK ENTRY -- DOOM's my_get_event_hook, with primeLua's item ring.
 *
 * Reached through the patched slot by the OS's input thread with r0 = the event
 * buffer the OS passed to get_event.  The firmware calls get_event BLOCKING and
 * expects it to fill that buffer, so hp_svc_get_event() is called first and
 * unconditionally -- a hook that returned without doing it would leave the
 * Operating System's own input waiting forever.
 *
 * The event is only READ here, never rewritten (the "synthetic contact" trick
 * in the code this replaces edited input the firmware's drivers were still
 * consuming).
 * ------------------------------------------------------------------------- */
void plua_input_hook(void *ev)
{
	struct plua_in_state *s = &plua_in_state;
	const unsigned char *e = (const unsigned char *)ev;

	hp_svc_get_event((unsigned)(unsigned long)ev);
	s->hook_calls++;
	if (!s->armed) {
		s->skipped++;
		return;
	}

	{
		unsigned type = *(const unsigned *)(e + EV_TYPE);

		if (type == EV_TYPE_KEY) {
			unsigned flag = *(const unsigned *)(e + EV_KEY_FLAG);
			unsigned short kid =
				*(const unsigned short *)(e + EV_KEY_ID);
			int down;

			if (kid == 0)
				return;
			/* DOOM accepts exactly two flag values and ignores the
			 * rest; anything else is counted so a device surprise
			 * shows up in key.hookstats() instead of a dead keyboard */
			if (flag == EV_KEY_PRESS)
				down = 1;
			else if (flag == EV_KEY_RELEASE)
				down = 0;
			else {
				s->bad_type++;
				return;
			}
			if (down)
				s->key_press++;
			else
				s->key_release++;
			push_item(s, 1, (unsigned char)down, 0, kid, 0, 0, 0, 0);
			return;
		}

		if (type != EV_TYPE_TOUCH) {
			s->bad_type++;
			return;
		}

		{
			int n = *(const unsigned short *)(e + EV_TOUCH_N);
			int i, lim;

			s->touches++;
			if (n <= 0)
				return;
			lim = n > 8 ? 8 : n;	/* the firmware's own limit */
			for (i = 0; i < lim; i++) {
				const unsigned char *p =
					e + EV_ITEM + EV_ITEM_SZ * i;
				unsigned act = *(const unsigned *)p;
				unsigned short x, y;

				/* slot 4 is "this contact entry is live" */
				if (*(const unsigned short *)(p + 4) != 0)
					continue;
				x = *(const unsigned short *)(p + 6);
				y = *(const unsigned short *)(p + 8);

				if (act == EV_ACT_PRESS) {
					s->touching = 1;
					s->last_x = x;
					s->last_y = y;
					s->touch_press++;
					push_item(s, 2, 0, EV_ACT_PRESS, 0, x, y,
						  0, 0);
				} else if (act == EV_ACT_MOVE) {
					int dx, dy;
					if (!s->touching)
						continue;
					dx = (int)x - s->last_x;
					dy = (int)y - s->last_y;
					if (dx >= -EV_MOVE_SLOP &&
					    dx <= EV_MOVE_SLOP &&
					    dy >= -EV_MOVE_SLOP &&
					    dy <= EV_MOVE_SLOP) {
						s->touch_jitter++;
						continue;
					}
					s->last_x = x;
					s->last_y = y;
					s->touch_moves++;
					push_item(s, 2, 0, EV_ACT_MOVE, 0, x, y,
						  (short)dx, (short)dy);
				} else if (act == EV_ACT_RELEASE) {
					s->touching = 0;
					s->touch_release++;
					push_item(s, 2, 0, EV_ACT_RELEASE, 0, x, y,
						  0, 0);
				}
			}
		}
	}
}

/* ---------------------------------------------------------------------------
 * install / remove
 * ------------------------------------------------------------------------- */
int plua_input_install(void)
{
	struct plua_in_state *s = &plua_in_state;
	unsigned patch[2];
	int i;

	/* the entry address: taken from the state itself, so it is whatever the
	 * loader's R_ARM_RELATIVE pass made of it */
	if (!s->hook_fn)
		s->hook_fn = (unsigned)(unsigned long)plua_input_hook;

	if (!s->allow_hook)
		return 0;		/* policy: interception is refused */

	if (!s->saved_valid) {
		if (slot_is_trap()) {
			/* a session that died inside the hook left our trap in
			 * the slot and we never captured the original: the
			 * firmware's known stub is the only trustworthy
			 * starting point */
			slot_restore_defaults(s);
		} else {
			volatile unsigned *slot =
				(volatile unsigned *)PLUA_FW_EVENT_SLOT;
			for (i = 0; i < 4; i++)
				s->saved[i] = slot[i];
			if (s->saved[0] == PLUA_SLOT_TRAP)
				return 0;	/* raced with another hook */
			s->saved_valid = 1;
		}
	}

	/* arm BEFORE patching: the first event after the patch must already find
	 * a listener, and the ring must start empty */
	s->head = 0;
	s->tail = 0;
	s->armed = 1;
	s->install_count++;

	patch[0] = PLUA_SLOT_TRAP;
	patch[1] = s->hook_fn;
	slot_copy((volatile unsigned *)PLUA_FW_EVENT_SLOT, patch, 2);
	if (!slot_is_trap()) {
		s->armed = 0;
		return 0;			/* the patch did not take */
	}
	return 1;
}

void plua_input_remove(void)
{
	struct plua_in_state *s = &plua_in_state;
	unsigned back[4];
	volatile unsigned *slot = (volatile unsigned *)PLUA_FW_EVENT_SLOT;
	int i, ok;

	s->armed = 0;				/* stop queueing first */
	s->head = 0;
	s->tail = 0;				/* and drop whatever is queued */
	s->remove_count++;
	if (!slot_is_trap())
		return;				/* nothing of ours in the slot */
	if (!s->saved_valid || s->saved[0] == PLUA_SLOT_TRAP) {
		/* no trustworthy original: restore the firmware's own stub */
		slot_restore_defaults(s);
	}
	slot_copy(slot, s->saved, 4);
	s->restore_writes++;

	/* VERIFY: read the slot back.  A trap that survives here is the
	 * machine's input path pointing at this image -- the launcher checks
	 * the same thing after every run and logs it. */
	for (i = 0; i < 4; i++)
		back[i] = slot[i];
	ok = 1;
	for (i = 0; i < 4; i++)
		if (back[i] != s->saved[i])
			ok = 0;
	if (!ok) {
		/* one retry: a write that raced a concurrent event fetch can
		 * leave the opcode word behind, and a second pass fixes it */
		slot_copy(slot, s->saved, 4);
		for (i = 0; i < 4; i++)
			back[i] = slot[i];
		ok = 1;
		for (i = 0; i < 4; i++)
			if (back[i] != s->saved[i])
				ok = 0;
	}
	if (ok)
		s->restore_verified++;
	else
		s->restore_failed++;
}

int plua_input_hooked(void)
{
	return plua_in_state.armed && slot_is_trap();
}

int plua_input_pop(struct plua_item *out)
{
	struct plua_in_state *s = &plua_in_state;
	int t = s->tail;

	if (t == s->head)
		return 0;
	*out = s->q[t];
	s->tail = (t + 1) & (PLUA_Q_SIZE - 1);
	return 1;
}

/* Diagnostics for key.hookstats().  The order is part of the Lua API:
 *   0 armed, 1 slot-is-ours, 2 hook_calls, 3 drop_unarmed, 4 bad_type,
 *   5 key_press, 6 key_release, 7 touch_press, 8 touch_release,
 *   9 touch_moves, 10 touch_jitter, 11 q_full, 12 restore_writes,
 *   13 restore_verified, 14 restore_failed, 15 install_count,
 *   16 touch_frames, 17 touching */
int plua_input_stat(int which)
{
	struct plua_in_state *s = &plua_in_state;

	switch (which) {
	case 0: return s->armed;
	case 1: return slot_is_trap();
	case 2: return s->hook_calls;
	case 3: return s->skipped;
	case 4: return s->bad_type;
	case 5: return s->key_press;
	case 6: return s->key_release;
	case 7: return s->touch_press;
	case 8: return s->touch_release;
	case 9: return s->touch_moves;
	case 10: return s->touch_jitter;
	case 11: return s->q_full;
	case 12: return s->restore_writes;
	case 13: return s->restore_verified;
	case 14: return s->restore_failed;
	case 15: return s->install_count;
	case 16: return s->touches;
	case 17: return s->touching;
	case 18: return s->last_x;
	case 19: return s->last_y;
	}
	return -1;
}
