/*
 * plua_pix.h -- the pixel buffer every img decoder produces.
 *
 * One format for the whole image path: w*h pixels, 0xAARRGGBB, rows top to
 * bottom, allocated with the runtime's malloc (the firmware heap) so the caller
 * can hand the buffer straight to hp_blit / hp_blit_scaled, or copy it into the
 * px array of an hp_grob for the img library's blit functions.
 *
 * PNG comes from primetcc's rt/hp_image.c (see plua_img.c); JPEG from
 * port/plua_jpg.c.  Both are display-grade decoders: they report a reason on
 * failure and never abort.
 */
#ifndef PLUA_PIX_H
#define PLUA_PIX_H

/* Decode a baseline JPEG.  Returns a malloc'd ARGB buffer (free() it) or NULL,
 * with *w and *h set on success and a human-readable reason in err (never
 * truncated, err[0] is cleared first). */
unsigned *plua_jpg_decode(const unsigned char *data, unsigned len,
			  int *w, int *h, char *err, int errcap);

/* Decode a PNG: same contract, alpha preserved (0xAARRGGBB).  primeLua carries
 * its own decoder because primetcc's (rt/hp_image.c) returns a one-level-high
 * blue channel and rejects valid small streams -- see plua_png.c. */
unsigned *plua_png_decode(const unsigned char *data, unsigned len,
			  int *w, int *h, char *err, int errcap);

#endif /* PLUA_PIX_H */
