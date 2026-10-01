/*
 * plua_hp.c -- the HP Prime's own hardware, exposed to Lua as libraries.
 *
 * The implementations are primetcc's proven runtime (rt/hp_gfx.c,
 * hp_input.c, hp_sys.c, hp_random.c, hp_codec.c, hp_fixmath.c), compiled
 * into lua.elf and registered here.  This file is only the seam: it turns
 * Lua values into the C arguments, and stacks back.
 *
 * NAMING: the Lua side gets the CONVENTIONAL names, with no "hp_" prefix --
 * gfx.fillrect(), key.getkey(), codec.crc32(), fx.sin().  The prefix belongs
 * to the C symbols, which have to coexist with newlib and with Lua's own
 * namespace; nobody writing a Lua script should have to know that.
 *
 * Each library is registered twice, on purpose:
 *     gfx.line(...)                 -- straight from the global table
 *     local gfx = require("gfx")    -- for scripts that prefer modules
 * so both idioms work on the calculator.
 *
 * A script that wants to draw installs nothing special: hp.gfx talks to the
 * LCD through the firmware's get_lcd API and draws into a GROB (off-screen
 * bitmap) that gfx.blit() presents in one go -- the same double-buffered
 * pattern the C demos use, without the flicker.
 */
#include <stdlib.h>
#include <time.h>          /* clock(), CLOCKS_PER_SEC for sys.clock() */
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#include "hp_gfx.h"
#include "plua_input.h"
#include "hp_sys.h"
#include "hp_random.h"
#include "hp_codec.h"
#include "hp_fixmath.h"
#include "hp_fonts.h"

#include "plua_dd.h"
#include "plua_port.h"
#include "plua_run.h"       /* the abort word the interrupt checks read */

/* ---------------------------------------------------------------------------
 * THE ABORT HOOK -- "press a key to stop it" for a script that never returns.
 *
 * While a program runs, the LAUNCHER is streaming the output ring and watching
 * the keyboard (calc/main.py: PrimeLua.stream_run).  The ON key -- the
 * calculator's cancel key, and the one a runaway script's author is going to
 * reach for -- sets the run state's abort word; this hook is where that word is
 * noticed: Lua's own debug hook, every PLUA_ABORT_INTERVAL VM instructions,
 * raising an ordinary Lua error.  The script then unwinds exactly as if it had
 * called error(), which means pcall, xpcall and to-be-closed variables behave,
 * and the exit code and the message travel back through the ring like any other
 * run.  It is primeLua's KeyboardInterrupt, and it is catchable for the same
 * reason: cleanup belongs to the script.
 *
 * WHY A COUNT HOOK AND NOT A CHECK IN THE LOOPS: primeLua does not own the
 * script's loops.  A count hook is the interpreter's own mechanism for exactly
 * this, and its cost is one C call per interval -- nothing next to the "no
 * output until it exits" problem it solves.  The interval is also the
 * worst-case latency of a key press: 20 000 VM instructions is a few
 * milliseconds even for a tight numeric loop.
 *
 * WHAT IT DOES NOT COVER: a coroutine the script creates has its own hook
 * settings and is not interruptible from here.  Blocking C calls are covered
 * separately (abort_check below), because the hook alone would leave a script
 * sitting in sys.sleep(5000) deaf to the key for five seconds.
 * ------------------------------------------------------------------------- */
#define PLUA_ABORT_INTERVAL	20000
/* how long one slice of a blocking wait may be, in ms: the worst case between
 * pressing ON and the script unwinding */
#define PLUA_ABORT_SLICE	20

static void plua_abort_hook(lua_State *L, lua_Debug *ar)
{
	(void)ar;
	if (plua_run_abort_wanted()) {
		plua_run_abort_taken();
		luaL_error(L, "interrupted");
	}
}

/* THE SAME INTERRUPT, INSIDE A BLOCKING C CALL.
 *
 * The count hook only runs between VM instructions, so a script sitting in
 * key.getkey(5000) or sys.sleep(5000) would ignore the interrupt for as long as
 * it asked to wait -- which is exactly when somebody is pressing ON.  The
 * bindings that can block therefore slice their wait and check the same flag,
 * raising the same error: to the script it is one interrupt, wherever it was.
 *
 * (A wait is sliced, not shortened: the total is unchanged unless an interrupt
 * arrives.) */
static void abort_check(lua_State *L)
{
	if (plua_run_abort_wanted()) {
		plua_run_abort_taken();
		luaL_error(L, "interrupted");
	}
}

static void sleep_checked(lua_State *L, int ms)
{
	while (ms > 0) {
		int step = ms > PLUA_ABORT_SLICE ? PLUA_ABORT_SLICE : ms;
		abort_check(L);
		hp_sys_sleep(step);
		ms -= step;
	}
	abort_check(L);
}

/* the input state (port/plua_input.c) lives in this same image, so no
 * launcher-side handshake is needed to reach it -- plua_input.h declares it. */
void hp_set_input_state(unsigned v, unsigned dbg_sp);

/* ---------------------------------------------------------------------------
 * helpers
 * ------------------------------------------------------------------------- */
static int coord(lua_State *L, int i);   /* see the definition below */

static hp_color check_color(lua_State *L, int idx)
{
	return (hp_color)(unsigned)luaL_checkinteger(L, idx);
}

static void push_color(lua_State *L, hp_color c)
{
	lua_pushinteger(L, (lua_Integer)(unsigned)c);
}

/* ---------------------------------------------------------------------------
 * gfx -- drawing
 * ------------------------------------------------------------------------- */
/* The default draw target is an off-screen bitmap the size of the screen:
 * a script draws a whole frame and gfx.blit() presents it in one copy, which
 * is what keeps it from flickering.  gfx.select(g) switches to another GROB
 * (or gfx.select() for direct-to-screen drawing). */
static hp_grob *screen_buf;

/* Our shadow of hp_gfx.c's draw target.  hp_gfx keeps it in a static we cannot
 * read (primetcc is not modified for this port), and because THIS IMAGE STAYS
 * RESIDENT between runs the target outlives a script: a run that freed the GROB
 * it had selected used to leave the next run painting into released memory.
 * NULL means "the screen itself" (gfx.select(false), and also "before
 * gfx.init()", when nothing draws at all). */
static hp_grob *cur_tgt;

/* The default back buffer, created the first time a script actually wants it.
 *
 * gfx.init() deliberately does NOT allocate it: a full-screen buffer is 307 KB
 * out of the firmware heap, and a script that draws straight to the screen
 * (gfx.select(false), which is what the graphics demo does) has no use for it.
 * primetcc's notes also record that big alloc/free patterns are the least
 * trusted part of this firmware, so the port allocates nothing it does not
 * need. */
/* The screen's pixels, as the firmware describes them (LCD struct, svc
 * 0x1008D).  hp_gfx.c keeps its own copy private, so it is read here. */
static unsigned *screen_fb(int *w, int *h)
{
	unsigned long p = hp_sys_get_lcd(), q = 0;
	unsigned *fb = 0;
	if (p) {
		q = *(unsigned long *)p;
		if (q) {
			if (w)
				*w = *(unsigned short *)(q + 2);
			if (h)
				*h = *(unsigned short *)(q + 4);
			fb = (unsigned *)(*(unsigned int *)(q + 16));
		}
	}
	return fb;
}

static hp_grob *backbuf(void)
{
	if (!screen_buf) {
		int w = hp_gfx_w(), h = hp_gfx_h();
		unsigned *fb;
		screen_buf = hp_grob_new(w, h);
		if (screen_buf) {
			/* Seed it with what is on the screen.  Starting the buffer
			 * blank would make the first gfx.blit() wipe everything a
			 * script drew before it decided to buffer. */
			fb = screen_fb(0, 0);
			if (fb) {
				hp_grob_select(screen_buf);
				hp_blit(fb, w, h, 0, 0);
			}
		}
	}
	return screen_buf;
}

static int g_init(lua_State *L)
{
	if (!hp_gfx_init()) {
		lua_pushboolean(L, 0);
		return 1;
	}
	/* ALWAYS re-point the target, even when the back buffer is already there:
	 * this image stays resident between runs (lua.slot), so the C statics --
	 * including hp_gfx.c's current draw target -- survive from the previous
	 * script.  Without this, a script that ran before this one could leave the
	 * target inside a GROB it has since freed, and the next gfx.clear() would
	 * paint into released memory.
	 *
	 * The starting target is the SCREEN: drawing is visible without a blit,
	 * and gfx.select() opts into buffering. */
	hp_grob_select(0);
	cur_tgt = 0;
	lua_pushboolean(L, 1);
	return 1;
}

static int g_width(lua_State *L)
{
	lua_pushinteger(L, hp_gfx_w());
	return 1;
}

static int g_height(lua_State *L)
{
	lua_pushinteger(L, hp_gfx_h());
	return 1;
}

/* gfx.rgb(r, g, b) -- the same packing HP_RGB uses (0xAARRGGBB, opaque) */
static int g_rgb(lua_State *L)
{
	int r = coord(L, 1);
	int g = coord(L, 2);
	int b = coord(L, 3);
	push_color(L, HP_RGB(r, g, b));
	return 1;
}

static int g_clear(lua_State *L)
{
	hp_clear(check_color(L, 1));
	return 0;
}

/* Coordinates are accepted as NUMBERS, not integers.
 *
 * MicroPython's Lua is stock Lua 5.4, where luaL_checkinteger() REJECTS a float:
 * `gfx.line(1, 2.5, 3, 4)` raises "number has no integer representation".  The
 * calculator's own scripts compute positions in floating point (`stroke.x + i *
 * seg`, midpoints, scaled values), so integer-only arguments turn ordinary
 * drawing into a runtime error.  Truncation is what every other graphics API on
 * this machine does with a fractional pixel.
 *
 * (The bug showed up as: a TI-Nspire style script driving gfx.line ->
 * "bad argument #3 to 'line' (number has no integer representation)".  That
 * script, and the ti.lua layer it required, now live outside this tree -- see
 * ../_attic/primeLua-ti/.) */
static int coord(lua_State *L, int i)
{
	return (int)luaL_checknumber(L, i);
}

static int g_pixel(lua_State *L)
{
	abort_check(L);	/* the interrupt, for free (see above) */
	hp_pixel(coord(L, 1), coord(L, 2),
		 check_color(L, 3));
	return 0;
}

static int g_getpixel(lua_State *L)
{
	abort_check(L);	/* the interrupt, for free (see above) */
	push_color(L, hp_get_pixel(coord(L, 1),
				   coord(L, 2)));
	return 1;
}

static int g_line(lua_State *L)
{
	abort_check(L);	/* the interrupt, for free (see above) */
	hp_line(coord(L, 1), coord(L, 2),
		coord(L, 3), coord(L, 4),
		check_color(L, 5));
	return 0;
}

static int g_hline(lua_State *L)
{
	abort_check(L);	/* the interrupt, for free (see above) */
	hp_hline(coord(L, 1), coord(L, 2),
		 coord(L, 3), check_color(L, 4));
	return 0;
}

static int g_vline(lua_State *L)
{
	abort_check(L);	/* the interrupt, for free (see above) */
	hp_vline(coord(L, 1), coord(L, 2),
		 coord(L, 3), check_color(L, 4));
	return 0;
}

static int g_rect(lua_State *L)
{
	abort_check(L);	/* the interrupt, for free (see above) */
	hp_rect(coord(L, 1), coord(L, 2),
		coord(L, 3), coord(L, 4),
		check_color(L, 5));
	return 0;
}

static int g_fillrect(lua_State *L)
{
	abort_check(L);	/* the interrupt, for free (see above) */
	hp_fill_rect(coord(L, 1), coord(L, 2),
		     coord(L, 3), coord(L, 4),
		     check_color(L, 5));
	return 0;
}

static int g_circle(lua_State *L)
{
	abort_check(L);	/* the interrupt, for free (see above) */
	hp_circle(coord(L, 1), coord(L, 2),
		  coord(L, 3), check_color(L, 4));
	return 0;
}

static int g_fillcircle(lua_State *L)
{
	abort_check(L);	/* the interrupt, for free (see above) */
	hp_fill_circle(coord(L, 1), coord(L, 2),
		       coord(L, 3), check_color(L, 4));
	return 0;
}

static int g_triangle(lua_State *L)
{
	abort_check(L);	/* the interrupt, for free (see above) */
	hp_triangle(coord(L, 1), coord(L, 2),
		    coord(L, 3), coord(L, 4),
		    coord(L, 5), coord(L, 6),
		    check_color(L, 7));
	return 0;
}

static int g_filltriangle(lua_State *L)
{
	abort_check(L);	/* the interrupt, for free (see above) */
	hp_fill_triangle(coord(L, 1),
			 coord(L, 2),
			 coord(L, 3),
			 coord(L, 4),
			 coord(L, 5),
			 coord(L, 6),
			 check_color(L, 7));
	return 0;
}

static int g_text(lua_State *L)
{
	abort_check(L);	/* the interrupt, for free (see above) */
	hp_text(coord(L, 1), coord(L, 2),
		luaL_checkstring(L, 3), check_color(L, 4));
	return 0;
}

static int g_textbg(lua_State *L)
{
	abort_check(L);	/* the interrupt, for free (see above) */
	hp_text_bg(coord(L, 1), coord(L, 2),
		   luaL_checkstring(L, 3), check_color(L, 4),
		   check_color(L, 5));
	return 0;
}

static int g_textwidth(lua_State *L)
{
	lua_pushinteger(L, hp_text_w(luaL_checkstring(L, 1)));
	return 1;
}

/* gfx.blit()              present the default back buffer and keep drawing
 * gfx.blit(grob, x, y)    copy a specific GROB to the screen
 * Both go through the screen target, so a script never has to remember to
 * switch back. */
static int g_blit(lua_State *L)
{
	abort_check(L);	/* the interrupt, for free (see above) */
	hp_grob **ud;
	int x, y;
	/* no argument: present the default back buffer -- one copy to the screen,
	 * then keep buffering it (no flicker, and the script does not have to
	 * remember to switch back) */
	if (lua_isnoneornil(L, 1)) {
		hp_grob *keep = cur_tgt;
		if (!screen_buf)
			return 0;               /* nothing buffered: nothing to
						 * present, and drawing already went
						 * to the screen */
		hp_grob_select(0);              /* draw onto the screen ... */
		hp_grob_blit(screen_buf, 0, 0); /* ... one copy, no flicker */
		hp_grob_select(keep);           /* ... and back to what the script
						 * had selected, not blindly to the
						 * back buffer */
		cur_tgt = keep;
		return 0;
	}
	/* with a grob: copy it onto the CURRENT draw target, like every other
	 * primitive, so `gfx.select(g); gfx.blit(sprite, x, y)` composes
	 * off-screen and a later gfx.blit() shows the result */
	ud = (hp_grob **)luaL_testudata(L, 1, "hp.grob");
	if (!ud || !*ud)
		return luaL_error(L, "gfx.blit: expected a grob");
	x = (int)luaL_optinteger(L, 2, 0);
	y = (int)luaL_optinteger(L, 3, 0);
	hp_grob_blit(*ud, x, y);
	return 0;
}

/* gfx.blitpixels(data, w, h, x, y [, key])
 * `data` is a Lua string of w*h little-endian ARGB words (string.pack is the
 * natural producer) -- a sprite without any file I/O. */
static int g_blitpixels(lua_State *L)
{
	size_t len;
	const char *data = luaL_checkstring(L, 1);
	int w = coord(L, 2);
	int h = coord(L, 3);
	int x = coord(L, 4);
	int y = coord(L, 5);
	len = luaL_len(L, 1);
	if ((size_t)w * h * 4 > len)
		return luaL_error(L, "blitpixels: need %d bytes, got %d",
				  w * h * 4, (int)len);
	if (lua_isnoneornil(L, 6))
		hp_blit((const unsigned *)data, w, h, x, y);
	else
		hp_blit_key((const unsigned *)data, w, h, x, y,
			    check_color(L, 6));
	return 0;
}

/* ---- GROBs (off-screen bitmaps, the double-buffering primitive) ---- */
static int g_newgrob(lua_State *L)
{
	hp_grob *g = hp_grob_new(coord(L, 1),
				 coord(L, 2));
	hp_grob **ud;
	if (!g)
		return luaL_error(L, "gfx.newgrob: out of memory");
	ud = (hp_grob **)lua_newuserdatauv(L, sizeof(hp_grob *), 0);
	*ud = g;
	luaL_setmetatable(L, "hp.grob");
	return 1;
}

static int g_freegrob(lua_State *L)
{
	hp_grob **ud = (hp_grob **)luaL_checkudata(L, 1, "hp.grob");
	if (*ud) {
		/* if this grob is the CURRENT draw target, fall back to the default
		 * back buffer first -- otherwise the next draw writes into memory the
		 * firmware has already taken back (and, because the image is
		 * resident, that mistake would outlive the run) */
		if (cur_tgt == *ud) {
			hp_grob *b = backbuf(); /* never leave a dangling target */
			hp_grob_select(b);
			cur_tgt = b;
		}
		hp_grob_free(*ud);
		*ud = 0;
	}
	return 0;
}

/* gfx.select(grob)   draw into that GROB
 * gfx.select()       back to the default back buffer (the usual case)
 * gfx.select(false)  draw straight onto the screen, no buffering */
static int g_select(lua_State *L)
{
	hp_grob **ud;
	if (lua_isnoneornil(L, 1) ||
	    (lua_isboolean(L, 1) && lua_toboolean(L, 1))) {
		hp_grob *b = backbuf();         /* the default back buffer */
		hp_grob_select(b);
		cur_tgt = b;
		return 0;
	}
	if (lua_isboolean(L, 1) && !lua_toboolean(L, 1)) {
		hp_grob_select(0);              /* the screen itself */
		cur_tgt = 0;
		return 0;
	}
	ud = (hp_grob **)luaL_testudata(L, 1, "hp.grob");
	if (!ud)
		return luaL_error(L, "gfx.select: expected a grob, or no argument "
				  "for the default back buffer");
	if (!*ud)
		return luaL_error(L, "gfx.select: this grob was already freed");
	hp_grob_select(*ud);
	cur_tgt = *ud;
	return 0;
}

/* gfx.freebackbuf() -- release the default back buffer (307 KB) and go back to
 * drawing straight on the screen.  For scripts that are done buffering (the
 * firmware heap is small, and this is the one allocation the library makes on
 * a script's behalf), and it makes the seeding behaviour testable. */
static int g_freebackbuf(lua_State *L)
{
	if (screen_buf) {
		if (cur_tgt == screen_buf) {
			hp_grob_select(0);
			cur_tgt = 0;
		}
		hp_grob_free(screen_buf);
		screen_buf = 0;
	}
	return 0;
}

/* gfx.fb() -- the framebuffer address and size as the FIRMWARE describes them
 * right now (read from the LCD struct hp_gfx_init reads, svc 0x1008D; that is
 * the only way to see them, hp_gfx.c keeps its copy private).  Diagnostics
 * only, but a valuable one: if a whole-screen draw resets the box, the first
 * question is whether the buffer the firmware named is the one we think. */
static int g_fb(lua_State *L)
{
	int w = 0, h = 0;
	unsigned *fb = screen_fb(&w, &h);
	lua_pushinteger(L, (lua_Integer)(unsigned long)fb);
	lua_pushinteger(L, w);
	lua_pushinteger(L, h);
	return 3;
}

/* gfx.target() -> "screen" | "default" | "grob"
 * Which of the three the primitives are drawing into right now. */
static int g_target(lua_State *L)
{
	if (!cur_tgt)
		lua_pushliteral(L, "screen");
	else if (cur_tgt == screen_buf)
		lua_pushliteral(L, "default");
	else
		lua_pushliteral(L, "grob");
	return 1;
}

static int g_grobwidth(lua_State *L)
{
	hp_grob **ud = (hp_grob **)luaL_testudata(L, 1, "hp.grob");
	lua_pushinteger(L, hp_grob_w(ud ? *ud : 0));
	return 1;
}

static int g_grobheight(lua_State *L)
{
	hp_grob **ud = (hp_grob **)luaL_testudata(L, 1, "hp.grob");
	lua_pushinteger(L, hp_grob_h(ud ? *ud : 0));
	return 1;
}

/* sys.log(text) -- append a line to the interpreter's durable diagnostics
 * (plua.log, the same file the C side writes and the launcher's "log" row
 * shows).
 *
 * Why a script wants this: on the calculator a reboot takes the console and
 * the 32 KB output ring with it, and Lua-side io.open() is not always usable
 * (the firmware only accepts "rb"/"wb+" -- now translated, but this path needs
 * no file handling from the script at all).  Write a marker before a risky
 * call and the file names the culprit after the machine comes back.
 *
 * It rewrites the file per call, so use it for a handful of markers per run,
 * not in a loop. */
static int s_log(lua_State *L)
{
	size_t n;
	const char *msg = luaL_checklstring(L, 1, &n);
	if (n > 0 && msg[n - 1] == '\n')
		plua_diag(msg);
	else {
		char buf[192];
		if (n > sizeof(buf) - 2)
			n = sizeof(buf) - 2;
		memcpy(buf, msg, n);
		buf[n] = '\n';
		buf[n + 1] = 0;
		plua_diag(buf);
	}
	return 0;
}

/* sys.version() -> the interpreter's API version string (see PLUA_VERSION).
 * Printed by the launcher before every run and logged by the diagnostics, so a
 * script that needs a newer binding than the lua.elf on the device says so
 * instead of failing with "attempt to call a nil value". */
static int s_version(lua_State *L)
{
	lua_pushliteral(L, PLUA_VERSION);
	return 1;
}

/* ---------------------------------------------------------------------------
 * dd -- double-double arithmetic: ~31 significant decimal digits
 *
 * A NEW library, on purpose: Lua's `math` and the number type are untouched
 * (they stay bit-identical to desktop Lua, which the differential tests rely
 * on).  A dd value is a userdata with the usual operators, so scripts read
 * naturally:
 *
 *     local dd = dd
 *     local x = dd.pi() * dd(2)          -- 2*pi to 31 digits
 *     print(dd.sin(x), dd.sqrt(dd(2)))   -- printed with 33 digits
 *     print(dd.tostring(x, 20))          -- or ask for fewer
 *     local hi, lo = dd.parts(x)         -- the exact two doubles
 *
 * `dd(2)` is a call: the library table's __call makes it a constructor, and
 * numbers, integer Lua numbers and numeric strings all work:
 *
 *     dd(1) dd(1.5) dd("0.1") dd("3.14159265358979323846264338327950288")
 *
 * Accuracy (measured on the host against mpmath at 60 digits, see
 * tools/gen_dd_expected.py and tests/test_dd_host.c): a few dd-ulp for
 * sqrt/log/sin/cos/tan/asin/acos/atan/atan2/hypot (1 dd-ulp = 2^-106 relative,
 * i.e. ~32 digits), ~10 for pow, ~46 for exp at |x| > 300.  Argument reduction
 * is Cody-Waite in three words, so sin() keeps its digits out to |x| ~ 1e6.
 * ------------------------------------------------------------------------- */
#define DD_MT "plua.dd"

static plua_dd *dd_push(lua_State *L, plua_dd v)
{
	plua_dd *ud = (plua_dd *)lua_newuserdatauv(L, sizeof(plua_dd), 0);
	*ud = v;
	luaL_setmetatable(L, DD_MT);
	return ud;
}

/* a dd, a number/integer, or a numeric string */
static plua_dd dd_check(lua_State *L, int idx)
{
	plua_dd *ud = (plua_dd *)luaL_testudata(L, idx, DD_MT);
	if (ud)
		return *ud;
	if (lua_isinteger(L, idx))
		return plua_dd_from_long((long long)lua_tointeger(L, idx));
	if (lua_type(L, idx) == LUA_TNUMBER)
		return plua_dd_from_double((double)lua_tonumber(L, idx));
	if (lua_type(L, idx) == LUA_TSTRING) {
		const char *end;
		plua_dd v = plua_dd_from_string(lua_tostring(L, idx), &end);
		if (plua_dd_is_nan(v) && (end == 0 || *end != 0))
			return luaL_error(L, "dd: '%s' is not a number",
					  lua_tostring(L, idx)), v;
		return v;
	}
	luaL_error(L, "dd: expected a dd value, a number or a numeric string");
	return plua_dd_from_double(0.0);
}

/* dd(x) / dd(x, lo) -- reached through __call, which passes the library table
 * itself as the first argument */
static int dd_call(lua_State *L)
{
	int base = lua_istable(L, 1) ? 2 : 1;
	if (lua_isnoneornil(L, base))
		return (int)((dd_push(L, plua_dd_from_double(0.0)), 1));
	if (lua_isinteger(L, base) && lua_isnoneornil(L, base + 1)) {
		dd_push(L, plua_dd_from_long((long long)lua_tointeger(L, base)));
		return 1;
	}
	if (lua_isnoneornil(L, base + 1))
		dd_push(L, dd_check(L, base));
	else
		dd_push(L, plua_dd_from_parts((double)luaL_checknumber(L, base),
					      (double)luaL_checknumber(L, base + 1)));
	return 1;
}

#define DD_UNARY(lname, cfn)                                                \
	static int dd_##lname(lua_State *L)                                 \
	{                                                                   \
		dd_push(L, cfn(dd_check(L, 1)));                            \
		return 1;                                                   \
	}

#define DD_BINARY(lname, cfn)                                               \
	static int dd_##lname(lua_State *L)                                 \
	{                                                                   \
		dd_push(L, cfn(dd_check(L, 1), dd_check(L, 2)));            \
		return 1;                                                   \
	}

DD_UNARY(abs, plua_dd_abs)
DD_UNARY(sqrt, plua_dd_sqrt)
DD_UNARY(exp, plua_dd_exp)
DD_UNARY(log, plua_dd_log)
DD_UNARY(log10, plua_dd_log10)
DD_UNARY(sin, plua_dd_sin)
DD_UNARY(cos, plua_dd_cos)
DD_UNARY(tan, plua_dd_tan)
DD_UNARY(asin, plua_dd_asin)
DD_UNARY(acos, plua_dd_acos)
DD_UNARY(atan, plua_dd_atan)
DD_UNARY(floor, plua_dd_floor)
DD_UNARY(ceil, plua_dd_ceil)
DD_UNARY(neg, plua_dd_neg)

/* dd.tonumber(x) -> the nearest Lua number (a plain double) */
static int dd_todouble(lua_State *L)
{
	lua_pushnumber(L, plua_dd_to_double(dd_check(L, 1)));
	return 1;
}
DD_BINARY(add, plua_dd_add)
DD_BINARY(sub, plua_dd_sub)
DD_BINARY(mul, plua_dd_mul)
DD_BINARY(div, plua_dd_div)
DD_BINARY(pow, plua_dd_pow)
DD_BINARY(fmod, plua_dd_fmod)
DD_BINARY(hypot, plua_dd_hypot)
DD_BINARY(atan2, plua_dd_atan2)

/* the operators: the two operands may each be a dd or a plain number */
#define DD_ARITH(lname, cfn)                                                \
	static int dd_##lname##_m(lua_State *L)                             \
	{                                                                   \
		dd_push(L, cfn(dd_check(L, 1), dd_check(L, 2)));            \
		return 1;                                                   \
	}
DD_ARITH(add, plua_dd_add)
DD_ARITH(sub, plua_dd_sub)
DD_ARITH(mul, plua_dd_mul)
DD_ARITH(div, plua_dd_div)
DD_ARITH(pow, plua_dd_pow)

static int dd_unm_m(lua_State *L)
{
	dd_push(L, plua_dd_neg(dd_check(L, 1)));
	return 1;
}

static int dd_lt_m(lua_State *L)
{
	int c = plua_dd_cmp(dd_check(L, 1), dd_check(L, 2));
	lua_pushboolean(L, c == -1);
	return 1;
}

static int dd_le_m(lua_State *L)
{
	int c = plua_dd_cmp(dd_check(L, 1), dd_check(L, 2));
	lua_pushboolean(L, c == -1 || c == 0);
	return 1;
}

static int dd_eq_m(lua_State *L)
{
	lua_pushboolean(L, plua_dd_cmp(dd_check(L, 1), dd_check(L, 2)) == 0);
	return 1;
}

static int dd_to_m(lua_State *L)
{
	char buf[64];
	/* __tostring gets no extra arguments, so it prints the full 33 digits;
	 * dd.tostring(x, n) is the one that takes a width */
	if (plua_dd_to_string(dd_check(L, 1), buf, (int)sizeof(buf), 33) < 0)
		return luaL_error(L, "dd: cannot format");
	lua_pushstring(L, buf);
	return 1;
}

/* dd.tostring(x [, sigdigits]) */
static int dd_tostring(lua_State *L)
{
	char buf[64];
	int n = (int)luaL_optinteger(L, 2, 33);
	if (plua_dd_to_string(dd_check(L, 1), buf, (int)sizeof(buf), n) < 0)
		return luaL_error(L, "dd: cannot format");
	lua_pushstring(L, buf);
	return 1;
}

/* dd.new(hi, lo) -- the two doubles as they are, for bit fiddling */
static int dd_new(lua_State *L)
{
	dd_push(L, plua_dd_from_parts((double)luaL_checknumber(L, 1),
				      (double)luaL_optnumber(L, 2, 0.0)));
	return 1;
}

/* dd.parts(x) -> hi, lo : the exact two doubles, no rounding */
static int dd_parts(lua_State *L)
{
	plua_dd v = dd_check(L, 1);
	lua_pushnumber(L, v.hi);
	lua_pushnumber(L, v.lo);
	return 2;
}

static int dd_isnan(lua_State *L)
{
	lua_pushboolean(L, plua_dd_is_nan(dd_check(L, 1)));
	return 1;
}

static int dd_isinf(lua_State *L)
{
	lua_pushboolean(L, plua_dd_is_inf(dd_check(L, 1)));
	return 1;
}

static int dd_pi(lua_State *L)
{
	dd_push(L, plua_dd_pi());
	return 1;
}

static int dd_e(lua_State *L)
{
	dd_push(L, plua_dd_e());
	return 1;
}

static int dd_ln2(lua_State *L)
{
	dd_push(L, plua_dd_ln2());
	return 1;
}

static int dd_ln10(lua_State *L)
{
	dd_push(L, plua_dd_ln10());
	return 1;
}

static const luaL_Reg dd_lib[] = {
	{ "new", dd_new },
	{ "parts", dd_parts },
	{ "tostring", dd_tostring },
	{ "tonumber", dd_todouble },
	{ "isnan", dd_isnan },
	{ "isinf", dd_isinf },
	{ "pi", dd_pi },
	{ "e", dd_e },
	{ "ln2", dd_ln2 },
	{ "ln10", dd_ln10 },
	{ "abs", dd_abs },
	{ "sqrt", dd_sqrt },
	{ "exp", dd_exp },
	{ "log", dd_log },
	{ "log10", dd_log10 },
	{ "sin", dd_sin },
	{ "cos", dd_cos },
	{ "tan", dd_tan },
	{ "asin", dd_asin },
	{ "acos", dd_acos },
	{ "atan", dd_atan },
	{ "atan2", dd_atan2 },
	{ "pow", dd_pow },
	{ "fmod", dd_fmod },
	{ "hypot", dd_hypot },
	{ "floor", dd_floor },
	{ "ceil", dd_ceil },
	{ "neg", dd_neg },
	{ "add", dd_add },
	{ "sub", dd_sub },
	{ "mul", dd_mul },
	{ "div", dd_div },
	{ NULL, NULL }
};

/* ---------------------------------------------------------------------------
 * font -- real text: Montserrat (proportional) and Cascadia Code Light
 * (monospace), 16 / 24 / 32 px
 *
 * This is primetcc's own font engine (rt/hp_fonts.c) drawing from two families
 * primeLua generates for itself:
 *
 *     prop  Montserrat        port/plua_font_montserrat_data.c
 *                             (tools/make_font_montserrat.py, out of primetcc's
 *                              glyph tables)
 *     mono  Cascadia Code Light  port/plua_font_cascadia_data.c
 *                             (tools/make_font_cascadia.py, rasterized offline)
 *
 * Real anti-aliased glyphs for ~43 KB of image instead of the 70 KB that
 * linking primetcc's full eight-face table costs.
 *
 *     local h = font.prop()                 -- or font.mono()
 *     font.draw(h, 10, 20, "Hello", gfx.WHITE)
 *     local w = font.width(h, "Hello")      -- 101 px at 16 px Montserrat
 *     local lh = font.height(h)             -- 21 px line height
 *
 * Sizes: 16 / 24 / 32 px ship for BOTH families.  font.prop(size) and
 * font.mono(size) take any size and round to the CLOSEST face that exists --
 * primetcc's own rule (hp_fonts.c pick()) -- so font.prop(20) draws 24 px and
 * font.mono(14) draws 16 px instead of failing.
 *
 * font.mono() is Cascadia Code Light: a fixed cell, so every glyph -- ":"
 * included -- advances by the same amount and a clock's digits never shift.
 * font.cascadia(size) is that same face under an explicit name (the engine's
 * mono accessor resolves to Cascadia too, see hp_font_mono_data() in the
 * generated file).
 *
 * On top of those two families there is font.big(): the clock's digit face,
 * Cascadia rasterized natively at 56 px with only '-' '.' '/' '0'-'9' ':'
 * (port/plua_font_big_data.c).  Big text has to be rasterized big -- scaling
 * a small face up multiplies its antialiased edges into grey fringes.
 *
 * The 5x8 text in gfx.text() stays as it was: it is the cheap fixed-width one,
 * and some scripts want exactly that.
 * ------------------------------------------------------------------------- */
static int f_handle(lua_State *L, const hp_font *f)
{
	const hp_font **ud = (const hp_font **)lua_newuserdatauv(
		L, sizeof(const hp_font *), 0);
	*ud = f;
	luaL_setmetatable(L, "plua.font");
	return 1;
}

/* font.prop([size]) -> handle (Montserrat, proportional) */
static int f_prop(lua_State *L)
{
	int size = (int)luaL_optinteger(L, 1, PLUA_FONT_SIZE);
	return f_handle(L, hp_font_prop(size));
}

/* font.mono([size]) -> handle.  The engine's monospace family IS Cascadia Code
 * Light in this build (hp_font_mono_data() comes from the Cascadia file), and
 * font.cascadia() below hands back the same face under its own name. */
static int f_mono(lua_State *L)
{
	int size = (int)luaL_optinteger(L, 1, PLUA_FONT_SIZE);
	return f_handle(L, hp_font_mono(size));
}

static const hp_font *check_font(lua_State *L, int idx)
{
	const hp_font **ud = (const hp_font **)luaL_checkudata(L, idx, "plua.font");
	if (!ud || !*ud)
		return hp_font_prop(PLUA_FONT_SIZE);   /* 16 px default */
	return *ud;
}

/* font.height(handle) -> line height in px (ascent + descent) */
static int f_height(lua_State *L)
{
	lua_pushinteger(L, hp_font_h(check_font(L, 1)));
	return 1;
}

/* font.width(handle, s) -> pixel width of s in that face */
static int f_width(lua_State *L)
{
	lua_pushinteger(L, hp_font_w(check_font(L, 1), luaL_checkstring(L, 2)));
	return 1;
}

/* font.advance(handle, ch) -> pixels the pen moves for one character */
static int f_advance(lua_State *L)
{
	size_t n;
	const char *s = luaL_checklstring(L, 2, &n);
	lua_pushinteger(L, hp_font_advance(check_font(L, 1),
					   n ? (unsigned char)s[0] : 0));
	return 1;
}

/* font.draw(handle, x, y, s, colour) -- into the CURRENT gfx target */
static int f_draw(lua_State *L)
{
	hp_font_draw(check_font(L, 1), coord(L, 2),
		     coord(L, 3), luaL_checkstring(L, 4),
		     check_color(L, 5));
	return 0;
}

/* font.drawbg(handle, x, y, s, fg, bg) -- solid background first, like the
 * calculators own text: readable over anything */
static int f_drawbg(lua_State *L)
{
	hp_font_draw_bg(check_font(L, 1), coord(L, 2),
			coord(L, 3), luaL_checkstring(L, 4),
			check_color(L, 5), check_color(L, 6));
	return 0;
}

/* font.cascadia([size]) -> handle.  Cascadia Code Light, the monospace family:
 * every glyph -- ":" included -- has the same advance, so a clock's digits never
 * shift.  Ships at 16/24/32 px and rounds to the nearest (see
 * plua_font_cascadia() in port/plua_font_cascadia_data.c); identical to
 * font.mono(size), kept as a name because scripts and docs say "cascadia". */
const hp_font *plua_font_cascadia(int size);

static int f_cascadia(lua_State *L)
{
	int size = (int)luaL_optinteger(L, 1, PLUA_FONT_SIZE);
	return f_handle(L, plua_font_cascadia(size));
}

/* font.big() -> handle.  The clock's digit face: Cascadia Code Light rasterized
 * natively at 56 px (cell 33 px, line height 66 px) -- port/plua_font_big_data.c,
 * regenerated with `make font-big BIGSIZE=n`.
 *
 * It is NOT a third text family, it is a single-purpose face: it carries only
 * '-' '.' '/' '0'-'9' ':' (0x2D..0x3A), so any other character draws NOTHING --
 * use font.prop()/font.mono() for text.  And it exists because scaling looks
 * bad: magnifying the 32 px face multiplies its antialiased edge pixels into
 * wide grey fringes, which is exactly what the clock demo's first version did
 * (GROB + scaled blit) and exactly how it looked on the calculator. */
const hp_font *plua_font_big(void);

static int f_big(lua_State *L)
{
	return f_handle(L, plua_font_big());
}

static const luaL_Reg font_lib[] = {
	{ "prop", f_prop },
	{ "mono", f_mono },
	{ "cascadia", f_cascadia },
	{ "big", f_big },
	{ "height", f_height },
	{ "width", f_width },
	{ "advance", f_advance },
	{ "draw", f_draw },
	{ "drawbg", f_drawbg },
	{ NULL, NULL }
};


/* ---------------------------------------------------------------------------
 * key -- keyboard and touch
 *
 * The implementation is primeLua's own (port/plua_input.c) and its MECHANISM
 * is PureDOOM's: the firmware's get_event is blocking and belongs to the OS
 * input thread, so input is OBSERVED through a slot hook and queued, never
 * polled directly and never rewritten.  Scripts see the same API they always
 * had.
 * ------------------------------------------------------------------------- */
static int k_install(lua_State *L)
{
	lua_pushboolean(L, plua_input_install());
	return 1;
}

static int k_remove(lua_State *L)
{
	plua_input_remove();
	return 0;
}

static int k_hooked(lua_State *L)
{
	lua_pushboolean(L, plua_input_hooked());
	return 1;
}

/* key.hookstats() -> table
 *
 * Every number the input path has, for the case where input misbehaves on the
 * machine and the emulator is perfectly happy: which layer stopped, and whether
 * the slot restore after the last run was verified.  See plua_input_stat() for
 * the exact meaning of each field. */
static int k_hookstats(lua_State *L)
{
	static const char *names[] = {
		"armed", "slot", "calls", "dropped_unarmed", "bad_type",
		"key_press", "key_release", "touch_press", "touch_release",
		"touch_moves", "touch_jitter", "queue_full", "restore_writes",
		"restore_verified", "restore_failed", "installs",
		"touch_frames", "touching", "touch_x", "touch_y"
	};
	int i;
	lua_createtable(L, 0, (int)(sizeof(names) / sizeof(names[0])));
	for (i = 0; i < (int)(sizeof(names) / sizeof(names[0])); i++) {
		lua_pushinteger(L, plua_input_stat(i));
		lua_setfield(L, -2, names[i]);
	}
	return 1;
}

/* key.event() -> table | nil
 *   {type = "key",   code = <scan code>, down = true/false}
 *   {type = "touch", x = , y = , phase = "press"/"move"/"release"}
 */
static int k_event(lua_State *L)
{
	struct plua_item it;
	if (!plua_input_pop(&it)) {
		lua_pushnil(L);
		return 1;
	}
	if (it.type == 1) {
		lua_newtable(L);
		lua_pushstring(L, "key");
		lua_setfield(L, -2, "type");
		lua_pushinteger(L, (lua_Integer)it.key);
		lua_setfield(L, -2, "code");
		lua_pushboolean(L, it.down);
		lua_setfield(L, -2, "down");
		return 1;
	}
	lua_newtable(L);
	lua_pushstring(L, "touch");
	lua_setfield(L, -2, "type");
	lua_pushinteger(L, (lua_Integer)it.action);
	lua_setfield(L, -2, "phase");
	lua_pushinteger(L, (lua_Integer)it.x);
	lua_setfield(L, -2, "x");
	lua_pushinteger(L, (lua_Integer)it.y);
	lua_setfield(L, -2, "y");
	/* a move carries how far the finger travelled since the last ACCEPTED
	 * position -- DOOM's mouse delta, which is what a gesture handler
	 * wants, next to the absolute pixel the contact is at */
	lua_pushinteger(L, (lua_Integer)it.dx);
	lua_setfield(L, -2, "dx");
	lua_pushinteger(L, (lua_Integer)it.dy);
	lua_setfield(L, -2, "dy");
	return 1;
}

/* key.touch() -> down, x, y | nil
 *
 * The live contact, not an event: is a finger down right now, and where was it
 * last accepted.  This is DOOM's g_is_touching / g_last_touch_x / g_last_touch_y,
 * and it is what a script polls inside its own frame loop (key.event() is the
 * event stream; this is the state). */
static int k_touch(lua_State *L)
{
	if (!plua_input_stat(17)) {
		lua_pushboolean(L, 0);
		return 1;
	}
	lua_pushboolean(L, 1);
	lua_pushinteger(L, (lua_Integer)plua_input_stat(18));
	lua_pushinteger(L, (lua_Integer)plua_input_stat(19));
	return 3;
}

/* key.getkey([wait_ms]) -> code | nil
 * Polls (and optionally waits) for a KEY PRESS, like the calculator's own
 * GETKEY: the code is the physical scan code, so it does not depend on the
 * alpha or shift state.  Touch items are left in the ring for key.gettouch(),
 * and key.event() sees everything in order. */
static int k_getkey(lua_State *L)
{
	int wait = (int)luaL_optinteger(L, 1, 0);
	int spent = 0;
	struct plua_item it;
	for (;;) {
		while (plua_input_pop(&it)) {
			if (it.type == 1 && it.down) {
				lua_pushinteger(L, (lua_Integer)it.key);
				return 1;
			}
			/* a release or a touch: keep looking (the touch items stay
			 * available to key.event() through the same ring, so they
			 * are simply skipped here) */
		}
		if (spent >= wait) {
			lua_pushnil(L);
			return 1;
		}
		abort_check(L);		/* ON can stop even a wait like this */
		hp_sys_sleep(5);
		spent += 5;
	}
}

/* key.gettouch([wait_ms]) -> x, y | nil */
static int k_gettouch(lua_State *L)
{
	int wait = (int)luaL_optinteger(L, 1, 0);
	int spent = 0;
	struct plua_item it;
	for (;;) {
		while (plua_input_pop(&it)) {
			if (it.type == 2 &&
			    (it.action == 1 /* press */ || it.action == 2 /* move */)) {
				lua_pushinteger(L, (lua_Integer)it.x);
				lua_pushinteger(L, (lua_Integer)it.y);
				return 2;
			}
		}
		if (spent >= wait) {
			lua_pushnil(L);
			return 1;
		}
		abort_check(L);
		hp_sys_sleep(5);
		spent += 5;
	}
}

static int k_sleep(lua_State *L)
{
	sleep_checked(L, (int)luaL_optinteger(L, 1, 0));
	return 0;
}

/* ---------------------------------------------------------------------------
 * sys -- memory and time-ish
 * ------------------------------------------------------------------------- */
static int s_memory(lua_State *L)
{
	lua_pushinteger(L, (lua_Integer)hp_sys_heap_free());
	return 1;
}

static int s_maxalloc(lua_State *L)
{
	lua_pushinteger(L, (lua_Integer)hp_sys_max_alloc());
	return 1;
}

/* sys.time() -> Unix seconds from the FIRMWARE clock (svc 0x100A5), or nil.
 *
 * This is the function that tells the two time sources apart, and scripts use
 * it exactly that way -- examples/clock.lua reports "firmware clock (svc
 * 0x100A5)" when it is non-nil and "launcher epoch (no firmware clock on this
 * machine)" when it is nil.  Without it bound, a working firmware clock looks
 * like a broken one AND the clock demo stops ticking: it takes os.time() as the
 * true wall clock, and with only the launcher epoch (a fixed snapshot taken
 * before the run) every second is identical. */
long long plua_fw_now(unsigned long long *tail);

static int s_time(lua_State *L)
{
	long long s = plua_fw_now(0);
	if (s < 0)
		lua_pushnil(L);
	else
		lua_pushinteger(L, (lua_Integer)s);
	return 1;
}

/* sys.clock() -> seconds since the program started, from os.clock()'s source
 * (the firmware's sub-second counter, self-calibrated). */
static int s_clock(lua_State *L)
{
	lua_pushnumber(L, (lua_Number)clock() / (lua_Number)CLOCKS_PER_SEC);
	return 1;
}

static int s_sleep(lua_State *L)
{
	sleep_checked(L, (int)luaL_optinteger(L, 1, 0));
	return 0;
}

/* ---------------------------------------------------------------------------
 * rand -- deterministic PCG32 (math.random is Lua's own, seeded by time)
 * ------------------------------------------------------------------------- */
static hp_rng rng_state;
static int rng_seeded;

static void rng_ensure(void)
{
	if (!rng_seeded) {
		hp_rng_seed(&rng_state, 0x9E3779B9u);
		rng_seeded = 1;
	}
}

static int r_seed(lua_State *L)
{
	hp_rng_seed(&rng_state, (unsigned)luaL_checkinteger(L, 1));
	rng_seeded = 1;
	return 0;
}

static int r_u32(lua_State *L)
{
	rng_ensure();
	lua_pushinteger(L, (lua_Integer)hp_rng_u32(&rng_state));
	return 1;
}

static int r_range(lua_State *L)
{
	int lo = coord(L, 1);
	int hi = coord(L, 2);
	rng_ensure();
	lua_pushinteger(L, hp_rng_range(&rng_state, lo, hi));
	return 1;
}

/* rand.noise(x, y [, seed]) -> 0..1 (Lua number) */
static int r_noise(lua_State *L)
{
	int x = coord(L, 1);
	int y = coord(L, 2);
	unsigned seed = (unsigned)luaL_optinteger(L, 3, 0);
	lua_pushnumber(L, (lua_Number)hp_fx_to_double(
			  hp_noise2_fx(hp_fx_int(x), hp_fx_int(y), seed)));
	return 1;
}

/* ---------------------------------------------------------------------------
 * codec -- checksums and encodings
 * ------------------------------------------------------------------------- */
static int c_crc32(lua_State *L)
{
	size_t n;
	const char *s = luaL_checklstring(L, 1, &n);
	lua_pushinteger(L, (lua_Integer)hp_crc32(s, (unsigned)n));
	return 1;
}

static int c_adler32(lua_State *L)
{
	size_t n;
	const char *s = luaL_checklstring(L, 1, &n);
	lua_pushinteger(L, (lua_Integer)hp_adler32(s, (unsigned)n));
	return 1;
}

static int c_hex(lua_State *L)
{
	size_t n;
	const char *s = luaL_checklstring(L, 1, &n);
	char *buf = (char *)malloc(2 * n + 1);
	if (!buf)
		return luaL_error(L, "codec.hex: out of memory");
	hp_hex_encode(buf, s, (unsigned)n);
	lua_pushlstring(L, buf, 2 * n);
	free(buf);
	return 1;
}

static int c_unhex(lua_State *L)
{
	size_t n;
	const char *s = luaL_checklstring(L, 1, &n);
	unsigned char *buf = (unsigned char *)malloc(n / 2 + 1);
	int got;
	if (!buf)
		return luaL_error(L, "codec.unhex: out of memory");
	got = hp_hex_decode(buf, s, (unsigned)n);
	if (got < 0) {
		free(buf);
		return luaL_error(L, "codec.unhex: bad hex string");
	}
	lua_pushlstring(L, (const char *)buf, (size_t)got);
	free(buf);
	return 1;
}

static int c_base64(lua_State *L)
{
	size_t n;
	const char *s = luaL_checklstring(L, 1, &n);
	char *buf = (char *)malloc(((n + 2) / 3) * 4 + 4);
	int len;
	if (!buf)
		return luaL_error(L, "codec.base64: out of memory");
	len = hp_b64_encode(buf, s, (unsigned)n);
	lua_pushlstring(L, buf, (size_t)len);
	free(buf);
	return 1;
}

static int c_unbase64(lua_State *L)
{
	size_t n;
	const char *s = luaL_checklstring(L, 1, &n);
	unsigned char *buf = (unsigned char *)malloc(((n + 3) / 4) * 3 + 4);
	int got;
	if (!buf)
		return luaL_error(L, "codec.unbase64: out of memory");
	got = hp_b64_decode(buf, s, (unsigned)n);
	if (got < 0) {
		free(buf);
		return luaL_error(L, "codec.unbase64: bad base64 string");
	}
	lua_pushlstring(L, (const char *)buf, (size_t)got);
	free(buf);
	return 1;
}

/* ---------------------------------------------------------------------------
 * fx -- Q16.16 fixed point (fast on a machine with no FPU)
 * ------------------------------------------------------------------------- */
#define FX_UNARY(lname, cname)                                             \
	static int fx_##lname(lua_State *L)                                \
	{                                                                  \
		lua_pushinteger(L, hp_fx_##cname(                          \
			(hp_fx)luaL_checkinteger(L, 1)));                  \
		return 1;                                                  \
	}

#define FX_BINARY(lname, cname)                                            \
	static int fx_##lname(lua_State *L)                                \
	{                                                                  \
		lua_pushinteger(L, hp_fx_##cname(                          \
			(hp_fx)luaL_checkinteger(L, 1),                    \
			(hp_fx)luaL_checkinteger(L, 2)));                  \
		return 1;                                                  \
	}

FX_UNARY(neg, neg)
FX_UNARY(abs, abs)
FX_UNARY(sqrt, sqrt)
FX_UNARY(sin, sin)
FX_UNARY(cos, cos)
FX_UNARY(tan, tan)
FX_UNARY(trunc, trunc)
FX_UNARY(round, round)
/* NOT FX_UNARY: this one HANDS BACK A NUMBER.  Pushing a double through
 * lua_pushinteger() turns 1.5 into 1, which is exactly what "fx.todouble(
 * fx.fromdouble(1.5)) == 1.5" caught. */
static int fx_todouble(lua_State *L)
{
	lua_pushnumber(L, hp_fx_to_double((hp_fx)luaL_checkinteger(L, 1)));
	return 1;
}
FX_BINARY(add, add)
FX_BINARY(sub, sub)
FX_BINARY(mul, mul)
FX_BINARY(div, div)

FX_BINARY(atan2, atan2)

static int fx_fromdouble(lua_State *L)
{
	lua_pushinteger(L, hp_fx_from_double(luaL_checknumber(L, 1)));
	return 1;
}

static int fx_lerp(lua_State *L)
{
	lua_pushinteger(L, hp_fx_lerp((hp_fx)luaL_checkinteger(L, 1),
				      (hp_fx)luaL_checkinteger(L, 2),
				      (hp_fx)luaL_checkinteger(L, 3)));
	return 1;
}

static int fx_int(lua_State *L)
{
	lua_pushinteger(L, hp_fx_int(coord(L, 1)));
	return 1;
}

/* ---------------------------------------------------------------------------
 * registration
 * ------------------------------------------------------------------------- */
static const luaL_Reg gfx_lib[] = {
	{ "init", g_init },
	{ "width", g_width },
	{ "height", g_height },
	{ "rgb", g_rgb },
	{ "clear", g_clear },
	{ "pixel", g_pixel },
	{ "getpixel", g_getpixel },
	{ "line", g_line },
	{ "hline", g_hline },
	{ "vline", g_vline },
	{ "rect", g_rect },
	{ "fillrect", g_fillrect },
	{ "circle", g_circle },
	{ "fillcircle", g_fillcircle },
	{ "triangle", g_triangle },
	{ "filltriangle", g_filltriangle },
	{ "text", g_text },
	{ "textbg", g_textbg },
	{ "textwidth", g_textwidth },
	{ "blit", g_blit },
	{ "blitpixels", g_blitpixels },
	{ "newgrob", g_newgrob },
	{ "freegrob", g_freegrob },
	{ "select", g_select },
	{ "target", g_target },
	{ "fb", g_fb },
	{ "freebackbuf", g_freebackbuf },
	{ "grobwidth", g_grobwidth },
	{ "grobheight", g_grobheight },
	{ NULL, NULL }
};

static const luaL_Reg key_lib[] = {
	{ "install", k_install },
	{ "remove", k_remove },
	{ "hooked", k_hooked },
	{ "hookstats", k_hookstats },
	{ "event", k_event },
	{ "getkey", k_getkey },
	{ "gettouch", k_gettouch },
	{ "touch", k_touch },
	{ "sleep", k_sleep },
	{ NULL, NULL }
};

static const luaL_Reg sys_lib[] = {
	{ "log", s_log },
	{ "version", s_version },
	{ "time", s_time },
	{ "clock", s_clock },
	{ "memory", s_memory },
	{ "maxalloc", s_maxalloc },
	{ "sleep", s_sleep },
	{ NULL, NULL }
};

static const luaL_Reg rand_lib[] = {
	{ "seed", r_seed },
	{ "u32", r_u32 },
	{ "range", r_range },
	{ "noise", r_noise },
	{ NULL, NULL }
};

static const luaL_Reg codec_lib[] = {
	{ "crc32", c_crc32 },
	{ "adler32", c_adler32 },
	{ "hex", c_hex },
	{ "unhex", c_unhex },
	{ "base64", c_base64 },
	{ "unbase64", c_unbase64 },
	{ NULL, NULL }
};

static const luaL_Reg fx_lib[] = {
	{ "int", fx_int },
	{ "trunc", fx_trunc },
	{ "round", fx_round },
	{ "todouble", fx_todouble },
	{ "fromdouble", fx_fromdouble },
	{ "neg", fx_neg },
	{ "abs", fx_abs },
	{ "add", fx_add },
	{ "sub", fx_sub },
	{ "mul", fx_mul },
	{ "div", fx_div },
	{ "lerp", fx_lerp },
	{ "sqrt", fx_sqrt },
	{ "sin", fx_sin },
	{ "cos", fx_cos },
	{ "tan", fx_tan },
	{ "atan2", fx_atan2 },
	{ NULL, NULL }
};

static void set_int(lua_State *L, const char *k, lua_Integer v)
{
	lua_pushinteger(L, v);
	lua_setfield(L, -2, k);
}

static void set_color(lua_State *L, const char *k, hp_color c)
{
	push_color(L, c);
	lua_setfield(L, -2, k);
}

/* The loader `require` calls: return the library table it closed over.
 *
 * It MUST be a function.  A first version of this put the table itself into
 * package.preload, which silently does nothing useful: Lua's preload searcher
 * hands whatever it finds to findloader(), which only accepts a FUNCTION and
 * otherwise moves on to the file searcher -- so `require("gfx")` failed with
 * "no file 'C:\DATA\gfx.lua'" while `gfx.line(...)` worked. */
static int lib_loader(lua_State *L)
{
	lua_pushvalue(L, lua_upvalueindex(1));
	return 1;
}

/* Register a library: a global table AND a working package.preload loader, so
 * both `gfx.line(...)` and `local gfx = require("gfx")` work. */
static void register_lib(lua_State *L, const char *name, const luaL_Reg *regs)
{
	lua_createtable(L, 0, 8);        /* luaL_newlib would size a pointer */
	luaL_setfuncs(L, regs, 0);       /* tbl */
	lua_pushvalue(L, -1);            /* tbl, tbl */
	lua_setglobal(L, name);          /* tbl */
	lua_getglobal(L, "package");     /* tbl, pkg */
	lua_getfield(L, -1, "preload");  /* tbl, pkg, pre */
	lua_pushvalue(L, -3);            /* tbl, pkg, pre, tbl */
	lua_pushcclosure(L, lib_loader, 1);   /* tbl, pkg, pre, loader */
	lua_setfield(L, -2, name);       /* pre[name] = loader */
	lua_pop(L, 3);
}

int luaopen_hp(lua_State *L);

/* linit.c's luaL_openlibs, renamed by the Makefile so this wrapper can call it
 * and then add the calculator's own libraries: by the time a script runs, gfx,
 * key, sys, rand, codec and fx are ordinary globals, exactly like `string`. */
void plua_openlibs(lua_State *L);

/* ---------------------------------------------------------------------------
 * THE CHEAP INTERRUPT: the paths primeLua owns.
 *
 * A Lua debug hook makes the VM call luaG_traceexec before every instruction,
 * which is what the interrupt in the loop costs (see plua_run.c for the
 * measurement).  That price is only worth paying when the user asks for it, so
 * the DEFAULT interrupt path is this one instead: print() and io.write() and
 * every gfx/font/sys binding check the abort flag themselves -- one load each.
 *
 * What that covers, and what it does not: a script that prints, draws, sleeps
 * or waits for a key is interrupted with no hook at all, because those calls
 * are what such a loop is made of (a drawing loop draws, a logger prints).
 * A pure numeric loop -- fib, a sieve -- makes none of those calls and cannot
 * be stopped without the hook; that is what the "interrupt" marker is for.
 * ------------------------------------------------------------------------- */
static int plua_ref_print;      /* registry refs to the originals we wrapped */
static int plua_ref_iowrite;

static int plua_print_wrap(lua_State *L)
{
	int n = lua_gettop(L);
	abort_check(L);
	if (plua_ref_print) {
		lua_rawgeti(L, LUA_REGISTRYINDEX, plua_ref_print);
		lua_insert(L, 1);
		lua_call(L, n, 0);
	}
	return 0;
}

static int plua_iowrite_wrap(lua_State *L)
{
	int n = lua_gettop(L);
	abort_check(L);
	if (plua_ref_iowrite) {
		lua_rawgeti(L, LUA_REGISTRYINDEX, plua_ref_iowrite);
		lua_insert(L, 1);
		lua_call(L, n, 1);
		return 1;
	}
	lua_pushnil(L);
	return 1;
}

/* wrap print and io.write: the two ways a script shows it is doing something */
static void plua_wrap_console(lua_State *L)
{
	lua_getglobal(L, "print");
	if (lua_isfunction(L, -1)) {
		lua_pushvalue(L, -1);
		plua_ref_print = luaL_ref(L, LUA_REGISTRYINDEX);
		lua_pushcfunction(L, plua_print_wrap);
		lua_setglobal(L, "print");
	}
	lua_pop(L, 1);
	lua_getglobal(L, "io");
	if (lua_istable(L, -1)) {
		lua_getfield(L, -1, "write");
		if (lua_isfunction(L, -1)) {
			lua_pushvalue(L, -1);
			plua_ref_iowrite = luaL_ref(L, LUA_REGISTRYINDEX);
			lua_pushcfunction(L, plua_iowrite_wrap);
			lua_setfield(L, -3, "write");
		}
		lua_pop(L, 1);
	}
	lua_pop(L, 1);
}

void luaL_openlibs(lua_State *L)
{
	plua_openlibs(L);
	luaopen_hp(L);
	lua_pop(L, 1);            /* the opener's return value; libs are globals */
	plua_wrap_console(L);
	/* only when the launcher asked for it: a debug hook is visible to scripts,
	 * and nothing is there to press a key without a launcher (see plua_run.h) */
	if (plua_run_interrupt())
		lua_sethook(L, plua_abort_hook, LUA_MASKCOUNT,
			    PLUA_ABORT_INTERVAL);
}

int luaopen_hp(lua_State *L)
{
	/* font handle userdata: points at a face in the image, nothing to free */
	if (luaL_newmetatable(L, "plua.font")) {
		lua_pushcfunction(L, f_height);
		lua_setfield(L, -2, "__index");
	}
	lua_pop(L, 1);

	/* GROB userdata: free the bitmap when Lua collects it */
	if (luaL_newmetatable(L, "hp.grob")) {
		lua_pushcfunction(L, g_freegrob);
		lua_setfield(L, -2, "__gc");
	}
	lua_pop(L, 1);

	register_lib(L, "gfx", gfx_lib);
	register_lib(L, "key", key_lib);
	register_lib(L, "sys", sys_lib);
	register_lib(L, "rand", rand_lib);
	register_lib(L, "codec", codec_lib);
	register_lib(L, "fx", fx_lib);
	register_lib(L, "font", font_lib);
	register_lib(L, "dd", dd_lib);
	/* the dd userdata carries its own arithmetic, and falls back to the
	 * library table so x:sqrt() reads like dd.sqrt(x) */
	if (luaL_newmetatable(L, DD_MT)) {
		lua_pushcfunction(L, dd_add_m);
		lua_setfield(L, -2, "__add");
		lua_pushcfunction(L, dd_sub_m);
		lua_setfield(L, -2, "__sub");
		lua_pushcfunction(L, dd_mul_m);
		lua_setfield(L, -2, "__mul");
		lua_pushcfunction(L, dd_div_m);
		lua_setfield(L, -2, "__div");
		lua_pushcfunction(L, dd_pow_m);
		lua_setfield(L, -2, "__pow");
		lua_pushcfunction(L, dd_unm_m);
		lua_setfield(L, -2, "__unm");
		lua_pushcfunction(L, dd_lt_m);
		lua_setfield(L, -2, "__lt");
		lua_pushcfunction(L, dd_le_m);
		lua_setfield(L, -2, "__le");
		lua_pushcfunction(L, dd_eq_m);
		lua_setfield(L, -2, "__eq");
		lua_pushcfunction(L, dd_to_m);
		lua_setfield(L, -2, "__tostring");
		lua_pushstring(L, "dd");
		lua_setfield(L, -2, "__name");
	}
	lua_pop(L, 1);
	luaL_getmetatable(L, DD_MT);
	lua_getglobal(L, "dd");
	lua_setfield(L, -2, "__index");
	lua_pop(L, 1);
	/* dd(2) / dd("0.1"): the TABLE has to be callable, which needs __call on a
	 * metatable of the table (putting __call inside the table itself does
	 * nothing -- Lua looked it up in the table's metatable instead, and
	 * dd(2) failed with "attempt to call a table value") */
	lua_getglobal(L, "dd");
	lua_newtable(L);
	lua_pushcfunction(L, dd_call);
	lua_setfield(L, -2, "__call");
	lua_pushstring(L, "dd");
	lua_setfield(L, -2, "__name");
	lua_setmetatable(L, -2);
	lua_pop(L, 1);

	/* colours and screen geometry as constants on the gfx table */
	lua_getglobal(L, "gfx");
	set_color(L, "BLACK", HP_BLACK);
	set_color(L, "WHITE", HP_WHITE);
	set_color(L, "RED", HP_RED);
	set_color(L, "GREEN", HP_GREEN);
	set_color(L, "BLUE", HP_BLUE);
	set_color(L, "YELLOW", HP_YELLOW);
	set_color(L, "CYAN", HP_CYAN);
	set_color(L, "MAGENTA", HP_MAGENTA);
	set_color(L, "ORANGE", HP_ORANGE);
	set_color(L, "GRAY", HP_GRAY);
	set_int(L, "WIDTH", HP_LCD_W);
	set_int(L, "HEIGHT", HP_LCD_H);
	lua_pop(L, 1);

	/* Key codes = the firmware's DEVICE KEY IDS (hp_input.h's table, verified
	 * against the calculator's scan-code matrix), which is what the events in
	 * the input queue carry and therefore what key.getkey() returns.
	 *
	 * These are NOT the PPL GETKEY numbers, and getting that wrong is not a
	 * theoretical worry: with the GETKEY numbering registered here, RIGHT
	 * (device 0x04) looked like the GETKEY code for ESC, so a game quit when
	 * the right arrow was pressed.  The two tables, side by side:
	 *
	 *            device id (here, key.getkey)   PPL GETKEY / keyboard() bit
	 *   esc            0x01                                4
	 *   left           0x02                                7
	 *   up             0x03                                2
	 *   right          0x04                                8
	 *   down           0x05                               12
	 *   backspace      0x0C                               19
	 *   enter          0x0D                               30
	 *
	 * The ids live in port/plua_input.h, next to the scan-code matrix they
	 * were verified against: primeLua owns the input path now, so the table
	 * cannot be changed by another project's rewrite.  (Python code that
	 * polls hpprime.keyboard() needs the OTHER column: its bit numbers are
	 * the PPL codes -- see the launcher's selector.) */
	lua_getglobal(L, "key");
	set_int(L, "ESC", HP_KEY_ESC);
	set_int(L, "LEFT", HP_KEY_LEFT);
	set_int(L, "UP", HP_KEY_UP);
	set_int(L, "RIGHT", HP_KEY_RIGHT);
	set_int(L, "DOWN", HP_KEY_DOWN);
	set_int(L, "BACKSPACE", HP_KEY_BACKSPACE);
	set_int(L, "ENTER", HP_KEY_ENTER);
	set_int(L, "SPACE", HP_KEY_SPACE);
	set_int(L, "ON", HP_KEY_ON);
	set_int(L, "SHIFT", HP_KEY_SHIFT);
	set_int(L, "APPS", HP_KEY_APPS);
	set_int(L, "SYMB", HP_KEY_F1);
	set_int(L, "PLOT", HP_KEY_F2);
	set_int(L, "NUM", HP_KEY_F3);
	set_int(L, "VIEW", HP_KEY_F4);
	set_int(L, "CAS", HP_KEY_F5);
	set_int(L, "MENU", HP_KEY_F6);
	set_int(L, "HELP", HP_KEY_HELP);
	set_int(L, "ALPHA", HP_KEY_ALPHA);
	set_int(L, "PLUSMINUS", HP_KEY_PLUSMINUS);
	set_int(L, "X2", HP_KEY_X2);
	/* the 7/Q key: its own device id, and 'q' in alpha mode */
	set_int(L, "Q", HP_KEY_Q);
	lua_pop(L, 1);

	/* The input hook and its state are primeLua's own (port/plua_input.c),
	 * compiled into this image; nothing has to be handed to anybody at
	 * open time.  The launcher still finds the state by the "PRIMEIN" magic
	 * to check the slot after every run -- see plua_input.c's header for the
	 * layout both sides share. */

	lua_pushboolean(L, 1);
	return 1;
}
