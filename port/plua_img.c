/*
 * plua_img.c -- the `img` library: image files in, GROBs out, blitting with
 * scaling.  This is the only place that knows about image *formats*: the PNG
 * decoders are primeLua's own port/plua_png.c and port/plua_jpg.c (primetcc's
 * PNG decoder is measurably wrong, see plua_png.c).  Both produce the same thing -- a
 * w*h ARGB buffer -- which is then copied into an ordinary hp_grob, so every
 * existing gfx call (select, blit, grobwidth, freegrob, target) works on a
 * loaded image without knowing where it came from.
 *
 *     local img = img
 *     local g = assert(img.load("photo.jpg"))      -- or "sprite.png"
 *     print(gfx.grobwidth(g), gfx.grobheight(g))
 *     img.blit(g, 10, 10)                          -- 1:1 onto the current target
 *     img.blit(g, 10, 10, gfx.MAGENTA)             -- ... with a colour key
 *     img.strblit(g, 0, 0, 160, 120)               -- scaled (stretch) blit
 *     img.strblitpart(g, 0, 0, 64, 64, 8, 8, 16, 16)
 *     local half = img.scale(g, 160, 120)          -- a new, smaller GROB
 *
 * What it does NOT do, deliberately:
 *   * PNG alpha is preserved and img.blit alpha-blends it (0 = skip the pixel),
 *     and a colour key (img.blit(g, x, y, key)) still works as well;
 *   * progressive JPEGs and 12-bit/CMYK files: plua_jpg.c reports them as
 *     errors with a message instead of guessing;
 *   * image resampling beyond nearest neighbour: the screen is 320x240 and a
 *     loaded photo is usually *shrunk*, where nearest neighbour is hard to tell
 *     from bilinear on a 4-bit-per-channel panel, and it is 5x cheaper.
 *
 * Scaling is done on the fly from the source GROB (no temporary buffer), and
 * out-of-range destinations are dropped by hp_pixel itself.
 */
#include <stddef.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"

#include "hp_gfx.h"

#include "plua_port.h"
#include "plua_pix.h"

#ifndef NULL
#define NULL ((void *)0)
#endif

/* the runtime's allocator (hp_rt.c) -- same heap as every other allocation */
void *malloc(size_t n);
void free(void *p);

/* the buffer for one decoded image; freed as soon as the GROB has a copy */
#define IMG_MAXBYTES (8u * 1024u * 1024u)

static hp_grob *to_grob(lua_State *L, int idx)
{
	hp_grob **ud = (hp_grob **)luaL_checkudata(L, idx, "hp.grob");

	if (!ud || !*ud)
		luaL_error(L, "img: expected a grob (from img.load or gfx.newgrob)");
	return *ud;
}

static int opt_key(lua_State *L, int idx, int *has_key, hp_color *key)
{
	if (lua_isnoneornil(L, idx)) {
		*has_key = 0;
		*key = 0;
		return 0;
	}
	*has_key = 1;
	*key = (hp_color)luaL_checkinteger(L, idx);
	return 1;
}

/* ---- reading a whole file through primeLua's stdio shim (relative names are
 * resolved to the script's folder there, exactly like io.open) ---- */
static unsigned char *load_file(const char *path, unsigned *len)
{
	FILE *f;
	unsigned char *buf = NULL;
	long n;

	*len = 0;
	f = fopen(path, "rb");
	if (!f)
		return NULL;
	if (fseek(f, 0, SEEK_END) != 0) {
		fclose(f);
		return NULL;
	}
	n = ftell(f);
	if (n <= 0 || (unsigned long)n > IMG_MAXBYTES) {
		fclose(f);
		return NULL;
	}
	if (fseek(f, 0, SEEK_SET) != 0) {
		fclose(f);
		return NULL;
	}
	buf = (unsigned char *)malloc((size_t)n);
	if (!buf) {
		fclose(f);
		return NULL;
	}
	if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
		free(buf);
		fclose(f);
		return NULL;
	}
	fclose(f);
	*len = (unsigned)n;
	return buf;
}

/* PNG magic, or JPEG SOI -- sniffed from the bytes, so a .dat works too */
static int sniff(const unsigned char *d, unsigned len)
{
	static const unsigned char png[8] =
		{ 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };

	if (len >= 2 && d[0] == 0xFF && d[1] == 0xD8)
		return 2;                        /* JPEG */
	if (len >= 8) {
		int i, ok = 1;
		for (i = 0; i < 8; i++)
			if (d[i] != png[i])
				ok = 0;
		if (ok)
			return 1;                /* PNG */
	}
	return 0;
}

/* decode whatever the bytes are; returns an ARGB buffer the caller frees */
static unsigned *decode(const unsigned char *data, unsigned len, int *w, int *h,
			char *err, int errcap)
{
	int kind = sniff(data, len);

	err[0] = 0;
	if (kind == 1)
		return plua_png_decode(data, len, w, h, err, errcap);
	if (kind == 2)
		return plua_jpg_decode(data, len, w, h, err, errcap);
	{
		const char *m = "not a PNG or JPEG";
		int i;
		for (i = 0; m[i] && i < errcap - 1; i++)
			err[i] = m[i];
		err[i] = 0;
	}
	return NULL;
}

/* wrap decoded pixels in a GROB the rest of gfx already understands */
static int push_grob_from_pixels(lua_State *L, const unsigned *px, int w, int h)
{
	hp_grob *g = hp_grob_new(w, h);
	int i;

	if (!g) {
		lua_pushnil(L);
		lua_pushliteral(L, "out of memory for the image");
		return 2;
	}
	for (i = 0; i < w * h; i++)
		g->px[i] = px[i];
	return plua_push_grob(L, g) ? 1 : luaL_error(L, "img: internal error");
}

/* ---- loader ---- */
static int img_from_bytes(lua_State *L, const unsigned char *data, unsigned len)
{
	int w = 0, h = 0;
	unsigned *px;
	char err[96];

	px = decode(data, len, &w, &h, err, (int)sizeof(err));
	if (!px || w <= 0 || h <= 0) {
		lua_pushnil(L);
		lua_pushstring(L, err[0] ? err : "decode failed");
		return 2;
	}
	{
		int n = push_grob_from_pixels(L, px, w, h);
		free(px);
		return n;
	}
}

/* img.load(path) -> grob | nil, err */
static int i_load(lua_State *L)
{
	const char *path = luaL_checkstring(L, 1);
	char resolved[PLUA_PATH_MAX];
	unsigned char *data;
	unsigned len = 0;

	plua_resolve_path(path, resolved, (int)sizeof(resolved));
	data = load_file(resolved, &len);
	if (!data) {
		lua_pushnil(L);
		lua_pushfstring(L, "cannot read %s", path);
		return 2;
	}
	{
		int n = img_from_bytes(L, data, len);
		free(data);
		return n;
	}
}

/* img.loadmem(bytes) -> grob | nil, err : for images embedded in the script */
static int i_loadmem(lua_State *L)
{
	size_t len = 0;
	const char *data = luaL_checklstring(L, 1, &len);

	return img_from_bytes(L, (const unsigned char *)data, (unsigned)len);
}

/* img.info(path) -> w, h, "png"|"jpg" | nil, err */
static int i_info(lua_State *L)
{
	const char *path = luaL_checkstring(L, 1);
	char resolved[PLUA_PATH_MAX];
	unsigned char *data;
	unsigned len = 0;
	int w = 0, h = 0;
	char err[96];
	unsigned *px;
	int kind;

	plua_resolve_path(path, resolved, (int)sizeof(resolved));
	data = load_file(resolved, &len);
	if (!data) {
		lua_pushnil(L);
		lua_pushfstring(L, "cannot read %s", path);
		return 2;
	}
	kind = sniff(data, len);
	px = decode(data, len, &w, &h, err, (int)sizeof(err));
	free(data);
	if (!px) {
		lua_pushnil(L);
		lua_pushstring(L, err[0] ? err : "decode failed");
		return 2;
	}
	free(px);
	lua_pushinteger(L, w);
	lua_pushinteger(L, h);
	lua_pushstring(L, kind == 1 ? "png" : "jpg");
	return 3;
}

/* ---- drawing ---- */
static void put_px(int x, int y, unsigned c)
{
	unsigned a = (c >> 24) & 0xFF;

	if (a == 0xFF || a == 0)
		hp_pixel(x, y, c);
	else
		hp_pixel_a(x, y, c, a);
}

/* img.blit(g, x, y [, key]) -- 1:1 copy onto the current target */
static int i_blit(lua_State *L)
{
	hp_grob *g = to_grob(L, 1);
	int x = (int)luaL_checkinteger(L, 2);
	int y = (int)luaL_checkinteger(L, 3);
	int has_key;
	hp_color key;
	int i;

	opt_key(L, 4, &has_key, &key);
	if (!has_key) {
		/* the runtime's optimised loops for the common cases */
		hp_blit(g->px, g->w, g->h, x, y);
		return 0;
	}
	for (i = 0; i < g->h; i++) {
		const unsigned *src = g->px + (size_t)i * g->w;
		int j;
		for (j = 0; j < g->w; j++) {
			unsigned c = src[j];
			if ((c & 0xFFFFFFu) == (key & 0xFFFFFFu))
				continue;
			put_px(x + j, y + i, c);
		}
	}
	return 0;
}

/* shared body of strblit / strblitpart: sample a source rectangle into a
 * destination rectangle, nearest neighbour */
static void scaled_copy(const hp_grob *g, int dx, int dy, int dw, int dh,
			int sx, int sy, int sw, int sh, int has_key, hp_color key)
{
	int i, j;

	if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0)
		return;
	for (i = 0; i < dh; i++) {
		int src_y = sy + (int)((long)i * sh / dh);
		const unsigned *row;

		if (src_y < 0 || src_y >= g->h)
			continue;
		row = g->px + (size_t)src_y * g->w;
		for (j = 0; j < dw; j++) {
			int src_x = sx + (int)((long)j * sw / dw);
			unsigned c;

			if (src_x < 0 || src_x >= g->w)
				continue;
			c = row[src_x];
			if (has_key && (c & 0xFFFFFFu) == (key & 0xFFFFFFu))
				continue;
			put_px(dx + j, dy + i, c);
		}
	}
}

/* img.strblit(g, x, y, w, h [, key]) -- scaled copy */
static int i_strblit(lua_State *L)
{
	hp_grob *g = to_grob(L, 1);
	int x = (int)luaL_checkinteger(L, 2);
	int y = (int)luaL_checkinteger(L, 3);
	int w = (int)luaL_checkinteger(L, 4);
	int h = (int)luaL_checkinteger(L, 5);
	int has_key;
	hp_color key;

	opt_key(L, 6, &has_key, &key);
	scaled_copy(g, x, y, w, h, 0, 0, g->w, g->h, has_key, key);
	return 0;
}

/* img.strblitpart(g, dx, dy, dw, dh, sx, sy, sw, sh [, key]) */
static int i_strblitpart(lua_State *L)
{
	hp_grob *g = to_grob(L, 1);
	int dx = (int)luaL_checkinteger(L, 2);
	int dy = (int)luaL_checkinteger(L, 3);
	int dw = (int)luaL_checkinteger(L, 4);
	int dh = (int)luaL_checkinteger(L, 5);
	int sx = (int)luaL_checkinteger(L, 6);
	int sy = (int)luaL_checkinteger(L, 7);
	int sw = (int)luaL_checkinteger(L, 8);
	int sh = (int)luaL_checkinteger(L, 9);
	int has_key;
	hp_color key;

	opt_key(L, 10, &has_key, &key);
	scaled_copy(g, dx, dy, dw, dh, sx, sy, sw, sh, has_key, key);
	return 0;
}

/* img.blitpart(g, dx, dy, sx, sy, sw, sh [, key]) -- unscaled, 1:1 */
static int i_blitpart(lua_State *L)
{
	hp_grob *g = to_grob(L, 1);
	int dx = (int)luaL_checkinteger(L, 2);
	int dy = (int)luaL_checkinteger(L, 3);
	int sx = (int)luaL_checkinteger(L, 4);
	int sy = (int)luaL_checkinteger(L, 5);
	int sw = (int)luaL_checkinteger(L, 6);
	int sh = (int)luaL_checkinteger(L, 7);
	int has_key;
	hp_color key;

	opt_key(L, 8, &has_key, &key);
	scaled_copy(g, dx, dy, sw, sh, sx, sy, sw, sh, has_key, key);
	return 0;
}

/* img.scale(g, w, h [, key]) -> grob : a new GROB, the source untouched */
static int i_scale(lua_State *L)
{
	hp_grob *g = to_grob(L, 1);
	int w = (int)luaL_checkinteger(L, 2);
	int h = (int)luaL_checkinteger(L, 3);
	int has_key;
	hp_color key;
	hp_grob *out;
	hp_grob *keep = plua_current_grob();
	int i, j;

	opt_key(L, 4, &has_key, &key);
	if (w <= 0 || h <= 0)
		return luaL_error(L, "img.scale: bad size %dx%d", w, h);
	out = hp_grob_new(w, h);
	if (!out)
		return luaL_error(L, "img.scale: out of memory");
	for (i = 0; i < h; i++) {
		int src_y = (int)((long)i * g->h / h);
		const unsigned *row = g->px + (size_t)src_y * g->w;

		for (j = 0; j < w; j++) {
			int src_x = (int)((long)j * g->w / w);
			unsigned c = row[src_x];

			if (has_key && (c & 0xFFFFFFu) == (key & 0xFFFFFFu))
				c = 0xFF000000u;
			out->px[(size_t)i * w + j] = c;
		}
	}
	(void)keep;
	if (!plua_push_grob(L, out))
		return luaL_error(L, "img.scale: internal error");
	return 1;
}

static const luaL_Reg img_lib[] = {
	{ "load", i_load },
	{ "loadmem", i_loadmem },
	{ "info", i_info },
	{ "blit", i_blit },
	{ "strblit", i_strblit },
	{ "blitpart", i_blitpart },
	{ "strblitpart", i_strblitpart },
	{ "scale", i_scale },
	{ NULL, NULL }
};

const struct luaL_Reg *plua_img_lib(void)
{
	return (const struct luaL_Reg *)img_lib;
}
