/*
 * plua_input.h -- primeLua's keyboard/touch backend (see plua_input.c).
 *
 * The Lua-visible API in port/plua_hp.c is unchanged by the rewrite:
 * key.install/remove/hooked/event/getkey/gettouch/sleep.  What changed is HOW
 * it works: the mechanism is now PureDOOM's (hp_puredoom.c: install_input_hack
 * / my_get_event_hook), not the home-grown trampoline this file used to carry.
 */
#ifndef PLUA_INPUT_H
#define PLUA_INPUT_H

/* The one instance, in this image's .data.  Its address is published in
 * plua.log ("in_state=") AND returned to the launcher by plua_entry, so the
 * launcher can disarm the hook and put the firmware's slot bytes back WITHOUT
 * scanning lua.elf -- that scan does not work on the real calculator's file
 * API (see port/plua_main.c). */
struct plua_in_state;
extern struct plua_in_state plua_in_state;

/* one queued input item, as key.event()/key.getkey()/key.gettouch() see it */
struct plua_item {
	unsigned char type;		/* 1 = key, 2 = touch */
	unsigned char down;		/* key: 1 press, 0 release */
	unsigned char action;		/* touch: 1 press, 2 move, 8 release */
	unsigned char pad;
	unsigned short key;		/* key: device scan code (see hp_input.h) */
	unsigned short x, y;		/* touch: screen pixels */
	short dx, dy;			/* touch move: pixels since the last move */
};

/* Is intercepting the firmware's input path allowed at all?  The launcher sets
 * this word from the "noinputhook" marker file.  Interception is ON by default
 * -- the mechanism is the one DOOM runs on this machine -- and the marker is
 * the escape hatch for a session that wants the firmware's input path left
 * completely alone. */
#define PLUA_IN_ALLOW_HOOK_OFF	0
#define PLUA_IN_ALLOW_HOOK_ON	1

/* arm the hook: capture the firmware's slot bytes, patch the slot, listen.
 * Returns 1 on success, 0 when the hook is not allowed (see above) or the slot
 * cannot be patched.  Idempotent. */
int plua_input_install(void);

/* disarm and put the firmware's own bytes back, VERIFIED (read back and
 * compare).  Safe to call when not installed. */
void plua_input_remove(void);

/* 1 while a program is listening AND the slot really holds our patch. */
int plua_input_hooked(void);

/* THE HOOK ENTRY.  Reached through the patched slot by the OS's input thread,
 * with r0 = the event buffer the OS passed to get_event, and entered by a
 * BRANCH (ldr pc, [pc, #-4]) -- so it returns with the LR it was called with,
 * exactly like DOOM's my_get_event_hook. */
void plua_input_hook(void *ev);

/* take one item off the ring: 1 = filled in, 0 = empty. */
int plua_input_pop(struct plua_item *out);

/* diagnostics (key.hookstats()): what the input path has seen and whether the
 * slot restore worked.  Index order is documented in plua_hp.c. */
int plua_input_stat(int which);

/* ---------------------------------------------------------------------------
 * THE DEVICE KEY IDS -- this is primeLua's own copy of a hardware table.
 *
 * These are the firmware's physical scan codes: what the events carry and what
 * key.getkey() returns.  They are NOT the PPL GETKEY numbers (registering those
 * once made the RIGHT arrow quit a program, because device 0x04 is what GETKEY
 * calls ESC).
 *
 * The table was verified on the machine against the full scan-code matrix
 * (SCANCODE(row,col) -> device id):
 *   (0,0)=on 0x83   (1,0)=eex 0x50   (1,1)=0 0x30   (1,2)=divide 0x54
 *   (1,3)=mul 0x58  (1,4)=minus 0xB7 (1,5)=add 0xB9  (1,6)=space 0x20
 *   (1,7)=left 0x02 (2,0)=backspace 0x0C (2,1)=define 0x44 (2,2)=units 0x43
 *   (2,3)=alpha 0xB6 (2,4)=ab/c 0x45 (2,5)=vars 0x41 (2,6)=sin 0x47
 *   (2,7)=toolbox 0x42 (3,0)=+/- 0x4D (3,1)=square 0x4C (3,2)=log 0x4B
 *   (3,3)=ln 0x4A (3,4)=tan 0x49 (3,5)=cos 0x48 (3,6)=shift 0x8B
 *   (3,7)=pow 0x46 (4,0)=num 0xB3 (4,1)=plot 0xB2 (4,2)=symb 0x91
 *   (4,3)=home (4,4)=apps 0xB1 (4,5)=down 0x05 (4,6)=esc 0x01
 *   (4,7)=bracket 0x4E (5,0)=9 0x53 (5,1)=cas 0xB5 (5,2)=menu 0x93
 *   (5,3)=view 0xB4 (5,4)=up 0x03 (5,5)=1 0x59 (5,6)=period 0xB8
 *   (5,7)=help 0x95 (6,0)=comma 0x4F (6,1)=8 0x52 (6,2)=7/Q 0x51
 *   (6,3)=6 0x57 (6,4)=5 0x56 (6,5)=4 0x55 (6,6)=3 0x33 (6,7)=2 0x5A
 *   (7,0)=enter 0x0D (7,1)=right 0x04
 * NOTE: the letter keys ARE the number/symbol keys in alpha mode, e.g. 'q' is
 * the 7/Q key = 0x51.
 * ------------------------------------------------------------------------- */
#define HP_KEY_ESC       0x01
#define HP_KEY_LEFT      0x02
#define HP_KEY_UP        0x03
#define HP_KEY_RIGHT     0x04
#define HP_KEY_DOWN      0x05
#define HP_KEY_BACKSPACE 0x0C
#define HP_KEY_ENTER     0x0D
#define HP_KEY_SPACE     0x20
#define HP_KEY_ON        0x83   /* ON/Cancel */
#define HP_KEY_SHIFT     0x8B
#define HP_KEY_Q         0x51   /* 7/Q key: 'q' in alpha mode */
#define HP_KEY_F1        0x91   /* symb */
#define HP_KEY_F2        0xB2   /* plot */
#define HP_KEY_F3        0xB3   /* num  */
#define HP_KEY_F4        0xB4   /* view */
#define HP_KEY_F5        0xB5   /* cas  */
#define HP_KEY_F6        0x93   /* menu */
#define HP_KEY_APPS      0xB1
#define HP_KEY_HELP      0x95
#define HP_KEY_ALPHA     0xB6
#define HP_KEY_PLUSMINUS 0x4D
#define HP_KEY_X2        0x4C

#endif /* PLUA_INPUT_H */
