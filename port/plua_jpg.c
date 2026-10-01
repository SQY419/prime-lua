/*
 * plua_jpg.c -- baseline JPEG (JFIF) decoder for primeLua's img library.
 *
 * WHY THIS EXISTS
 *   The Prime's firmware can *display* a JPEG but exposes no way to get one into
 *   a GROB, and primeLua had no JPEG path at all (primetcc ships a PNG decoder,
 *   rt/hp_image.c, which port/plua_img.c reuses for PNG).
 *
 * WHAT IT DECODES
 *   Baseline and extended-sequential Huffman JPEGs (SOF0/SOF1), 8-bit samples,
 *   1 or 3 components (grayscale / YCbCr), any sampling factors -- 4:4:4,
 *   4:2:2, 4:2:0, 4:1:1 -- with DRI restart markers, up to four quantisation
 *   and Huffman tables.  Chroma is upsampled with the usual 3:1 "fancy" filter
 *   for 2x factors and nearest-neighbour otherwise, and YCbCr->RGB uses the
 *   standard float coefficients, so the output is within a couple of levels of
 *   libjpeg (tests/test_img.py measures that against PIL).
 *
 * WHAT IT REJECTS, WITH A MESSAGE
 *   Progressive (SOF2) and arithmetic coding, 12-bit precision, CMYK/4-component
 *   files, and dimensions above 2048 px per side (the firmware heap has to hold
 *   one plane per component plus the ARGB output).
 *
 * Deliberately simple: canonical Huffman decoding bit by bit, a separable float
 * IDCT with the cosine basis as a constant table (no libm call, no soft-float
 * surprises), and no fancy block-skipping.  Decoding a VGA-sized photo takes a
 * fraction of a second on the ARM926, which is fine for loading an image.
 */
#include <stddef.h>

#include "plua_pix.h"

/* the runtime's allocator (hp_rt.c) -- the firmware heap */
void *malloc(size_t n);
void free(void *p);

#ifndef NULL
#define NULL ((void *)0)
#endif

#define JP_MAXCOMP 4
#define JP_MAXQ    4
#define JP_MAXHUF  8                  /* 2 classes x 4 tables */
#define JP_MAXDIM  2048

/* ---- the IDCT basis: c(u,x) = a(u) * cos((2x+1) u pi / 16), a(0)=1/sqrt(8),
 * a(u)=1/2.  Generated once, so the decoder needs no libm at all. ---- */
static const float jp_idct_cos[8][8] = {
	{ +0.353553391f, +0.353553391f, +0.353553391f, +0.353553391f,
	  +0.353553391f, +0.353553391f, +0.353553391f, +0.353553391f },
	{ +0.490392640f, +0.415734806f, +0.277785117f, +0.097545161f,
	  -0.097545161f, -0.277785117f, -0.415734806f, -0.490392640f },
	{ +0.461939766f, +0.191341716f, -0.191341716f, -0.461939766f,
	  -0.461939766f, -0.191341716f, +0.191341716f, +0.461939766f },
	{ +0.415734806f, -0.097545161f, -0.490392640f, -0.277785117f,
	  +0.277785117f, +0.490392640f, +0.097545161f, -0.415734806f },
	{ +0.353553391f, -0.353553391f, -0.353553391f, +0.353553391f,
	  +0.353553391f, -0.353553391f, -0.353553391f, +0.353553391f },
	{ +0.277785117f, -0.490392640f, +0.097545161f, +0.415734806f,
	  -0.415734806f, -0.097545161f, +0.490392640f, -0.277785117f },
	{ +0.191341716f, -0.461939766f, +0.461939766f, -0.191341716f,
	  -0.191341716f, +0.461939766f, -0.461939766f, +0.191341716f },
	{ +0.097545161f, -0.277785117f, +0.415734806f, -0.490392640f,
	  +0.490392640f, -0.415734806f, +0.277785117f, -0.097545161f },
};

/* zigzag[k] = the natural (row-major) index of the k-th coefficient */
static const unsigned char jp_zigzag[64] = {
	0, 1, 8, 16, 9, 2, 3, 10,
	17, 24, 32, 25, 18, 11, 4, 5,
	12, 19, 26, 33, 40, 48, 41, 34,
	27, 20, 13, 6, 7, 14, 21, 28,
	35, 42, 49, 56, 57, 50, 43, 36,
	29, 22, 15, 23, 30, 37, 44, 51,
	58, 59, 52, 45, 38, 31, 39, 46,
	53, 60, 61, 54, 47, 55, 62, 63
};

/* ---- bit reader: Huffman bits come MSB first, and a 0xFF byte in the entropy
 * stream is followed by 0x00 (byte stuffing) or is a marker ---- */
typedef struct {
	const unsigned char *d;
	unsigned n, pos;
	unsigned buf;
	int bits;
	int bad;
} jp_bits;

static void jp_fill(jp_bits *b)
{
	while (b->bits <= 24 && b->pos < b->n) {
		unsigned char c = b->d[b->pos];
		if (c == 0xFF) {
			unsigned char c2 = (b->pos + 1 < b->n) ? b->d[b->pos + 1] : 0;
			if (c2 == 0x00) {
				b->pos += 2;      /* stuffed 0xFF */
			} else {
				break;            /* restart or EOI: stop here */
			}
		} else {
			b->pos++;
		}
		b->buf = (b->buf << 8) | c;
		b->bits += 8;
	}
}

static unsigned jp_get(jp_bits *b, int n)
{
	unsigned v;

	if (n <= 0)
		return 0;
	jp_fill(b);
	if (b->bits < n) {
		b->bad = 1;
		v = (b->buf << (n - b->bits)) & ((1u << n) - 1);   /* zero pad */
		b->bits = 0;
		return v;
	}
	v = (b->buf >> (b->bits - n)) & ((1u << n) - 1);
	b->bits -= n;
	return v;
}

static void jp_align(jp_bits *b)
{
	b->bits = 0;
	b->buf = 0;
}

/* ---- canonical Huffman tables ---- */
typedef struct {
	unsigned char val[256];
	unsigned char bl[17];
	unsigned short order[256];
} jp_huff;

static void jp_huff_build(jp_huff *h, const unsigned char *len,
			  const unsigned char *val, int n)
{
	unsigned next[17];
	unsigned first = 0;
	int i, l, o = 0;

	for (i = 0; i < 17; i++)
		h->bl[i] = 0;
	for (i = 0; i < n; i++) {
		h->val[i] = val[i];
		if (len[i] && len[i] <= 16)
			h->bl[len[i]]++;
	}
	for (l = 1; l <= 16; l++) {
		next[l] = first;
		first = (first + h->bl[l]) << 1;
	}
	for (l = 1; l <= 16; l++)
		for (i = 0; i < n; i++)
			if (len[i] == l)
				h->order[o++] = (unsigned short)i;
	(void)next;
}

static int jp_huff_decode(const jp_huff *h, jp_bits *b)
{
	unsigned cur = 0, first = 0, index = 0;
	int l;

	for (l = 1; l <= 16; l++) {
		cur = (cur << 1) | jp_get(b, 1);
		if (cur - first < h->bl[l]) {
			unsigned k = index + (cur - first);
			if (k >= 256)
				return -1;
			return h->val[h->order[k]];
		}
		index += h->bl[l];
		first = (first + h->bl[l]) << 1;
	}
	return -1;
}

/* JPEG's "extend": the low bits are a signed value centred on zero */
static int jp_extend(unsigned v, int n)
{
	if (n == 0)
		return 0;
	if (v < (1u << (n - 1)))
		return (int)v - (int)((1u << n) - 1);
	return (int)v;
}

/* ---- markers ---- */
#define JP_SOF0 0xC0
#define JP_SOF1 0xC1
#define JP_DHT  0xC4
#define JP_SOI  0xD8
#define JP_EOI  0xD9
#define JP_SOS  0xDA
#define JP_DQT  0xDB
#define JP_DRI  0xDD

typedef struct {
	int id, hs, vs, tq, td, ta;
	int dw, dh;                   /* plane size in samples (MCU padded) */
	int dcpred;
	unsigned char *plane;
} jp_comp;

typedef struct {
	const unsigned char *d;
	unsigned n;

	int w, h, ncomp, prec;
	int maxhs, maxvs;
	int mcus_x, mcus_y;
	int restart_interval;

	jp_comp comp[JP_MAXCOMP];
	int have_q[JP_MAXQ];
	unsigned short quant[JP_MAXQ][64];
	jp_huff huff[JP_MAXHUF];
	int have_h[JP_MAXHUF];

	unsigned *out;                /* w*h ARGB */
	char *err;
	int errcap;
} jp_ctx;

static void jp_err(jp_ctx *c, const char *msg)
{
	int i = 0;

	if (!c->err || c->errcap <= 0 || c->err[0])
		return;
	while (msg[i] && i < c->errcap - 1) {
		c->err[i] = msg[i];
		i++;
	}
	c->err[i] = 0;
}

static void jp_idct(const int *coef, unsigned char *dst, int dw)
{
	float tmp[64];
	int u, v, x, y;

	for (v = 0; v < 8; v++)
		for (x = 0; x < 8; x++) {
			float s = 0.0f;
			for (u = 0; u < 8; u++)
				s += jp_idct_cos[u][x] * (float)coef[v * 8 + u];
			tmp[v * 8 + x] = s;
		}
	for (y = 0; y < 8; y++) {
		unsigned char *row = dst + (size_t)y * dw;
		for (x = 0; x < 8; x++) {
			float s = 0.0f;
			int val;
			for (v = 0; v < 8; v++)
				s += jp_idct_cos[v][y] * tmp[v * 8 + x];
			val = (int)(s + 128.5f);
			if (val < 0)
				val = 0;
			if (val > 255)
				val = 255;
			row[x] = (unsigned char)val;
		}
	}
}

static int jp_block(jp_ctx *c, jp_bits *b, jp_comp *cp, int *coef)
{
	const jp_huff *hd = &c->huff[cp->td];
	const jp_huff *ha = &c->huff[4 + cp->ta];
	const unsigned short *q = c->quant[cp->tq];
	int k, t;

	for (k = 0; k < 64; k++)
		coef[k] = 0;
	t = jp_huff_decode(hd, b);
	if (t < 0)
		return -1;
	cp->dcpred += jp_extend(jp_get(b, t), t);
	coef[0] = cp->dcpred * q[0];
	for (k = 1; k < 64; ) {
		int rs = jp_huff_decode(ha, b);
		int s, r;
		if (rs < 0)
			return -1;
		s = rs & 15;
		r = rs >> 4;
		if (s == 0) {
			if (r != 15)
				break;                    /* EOB */
			k += 16;
			continue;
		}
		k += r;
		if (k > 63)
			return -1;
		/* q is stored in natural order, so the zigzag index must be
		 * mapped through jp_zigzag here too -- using q[k] instead
		 * quantises with the wrong coefficient (smooth images look
		 * fine, detailed ones fall apart) */
		coef[jp_zigzag[k]] = jp_extend(jp_get(b, s), s) * q[jp_zigzag[k]];
		k++;
	}
	return 0;
}

static int jp_sof(jp_ctx *c, const unsigned char *p, unsigned len)
{
	unsigned i;

	if (len < 6)
		return -1;
	c->prec = p[0];
	c->h = (p[1] << 8) | p[2];
	c->w = (p[3] << 8) | p[4];
	c->ncomp = p[5];
	if (c->prec != 8) {
		jp_err(c, "only 8-bit JPEGs are supported");
		return -1;
	}
	if (c->w <= 0 || c->h <= 0 || c->w > JP_MAXDIM || c->h > JP_MAXDIM) {
		jp_err(c, "JPEG too large (max 2048x2048)");
		return -1;
	}
	if (c->ncomp != 1 && c->ncomp != 3) {
		jp_err(c, "only grayscale and YCbCr JPEGs are supported");
		return -1;
	}
	if (len < (unsigned)(6 + 3 * c->ncomp))
		return -1;
	for (i = 0; i < (unsigned)c->ncomp; i++) {
		jp_comp *cp = &c->comp[i];
		cp->id = p[6 + 3 * i];
		cp->hs = p[7 + 3 * i] >> 4;
		cp->vs = p[7 + 3 * i] & 15;
		cp->tq = p[8 + 3 * i];
		if (cp->hs < 1 || cp->hs > 4 || cp->vs < 1 || cp->vs > 4
		    || cp->tq >= JP_MAXQ)
			return -1;
		if (cp->hs > c->maxhs)
			c->maxhs = cp->hs;
		if (cp->vs > c->maxvs)
			c->maxvs = cp->vs;
	}
	c->mcus_x = (c->w + c->maxhs * 8 - 1) / (c->maxhs * 8);
	c->mcus_y = (c->h + c->maxvs * 8 - 1) / (c->maxvs * 8);
	for (i = 0; i < (unsigned)c->ncomp; i++) {
		jp_comp *cp = &c->comp[i];
		cp->dw = c->mcus_x * cp->hs * 8;
		cp->dh = c->mcus_y * cp->vs * 8;
		cp->plane = (unsigned char *)malloc((size_t)cp->dw * cp->dh);
		if (!cp->plane) {
			jp_err(c, "out of memory for the JPEG planes");
			return -1;
		}
	}
	return 0;
}

static int jp_dqt(jp_ctx *c, const unsigned char *p, unsigned len)
{
	unsigned i = 0;

	while (i < len) {
		int pq = p[i] >> 4, tq = p[i] & 15, k;
		i++;
		if (tq >= JP_MAXQ)
			return -1;
		for (k = 0; k < 64; k++) {
			unsigned v;
			if (pq) {
				if (i + 1 >= len)
					return -1;
				v = (unsigned)((p[i] << 8) | p[i + 1]);
				i += 2;
			} else {
				if (i >= len)
					return -1;
				v = p[i++];
			}
			c->quant[tq][jp_zigzag[k]] = (unsigned short)v;
		}
		c->have_q[tq] = 1;
	}
	return 0;
}

static int jp_dht(jp_ctx *c, const unsigned char *p, unsigned len)
{
	unsigned i = 0;

	while (i < len) {
		int tc, th, k, slot = 0;
		unsigned char lens[256], vals[256];
		unsigned total = 0;
		int counts[17];

		if (i + 17 > len)
			return -1;
		tc = p[i] >> 4;
		th = p[i] & 15;
		i++;
		if (tc > 1 || th > 3)
			return -1;
		for (k = 1; k <= 16; k++) {
			counts[k] = p[i + k - 1];
			total += (unsigned)counts[k];
		}
		i += 16;
		if (total > 256 || i + total > len)
			return -1;
		for (k = 0; k < 256; k++)
			lens[k] = 0;
		for (k = 1; k <= 16; k++) {
			int n;
			for (n = 0; n < counts[k]; n++) {
				lens[slot] = (unsigned char)k;
				slot++;
			}
		}
		for (k = 0; k < (int)total; k++)
			vals[k] = p[i + k];
		jp_huff_build(&c->huff[tc * 4 + th], lens, vals, (int)total);
		c->have_h[tc * 4 + th] = 1;
		i += total;
	}
	return 0;
}

static int jp_scan(jp_ctx *c, jp_bits *b)
{
	int coef[64];
	int mcux, mcuy;

	for (mcuy = 0; mcuy < c->mcus_y; mcuy++) {
		for (mcux = 0; mcux < c->mcus_x; mcux++) {
			int ci;

			if (c->restart_interval
			    && (mcux || mcuy)
			    && ((mcuy * c->mcus_x + mcux) % c->restart_interval) == 0) {
				jp_align(b);
				while (b->pos + 1 < b->n
				       && !(b->d[b->pos] == 0xFF
					    && b->d[b->pos + 1] >= 0xD0
					    && b->d[b->pos + 1] <= 0xD7))
					b->pos++;
				if (b->pos + 1 < b->n)
					b->pos += 2;
				for (ci = 0; ci < c->ncomp; ci++)
					c->comp[ci].dcpred = 0;
			}
			for (ci = 0; ci < c->ncomp; ci++) {
				jp_comp *cp = &c->comp[ci];
				int by, bx;

				for (by = 0; by < cp->vs; by++)
					for (bx = 0; bx < cp->hs; bx++) {
						int px = (mcux * cp->hs + bx) * 8;
						int py = (mcuy * cp->vs + by) * 8;
						if (jp_block(c, b, cp, coef)) {
							jp_err(c, "truncated JPEG data");
							return -1;
						}
						jp_idct(coef, cp->plane
							+ (size_t)py * cp->dw + px,
							cp->dw);
					}
			}
		}
	}
	if (b->bad && !c->err[0])
		jp_err(c, "truncated JPEG data");
	return 0;
}

static int jp_sos(jp_ctx *c, const unsigned char *p, unsigned len, jp_bits *b)
{
	int ns, i;

	if (len < 1)
		return -1;
	ns = p[0];
	if (ns != c->ncomp || len < (unsigned)(1 + 2 * ns + 3))
		return -1;
	for (i = 0; i < ns; i++) {
		int cs = p[1 + 2 * i];
		int tbl = p[2 + 2 * i];
		int k, found = 0;

		for (k = 0; k < c->ncomp; k++)
			if (c->comp[k].id == cs) {
				c->comp[k].td = tbl >> 4;
				c->comp[k].ta = tbl & 15;
				found = 1;
			}
		if (!found)
			return -1;
	}
	for (i = 0; i < ns; i++) {
		jp_comp *cp = &c->comp[i];
		if (cp->td > 3 || cp->ta > 3
		    || !c->have_h[cp->td] || !c->have_h[4 + cp->ta]
		    || !c->have_q[cp->tq]) {
			jp_err(c, "JPEG is missing a Huffman or quantisation table");
			return -1;
		}
		cp->dcpred = 0;
	}
	jp_align(b);
	return jp_scan(c, b);
}

/* One row of a component, upsampled to the full width.
 *
 * For the common 2x factors this is libjpeg's h2v2 "fancy" upsampler, expressed
 * the way libjpeg writes it: mix the two source rows 3:1 vertically (scaled by
 * 4), then mix neighbouring columns 3:1 horizontally, with the EVEN output pixel
 * taking its neighbour from the left and the ODD one from the right.  Getting
 * that last detail wrong (only filtering the odd pixels, as a first version did)
 * shifts every saturated colour edge by half a chroma sample -- visible as a
 * 100+ level error on synthetic red/blue borders while photos look fine.
 * Build with -DJPG_UPSAMPLE_NEAREST to see the difference for yourself.
 */
#define JP_COLSUM_MAX (JP_MAXDIM + 2)

static void jp_fetch_row(const jp_comp *cp, int maxhs, int maxvs, int full_w,
			 int y, unsigned char *out)
{
	static unsigned colsum[JP_COLSUM_MAX];
	int x, sy = y * cp->vs / maxvs;
	const unsigned char *row;
	const unsigned char *row2 = NULL;
	int two_y = (cp->vs * 2 == maxvs);
	int two_x = (cp->hs * 2 == maxhs);
	int nsrc;
	int even = (y % 2) == 0;

	/* libjpeg's vertical triangle, for a 2x reduction only: an even output row
	 * leans on the chroma row ABOVE it (1:3), an odd one on the row below
	 * (3:1) -- the row pairs are (k-1,k) and (k,k+1), not (k,k+1) twice.
	 * Sliding that window is what removes a half-sample vertical shift at
	 * colour edges; without it, 4:2:0 came out 6 levels off with 11% of pixels
	 * more than 32 levels wrong while photos still looked fine.  A component
	 * that is NOT subsampled vertically uses its own row untouched. */
	if (two_y) {
		row = cp->plane + (size_t)(even ? (sy > 0 ? sy - 1 : sy) : sy)
			* cp->dw;
		row2 = cp->plane
			+ (size_t)(even ? sy : (sy + 1 < cp->dh ? sy + 1 : sy))
			* cp->dw;
	} else {
		row = cp->plane + (size_t)sy * cp->dw;
		row2 = NULL;
	}
	(void)row2;
	nsrc = (full_w * cp->hs + maxhs - 1) / maxhs;
	if (nsrc > cp->dw)
		nsrc = cp->dw;
	if (nsrc > JP_COLSUM_MAX)
		nsrc = JP_COLSUM_MAX;

	for (x = 0; x < nsrc; x++) {
		unsigned a = row[x];
		if (two_y) {
			unsigned b = row2[x];
			colsum[x] = even ? (a + b * 3) : (a * 3 + b);
		} else {
			colsum[x] = a * 4;            /* keep the 4x scale */
		}
	}
	if (nsrc == 0) {
		for (x = 0; x < full_w; x++)
			out[x] = 0;
		return;
	}
	for (x = 0; x < full_w; x++) {
		int i, j;

		if (two_x) {
			i = x >> 1;
			if (i >= nsrc)
				i = nsrc - 1;
			if (x & 1)
				j = (i + 1 < nsrc) ? i + 1 : i;
			else
				j = (i > 0) ? i - 1 : i;
			out[x] = (unsigned char)((colsum[i] * 3 + colsum[j] + 8) >> 4);
		} else {
			i = x * cp->hs / maxhs;
			if (i >= nsrc)
				i = nsrc - 1;
			out[x] = (unsigned char)((colsum[i] + 2) >> 2);
		}
	}
}

static int jp_color(jp_ctx *c)
{
	int x, y;
	const jp_comp *yc = &c->comp[0];
	unsigned char *cb_row, *cr_row;

	if (c->ncomp == 1) {
		for (y = 0; y < c->h; y++) {
			unsigned *o = c->out + (size_t)y * c->w;
			int sy = y * yc->vs / c->maxvs;
			const unsigned char *row = yc->plane + (size_t)sy * yc->dw;
			for (x = 0; x < c->w; x++) {
				int sx = x * yc->hs / c->maxhs;
				unsigned v = row[sx];
				o[x] = 0xFF000000u | (v << 16) | (v << 8) | v;
			}
		}
		return 0;
	}
	cb_row = (unsigned char *)malloc((size_t)c->w);
	cr_row = (unsigned char *)malloc((size_t)c->w);
	if (!cb_row || !cr_row) {
		free(cb_row);
		free(cr_row);
		jp_err(c, "out of memory");
		return -1;
	}
	for (y = 0; y < c->h; y++) {
		unsigned *o = c->out + (size_t)y * c->w;
		int sy = y * yc->vs / c->maxvs;
		const unsigned char *yrow = yc->plane + (size_t)sy * yc->dw;

		jp_fetch_row(&c->comp[1], c->maxhs, c->maxvs, c->w, y, cb_row);
		jp_fetch_row(&c->comp[2], c->maxhs, c->maxvs, c->w, y, cr_row);
		for (x = 0; x < c->w; x++) {
			int sx = x * yc->hs / c->maxhs;
			int Y = yrow[sx];
			int cb = (int)cb_row[x] - 128;
			int cr = (int)cr_row[x] - 128;
			int r = (int)(Y + 1.402f * cr + 0.5f);
			int g = (int)(Y - 0.344136f * cb - 0.714136f * cr + 0.5f);
			int b = (int)(Y + 1.772f * cb + 0.5f);
			if (r < 0) r = 0;
			if (r > 255) r = 255;
			if (g < 0) g = 0;
			if (g > 255) g = 255;
			if (b < 0) b = 0;
			if (b > 255) b = 255;
			o[x] = 0xFF000000u | ((unsigned)r << 16)
				| ((unsigned)g << 8) | (unsigned)b;
		}
	}
	free(cb_row);
	free(cr_row);
	return 0;
}

static void jp_ctx_free(jp_ctx *c)
{
	int i;

	for (i = 0; i < JP_MAXCOMP; i++)
		if (c->comp[i].plane) {
			free(c->comp[i].plane);
			c->comp[i].plane = NULL;
		}
	if (c->out) {
		free(c->out);
		c->out = NULL;
	}
}

/* ---------------------------------------------------------------------------
 * plua_jpg_decode -- the entry point (see plua_pix.h)
 * ------------------------------------------------------------------------- */
unsigned *plua_jpg_decode(const unsigned char *data, unsigned len,
			  int *ow, int *oh, char *err, int errcap)
{
	static jp_ctx ctx;                    /* planes are freed before returning */
	jp_bits bits;
	unsigned pos = 0;
	int done = 0, saw_sof = 0, saw_sos = 0;

	ctx.d = data;
	ctx.n = len;
	ctx.err = err;
	ctx.errcap = errcap;
	ctx.w = ctx.h = ctx.ncomp = 0;
	ctx.prec = 0;
	ctx.maxhs = ctx.maxvs = 0;
	ctx.mcus_x = ctx.mcus_y = 0;
	ctx.restart_interval = 0;
	ctx.out = NULL;
	{
		int i;
		for (i = 0; i < JP_MAXCOMP; i++) {
			ctx.comp[i].plane = NULL;
			ctx.comp[i].dcpred = 0;
		}
		for (i = 0; i < JP_MAXQ; i++)
			ctx.have_q[i] = 0;
		for (i = 0; i < JP_MAXHUF; i++)
			ctx.have_h[i] = 0;
	}
	if (err && errcap > 0)
		err[0] = 0;
	if (len < 4 || data[0] != 0xFF || data[1] != JP_SOI) {
		jp_err(&ctx, "not a JPEG (no SOI marker)");
		return NULL;
	}
	pos = 2;
	while (pos + 1 < len && !done) {
		unsigned char marker;
		unsigned mlen;

		if (data[pos] != 0xFF) {
			pos++;
			continue;
		}
		marker = data[pos + 1];
		pos += 2;
		if (marker == 0xFF)
			continue;
		if (marker == JP_EOI) {
			done = 1;
			break;
		}
		if (marker == JP_SOI || (marker >= 0xD0 && marker <= 0xD7))
			continue;             /* standalone markers */
		if (pos + 1 >= len) {
			jp_err(&ctx, "truncated JPEG (marker length)");
			break;
		}
		mlen = (unsigned)((data[pos] << 8) | data[pos + 1]);
		if (mlen < 2 || pos + mlen > len) {
			jp_err(&ctx, "truncated JPEG segment");
			break;
		}
		switch (marker) {
		case JP_SOF0:
		case JP_SOF1:
			if (jp_sof(&ctx, data + pos + 2, mlen - 2)) {
				if (!err[0])
					jp_err(&ctx, "bad JPEG frame header");
				done = 1;
			}
			saw_sof = 1;
			break;
		case 0xC2:
			jp_err(&ctx, "progressive JPEG is not supported");
			done = 1;
			break;
		case 0xC3:
		case 0xC5:
		case 0xC6:
		case 0xC7:
		case 0xC9:
		case 0xCA:
		case 0xCB:
		case 0xCD:
		case 0xCE:
		case 0xCF:
			jp_err(&ctx, "unsupported JPEG coding (arithmetic/lossless)");
			done = 1;
			break;
		case JP_DQT:
			if (jp_dqt(&ctx, data + pos + 2, mlen - 2)) {
				jp_err(&ctx, "bad quantisation table");
				done = 1;
			}
			break;
		case JP_DHT:
			if (jp_dht(&ctx, data + pos + 2, mlen - 2)) {
				jp_err(&ctx, "bad Huffman table");
				done = 1;
			}
			break;
		case JP_DRI:
			if (mlen >= 4)
				ctx.restart_interval =
					(data[pos + 2] << 8) | data[pos + 3];
			break;
		case JP_SOS:
			if (!saw_sof) {
				jp_err(&ctx, "JPEG starts with a scan");
				done = 1;
				break;
			}
			ctx.out = (unsigned *)malloc((size_t)ctx.w * ctx.h
						     * sizeof(unsigned));
			if (!ctx.out) {
				jp_err(&ctx, "out of memory for the image");
				done = 1;
				break;
			}
			bits.d = data;
			bits.n = len;
			bits.pos = pos + mlen;
			bits.buf = 0;
			bits.bits = 0;
			bits.bad = 0;
			saw_sos = 1;
			if (jp_sos(&ctx, data + pos + 2, mlen - 2, &bits)
			    || jp_color(&ctx)) {
				if (!err[0])
					jp_err(&ctx, "JPEG decode failed");
				done = 1;
			} else {
				done = 1;    /* one scan is all a baseline file has */
			}
			break;
		default:
			break;               /* APPn, COM, ... : skipped */
		}
		pos += mlen;
	}
	if (!saw_sof || !saw_sos) {
		if (!err[0])
			jp_err(&ctx, "JPEG has no frame or no scan");
		jp_ctx_free(&ctx);
		return NULL;
	}
	if (err && err[0]) {
		jp_ctx_free(&ctx);
		return NULL;
	}
	if (ow)
		*ow = ctx.w;
	if (oh)
		*oh = ctx.h;
	{
		unsigned *out = ctx.out;
		int i;

		for (i = 0; i < JP_MAXCOMP; i++)
			if (ctx.comp[i].plane)
				free(ctx.comp[i].plane);
		ctx.out = NULL;
		return out;
	}
}
