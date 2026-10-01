/*
 * plua_png.c -- PNG decoder for primeLua's img library (see plua_pix.h).
 *
 * WHY NOT primetcc's rt/hp_image.c, WHICH ALSO DECODES PNG
 *   Because it is measurably wrong: decoding a PNG written by PIL returns every
 *   blue sample one level too high (tests/test_img.py pins that down now), and
 *   it rejects small streams that zlib and PIL both accept -- its row unfilter
 *   loops index the filter byte and the previous row with the same i, which is
 *   off by one for Sub/Up/Average/Paeth rows.  primeLua must not patch primetcc,
 *   so the port carries its own decoder, written the same way the JPEG one was:
 *   self-contained, no libm, no file I/O, checked against PIL pixel by pixel in
 *   tests/test_img.py (PNG is lossless, so a correct decoder matches EXACTLY --
 *   the test demands zero differing pixels).
 *
 * WHAT IT DECODES
 *   Colour types 0 (gray), 2 (RGB), 3 (palette), 4 (gray+alpha) and 6 (RGBA),
 *   bit depths 1/2/4/8/16 (sub-byte depths via the palette or a grayscale ramp),
 *   all five row filters, tRNS transparency for types 0/2/3, every zlib block
 *   type (stored, fixed and dynamic Huffman) with multiple IDAT chunks.
 *   Adam7 interlacing, 16-bit-per-channel *output* (the high byte is taken) and
 *   images above 1024 px per side are rejected with a message.
 *
 * Output is 0xAARRGGBB: alpha is preserved, so a transparent PNG works with
 * img.blit (the blitter alpha-blends) as well as with a colour key.
 */
#include <stddef.h>

#include "plua_pix.h"

void *malloc(size_t n);
void free(void *p);

#ifndef NULL
#define NULL ((void *)0)
#endif

#define PNG_MAXDIM  1024
#define PNG_LZ      32768

/* ---- deflate bit reader: bits are packed LSB first ---- */
typedef struct {
	const unsigned char *d;
	unsigned n, pos;
	unsigned bit;
	int bad;
} pn_bits;

static unsigned pn_bits_get(pn_bits *b, unsigned n)
{
	unsigned v = 0, i;

	for (i = 0; i < n; i++) {
		if (b->pos >= b->n) {
			b->bad = 1;
			return v;
		}
		v |= (unsigned)((b->d[b->pos] >> b->bit) & 1u) << i;
		if (++b->bit == 8) {
			b->bit = 0;
			b->pos++;
		}
	}
	return v;
}

/* canonical Huffman, decoded MSB-first over the LSB-first bit stream */
typedef struct {
	unsigned char bl[16];
	unsigned short order[288];
	int n;
} pn_huff;

static void pn_huff_build(pn_huff *h, const unsigned char *len, int n)
{
	unsigned next[16], first = 0;
	int i, l, o = 0;

	for (i = 0; i < 16; i++)
		h->bl[i] = 0;
	for (i = 0; i < n; i++)
		if (len[i])
			h->bl[len[i]]++;
	for (l = 1; l <= 15; l++) {
		next[l] = first;
		first = (first + h->bl[l]) << 1;
	}
	for (l = 1; l <= 15; l++)
		for (i = 0; i < n; i++)
			if (len[i] == l)
				h->order[o++] = (unsigned short)i;
	h->n = o;
}

static int pn_huff_decode(const pn_huff *h, pn_bits *b)
{
	unsigned cur = 0, first = 0, index = 0;
	int l;

	for (l = 1; l <= 15; l++) {
		cur = (cur << 1) | pn_bits_get(b, 1);
		if (cur - first < h->bl[l]) {
			unsigned k = index + (cur - first);
			if ((int)k >= h->n)
				return -1;
			return h->order[k];
		}
		index += h->bl[l];
		first = (first + h->bl[l]) << 1;
	}
	return -1;
}

/* ---- inflate into a caller-sized buffer ---- */
static const unsigned char pn_cl_order[19] =
	{ 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
static const unsigned short pn_len_base[29] =
	{ 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59,
	  67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const unsigned char pn_len_extra[29] =
	{ 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4,
	  5, 5, 5, 5, 0 };
static const unsigned short pn_dist_base[30] =
	{ 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385,
	  513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const unsigned char pn_dist_extra[30] =
	{ 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10,
	  10, 11, 11, 12, 12, 13, 13 };

/* returns the number of bytes written, or -1 */
static long pn_inflate(const unsigned char *in, unsigned inlen,
		       unsigned char *out, unsigned outcap)
{
	pn_bits b;
	unsigned char win[PNG_LZ];
	unsigned wpos = 0, outpos = 0;
	unsigned char lens[320];
	pn_huff lit, dist;
	int final = 0;

	if (inlen < 2)
		return -1;
	if ((in[0] & 0x0F) != 8 || ((in[0] << 8) | in[1]) % 31 != 0)
		return -1;
	b.d = in;
	b.n = inlen;
	b.pos = 2;
	b.bit = 0;
	b.bad = 0;
	{
		int i;
		for (i = 0; i < PNG_LZ; i++)
			win[i] = 0;
	}
	while (!final) {
		unsigned btype;

		final = (int)pn_bits_get(&b, 1);
		btype = pn_bits_get(&b, 2);
		if (btype == 0) {
			unsigned len;

			if (b.bit) {
				b.bit = 0;
				b.pos++;
			}
			if (b.pos + 4 > b.n)
				return -1;
			len = (unsigned)b.d[b.pos] | ((unsigned)b.d[b.pos + 1] << 8);
			b.pos += 4;
			if (b.pos + len > b.n)
				return -1;
			while (len--) {
				unsigned char c = b.d[b.pos++];
				if (outpos >= outcap)
					return -1;
				out[outpos++] = c;
				win[wpos++ & (PNG_LZ - 1)] = c;
			}
		} else if (btype == 1 || btype == 2) {
			int i;

			if (btype == 1) {
				for (i = 0; i < 288; i++)
					lens[i] = (unsigned char)
						(i < 144) ? 8
						: (i < 256) ? 9
						: (i < 280) ? 7 : 8;
				pn_huff_build(&lit, lens, 288);
				for (i = 0; i < 30; i++)
					lens[i] = 5;
				pn_huff_build(&dist, lens, 30);
			} else {
				unsigned hlit, hdist, hclen;
				unsigned char cll[19];
				pn_huff clh;
				unsigned k = 0;

				hlit = pn_bits_get(&b, 5) + 257;
				hdist = pn_bits_get(&b, 5) + 1;
				hclen = pn_bits_get(&b, 4) + 4;
				if (hlit > 286 || hdist > 30)
					return -1;
				for (i = 0; i < 19; i++)
					cll[i] = 0;
				for (i = 0; i < (int)hclen; i++)
					cll[pn_cl_order[i]] = (unsigned char)
						pn_bits_get(&b, 3);
				pn_huff_build(&clh, cll, 19);
				while (k < hlit + hdist) {
					int sym = pn_huff_decode(&clh, &b);
					int rep, val;

					if (sym < 0)
						return -1;
					if (sym < 16) {
						lens[k++] = (unsigned char)sym;
						continue;
					}
					if (sym == 16) {
						if (k == 0)
							return -1;
						val = lens[k - 1];
						rep = 3 + (int)pn_bits_get(&b, 2);
					} else if (sym == 17) {
						val = 0;
						rep = 3 + (int)pn_bits_get(&b, 3);
					} else {
						val = 0;
						rep = 11 + (int)pn_bits_get(&b, 7);
					}
					while (rep-- && k < hlit + hdist)
						lens[k++] = (unsigned char)val;
				}
				pn_huff_build(&lit, lens, (int)hlit);
				pn_huff_build(&dist, lens + hlit, (int)hdist);
			}
			for (;;) {
				int sym = pn_huff_decode(&lit, &b);
				unsigned len, dcode, d;

				if (sym < 0)
					return -1;
				if (sym < 256) {
					if (outpos >= outcap)
						return -1;
					out[outpos++] = (unsigned char)sym;
					win[wpos++ & (PNG_LZ - 1)] = (unsigned char)sym;
					continue;
				}
				if (sym == 256)
					break;
				sym -= 257;
				if (sym >= 29)
					return -1;
				len = pn_len_base[sym]
					+ pn_bits_get(&b, pn_len_extra[sym]);
				dcode = (unsigned)pn_huff_decode(&dist, &b);
				if (dcode >= 30)
					return -1;
				d = pn_dist_base[dcode]
					+ pn_bits_get(&b, pn_dist_extra[dcode]);
				while (len--) {
					unsigned char c =
						win[(wpos - d) & (PNG_LZ - 1)];
					if (outpos >= outcap)
						return -1;
					out[outpos++] = c;
					win[wpos++ & (PNG_LZ - 1)] = c;
				}
			}
		} else {
			return -1;
		}
		if (b.bad)
			return -1;
	}
	return (long)outpos;
}

/* ---- PNG container ---- */
static unsigned pn_be32(const unsigned char *p)
{
	return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16)
		| ((unsigned)p[2] << 8) | p[3];
}

static void pn_err(char *err, int cap, const char *msg)
{
	int i = 0;

	if (!err || cap <= 0)
		return;
	while (msg[i] && i < cap - 1) {
		err[i] = msg[i];
		i++;
	}
	err[i] = 0;
}

/*
 * plua_png_decode -- the entry point (see plua_pix.h).
 */
unsigned *plua_png_decode(const unsigned char *data, unsigned len,
			  int *ow, int *oh, char *err, int errcap)
{
	static const unsigned char sig[8] =
		{ 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
	unsigned w = 0, h = 0, depth = 0, ctype = 0, interlace = 0;
	unsigned char plte[256 * 3];
	unsigned char trns[256];
	unsigned plte_n = 0, trns_n = 0;
	unsigned char *idat = NULL;
	unsigned idat_len = 0, idat_cap = 0;
	unsigned char *raw = NULL;
	long raw_len;
	unsigned *out = NULL;
	size_t bpp, stride, raw_need;
	unsigned pos = 8, y, x;
	int i;

	if (err && errcap > 0)
		err[0] = 0;
	if (len < 8) {
		pn_err(err, errcap, "not a PNG (too short)");
		return NULL;
	}
	for (i = 0; i < 8; i++)
		if (data[i] != sig[i]) {
			pn_err(err, errcap, "not a PNG (bad signature)");
			return NULL;
		}
	for (i = 0; i < 256; i++)
		trns[i] = 255;

	while (pos + 8 <= len) {
		unsigned clen = pn_be32(data + pos);
		const unsigned char *type = data + pos + 4;
		const unsigned char *body = data + pos + 8;

		if (pos + 12 + clen > len) {
			pn_err(err, errcap, "truncated PNG chunk");
			goto fail;
		}
		if (type[0] == 'I' && type[1] == 'H' && type[2] == 'D'
		    && type[3] == 'R') {
			if (clen < 13) {
				pn_err(err, errcap, "bad IHDR");
				goto fail;
			}
			w = pn_be32(body);
			h = pn_be32(body + 4);
			depth = body[8];
			ctype = body[9];
			interlace = body[12];
			if (w == 0 || h == 0 || w > PNG_MAXDIM || h > PNG_MAXDIM) {
				pn_err(err, errcap, "PNG too large (max 1024x1024)");
				goto fail;
			}
			if (interlace) {
				pn_err(err, errcap, "interlaced (Adam7) PNG is not supported");
				goto fail;
			}
			if (ctype > 6 || ctype == 1 || ctype == 5) {
				pn_err(err, errcap, "bad PNG colour type");
				goto fail;
			}
			if (depth != 1 && depth != 2 && depth != 4 && depth != 8
			    && depth != 16) {
				pn_err(err, errcap, "bad PNG bit depth");
				goto fail;
			}
			if ((ctype == 2 || ctype == 4 || ctype == 6) && depth < 8) {
				pn_err(err, errcap, "bad PNG bit depth for colour");
				goto fail;
			}
			if (ctype == 3 && depth == 16) {
				pn_err(err, errcap, "palette PNG cannot be 16-bit");
				goto fail;
			}
		} else if (type[0] == 'P' && type[1] == 'L' && type[2] == 'T'
			   && type[3] == 'E') {
			plte_n = clen / 3;
			if (plte_n > 256)
				plte_n = 256;
			for (i = 0; i < (int)plte_n * 3; i++)
				plte[i] = body[i];
		} else if (type[0] == 't' && type[1] == 'R' && type[2] == 'N'
			   && type[3] == 'S') {
			trns_n = clen > 256 ? 256 : clen;
			for (i = 0; i < (int)trns_n; i++)
				trns[i] = body[i];
		} else if (type[0] == 'I' && type[1] == 'D' && type[2] == 'A'
			   && type[3] == 'T') {
			if (idat_len + clen > idat_cap) {
				unsigned ncap = (idat_len + clen) * 2 + 4096;
				unsigned char *nb = (unsigned char *)malloc(ncap);

				if (!nb) {
					pn_err(err, errcap, "out of memory");
					goto fail;
				}
				for (i = 0; i < (int)idat_len; i++)
					nb[i] = idat[i];
				if (idat)
					free(idat);
				idat = nb;
				idat_cap = ncap;
			}
			for (i = 0; i < (int)clen; i++)
				idat[idat_len + i] = body[i];
			idat_len += clen;
		} else if (type[0] == 'I' && type[1] == 'E' && type[2] == 'N'
			   && type[3] == 'D') {
			break;
		}
		pos += 12 + clen;
	}

	if (!w || !h || !idat_len) {
		pn_err(err, errcap, "PNG has no header or no image data");
		goto fail;
	}

	/* samples per pixel, and bytes per pixel for the row filters */
	{
		size_t channels = (ctype == 0 || ctype == 3) ? 1
			: (ctype == 2 ? 3 : (ctype == 4 ? 2 : 4));

		bpp = (channels * depth + 7) / 8;
		if (bpp == 0)
			bpp = 1;
	}
	stride = (size_t)w * bpp;                  /* filtered bytes per row */
	raw_need = (stride + 1) * h;
	raw = (unsigned char *)malloc((unsigned)raw_need);
	if (!raw) {
		pn_err(err, errcap, "out of memory for the PNG rows");
		goto fail;
	}
	raw_len = pn_inflate(idat, idat_len, raw, (unsigned)raw_need);
	if (raw_len < 0 || (size_t)raw_len < raw_need) {
		pn_err(err, errcap, "PNG inflate failed");
		goto fail;
	}

	out = (unsigned *)malloc((size_t)w * h * sizeof(unsigned));
	if (!out) {
		pn_err(err, errcap, "out of memory for the image");
		goto fail;
	}

	for (y = 0; y < h; y++) {
		unsigned char *row = raw + (stride + 1) * y;
		unsigned char *cur = row + 1;
		const unsigned char *prev = y ? (row - (stride + 1)) + 1 : NULL;
		unsigned filter = row[0];
		size_t k;

		switch (filter) {
		case 0:
			break;
		case 1:
			for (k = bpp; k < stride; k++)
				cur[k] = (unsigned char)(cur[k] + cur[k - bpp]);
			break;
		case 2:
			if (prev)
				for (k = 0; k < stride; k++)
					cur[k] = (unsigned char)(cur[k] + prev[k]);
			break;
		case 3:
			for (k = 0; k < stride; k++) {
				unsigned a = k >= bpp ? cur[k - bpp] : 0;
				unsigned b = prev ? prev[k] : 0;
				cur[k] = (unsigned char)(cur[k] + ((a + b) >> 1));
			}
			break;
		case 4:
			for (k = 0; k < stride; k++) {
				int a = k >= bpp ? cur[k - bpp] : 0;
				int b = prev ? prev[k] : 0;
				int c = (prev && k >= bpp) ? prev[k - bpp] : 0;
				int p = a + b - c;
				int pa = p > a ? p - a : a - p;
				int pb = p > b ? p - b : b - p;
				int pc = p > c ? p - c : c - p;
				int pr = (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
				cur[k] = (unsigned char)(cur[k] + pr);
			}
			break;
		default:
			pn_err(err, errcap, "bad PNG row filter");
			goto fail;
		}
	}

	/* unfiltered rows -> ARGB */
	for (y = 0; y < h; y++) {
		const unsigned char *cur = raw + (stride + 1) * y + 1;
		unsigned *dst = out + (size_t)y * w;

		for (x = 0; x < w; x++) {
			unsigned r, g, b, a = 255;

			if (ctype == 3) {
				unsigned idx;
				if (depth == 8)
					idx = cur[x];
				else if (depth == 4)
					idx = (cur[x >> 1] >> ((x & 1) ? 0 : 4)) & 15;
				else if (depth == 2)
					idx = (cur[x >> 2] >> (6 - 2 * (x & 3))) & 3;
				else
					idx = (cur[x >> 3] >> (7 - (x & 7))) & 1;
				if (idx >= plte_n) {
					r = g = b = 0;
				} else {
					r = plte[idx * 3];
					g = plte[idx * 3 + 1];
					b = plte[idx * 3 + 2];
					if (idx < trns_n)
						a = trns[idx];
				}
			} else if (ctype == 0 || ctype == 4) {
				unsigned v;
				int ch = (ctype == 4) ? 2 : 1;

				if (depth == 8)
					v = cur[x * ch];
				else if (depth == 16)
					v = cur[x * ch * 2];            /* high byte */
				else if (depth == 4)
					v = (cur[x >> 1] >> ((x & 1) ? 0 : 4)) & 15,
					v = v * 17;
				else if (depth == 2)
					v = ((cur[x >> 2] >> (6 - 2 * (x & 3))) & 3) * 85;
				else
					v = ((cur[x >> 3] >> (7 - (x & 7))) & 1) * 255;
				r = g = b = v;
				if (ctype == 4)
					a = (depth == 16) ? cur[x * 2 + 1] : cur[x * 2 + 1];
				if (ctype == 0 && trns_n >= 2) {
					unsigned key = ((unsigned)trns[0] << 8) | trns[1];
					if (v == key)
						a = 0;
				}
			} else {
				int step = (depth == 16) ? 2 : 1;
				const unsigned char *p = cur + (size_t)x * step
					* (ctype == 2 ? 3 : 4);

				r = p[0];
				g = p[step];
				b = p[step * 2];
				if (ctype == 6)
					a = p[step * 3];
				else if (trns_n >= 6) {
					unsigned kr = ((unsigned)trns[0] << 8) | trns[1];
					unsigned kg = ((unsigned)trns[2] << 8) | trns[3];
					unsigned kb = ((unsigned)trns[4] << 8) | trns[5];
					if (r == kr && g == kg && b == kb)
						a = 0;
				}
			}
			dst[x] = (a << 24) | (r << 16) | (g << 8) | b;
		}
	}

	if (ow)
		*ow = (int)w;
	if (oh)
		*oh = (int)h;
	free(idat);
	free(raw);
	return out;

fail:
	if (idat)
		free(idat);
	if (raw)
		free(raw);
	if (out)
		free(out);
	return NULL;
}
