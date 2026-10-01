/*
 * plua_touch_hook.h -- how the input hook (shared source) reaches primeLua's
 * touch recorder.
 *
 * The hook lives in primetcc's rt/hp_input_svc.c, which is compiled by BOTH
 * projects from the same file.  Nothing there may reference a primeLua symbol
 * directly (a primetcc build does not have one), and nothing there may use a
 * preprocessor conditional on a macro that is only defined on a command line
 * (the hook entry is hand-written assembly either way).  So the hook always
 * calls through a THUNK SELECTOR -- a pointer variable read through a literal
 * pool -- and this header is what primeLua includes to declare it and to
 * define HP_INPUT_RAW_TOUCH:
 *
 *   primeLua build:  the -I path reaches this header, the selector exists, and
 *                    port/plua_hp.c points it at plua_touch_thunk(); the hook
 *                    records every raw touch frame via port/plua_touch.c.
 *   primetcc build:  the header is not on the include path (this file lives in
 *                    primeLua), HP_INPUT_RAW_TOUCH is never defined, the pool
 *                    word holds 0 and the entry runs hp_input_hook_default()
 *                    -- byte-for-byte the behaviour it always had.
 */
#ifndef PLUA_TOUCH_HOOK_H
#define PLUA_TOUCH_HOOK_H

#define HP_INPUT_RAW_TOUCH 1

#include "plua_touch.h"

struct hp_input_state;

/* defined by the hook's translation unit (hp_input_svc.c); the naked entry
 * reads it through a literal pool, so it lives at a fixed address in the
 * loaded image and can simply be assigned. */
extern void *__plua_hook_thunk_sel;

/* the thunk primeLua installs: hands one raw frame to plua_touch_feed().  It
 * lives in port/plua_hp.c (it needs gfx/font-free, allocation-free code only,
 * which plua_touch.c is). */
void plua_touch_thunk(struct hp_input_state *st, void *ev);

#endif /* PLUA_TOUCH_HOOK_H */
