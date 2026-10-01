/*
 * plua_format.c -- primeLua's printf engine.
 *
 * Why this exists rather than reusing primetcc's runtime engine: Lua prints
 * every number with "%.14g" (LUA_NUMBER_FMT) and string.format() passes
 * arbitrary C formats straight through, so print()/tostring() fidelity is the
 * single most visible behaviour of the interpreter.  The TCC runtime's engine
 * was built for demos: its %g ignores the precision and emits the shortest
 * round-tripping text, %f stops at 15 decimals, ties round away from zero,
 * and anything at or above 1e19 prints as the literal string ">1e19".  A Lua
 * that renders 1e20 as ">1e19" is not a Lua port, so primeLua carries its own
 * C99 formatter.
 *
 * The numeric core is exact rather than approximate.  Every finite double is
 * a finite decimal: x = m * 2^e, so for e < 0 the value is exactly
 * (m * 5^-e) / 10^-e.  Computing m * 5^-e as a big integer therefore yields
 * ALL the exact decimal digits of the value (at most ~1080 of them), and
 * rounding to any requested precision is plain decimal rounding on known
 * digits -- no double rounding, no sticky-bit guesswork, ties resolved the way
 * glibc resolves them (round half to even).  That is what lets the
 * differential tests against the host `lua` binary match digit for digit.
 *
 * Supported: flags - + <space> # 0, width and precision (both via '*'),
 * length modifiers h hh l ll z t j, conversions d i u o x X c s p f F e E
 * g G a A %, and real snprintf semantics (the return value is the full length
 * even when the output was truncated, and the buffer is always terminated).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stddef.h>

/* ---------------------------------------------------------------------------
 * big integers: little-endian 32-bit limbs.
 *
 * 4096 bits is not arbitrary: the biggest value that must be represented is
 * m * 5^1074 for the smallest subnormal, a little under 2^2547, and the
 * longest exact expansion is a few hundred digits past that.
 * ------------------------------------------------------------------------- */
#define BN_LIMBS	128		/* 128 * 32 = 4096 bits */

typedef struct {
	int n;				/* used limbs; 0 means zero */
	unsigned int w[BN_LIMBS];
} bn;

static void bn_zero(bn *a)
{
	a->n = 0;
}

static void bn_set_u64(bn *a, unsigned long long v)
{
	a->n = 0;
	while (v) {
		a->w[a->n++] = (unsigned int)(v & 0xffffffffu);
		v >>= 32;
	}
}

static int bn_mul_small(bn *a, unsigned int m)
{
	unsigned long long carry = 0;
	int i;
	for (i = 0; i < a->n; i++) {
		unsigned long long cur = (unsigned long long)a->w[i] * m + carry;
		a->w[i] = (unsigned int)(cur & 0xffffffffu);
		carry = cur >> 32;
	}
	while (carry) {
		if (a->n >= BN_LIMBS)
			return -1;
		a->w[a->n++] = (unsigned int)(carry & 0xffffffffu);
		carry >>= 32;
	}
	return 0;
}

static int bn_shl(bn *a, int bits)
{
	int limbs = bits / 32, rest = bits % 32, i;
	if (a->n == 0)
		return 0;
	if (a->n + limbs + 1 > BN_LIMBS)
		return -1;
	if (limbs) {
		for (i = a->n - 1; i >= 0; i--)
			a->w[i + limbs] = a->w[i];
		for (i = 0; i < limbs; i++)
			a->w[i] = 0;
		a->n += limbs;
	}
	if (rest) {
		unsigned int carry = 0;
		for (i = limbs; i < a->n; i++) {
			unsigned int nw = (a->w[i] << rest) | carry;
			carry = a->w[i] >> (32 - rest);
			a->w[i] = nw;
		}
		if (carry) {
			if (a->n >= BN_LIMBS)
				return -1;
			a->w[a->n++] = carry;
		}
	}
	return 0;
}

static int bn_is_zero(const bn *a)
{
	return a->n == 0;
}

static unsigned int bn_divmod_small(bn *a, unsigned int d)
{
	unsigned long long rem = 0;
	int i;
	for (i = a->n - 1; i >= 0; i--) {
		unsigned long long cur = (rem << 32) | a->w[i];
		a->w[i] = (unsigned int)(cur / d);
		rem = cur % d;
	}
	while (a->n > 0 && a->w[a->n - 1] == 0)
		a->n--;
	return (unsigned int)rem;
}

/* ---------------------------------------------------------------------------
 * exact decimal expansion of a double
 *
 * value = 0.d[0] d[1] ... * 10^decpt, digits exact, trailing zeros included.
 * ------------------------------------------------------------------------- */
#define DEC_MAX		1120

struct decnum {
	char d[DEC_MAX];
	int nd;			/* exact digit count (0 means "the value is 0") */
	int decpt;		/* value = 0.d * 10^decpt */
	int sign;
	int kind;		/* 0 finite, 1 inf, 2 nan */
};

static int bn_to_decimal(const bn *a, char *out)
{
	static const unsigned int pow10_9 = 1000000000u;
	bn t = *a;
	char tmp[DEC_MAX];
	int n = 0, i;
	if (bn_is_zero(a))
		return 0;
	while (!bn_is_zero(&t)) {
		unsigned int rem = bn_divmod_small(&t, pow10_9);
		for (i = 0; i < 9; i++) {
			if (n >= DEC_MAX)
				return -1;
			tmp[n++] = (char)('0' + rem % 10);
			rem /= 10;
		}
	}
	while (n > 1 && tmp[n - 1] == '0')
		n--;				/* drop the group's leading zeros */
	for (i = 0; i < n; i++)
		out[i] = tmp[n - 1 - i];
	return n;
}

static int decompose(double x, struct decnum *v)
{
	union { double d; unsigned long long u; } c;
	unsigned long long m;
	int exp, e, j, nd;
	bn big;

	v->nd = 0;
	v->decpt = 1;
	v->sign = 0;
	v->kind = 0;

	c.d = x;
	v->sign = (int)(c.u >> 63);
	exp = (int)((c.u >> 52) & 0x7ff);
	m = c.u & 0xfffffffffffffULL;

	if (exp == 0x7ff) {
		v->kind = (m == 0) ? 1 : 2;
		return 0;
	}
	if (exp == 0) {
		if (m == 0)			/* signed zero */
			return 0;
		e = -1074;			/* subnormal */
	} else {
		m |= 1ULL << 52;
		e = exp - 1075;
	}

	bn_zero(&big);
	bn_set_u64(&big, m);

	if (e >= 0) {
		if (bn_shl(&big, e) < 0)	/* x = m * 2^e, an integer */
			return -1;
		nd = bn_to_decimal(&big, v->d);
		if (nd < 0)
			return -1;
		v->nd = nd;
		v->decpt = nd;
	} else {
		j = -e;				/* x = (m * 5^j) / 10^j */
		while (j >= 13) {
			if (bn_mul_small(&big, 1220703125u) < 0)
				return -1;
			j -= 13;
		}
		while (j-- > 0)
			if (bn_mul_small(&big, 5u) < 0)
				return -1;
		nd = bn_to_decimal(&big, v->d);
		if (nd < 0)
			return -1;
		v->nd = nd;
		v->decpt = nd - (-e);
	}
	return 0;
}

static int dec_digit(const struct decnum *v, int i)
{
	if (i < 0 || i >= v->nd)
		return 0;
	return v->d[i] - '0';
}

static int dec_nonzero_from(const struct decnum *v, int i)
{
	for (; i < v->nd; i++)
		if (v->d[i] != '0')
			return 1;
	return 0;
}

/* round to n significant digits, half to even */
static void dec_round(struct decnum *v, int n)
{
	int i, carry;
	if (n >= v->nd || n < 0)
		return;
	{
		int drop = dec_digit(v, n);
		int rest = dec_nonzero_from(v, n + 1);
		int keep = (n >= 1) ? dec_digit(v, n - 1) : 0;
		carry = (drop > 5 || (drop == 5 && (rest || (keep & 1))));
	}
	for (i = n - 1; i >= 0 && carry; i--) {
		int dg = dec_digit(v, i) + 1;
		if (dg == 10)
			v->d[i] = '0';
		else {
			v->d[i] = (char)('0' + dg);
			carry = 0;
		}
	}
	v->nd = n;
	if (carry) {				/* 999 -> 1000 */
		memmove(v->d + 1, v->d, (size_t)n);
		v->d[0] = '1';
		v->nd = n + 1;
		v->decpt++;
	}
}

/* ---------------------------------------------------------------------------
 * output sink: bounded writes, exact length accounting, so snprintf can
 * truncate without overflowing and still return the full length.
 * ------------------------------------------------------------------------- */
struct sink {
	char *buf;
	size_t cap;			/* bytes available including the NUL */
	size_t len;			/* characters produced */
};

static void out_ch(struct sink *s, char c)
{
	if (s->len + 1 < s->cap)
		s->buf[s->len] = c;
	s->len++;
}

static void out_rep(struct sink *s, char c, int n)
{
	int i;
	for (i = 0; i < n; i++)
		out_ch(s, c);
}

static void out_mem(struct sink *s, const char *p, size_t n)
{
	size_t i;
	for (i = 0; i < n; i++)
		out_ch(s, p[i]);
}

static void out_str(struct sink *s, const char *p)
{
	out_mem(s, p, strlen(p));
}

/* ---------------------------------------------------------------------------
 * integers
 * ------------------------------------------------------------------------- */
static void out_digits(struct sink *s, unsigned long long v, unsigned base,
		       int upper)
{
	char tmp[72];
	const char *dig = upper ? "0123456789ABCDEF" : "0123456789abcdef";
	int n = 0;
	if (v == 0) {
		tmp[n++] = '0';
	} else {
		while (v) {
			tmp[n++] = dig[v % base];
			v /= base;
		}
	}
	while (n > 0)
		out_ch(s, tmp[--n]);
}

static void fmt_int(struct sink *s, unsigned long long mag, int neg,
		    unsigned base, int upper, int prec, int width, int zero,
		    int plus, int space, int alt, int minus)
{
	char dig[72];
	char prefix[4];
	int ndig = 0, npre = 0, i, total, pad;
	unsigned long long t = mag;

	if (t == 0) {
		if (prec != 0)
			dig[ndig++] = '0';
	} else {
		char tmp[72];
		const char *dg = upper ? "0123456789ABCDEF" : "0123456789abcdef";
		while (t) {
			tmp[ndig++] = dg[t % base];
			t /= base;
		}
		for (i = 0; i < ndig; i++)
			dig[i] = tmp[ndig - 1 - i];
	}
	while (ndig < prec) {			/* precision pads with zeros */
		memmove(dig + 1, dig, (size_t)ndig);
		dig[0] = '0';
		ndig++;
	}

	if (neg)
		prefix[npre++] = '-';
	else if (plus)
		prefix[npre++] = '+';
	else if (space)
		prefix[npre++] = ' ';
	if (alt && base == 16 && mag != 0) {
		prefix[npre++] = '0';
		prefix[npre++] = upper ? 'X' : 'x';
	}
	if (alt && base == 8 && (ndig == 0 || dig[0] != '0'))
		prefix[npre++] = '0';

	if (prec >= 0)
		zero = 0;			/* '0' is ignored with a precision */

	total = npre + ndig;
	pad = (width > total) ? width - total : 0;
	if (minus) {				/* left justified */
		out_mem(s, prefix, (size_t)npre);
		out_mem(s, dig, (size_t)ndig);
		out_rep(s, ' ', pad);
		return;
	}
	if (zero) {				/* right justified, zero filled */
		out_mem(s, prefix, (size_t)npre);
		out_rep(s, '0', pad);
	} else {				/* right justified, space filled */
		out_rep(s, ' ', pad);
		out_mem(s, prefix, (size_t)npre);
	}
	out_mem(s, dig, (size_t)ndig);
}

/* ---------------------------------------------------------------------------
 * floating point
 *
 * All three styles emit straight into the sink rather than into a staging
 * buffer: a format like "%.4000f" has to produce (and count) 4000 characters
 * without allocating anything.
 * ------------------------------------------------------------------------- */

static const char *sign_prefix(int neg, int plus, int space)
{
	return neg ? "-" : (plus ? "+" : (space ? " " : ""));
}

static void fmt_infnan(struct sink *s, const struct decnum *v, int upper,
		       int width, int minus, int plus, int space)
{
	const char *pref = sign_prefix(v->sign, plus, space);
	const char *word = (v->kind == 1) ? (upper ? "INF" : "inf")
					  : (upper ? "NAN" : "nan");
	int total = (int)strlen(pref) + 3;
	int pad = (width > total) ? width - total : 0;
	if (!minus)
		out_rep(s, ' ', pad);
	out_str(s, pref);
	out_str(s, word);
	if (minus)
		out_rep(s, ' ', pad);
}

/* %f: prec digits after the point, rounded exactly at that position */
static void fmt_f(struct sink *s, const struct decnum *v, int prec, int width,
		  int zero, int plus, int space, int alt, int minus, int upper)
{
	struct decnum r = *v;
	int i, q, intlen, fraclen, total, pad, head;

	if (v->kind) {
		fmt_infnan(s, v, upper, width, minus, plus, space);
		return;
	}
	q = r.decpt + prec;			/* significant digits to keep */
	if (r.nd == 0) {
		/* value is zero: nothing to round */
	} else if (q > 0) {
		dec_round(&r, q);
	} else if (q == 0) {
		/* the value sits right at the rounding position: rounding up
		 * gives one unit at 10^-prec, i.e. digits "1" with the decimal
		 * point one further right */
		int d0 = dec_digit(&r, 0);
		int rest = dec_nonzero_from(&r, 1);
		if (d0 > 5 || (d0 == 5 && rest)) {
			r.d[0] = '1';
			r.nd = 1;
			r.decpt = 1 - prec;
		} else {
			r.nd = 0;
		}
	} else {
		r.nd = 0;			/* far too small to register */
	}

	if (r.nd == 0)
		r.decpt = 0;

	head = (v->sign || plus || space) ? 1 : 0;
	intlen = (r.decpt > 0) ? r.decpt : 1;
	fraclen = (prec > 0 || alt) ? prec + 1 : 0;
	total = head + intlen + fraclen;
	pad = (width > total) ? width - total : 0;

	if (minus) {				/* left justified: '0' ignored */
		out_str(s, sign_prefix(v->sign, plus, space));
		if (r.decpt <= 0) {
			out_ch(s, '0');
		} else {
			for (i = 0; i < r.decpt; i++)
				out_ch(s, (char)('0' + dec_digit(&r, i)));
		}
		if (fraclen) {
			out_ch(s, '.');
			for (i = 0; i < prec; i++)
				out_ch(s, (char)('0' + dec_digit(&r, r.decpt + i)));
		}
		out_rep(s, ' ', pad);
	} else {
		if (!zero)
			out_rep(s, ' ', pad);
		out_str(s, sign_prefix(v->sign, plus, space));
		if (zero)
			out_rep(s, '0', pad);
		if (r.decpt <= 0) {
			out_ch(s, '0');
		} else {
			for (i = 0; i < r.decpt; i++)
				out_ch(s, (char)('0' + dec_digit(&r, i)));
		}
		if (fraclen) {
			out_ch(s, '.');
			for (i = 0; i < prec; i++)
				out_ch(s, (char)('0' + dec_digit(&r, r.decpt + i)));
		}
	}
}

/* %e: prec digits after the point, i.e. prec+1 significant digits */
static void fmt_e(struct sink *s, const struct decnum *v, int prec, int width,
		  int zero, int plus, int space, int alt, int upper, int minus)
{
	struct decnum r = *v;
	int i, exp10, head, total, pad, exlen;
	char expbuf[16];
	int en = 0;

	if (v->kind) {
		fmt_infnan(s, v, upper, width, minus, plus, space);
		return;
	}
	if (r.nd != 0)
		dec_round(&r, prec + 1);
	exp10 = (r.nd == 0) ? 0 : r.decpt - 1;

	expbuf[en++] = (upper ? 'E' : 'e');
	if (exp10 < 0) {
		expbuf[en++] = '-';
		exp10 = -exp10;
	} else {
		expbuf[en++] = '+';
	}
	{
		char tmp[8];
		int tn = 0;
		if (exp10 == 0)
			tmp[tn++] = '0';
		while (exp10) {
			tmp[tn++] = (char)('0' + exp10 % 10);
			exp10 /= 10;
		}
		while (tn < 2)
			tmp[tn++] = '0';
		while (tn > 0)
			expbuf[en++] = tmp[--tn];
	}
	exlen = en;

	head = (v->sign || plus || space) ? 1 : 0;
	total = head + 1 + ((prec > 0 || alt) ? prec + 1 : 0) + exlen;
	pad = (width > total) ? width - total : 0;

	if (minus) {
		out_str(s, sign_prefix(v->sign, plus, space));
		out_ch(s, (char)('0' + dec_digit(&r, 0)));
		if (prec > 0 || alt) {
			out_ch(s, '.');
			for (i = 0; i < prec; i++)
				out_ch(s, (char)('0' + dec_digit(&r, 1 + i)));
		}
		out_mem(s, expbuf, (size_t)exlen);
		out_rep(s, ' ', pad);
	} else {
		if (!zero)
			out_rep(s, ' ', pad);
		out_str(s, sign_prefix(v->sign, plus, space));
		if (zero)
			out_rep(s, '0', pad);
		out_ch(s, (char)('0' + dec_digit(&r, 0)));
		if (prec > 0 || alt) {
			out_ch(s, '.');
			for (i = 0; i < prec; i++)
				out_ch(s, (char)('0' + dec_digit(&r, 1 + i)));
		}
		out_mem(s, expbuf, (size_t)exlen);
	}
}

/* %g: %e or %f depending on the exponent, trailing zeros removed */
static void fmt_g(struct sink *s, const struct decnum *v, int prec, int width,
		  int zero, int plus, int space, int alt, int upper, int minus)
{
	struct decnum r = *v;
	int p = (prec < 0) ? 6 : (prec == 0 ? 1 : prec);
	int x;

	if (v->kind) {
		fmt_infnan(s, v, upper, width, minus, plus, space);
		return;
	}
	if (r.nd != 0)
		dec_round(&r, p);		/* X is taken AFTER rounding */
	x = (r.nd == 0) ? 0 : r.decpt - 1;

	if (x >= -4 && x < p) {
		int fprec;
		if (alt) {
			fprec = p - 1 - x;	/* '#' keeps the trailing zeros */
		} else {
			/* drop trailing zeros, then derive the number of
			 * fractional digits the remaining digits need -- passing
			 * a fixed precision to fmt_f would re-add them as
			 * padding.  Digits left of the point are never dropped. */
			while (r.nd > r.decpt && r.nd > 1 && r.d[r.nd - 1] == '0')
				r.nd--;
			fprec = r.nd - r.decpt;
			if (fprec < 0)
				fprec = 0;
		}
		fmt_f(s, &r, fprec, width, zero, plus, space, alt, minus,
		      upper);
	} else {
		int eprec;
		if (alt) {
			eprec = p - 1;
		} else {
			while (r.nd > 1 && r.d[r.nd - 1] == '0')
				r.nd--;
			eprec = r.nd - 1;
			if (eprec < 0)
				eprec = 0;
		}
		fmt_e(s, &r, eprec, width, zero, plus, space, alt, upper, minus);
	}
}

/* %a: exact hexadecimal significand, the way glibc prints it */
static void fmt_a(struct sink *s, double x, int prec, int width, int zero,
		  int plus, int space, int alt, int upper, int minus)
{
	union { double d; unsigned long long u; } c;
	struct decnum v;
	char dig[14];
	const char *hexd = upper ? "0123456789ABCDEF" : "0123456789abcdef";
	int neg, ex, lead, ndig, i, head, total, pad, exlen;
	char expbuf[16];
	int en = 0;
	unsigned long long m;

	v.kind = 0;
	v.sign = 0;
	c.d = x;
	neg = (int)(c.u >> 63);
	ex = (int)((c.u >> 52) & 0x7ff);
	m = c.u & 0xfffffffffffffULL;

	if (ex == 0x7ff) {
		v.sign = neg;
		v.kind = (m == 0) ? 1 : 2;
		fmt_infnan(s, &v, upper, width, minus, plus, space);
		return;
	}
	if (ex == 0) {
		lead = 0;
		ex = -1022;
		if (m == 0)
			ex = 0;			/* glibc prints 0x0p+0 for zero */
	} else {
		lead = 1;
		ex -= 1023;
	}
	for (i = 0; i < 13; i++)
		dig[i] = hexd[(m >> (4 * (12 - i))) & 0xf];

	if (prec >= 0 && prec < 13) {
		int drop = (int)((m >> (4 * (12 - prec))) & 0xf);
		int rest = 0;
		unsigned long long kept = (prec == 0) ? (unsigned long long)lead
			: (m >> (4 * (13 - prec)));
		for (i = 4 * (12 - prec) - 1; i >= 0; i--)
			if ((m >> i) & 1) {
				rest = 1;
				break;
			}
		ndig = prec;
		if (drop > 8 || (drop == 8 && (rest || (kept & 1)))) {
			unsigned long long keep = kept;
			keep++;
			if (prec == 0 || (keep >> (4 * prec))) {
				/* carry out of the significand: glibc does NOT
				 * renormalize, it prints 0x2p+(e-1), which is the
				 * same value as 0x1p+e. */
				lead++;
				m = 0;
			} else {
				m = keep << (4 * (13 - prec));
			}
			for (i = 0; i < 13; i++)
				dig[i] = hexd[(m >> (4 * (12 - i))) & 0xf];
		}
	} else if (prec >= 13) {
		ndig = 13;
	} else {
		ndig = 13;
		while (ndig > 0 && dig[ndig - 1] == '0')
			ndig--;				/* shortest exact form */
	}

	expbuf[en++] = upper ? 'P' : 'p';
	if (ex < 0) {
		expbuf[en++] = '-';
		ex = -ex;
	} else {
		expbuf[en++] = '+';
	}
	{
		char tmp[8];
		int tn = 0;
		if (ex == 0)
			tmp[tn++] = '0';
		while (ex) {
			tmp[tn++] = (char)('0' + ex % 10);
			ex /= 10;
		}
		while (tn > 0)
			expbuf[en++] = tmp[--tn];
	}
	exlen = en;

	head = (neg || plus || space) ? 1 : 0;
	total = head + 3 + ((ndig > 0 || alt) ? ndig + 1 : 0) + exlen;
	pad = (width > total) ? width - total : 0;

	if (minus) {
		out_str(s, sign_prefix(neg, plus, space));
		out_ch(s, '0');
		out_ch(s, upper ? 'X' : 'x');
		out_ch(s, (char)('0' + lead));
		if (ndig > 0 || alt) {
			out_ch(s, '.');
			for (i = 0; i < ndig; i++)
				out_ch(s, dig[i]);
		}
		out_mem(s, expbuf, (size_t)exlen);
		out_rep(s, ' ', pad);
	} else {
		if (!zero)
			out_rep(s, ' ', pad);
		out_str(s, sign_prefix(neg, plus, space));
		out_ch(s, '0');
		out_ch(s, upper ? 'X' : 'x');
		if (zero)			/* glibc pads between "0x" and the
					 * leading digit */
			out_rep(s, '0', pad);
		out_ch(s, (char)('0' + lead));
		if (ndig > 0 || alt) {
			out_ch(s, '.');
			for (i = 0; i < ndig; i++)
				out_ch(s, dig[i]);
		}
		out_mem(s, expbuf, (size_t)exlen);
	}
}

/* ---------------------------------------------------------------------------
 * the engine
 * ------------------------------------------------------------------------- */
static int plua_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
	struct sink s;
	const char *p;

	s.buf = buf;
	s.cap = size;
	s.len = 0;

	for (p = fmt; *p; p++) {
		int zero = 0, minus = 0, plus = 0, space = 0, alt = 0;
		int width = 0, prec = -1, lmod = 0;
		char conv;

		if (*p != '%') {
			out_ch(&s, *p);
			continue;
		}
		p++;
		for (;; p++) {
			if (*p == '0') zero = 1;
			else if (*p == '-') minus = 1;
			else if (*p == '+') plus = 1;
			else if (*p == ' ') space = 1;
			else if (*p == '#') alt = 1;
			else break;
		}
		if (*p == '*') {
			width = va_arg(ap, int);
			if (width < 0) {
				minus = 1;
				width = -width;
			}
			p++;
		} else {
			while (*p >= '0' && *p <= '9')
				width = width * 10 + (*p++ - '0');
		}
		if (*p == '.') {
			p++;
			prec = 0;
			if (*p == '*') {
				prec = va_arg(ap, int);
				if (prec < 0)
					prec = -1;
				p++;
			} else {
				while (*p >= '0' && *p <= '9')
					prec = prec * 10 + (*p++ - '0');
			}
		}
		switch (*p) {
		case 'h':
			p++;
			if (*p == 'h') {
				p++;
				lmod = 5;	/* signed/unsigned char */
			} else {
				lmod = 6;	/* short */
			}
			break;
		case 'l':
			p++;
			if (*p == 'l') {
				p++;
				lmod = 2;
			} else {
				lmod = 1;
			}
			break;
		case 'z':
			lmod = 3;
			p++;
			break;
		case 't':
			lmod = 4;
			p++;
			break;
		case 'j':
			lmod = 2;		/* long long covers intmax_t here */
			p++;
			break;
		default:
			break;
		}
		conv = *p;
		if (conv == 0)
			break;

		switch (conv) {
		case '%':
			out_ch(&s, '%');
			break;

		case 'd': case 'i': {
			long long v;
			if (lmod == 2)
				v = va_arg(ap, long long);
			else if (lmod == 1)
				v = (long long)va_arg(ap, long);
			else if (lmod == 3)
				/* %zd is the SIGNED size_t (ssize_t).  Reading it as
				 * an unsigned size_t zero-extends a negative value
				 * into a huge positive one -- invisible on a 64-bit
				 * host, wrong on the 32-bit target. */
				v = (long long)va_arg(ap, ptrdiff_t);
			else if (lmod == 4)
				v = (long long)va_arg(ap, ptrdiff_t);
			else if (lmod == 5)		/* %hhd: truncate to char */
				v = (long long)(signed char)va_arg(ap, int);
			else if (lmod == 6)		/* %hd: truncate to short */
				v = (long long)(short)va_arg(ap, int);
			else
				v = (long long)va_arg(ap, int);
			{
			unsigned long long mag = (v < 0)
				? (unsigned long long)(-(v + 1)) + 1ULL
				: (unsigned long long)v;
			fmt_int(&s, mag, v < 0, 10, 0, prec, width, zero, plus,
				space, 0, minus);
			}
			break;
		}
		case 'u': case 'o': case 'x': case 'X': {
			unsigned long long v;
			if (lmod == 2)
				v = va_arg(ap, unsigned long long);
			else if (lmod == 1)
				v = (unsigned long long)va_arg(ap, unsigned long);
			else if (lmod == 3 || lmod == 4)
				v = (unsigned long long)va_arg(ap, size_t);
			else if (lmod == 5)
				v = (unsigned long long)(unsigned char)va_arg(ap, unsigned int);
			else if (lmod == 6)
				v = (unsigned long long)(unsigned short)va_arg(ap, unsigned int);
			else
				v = (unsigned long long)va_arg(ap, unsigned int);
			unsigned base = (conv == 'o') ? 8 : (conv == 'u') ? 10 : 16;
			fmt_int(&s, v, 0, base, conv == 'X', prec, width, zero, 0, 0,
				alt, minus);
			break;
		}
		case 'p': {
			void *ptr = va_arg(ap, void *);
			if (ptr == 0) {
				out_str(&s, "(nil)");
			} else {
				out_str(&s, "0x");
				out_digits(&s, (unsigned long long)(unsigned long)ptr,
					   16, 0);
			}
			break;
		}
		case 'c': {
			int c = va_arg(ap, int);
			int pad = (width > 1) ? width - 1 : 0;
			if (!minus)
				out_rep(&s, ' ', pad);
			out_ch(&s, (char)c);
			if (minus)
				out_rep(&s, ' ', pad);
			break;
		}
		case 's': {
			const char *str = va_arg(ap, const char *);
			size_t n, pad;
			if (str == 0)
				str = "(null)";
			n = strlen(str);
			if (prec >= 0 && (size_t)prec < n)
				n = (size_t)prec;
			pad = (width > (int)n) ? (size_t)width - n : 0;
			if (!minus)
				out_rep(&s, ' ', (int)pad);
			out_mem(&s, str, n);
			if (minus)
				out_rep(&s, ' ', (int)pad);
			break;
		}
		case 'f': case 'F': {
			struct decnum v;
			double x = va_arg(ap, double);
			int pr = (prec < 0) ? 6 : prec;
			if (decompose(x, &v) < 0) {
				out_str(&s, "<fmt>");
				break;
			}
			fmt_f(&s, &v, pr, width, zero, plus, space, alt, minus,
			      conv == 'F');
			break;
		}
		case 'e': case 'E': {
			struct decnum v;
			double x = va_arg(ap, double);
			int pr = (prec < 0) ? 6 : prec;
			if (decompose(x, &v) < 0) {
				out_str(&s, "<fmt>");
				break;
			}
			fmt_e(&s, &v, pr, width, zero, plus, space, alt,
			      conv == 'E', minus);
			break;
		}
		case 'g': case 'G': {
			struct decnum v;
			double x = va_arg(ap, double);
			if (decompose(x, &v) < 0) {
				out_str(&s, "<fmt>");
				break;
			}
			fmt_g(&s, &v, prec, width, zero, plus, space, alt,
			      conv == 'G', minus);
			break;
		}
		case 'a': case 'A': {
			double x = va_arg(ap, double);
			fmt_a(&s, x, prec, width, zero, plus, space, alt,
			      conv == 'A', minus);
			break;
		}
		default:
			out_ch(&s, '%');
			out_ch(&s, conv);
			break;
		}
	}

	if (s.cap > 0) {
		size_t end = (s.len < s.cap) ? s.len : s.cap - 1;
		s.buf[end] = 0;
	}
	return (int)s.len;
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
	return plua_vsnprintf(buf, size, fmt, ap);
}

int snprintf(char *buf, size_t size, const char *fmt, ...)
{
	va_list ap;
	int n;
	va_start(ap, fmt);
	n = plua_vsnprintf(buf, size, fmt, ap);
	va_end(ap);
	return n;
}

int sprintf(char *buf, const char *fmt, ...)
{
	va_list ap;
	int n;
	va_start(ap, fmt);
	n = plua_vsnprintf(buf, (size_t)-1, fmt, ap);
	va_end(ap);
	return n;
}

int vsprintf(char *buf, const char *fmt, va_list ap)
{
	return plua_vsnprintf(buf, (size_t)-1, fmt, ap);
}
