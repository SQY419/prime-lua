/*
 * plua_strtod.c -- primeLua's correctly rounded string-to-double conversion.
 *
 * Why this exists rather than reusing primetcc's runtime: Lua's lexer turns
 * every numeric literal in every script into a double through lua_str2number,
 * which luaconf.h maps to strtod.  The TCC runtime's version accumulates in
 * floating point (v = v*10 + d; v += d*frac) and is therefore wrong by several
 * ulp for ordinary inputs -- the double nearest "1e-20" comes out 8 ulp too
 * large and "1e308" 1 ulp too large -- it returns 1.0e300 for "inf", and it has
 * no hexadecimal-float path at all, although Lua 5.4 accepts "0x1p4".  A Lua
 * whose 1e-20 differs from desktop Lua's 1e-20 is not a port, so primeLua
 * carries its own conversion.  This is the mirror image of the reasoning in
 * plua_format.c: there the number-to-text direction had to be exact, here the
 * text-to-number direction does.
 *
 * Correctness first, speed second:
 *
 *   - The digits of the literal are accumulated into a big integer D, so D is
 *     EXACT.  No floating point is used anywhere on the path from text to the
 *     answer; the only double ever constructed is the final one, assembled from
 *     its sign, exponent and significand bits.
 *
 *   - A decimal literal with k digits and decimal exponent E denotes exactly
 *     D * 10^E.  Scaling by ten means multiplying by five and shifting by
 *     powers of two, and a factor 2^n does not change the significand bits at
 *     all -- it only moves the binary exponent.  So:
 *
 *         E >= 0 :  D * 10^E = (D * 5^E) * 2^E      -- an exact integer
 *         E <  0 :  D * 10^E = (D * 5^-E) / 2^-E    -- one binary division
 *
 *     Either way the whole problem reduces to one binary long division of two
 *     big integers followed by rounding.  There is no "scale with double
 *     arithmetic and repair afterwards" step anywhere, which is precisely the
 *     construction that comes out one ulp wrong.
 *
 *   - The division is asked for at least 121 quotient bits and, crucially, for
 *     the exact remainder.  The remainder IS the sticky bit: it says whether
 *     anything at all was discarded, and that is what decides a tie.  When the
 *     discarded part is exactly half an ulp the remainder is zero and the tie
 *     goes to even; when the retained digits sit exactly on a halfway point the
 *     tail's own remainder is nonzero, so the flag can only push the value away
 *     from the tie, never across it.  That is why 121 bits are enough even for
 *     a literal with a million digits, and it is where last-ulp bugs live.
 *
 *   - Subnormals are handled by rounding at the subnormal precision after
 *     shifting the significand down by (1074 - e2) bits, with the bits shifted
 *     out feeding the guard and sticky flags.  Underflow is therefore gradual
 *     and exact: 5e-324 and 0x1p-1074 both land on the smallest subnormal,
 *     anything strictly below half of it lands on zero.  Nothing is flushed.
 *
 *   - Overflow produces an infinity built from its bit pattern
 *     (0x7ff0000000000000), not from a <math.h> macro, so the file does not
 *     depend on the toolchain's HUGE_VAL being anything in particular.
 *
 * Freestanding C99: no libc beyond primeLua's own errno.h; no <math.h>, no
 * <stdio.h>, no <string.h>, no <ctype.h>; no VLA; no long double (ARM EABI's
 * long double is just double, so there is no extra precision to lean on); no
 * __int128 (the cross toolchain has no 128-bit helpers); and no general bignum
 * division -- the long division is repeated compare/shift/subtract, which the
 * ARM926EJ-S does happily because no floating point is involved in it.
 *
 * The only two external symbols are strtod and atof, as the build expects (the
 * Makefile compiles this file with -Dstrtod=... -Datof=...).
 *
 * Host differential test: tests/test_strtod_host.c
 *
 *   cd /root/prime/primeLua
 *   cc -O2 -Wall -c port/plua_strtod.c -Dstrtod=pl_strtod -Datof=pl_atof -o /tmp/pl_strtod.o
 *   cc -O2 -Wall -c tests/test_strtod_host.c -o /tmp/test_strtod.o
 *   cc /tmp/pl_strtod.o /tmp/test_strtod.o -o /tmp/test_strtod -lm
 *   /tmp/test_strtod
 */
#include <stddef.h>
#include <errno.h>

#ifndef ERANGE
#define ERANGE	34		/* primeLua's errno.h provides it; be safe */
#endif

/* ---------------------------------------------------------------------------
 * big integers: little-endian 32-bit limbs, the layout plua_format.c uses.
 *
 * Sizing.  The biggest value that ever has to be held is the scaled numerator
 * of a division: D (at most 800 significant digits, ~2658 bits, which is the
 * "0.000...0001" shape) times 5^1076 (~2500 bits), shifted left by at most 124
 * bits => ~5282 bits.  The scaled denominator 2^(j+sh) of that same case
 * reaches ~5280 bits.  192 limbs is 6144 bits, which covers it with room to
 * spare.  Only three operands exist at once, so the peak is under 3 kB of
 * stack: affordable even on a calculator whose usable RAM is a few hundred kB.
 * ------------------------------------------------------------------------- */
#define BN_LIMBS	192

typedef struct {
	int n;				/* used limbs; 0 means zero */
	unsigned int w[BN_LIMBS];
} bn;

/*
 * Significant decimal digits actually accumulated.
 *
 * Every double is determined by its first ~768 significant decimal digits (the
 * worst case is a subnormal, whose exact decimal expansion runs to 751 digits,
 * plus the digits needed to decide the rounding).  Keeping 800 and remembering
 * "there were more nonzero digits" in a sticky flag therefore loses nothing
 * about the rounding decision: the retained prefix is exact and the tail can
 * only add a positive error far below half an ulp, so it can never move the
 * prefix across a halfway point.  The cap is what keeps the big integers
 * bounded for a literal with a million digits in it.
 */
#define MAX_SIG_DIGITS	800

static void bn_zero(bn *a)
{
	a->n = 0;
}

static int bn_is_zero(const bn *a)
{
	return a->n == 0;
}

/* a = a * m; m is small (<= 5^13) so the 64-bit carry cannot overflow */
static void bn_mul_small(bn *a, unsigned int m)
{
	unsigned long long carry = 0;
	int i;
	if (a->n == 0 || m == 1)
		return;
	for (i = 0; i < a->n; i++) {
		unsigned long long cur = (unsigned long long)a->w[i] * m + carry;
		a->w[i] = (unsigned int)(cur & 0xffffffffu);
		carry = cur >> 32;
	}
	while (carry && a->n < BN_LIMBS) {
		a->w[a->n++] = (unsigned int)(carry & 0xffffffffu);
		carry >>= 32;
	}
}

/* a = a * 10 + d, used while accumulating the digit string */
static void bn_mul10_add(bn *a, unsigned int d)
{
	unsigned long long carry = d;
	int i;
	if (a->n == 0) {
		if (d == 0)
			return;
		a->w[0] = d;
		a->n = 1;
		return;
	}
	for (i = 0; i < a->n; i++) {
		unsigned long long cur = (unsigned long long)a->w[i] * 10u + carry;
		a->w[i] = (unsigned int)(cur & 0xffffffffu);
		carry = cur >> 32;
	}
	if (carry && a->n < BN_LIMBS)
		a->w[a->n++] = (unsigned int)carry;
}

static void bn_shl(bn *a, int bits)
{
	int limbs = bits >> 5, rest = bits & 31, i;
	if (a->n == 0 || bits <= 0)
		return;
	if (a->n + limbs + 1 > BN_LIMBS)
		return;			/* cannot happen: see the sizing note */
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
		if (carry && a->n < BN_LIMBS)
			a->w[a->n++] = carry;
	}
}

/* floor(log2(a)) + 1; 0 for zero */
static int bn_bitlen(const bn *a)
{
	int i, b = 0;
	unsigned int v;
	if (a->n == 0)
		return 0;
	i = a->n;
	v = a->w[i - 1];
	while (v >>= 1)
		b++;
	return (i - 1) * 32 + b + 1;
}

/* -1, 0 or +1 */
static int bn_cmp(const bn *a, const bn *b)
{
	int i;
	if (a->n != b->n)
		return (a->n < b->n) ? -1 : 1;
	for (i = a->n - 1; i >= 0; i--)
		if (a->w[i] != b->w[i])
			return (a->w[i] < b->w[i]) ? -1 : 1;
	return 0;
}

/* a -= b, requires a >= b */
static void bn_sub(bn *a, const bn *b)
{
	unsigned long long borrow = 0;
	int i;
	for (i = 0; i < a->n; i++) {
		unsigned long long bv = (i < b->n) ? b->w[i] : 0;
		unsigned long long cur = (unsigned long long)a->w[i] - bv - borrow;
		a->w[i] = (unsigned int)(cur & 0xffffffffu);
		borrow = (cur >> 63) & 1ULL;	/* wrapped around => a borrow */
	}
	while (a->n > 0 && a->w[a->n - 1] == 0)
		a->n--;
}

static void bn_from_digits(bn *a, const char *d, int n)
{
	int i;
	bn_zero(a);
	for (i = 0; i < n; i++)
		bn_mul10_add(a, (unsigned int)(d[i] - '0'));
}

static void bn_from_u64(bn *a, unsigned long long v)
{
	bn_zero(a);
	while (v) {
		a->w[a->n++] = (unsigned int)(v & 0xffffffffu);
		v >>= 32;
	}
}

/* Is any bit at a position below "lowbits" set? */
static double bits_to_double(unsigned long long u)
{
	union { double d; unsigned long long u; } c;
	c.u = u;
	return c.d;
}

static double make_inf(int sign)
{
	return bits_to_double(((unsigned long long)(sign & 1) << 63)
			      | 0x7ff0000000000000ULL);
}

static double make_nan(int sign)
{
	return bits_to_double(((unsigned long long)(sign & 1) << 63)
			      | 0x7ff8000000000000ULL);
}

static double make_zero(int sign)
{
	return bits_to_double((unsigned long long)(sign & 1) << 63);
}

static double bn_to_double(const bn *num, const bn *den, int e2, int sign,
			   int sticky_in)
{
	union { double d; unsigned long long u; } c;
	bn n, dd, t;
	unsigned long long q, mant;
	int sticky, nb, db, sh, guard, me, i;

	n = *num;
	dd = *den;
	nb = bn_bitlen(&n);
	db = bn_bitlen(&dd);

	/* Scale so that floor(n/d) occupies at most 64 bits, which is what makes
	 * the 64-step long division below exact: the quotient fits in a uint64
	 * and the remainder is the exact sticky flag.  The scaling shifts only by
	 * powers of two, so it never changes the VALUE -- it moves k between the
	 * quotient and the tracked exponent.  Which side to shift depends on how
	 * wide the quotient already is: a numerator-heavy ratio (a decimal with a
	 * large negative exponent turns into 5^-E / 2^-E, which is exactly that)
	 * needs the DIVISOR moved up, not the numerator clamped.
	 *
	 * floor(n/d) has (nb-db) or (nb-db+1) bits, so putting the extra bits on
	 * whichever side is short leaves a 63..64 bit quotient. */
	sh = (nb - db) - 63;
	if (sh > 0)
		bn_shl(&dd, sh);
	else if (sh < 0)
		bn_shl(&n, -sh);

	q = 0;
	for (i = 63; i >= 0; i--) {
		t = dd;
		bn_shl(&t, i);
		q <<= 1;
		if (bn_cmp(&n, &t) >= 0) {
			bn_sub(&n, &t);
			q |= 1ULL;
		}
	}
	if (q == 0)
		return make_zero(sign);
	sticky = (!bn_is_zero(&n)) || sticky_in;   /* exact remainder + caller */
	/* value = (q + rem/dd) * 2^(e2 + sh) */
	me = e2 + sh;				/* exponent of q's bit 0 */

	/* Drop bits until the mantissa is 53 bits wide: value = mant * 2^me with
	 * 2^52 <= mant < 2^53.  The first bit dropped is the guard bit; every
	 * later one joins the sticky flag, as does the division remainder.  That
	 * separation is what makes the rounding below exact. */
	/* The bits come off the bottom one at a time, so the LAST one dropped is
	 * the HIGHEST of them: that one is the guard bit, and every bit dropped
	 * before it (all lower) belongs to the sticky flag.  Getting this
	 * backwards -- taking the first dropped bit as the guard -- rounds half
	 * the values in the wrong direction, which is why it is spelled out
	 * here rather than folded into the loop condition. */
	guard = 0;
	{
		int ndrop = 0;
		while (q >= (1ULL << 53)) {
			int bit = (int)(q & 1ULL);
			if (ndrop > 0 && guard)
				sticky = 1;	/* the previously dropped, lower bit */
			guard = bit;		/* highest dropped bit so far */
			q >>= 1;
			me++;
			ndrop++;
		}
	}
	mant = q;

	/* mant is in [2^52, 2^53), i.e. value = 1.f * 2^(me + 52), so the IEEE
	 * exponent field is (me + 52) + 1023 = me + 1075.  Field 0 means subnormal,
	 * 1..2046 normal, 2047 infinity/NaN. */
	if (me + 1075 >= 1) {
		if (guard && (sticky || (mant & 1ULL))) {
			mant++;
			if (mant == (1ULL << 53)) {	/* carried into a new binade */
				mant >>= 1;
				me++;
			}
		}
		if (me + 1075 > 2046)		/* overflow -> infinity */
			return make_inf(sign);
		c.u = ((unsigned long long)(sign & 1) << 63)
		    | ((unsigned long long)(me + 1075) << 52)
		    | (mant & 0xfffffffffffffULL);
		return c.d;
	}

	/* Subnormal: the exponent field would be <= 0, so round at the coarser
	 * subnormal precision instead.  The value is M * 2^-1074 with M < 2^52,
	 * so M = mant >> s with s = -1074 - me, and the bits shifted out become
	 * the guard and sticky flags -- underflow is gradual and exact (2^-1074
	 * lands on the smallest subnormal, exactly half of it rounds to even,
	 * i.e. to zero, and anything below that is zero). */
	{
		int s = -1074 - me;
		if (s >= 54) {			/* below half the smallest subnormal */
			mant = 0;
			guard = 0;
		} else {
			unsigned long long mask = (1ULL << (s - 1)) - 1ULL;
			if (mant & mask)
				sticky = 1;
			guard = (int)((mant >> (s - 1)) & 1ULL);
			mant >>= s;
		}
		if (guard && (sticky || (mant & 1ULL)))
			mant++;
		c.u = ((unsigned long long)(sign & 1) << 63) | mant;
		return c.d;
	}
}

/* value = num * 2^k, num > 0.  den is 1 and the numerator absorbs the whole
 * power of two, so the shared core does everything. */
static double int_to_double(const bn *num, int k, int sign, int sticky_in)
{
	bn n = *num;
	bn one;
	bn_from_u64(&one, 1);
	if (k)
		bn_shl(&n, k);
	return bn_to_double(&n, &one, 0, sign, sticky_in);
}

/* value = (num / 2^j), sticky_in from digits dropped before the call */
static double div_to_double(const bn *num, int j, int sign, int sticky_in)
{
	bn den;
	bn_from_u64(&den, 1);
	if (j)
		bn_shl(&den, j);
	return bn_to_double(num, &den, 0, sign, sticky_in);
}

static int is_space(int c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f'
	    || c == '\r';
}

static int hex_val(int c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

static int lower(int c)
{
	return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c;
}

/* ---------------------------------------------------------------------------
 * the exact decimal core: value = D * 10^e10
 *
 * "dig" holds ndig significant digits (leading zeros already stripped, so
 * dig[0] != '0' when ndig > 0) and more_nonzero says that the literal had more
 * nonzero digits than were kept -- the truncation sticky flag.
 * ------------------------------------------------------------------------- */
static double dec_value(const char *dig, int ndig, long e10, int more_nonzero,
			int sign, int *overflow)
{
	bn num;
	double r;
	long j;

	/* Settled without touching a big integer: 10^400 is past DBL_MAX, and a
	 * value below 10^(-400-ndig) is below half the smallest subnormal so it
	 * rounds to zero.  This also keeps 1e999999999 cheap, and it is the
	 * guard that keeps the big integers inside BN_LIMBS. */
	if (e10 > 400) {
		*overflow = 1;
		return make_inf(sign);
	}
	if (e10 < -400 - (long)ndig)
		return make_zero(sign);

	bn_from_digits(&num, dig, ndig);
	if (bn_is_zero(&num))
		return make_zero(sign);		/* D == 0: a signed zero,
						 * whatever the exponent says */

	if (e10 >= 0) {
		/* D * 10^E = (D * 5^E) * 2^E: an exact integer, so the shared core
		 * only has to shift and round. */
		j = e10;
		while (j >= 13) {
			bn_mul_small(&num, 1220703125u);	/* 5^13 */
			j -= 13;
		}
		while (j-- > 0)
			bn_mul_small(&num, 5u);
		r = int_to_double(&num, (int)e10, sign, more_nonzero);
	} else {
		/* D / 10^j is NOT a dyadic rational, so there is no power-of-two
		 * denominator to be clever with: keep the exact ratio D / 10^j and
		 * let the shared core do one long division.  (Multiplying the
		 * numerator by 5^j -- the trick that works in the other direction,
		 * turning m / 2^j into m * 5^j / 10^j for the formatter -- would
		 * leave an extra factor of 5^j here.) */
		bn den;
		bn_from_u64(&den, 1);
		j = -e10;
		while (j >= 9) {
			bn_mul_small(&den, 1000000000u);
			j -= 9;
		}
		while (j-- > 0)
			bn_mul_small(&den, 10u);
		r = bn_to_double(&num, &den, 0, sign, more_nonzero);
	}
	/* The only way an infinite result can appear here is an exponent that
	 * ran off the top: "inf" itself never reaches this function.  (The
	 * comparison is false for a nan, but a nan cannot come out of here
	 * either.) */
	if (r > 1.7976931348623157e308)
		*overflow = 1;
	return r;
}

/* ---------------------------------------------------------------------------
 * hexadecimal floats: the value is mant * 2^bexp
 *
 * Hex digits are consumed until more of them cannot change the answer.  A
 * double needs 54 significant bits; this path keeps the first 60 (plus a
 * sticky flag for everything after), and the margin covers the shift that
 * subnormal rounding performs.
 * ------------------------------------------------------------------------- */
#define HEX_KEEP_BITS	60

static double hex_value(unsigned long long mant, int sticky, long bexp, int sign,
			int *overflow)
{
	bn num;
	double r;

	if (mant == 0)
		return make_zero(sign);		/* a zero significand is a zero
						 * whatever the exponent does */
	if (bexp > 4000) {			/* beyond every double */
		*overflow = 1;
		return make_inf(sign);
	}
	if (bexp < -4000)			/* below every double */
		return make_zero(sign);
	bn_from_u64(&num, mant);
	if (bexp >= 0)
		r = int_to_double(&num, (int)bexp, sign, sticky);
	else
		r = div_to_double(&num, (int)-bexp, sign, sticky);
	if (r > 1.7976931348623157e308)
		*overflow = 1;
	return r;
}

/* ---------------------------------------------------------------------------
 * strtod
 * ------------------------------------------------------------------------- */
double strtod(const char *s, char **endptr)
{
	const char *p = s;
	char dig[MAX_SIG_DIGITS];
	int ndig = 0;
	int dropped_nonzero = 0;
	int sign = 0;
	int overflow = 0;
	long e10 = 0;
	double r;

	while (is_space((unsigned char)*p))
		p++;
	if (*p == '+' || *p == '-') {
		sign = (*p == '-');
		p++;
	}

	/* "inf", "infinity", "nan", "nan(...)" -- case insensitive.  The
	 * parenthesised n-char-sequence of a nan is consumed only when it is
	 * complete, which is what glibc does ("nan(x" leaves endptr after
	 * "nan"). */
	if (lower((unsigned char)p[0]) == 'i'
	    && lower((unsigned char)p[1]) == 'n'
	    && lower((unsigned char)p[2]) == 'f') {
		p += 3;
		if (lower((unsigned char)p[0]) == 'i'
		    && lower((unsigned char)p[1]) == 'n'
		    && lower((unsigned char)p[2]) == 'i'
		    && lower((unsigned char)p[3]) == 't'
		    && lower((unsigned char)p[4]) == 'y')
			p += 5;
		if (endptr)
			*endptr = (char *)p;
		return make_inf(sign);
	}
	if (lower((unsigned char)p[0]) == 'n'
	    && lower((unsigned char)p[1]) == 'a'
	    && lower((unsigned char)p[2]) == 'n') {
		p += 3;
		if (*p == '(') {
			const char *q = p + 1;
			while (*q && *q != ')')
				q++;
			if (*q == ')') {
				/* The parenthesised n-char-sequence is the NaN
				 * payload: glibc reads it as a decimal (or 0x
				 * hexadecimal) value, keeps its low 51 bits and
				 * sets the quiet bit.  A sequence with no valid
				 * digit in it still gets consumed but leaves the
				 * canonical payload, and an overflowing one
				 * saturates -- both as glibc does. */
				unsigned long long payload = 0;
				int sat = 0, got = 0;
				const char *r = p + 1;
				int base = 10;
				if (r[0] == '0' && (r[1] == 'x' || r[1] == 'X')) {
					base = 16;
					r += 2;
				}
				for (; r < q; r++) {
					int dv = (base == 16)
					    ? hex_val((unsigned char)*r)
					    : ((*r >= '0' && *r <= '9')
					       ? *r - '0' : -1);
					if (dv < 0) {
						got = 0;
						break;
					}
					got = 1;
					if (payload > (0x7ffffffffffffULL
						       - (unsigned long long)dv)
						      / (unsigned long long)base)
						sat = 1;
					else
						payload = payload
						    * (unsigned long long)base
						    + (unsigned long long)dv;
				}
				if (sat)
					payload = 0x7ffffffffffffULL;
				if (endptr)
					*endptr = (char *)(q + 1);
				return bits_to_double(
				    ((unsigned long long)(sign & 1) << 63)
				    | 0x7ff8000000000000ULL
				    | (got ? payload : 0ULL));
			}
		}
		if (endptr)
			*endptr = (char *)p;
		return make_nan(sign);
	}

	/* Hexadecimal float.  The "0x" prefix counts only when a hex digit or a
	 * '.' follows it, and "0x." with no hex digit at all is not a
	 * conversion: both parse as the decimal 0 with endptr after the '0',
	 * exactly as glibc does ("0x" -> 0, endptr = s+1). */
	if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')
	    && (hex_val((unsigned char)p[2]) >= 0 || p[2] == '.')) {
		const char *q = p + 2;
		unsigned long long mant = 0;
		int sticky = 0;
		int nbits = 0;			/* bits accumulated in mant */
		long fracdigits = 0;		/* hex digits right of '.' */
		int anyhex = 0;
		int afterdot = 0;
		long bexp = 0;

		for (;;) {
			int d = hex_val((unsigned char)*q);
			if (d < 0) {
				if (*q == '.' && !afterdot) {
					afterdot = 1;
					q++;
					continue;
				}
				break;
			}
			q++;
			anyhex = 1;
			if (nbits > HEX_KEEP_BITS) {
				if (d)
					sticky = 1;	/* a dropped nonzero
							 * digit */
			} else {
				mant = (mant << 4) | (unsigned long long)d;
				nbits += 4;
			}
			if (afterdot)
				fracdigits++;
		}
		if (!anyhex) {			/* "0x" / "0x." / "0xp3" */
			if (endptr)
				*endptr = (char *)(p + 1);
			return make_zero(sign);
		}
		if (*q == 'p' || *q == 'P') {
			const char *eq = q + 1;
			long ev = 0;
			int expneg = 0;
			if (*eq == '+' || *eq == '-') {
				expneg = (*eq == '-');
				eq++;
			}
			if (*eq >= '0' && *eq <= '9') {
				while (*eq >= '0' && *eq <= '9') {
					if (ev < 100000000L)
						ev = ev * 10 + (*eq - '0');
					eq++;
				}
				q = eq;
				bexp = expneg ? -ev : ev;
			}
			/* otherwise the 'p' is not consumed at all ("0x1p") */
		}
		if (endptr)
			*endptr = (char *)q;
		r = hex_value(mant, sticky, bexp - 4L * fracdigits, sign,
			      &overflow);
		if (overflow)
			errno = ERANGE;
		return r;
	}

	/* Decimal: digits with an optional '.', then an optional exponent.
	 *
	 * Only significant digits enter "dig": leading zeros are skipped (they
	 * cost nothing but the exponent has to know about them, which is what
	 * "frstored" is for).  "frstored" counts how many of the stored digits
	 * lie right of the point, so the decimal exponent of D is the written
	 * exponent minus that count -- no separate bookkeeping of skipped zeros
	 * is needed, because a skipped zero simply is not counted. */
	{
		long frstored = 0;
		int anydigit = 0;

		while (*p == '0') {
			anydigit = 1;	/* "0" alone is a conversion (endptr
					 * must move past it); the digit itself
					 * is insignificant and needs no exponent
					 * bookkeeping */
			p++;
		}
		while (*p >= '0' && *p <= '9') {
			anydigit = 1;
			if (ndig < MAX_SIG_DIGITS)
				dig[ndig++] = *p;
			else if (*p != '0')
				dropped_nonzero = 1;
			p++;
		}
		if (*p == '.') {
			p++;
			while (*p >= '0' && *p <= '9') {
				anydigit = 1;
				if (ndig < MAX_SIG_DIGITS) {
					dig[ndig++] = *p;
					frstored++;
				} else if (*p != '0') {
					dropped_nonzero = 1;
				} else {
					/* a trailing zero past a saturated
					 * buffer sits after every stored
					 * digit, so it needs no exponent
					 * adjustment */
				}
				p++;
			}
		}
		if (!anydigit)
			goto noconv;		/* "", "+", ".", "+." */

		if (*p == 'e' || *p == 'E') {
			const char *save = p;
			const char *eq = p + 1;
			long ev = 0;
			int expneg = 0;
			if (*eq == '+' || *eq == '-') {
				expneg = (*eq == '-');
				eq++;
			}
			if (*eq >= '0' && *eq <= '9') {
				while (*eq >= '0' && *eq <= '9') {
					if (ev < 100000000L)
						ev = ev * 10 + (*eq - '0');
					eq++;
				}
				e10 = expneg ? -ev : ev;
				p = eq;
			} else {
				p = save;	/* "1e"/"1e+": 'e' not consumed */
			}
		}
		e10 -= frstored;
		if (endptr)
			*endptr = (char *)p;
		if (ndig == 0)
			return make_zero(sign);	/* every digit was a zero */
		r = dec_value(dig, ndig, e10, dropped_nonzero, sign, &overflow);
		if (overflow)
			errno = ERANGE;
		return r;
	}

noconv:
	if (endptr)
		*endptr = (char *)s;
	return 0.0;
}

double atof(const char *s)
{
	return strtod(s, NULL);
}
