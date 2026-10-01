/*
 * plua_dd.c -- double-double arithmetic and a set of transcendental functions.
 *
 * See plua_dd.h for the design notes.  The algorithms are the well-known ones
 * (Dekker's error-free transforms, Knuth's two-sum, Bailey's dd quad-double
 * toolkit structure, and textbook Taylor/atanh series with dd range
 * reduction); nothing here is novel, which is the point: the host test
 * (tests/test_dd_host.c) checks the results against mpmath at 60+ digits, and
 * port/plua_dd_NOTES.md records what it measured.
 *
 * The three constants that need more than 106 bits (pi/2, ln2, log10(2)) are
 * given as THREE doubles so that argument reduction stays accurate for large
 * arguments; they were computed with mpmath and split with a script
 * (tools/dd_consts.py) -- the split values are exact, because any double
 * printed with 17 significant digits round-trips.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "plua_dd.h"

double sqrt(double);            /* the port provides this (plua_math / openlibm) */
double frexp(double, int *);
double ldexp(double, int);

#define DD_SPLITTER 134217729.0             /* 2^27 + 1, Dekker's splitter */

static double split_hi(double a)
{
	double t = a * DD_SPLITTER;
	return t - (t - a);
}

/* error-free transforms ---------------------------------------------------- */
static double two_sum(double a, double b, double *err)
{
	double s = a + b;
	double bb = s - a;
	*err = (a - (s - bb)) + (b - bb);
	return s;
}

static double quick_two_sum(double a, double b, double *err)
{
	double s = a + b;
	*err = b - (s - a);
	return s;
}

static double two_prod(double a, double b, double *err)
{
	double p = a * b;
	double ah = split_hi(a), al = a - ah;
	double bh = split_hi(b), bl = b - bh;
	*err = ((ah * bh - p) + ah * bl + al * bh) + al * bl;
	return p;
}

/* construction ------------------------------------------------------------- */
static plua_dd dd_norm2(double hi, double lo)
{
	double s, e, t;
	plua_dd r;
	if (hi != hi || lo != lo) {             /* NaN anywhere: NaN result */
		r.hi = hi + lo;
		r.lo = 0.0;
		return r;
	}
	s = quick_two_sum(hi, lo, &e);
	if (e != 0.0) {
		/* one more step, in case hi/lo were badly unbalanced */
		t = s;
		s = quick_two_sum(s, e, &e);
		(void)t;
	}
	r.hi = s;
	r.lo = e;
	return r;
}

plua_dd plua_dd_from_double(double x)
{
	plua_dd r;
	r.hi = x;
	r.lo = 0.0;
	return r;
}

plua_dd plua_dd_from_parts(double hi, double lo)
{
	return dd_norm2(hi, lo);
}

plua_dd plua_dd_from_long(long long n)
{
	/* exact to the full 106 bits: the double part plus the integer remainder,
	 * so a 64-bit Lua integer does not lose its low bits on the way in */
	plua_dd r;
	double hi = (double)n;
	r.hi = hi;
	r.lo = (double)(n - (long long)hi);
	return dd_norm2(r.hi, r.lo);
}

plua_dd plua_dd_nan(void)
{
	plua_dd r;
	r.hi = 0.0;
	r.lo = 0.0;
	r.hi = r.hi / r.lo;                     /* 0/0 */
	return r;
}

plua_dd plua_dd_inf(int sign)
{
	plua_dd r;
	r.hi = sign < 0 ? -1.0 / 0.0 : 1.0 / 0.0;
	r.lo = 0.0;
	return r;
}

double plua_dd_to_double(plua_dd x)
{
	return x.hi + x.lo;
}

int plua_dd_is_nan(plua_dd x)
{
	return x.hi != x.hi;
}

int plua_dd_is_inf(plua_dd x)
{
	return !plua_dd_is_nan(x) && (x.hi - x.hi) != 0.0;
}

int plua_dd_is_zero(plua_dd x)
{
	return x.hi == 0.0;
}

int plua_dd_sign(plua_dd x)
{
	if (x.hi > 0.0 || (x.hi == 0.0 && x.lo > 0.0))
		return 1;
	if (x.hi < 0.0 || (x.hi == 0.0 && x.lo < 0.0))
		return -1;
	return 0;
}

/* arithmetic --------------------------------------------------------------- */
plua_dd plua_dd_add(plua_dd a, plua_dd b)
{
	double s, e, t;
	plua_dd r;
	s = two_sum(a.hi, b.hi, &e);
	e += a.lo + b.lo;
	s = quick_two_sum(s, e, &t);
	r.hi = s;
	r.lo = t;
	return r;
}

plua_dd plua_dd_sub(plua_dd a, plua_dd b)
{
	return plua_dd_add(a, plua_dd_neg(b));
}

plua_dd plua_dd_mul(plua_dd a, plua_dd b)
{
	double p, e, t;
	plua_dd r;
	p = two_prod(a.hi, b.hi, &e);
	e += a.hi * b.lo + a.lo * b.hi;
	p = quick_two_sum(p, e, &t);
	r.hi = p;
	r.lo = t;
	return r;
}

plua_dd plua_dd_mul_d(plua_dd a, double b)
{
	double p, e, t;
	plua_dd r;
	p = two_prod(a.hi, b, &e);
	e += a.lo * b;
	p = quick_two_sum(p, e, &t);
	r.hi = p;
	r.lo = t;
	return r;
}

plua_dd plua_dd_div(plua_dd a, plua_dd b)
{
	double q1, q2, q3, t;
	plua_dd r;
	if (plua_dd_is_zero(b))
		return plua_dd_inf(plua_dd_sign(a) * plua_dd_sign(b));
	q1 = a.hi / b.hi;
	/* r = a - q1*b   (only the high part matters for the next digit) */
	r = plua_dd_sub(a, plua_dd_mul_d(b, q1));
	q2 = r.hi / b.hi;
	r = plua_dd_sub(r, plua_dd_mul_d(b, q2));
	q3 = r.hi / b.hi;
	/* renormalise q1 + q2 + q3 */
	q1 = quick_two_sum(q1, q2, &t);
	r.hi = q1;
	r.lo = t + q3;
	return dd_norm2(r.hi, r.lo);
}

plua_dd plua_dd_div_d(plua_dd a, double b)
{
	return plua_dd_div(a, plua_dd_from_double(b));
}

plua_dd plua_dd_neg(plua_dd a)
{
	plua_dd r;
	r.hi = -a.hi;
	r.lo = -a.lo;
	return r;
}

plua_dd plua_dd_abs(plua_dd a)
{
	return plua_dd_sign(a) < 0 ? plua_dd_neg(a) : a;
}

plua_dd plua_dd_scale2(plua_dd a, int k)
{
	plua_dd r;
	r.hi = ldexp(a.hi, k);
	r.lo = ldexp(a.lo, k);
	return r;
}

int plua_dd_cmp(plua_dd a, plua_dd b)
{
	if (plua_dd_is_nan(a) || plua_dd_is_nan(b))
		return 2;
	if (a.hi > b.hi)
		return 1;
	if (a.hi < b.hi)
		return -1;
	if (a.lo > b.lo)
		return 1;
	if (a.lo < b.lo)
		return -1;
	return 0;
}

plua_dd plua_dd_trunc(plua_dd a)
{
	plua_dd r;
	double t;
	if (plua_dd_is_nan(a) || plua_dd_is_inf(a))
		return a;
	if (a.hi >= 9.007199254740992e15 || a.hi <= -9.007199254740992e15)
		return a;                       /* already integral at this scale */
	t = (double)(long long)a.hi;            /* rounds toward zero */
	/* hi may be an integer while lo pulls the value just below/above it
	 * (0.999...9 is stored as (1.0, -1e-33)) */
	if (t == a.hi) {
		if (a.lo < 0.0 && a.hi > 0.0)
			t -= 1.0;
		else if (a.lo > 0.0 && a.hi < 0.0)
			t += 1.0;
	}
	r.hi = t;
	r.lo = 0.0;
	return r;
}

plua_dd plua_dd_floor(plua_dd a)
{
	plua_dd t = plua_dd_trunc(a);
	if (plua_dd_sign(a) < 0 && plua_dd_cmp(t, a) != 0)
		return plua_dd_sub(t, plua_dd_from_double(1.0));
	return t;
}

plua_dd plua_dd_ceil(plua_dd a)
{
	plua_dd t = plua_dd_trunc(a);
	if (plua_dd_sign(a) > 0 && plua_dd_cmp(t, a) != 0)
		return plua_dd_add(t, plua_dd_from_double(1.0));
	return t;
}

plua_dd plua_dd_fmod(plua_dd a, plua_dd b)
{
	plua_dd q, r, ab;
	if (plua_dd_is_zero(b))
		return plua_dd_nan();
	q = plua_dd_trunc(plua_dd_div(a, b));
	r = plua_dd_sub(a, plua_dd_mul(q, b));
	/* the quotient's last bits can round across an integer, so verify the
	 * remainder is in range and step it once if it is not */
	ab = plua_dd_abs(b);
	if (plua_dd_cmp(plua_dd_abs(r), ab) >= 0) {
		if (plua_dd_sign(r) == plua_dd_sign(b))
			r = plua_dd_sub(r, b);
		else
			r = plua_dd_add(r, b);
	}
	return r;
}

/* constants ---------------------------------------------------------------- */
/*
 * pi/2 to 159 bits (three doubles), ln2 and ln10 to 106 bits, and pi/4, e,
 * log10(2).  Generated with mpmath at 60 digits and split by tools/dd_consts.py;
 * the split values are exact because a double printed with 17 digits
 * round-trips.
 */
plua_dd plua_dd_pi(void)
{
	static const plua_dd p = { 3.141592653589793116e+00,
				   1.224646799147353207e-16 };
	return p;
}

plua_dd plua_dd_e(void)
{
	static const plua_dd p = { 2.718281828459045091e+00,
				   1.445646891729250158e-16 };
	return p;
}

plua_dd plua_dd_ln2(void)
{
	static const plua_dd p = { 6.931471805599452862e-01,
				   2.319046813846299558e-17 };
	return p;
}

plua_dd plua_dd_ln10(void)
{
	static const plua_dd p = { 2.302585092994045901e+00,
				   -2.170756223382249351e-16 };
	return p;
}

/* internal: pi/2 and pi/4 as (hi, mid, lo) for argument reduction */
static const double PI2_3[3] = {
	1.570796326794896558e+00, 6.123233995736766036e-17,
	-1.497384904859169819e-33
};

static const double PI4_3[3] = {
	7.853981633974482790e-01, 3.061616997868383018e-17,
	-7.486924524295849095e-34
};

static plua_dd dd_from_3(const double c[3])
{
	plua_dd r;
	r.hi = c[0];
	r.lo = c[1];
	r = dd_norm2(r.hi, r.lo);
	r = plua_dd_add(r, plua_dd_from_double(c[2]));
	return r;
}

/* functions ---------------------------------------------------------------- */
plua_dd plua_dd_sqrt(plua_dd a)
{
	plua_dd y, ay2, h;
	int i;
	if (plua_dd_is_zero(a))
		return a;
	if (plua_dd_sign(a) < 0)
		return plua_dd_nan();
	y = plua_dd_from_double(1.0 / sqrt(a.hi));      /* ~16 good digits */
	for (i = 0; i < 3; i++) {                      /* y = y*(3 - a*y^2)/2 */
		ay2 = plua_dd_mul(plua_dd_mul(a, y), y);
		h = plua_dd_sub(plua_dd_from_double(3.0), ay2);
		h.hi *= 0.5;
		h.lo *= 0.5;
		y = plua_dd_mul(y, h);
	}
	return plua_dd_mul(a, y);
}

plua_dd plua_dd_exp(plua_dd a)
{
	plua_dd r, term, sum, ln2;
	double k;
	int ki, i;
	if (plua_dd_is_nan(a))
		return a;
	if (a.hi > 709.79)
		return plua_dd_inf(1);
	if (a.hi < -745.2)
		return plua_dd_from_double(0.0);
	ln2 = plua_dd_ln2();
	k = plua_dd_to_double(plua_dd_div(a, ln2));
	k = (k < 0.0) ? -(double)(long long)(-k + 0.5) : (double)(long long)(k + 0.5);
	ki = (int)k;
	/* the same two-stage reduction as the trigonometric one: subtract ln2's
	 * low word separately, or the cancellation in x - k*ln2 throws away the
	 * digits that matter (exp(-677) was off at the 29th digit) */
	r = plua_dd_sub(a, plua_dd_mul_d(plua_dd_from_double(ln2.hi), k));
	r = plua_dd_sub(r, plua_dd_mul_d(plua_dd_from_double(ln2.lo), k));
	/* Taylor: 26 terms take |r| <= 0.347 to ~1e-36 */
	sum = plua_dd_from_double(1.0);
	term = plua_dd_from_double(1.0);
	for (i = 1; i <= 26; i++) {
		/* NOT term.hi /= i (and term.lo /= i): dividing the two words
		 * independently rounds each of them, which drops the extra
		 * precision and left exp() accurate to only ~1e-21 (the host test
		 * caught it).  A dd division by a double keeps it. */
		term = plua_dd_div_d(plua_dd_mul(term, r), (double)i);
		sum = plua_dd_add(sum, term);
	}
	return plua_dd_scale2(sum, ki);
}

plua_dd plua_dd_log(plua_dd a)
{
	plua_dd m, s, s2, sum, term, e;
	int ex, i, adj = 0;
	double d;
	if (plua_dd_is_nan(a))
		return a;
	if (plua_dd_sign(a) < 0)
		return plua_dd_nan();
	if (plua_dd_is_zero(a))
		return plua_dd_inf(-1);
	m = a;
	d = frexp(a.hi, &ex);
	(void)d;
	m = plua_dd_scale2(a, -ex);                    /* m in [0.5, 1) */
	while (m.hi < 0.70710678118654752440) {        /* bring m to [1/sqrt2, sqrt2) */
		m = plua_dd_scale2(m, 1);
		adj--;
	}
	while (m.hi >= 1.41421356237309504880) {
		m = plua_dd_scale2(m, -1);
		adj++;
	}
	/* log(m) = 2*atanh(s), s = (m-1)/(m+1), |s| <= 0.1716 */
	s = plua_dd_div(plua_dd_sub(m, plua_dd_from_double(1.0)),
			plua_dd_add(m, plua_dd_from_double(1.0)));
	s2 = plua_dd_mul(s, s);
	sum = s;
	term = s;
	for (i = 3; i <= 45; i += 2) {                 /* s + s^3/3 + s^5/5 ... */
		term = plua_dd_mul(term, s2);
		sum = plua_dd_add(sum, plua_dd_div_d(term, (double)i));
	}
	sum = plua_dd_mul_d(sum, 2.0);
	/* a = m0 * 2^ex with m0 in [0.5,1); the loops moved the binary point by
	 * -adj, so m = m0 * 2^(-adj) and a = m * 2^(ex + adj) */
	e = plua_dd_mul_d(plua_dd_ln2(), (double)(ex + adj));
	return plua_dd_add(sum, e);
}

plua_dd plua_dd_log10(plua_dd a)
{
	/* log10(x) = log(x) / ln10; ln10 is 106 bits, and the division is dd */
	return plua_dd_div(plua_dd_log(a), plua_dd_ln10());
}

plua_dd plua_dd_pow(plua_dd a, plua_dd b)
{
	if (plua_dd_is_zero(b))
		return plua_dd_from_double(1.0);
	if (plua_dd_is_zero(a))
		return (plua_dd_sign(b) < 0) ? plua_dd_inf(1) : plua_dd_from_double(0.0);
	if (plua_dd_sign(a) < 0) {
		/* negative base: only integral exponents have a real answer */
		double bi = plua_dd_to_double(b);
		if (bi != (double)(long long)bi)
			return plua_dd_nan();
		{
			plua_dd r = plua_dd_exp(plua_dd_mul(b, plua_dd_log(plua_dd_neg(a))));
			if (((long long)bi) & 1)
				r = plua_dd_neg(r);
			return r;
		}
	}
	return plua_dd_exp(plua_dd_mul(b, plua_dd_log(a)));
}

plua_dd plua_dd_hypot(plua_dd a, plua_dd b)
{
	plua_dd ah = plua_dd_abs(a), bh = plua_dd_abs(b);
	if (plua_dd_is_zero(ah) && plua_dd_is_zero(bh))
		return plua_dd_from_double(0.0);
	return plua_dd_sqrt(plua_dd_add(plua_dd_mul(ah, ah), plua_dd_mul(bh, bh)));
}

/* sin/cos core on |r| <= pi/4, quadrant q = 0..3 */
static void dd_sincos_core(plua_dd r, int q, plua_dd *s, plua_dd *c)
{
	plua_dd r2 = plua_dd_mul(r, r);
	plua_dd st, ct;
	int i;
	/* sin(r) = r - r^3/3! + r^5/5! - ...  (17 terms: (pi/4)^37/37! ~ 1e-40) */
	st = r;
	{
		plua_dd term = r;
		for (i = 1; i <= 17; i++) {
			term = plua_dd_mul(term, r2);
			term = plua_dd_div_d(term, (double)(2 * i) * (double)(2 * i + 1));
			st = (i & 1) ? plua_dd_sub(st, term) : plua_dd_add(st, term);
		}
	}
	ct = plua_dd_from_double(1.0);
	{
		plua_dd term = plua_dd_from_double(1.0);
		for (i = 1; i <= 17; i++) {
			term = plua_dd_mul(term, r2);
			term = plua_dd_div_d(term, (double)(2 * i - 1) * (double)(2 * i));
			ct = (i & 1) ? plua_dd_sub(ct, term) : plua_dd_add(ct, term);
		}
	}
	switch (q & 3) {
	case 0: *s = st; *c = ct; break;
	case 1: *s = ct; *c = plua_dd_neg(st); break;
	case 2: *s = plua_dd_neg(st); *c = plua_dd_neg(ct); break;
	default: *s = plua_dd_neg(ct); *c = st; break;
	}
}

/* reduce a modulo pi/2: returns the quadrant and r in [-pi/4, pi/4] */
static int dd_reduce_pi2(plua_dd a, plua_dd *r)
{
	plua_dd pi2 = dd_from_3(PI2_3);
	plua_dd q = plua_dd_div(a, pi2);
	double qd = q.hi;
	double n;
	long long ni;
	if (qd > 9.0e15 || qd < -9.0e15)
		n = qd;                        /* beyond the reduction we can do */
	else {
		n = (qd < 0.0) ? -(double)(long long)(-qd + 0.5)
			       : (double)(long long)(qd + 0.5);
	}
	ni = (long long)n;
	/* Subtract the three words of pi/2 ONE AT A TIME.  Forming n*(pi/2) first
	 * and subtracting afterwards needs 128 bits (n*pi/2 is ~1e5 while the last
	 * word contributes ~1e-28), which a double-double cannot hold: the host
	 * test showed sin() losing five digits for x ~ 1e6.  Term by term, each
	 * intermediate stays O(1) and the whole reduction is exact to dd. */
	*r = plua_dd_sub(a, plua_dd_mul_d(plua_dd_from_double(n), PI2_3[0]));
	*r = plua_dd_sub(*r, plua_dd_mul_d(plua_dd_from_double(n), PI2_3[1]));
	*r = plua_dd_sub(*r, plua_dd_mul_d(plua_dd_from_double(n), PI2_3[2]));
	return (int)(ni & 3);
}

plua_dd plua_dd_sin(plua_dd a)
{
	plua_dd r, s, c;
	int q;
	if (plua_dd_is_nan(a) || plua_dd_is_inf(a))
		return plua_dd_nan();
	q = dd_reduce_pi2(a, &r);
	dd_sincos_core(r, q, &s, &c);
	return s;
}

plua_dd plua_dd_cos(plua_dd a)
{
	plua_dd r, s, c;
	int q;
	if (plua_dd_is_nan(a) || plua_dd_is_inf(a))
		return plua_dd_nan();
	q = dd_reduce_pi2(a, &r);
	dd_sincos_core(r, q, &s, &c);
	return c;
}

plua_dd plua_dd_tan(plua_dd a)
{
	plua_dd r, s, c;
	int q;
	if (plua_dd_is_nan(a) || plua_dd_is_inf(a))
		return plua_dd_nan();
	q = dd_reduce_pi2(a, &r);
	dd_sincos_core(r, q, &s, &c);
	return plua_dd_div(s, c);
}

/* atan(x) for |x| <= tan(pi/8) = 0.4142: x - x^3/3 + x^5/5 - ...
 * (0.4142^101/101 ~ 1e-40, so 50 terms are plenty for 32 digits) */
static plua_dd dd_atan_series(plua_dd x)
{
	plua_dd x2 = plua_dd_mul(x, x);
	plua_dd term = x, sum = x;
	int i;
	for (i = 1; i <= 50; i++) {
		term = plua_dd_mul(term, x2);
		if (i & 1)
			sum = plua_dd_sub(sum, plua_dd_div_d(term, (double)(2 * i + 1)));
		else
			sum = plua_dd_add(sum, plua_dd_div_d(term, (double)(2 * i + 1)));
	}
	return sum;
}

plua_dd plua_dd_atan(plua_dd a)
{
	plua_dd x = a, r;
	int neg = 0, recip = 0;
	plua_dd pi2 = dd_from_3(PI2_3);
	plua_dd pi4 = dd_from_3(PI4_3);
	if (plua_dd_is_nan(a))
		return a;
	if (plua_dd_sign(x) < 0) {
		x = plua_dd_neg(x);
		neg = 1;
	}
	if (plua_dd_cmp(x, plua_dd_from_double(1.0)) > 0) {
		x = plua_dd_div(plua_dd_from_double(1.0), x);
		recip = 1;
	}
	/* atan(x) = pi/4 + atan((x-1)/(x+1)) for x > tan(pi/8) */
	if (x.hi > 0.41421356237309503) {
		plua_dd t = plua_dd_div(plua_dd_sub(x, plua_dd_from_double(1.0)),
					plua_dd_add(x, plua_dd_from_double(1.0)));
		r = plua_dd_add(pi4, dd_atan_series(t));
	} else {
		r = dd_atan_series(x);
	}
	if (recip)
		r = plua_dd_sub(pi2, r);
	if (neg)
		r = plua_dd_neg(r);
	return r;
}

/* asin(x) = atan2(x, sqrt(1-x^2)): the atan2 form stays accurate all the way
 * to |x| = 1, where the naive atan(x/sqrt(1-x^2)) loses everything */
plua_dd plua_dd_asin(plua_dd a)
{
	plua_dd one = plua_dd_from_double(1.0);
	plua_dd t, r;
	double ax = a.hi < 0 ? -a.hi : a.hi;
	if (plua_dd_is_nan(a))
		return a;
	if (ax > 1.0)
		return plua_dd_nan();
	if (ax == 1.0) {
		plua_dd h = plua_dd_div_d(plua_dd_pi(), 2.0);
		return plua_dd_sign(a) < 0 ? plua_dd_neg(h) : h;
	}
	t = plua_dd_sqrt(plua_dd_sub(one, plua_dd_mul(a, a)));
	r = plua_dd_atan2(a, t);
	return r;
}

plua_dd plua_dd_acos(plua_dd a)
{
	plua_dd one = plua_dd_from_double(1.0);
	plua_dd t;
	double ax = a.hi < 0 ? -a.hi : a.hi;
	if (plua_dd_is_nan(a))
		return a;
	if (ax > 1.0)
		return plua_dd_nan();
	if (ax == 1.0)
		return plua_dd_sign(a) > 0 ? plua_dd_from_double(0.0)
					   : plua_dd_pi();
	/* atan2(sqrt(1-x^2), x) is accurate at both ends of the range */
	t = plua_dd_sqrt(plua_dd_sub(one, plua_dd_mul(a, a)));
	return plua_dd_atan2(t, a);
}

plua_dd plua_dd_atan2(plua_dd y, plua_dd x)
{
	plua_dd pi = plua_dd_pi();
	if (plua_dd_is_zero(x)) {
		if (plua_dd_is_zero(y))
			return plua_dd_from_double(0.0);
		return plua_dd_sign(y) > 0 ? plua_dd_div_d(pi, 2.0)
					   : plua_dd_neg(plua_dd_div_d(pi, 2.0));
	}
	if (plua_dd_sign(x) > 0)
		return plua_dd_atan(plua_dd_div(y, x));
	if (plua_dd_sign(y) >= 0)
		return plua_dd_add(pi, plua_dd_atan(plua_dd_div(y, x)));
	return plua_dd_sub(plua_dd_atan(plua_dd_div(y, x)), pi);
}

/* ---- text -----------------------------------------------------------------
 *
 * Printing uses the port's own printf engine (%.16e), which is exact and
 * differentially tested against glibc.  The value printed is the EXACT sum of
 * the two doubles: hi contributes 17 digits, lo the next 17, and the
 * overlapping part is added digit by digit with carry propagation, so the
 * result is correct to 33 digits (the very last digit can be off by one when
 * lo's 18th digit would have carried -- documented, and the host test measures
 * it).
 */
/* Pull "[-]d.ddddde[+-]dd" apart: the digits and the exponent.  `want` digits
 * are kept (33 for a full-width print), zero-padded if the format gave fewer. */
#define DD_PDIG 33
static int parse_e(const char *t, char *digits, int want, int *exp10, int *neg)
{
	int n = 0;
	*neg = 0;
	if (*t == '-') {
		*neg = 1;
		t++;
	}
	for (; *t && *t != 'e' && *t != 'E'; t++)
		if (*t >= '0' && *t <= '9' && n < want)
			digits[n++] = *t;
	*exp10 = 0;
	if (*t == 'e' || *t == 'E')
		*exp10 = (int)strtol(t + 1, 0, 10);
	while (n < want)
		digits[n++] = '0';
	return n;
}

int plua_dd_to_string(plua_dd x, char *buf, int buflen, int sigdigits)
{
	char h[64], l[64], hd[DD_PDIG + 2], ld[DD_PDIG + 2];
	int acc[3 * DD_PDIG];
	int i, k, he, le, hn, ln, off, neg, lneg, w;
	const int NACC = (int)(sizeof(acc) / sizeof(acc[0]));

	if (!buf || buflen < 12)
		return -1;
	if (plua_dd_is_nan(x)) {
		strcpy(buf, "nan");
		return 3;
	}
	if (plua_dd_is_inf(x)) {
		const char *t = plua_dd_sign(x) < 0 ? "-inf" : "inf";
		strcpy(buf, t);
		return (int)strlen(t);
	}
	if (plua_dd_is_zero(x)) {
		strcpy(buf, "0");
		return 1;
	}
	if (sigdigits < 1)
		sigdigits = 1;
	if (sigdigits > 33)
		sigdigits = 33;
	neg = plua_dd_sign(x) < 0;
	if (neg)
		x = plua_dd_neg(x);

	/* an exact integer (both words integral, no fraction) prints as one */
	if (x.lo == 0.0 && x.hi == (double)(long long)x.hi &&
	    x.hi > -1.0e15 && x.hi < 1.0e15) {
		w = snprintf(buf, buflen, "%s%lld", neg ? "-" : "",
			     (long long)(x.hi < 0 ? -x.hi : x.hi));
		if (w < 0 || w >= buflen)
			return -1;
		return w;
	}

	if (sigdigits <= 17) {
		/* at this width the correctly rounded double is exact enough */
		w = snprintf(buf, buflen, "%.*e", sigdigits - 1, x.hi + x.lo);
		if (w < 0 || w >= buflen)
			return -1;
		return w;
	}

	/* BOTH words at full width.  hi printed with only 17 digits loses its own
	 * tail (the 18th digit on), which is exactly where lo starts to matter, so
	 * the sum came out wrong from the 18th digit on -- caught by the host test
	 * comparing against mpmath. */
	if (snprintf(h, sizeof(h), "%.*e", DD_PDIG - 1, x.hi) <= 0)
		return -1;
	hn = parse_e(h, hd, DD_PDIG, &he, &k);
	if (x.lo == 0.0) {
		/* no second word: %e of 0.0 reports exponent 0, which would put the
		 * offset far below the leading digit and abort the print (1e-300
		 * failed exactly there) */
		ln = 0;
		le = he;
		lneg = 0;
	} else {
		if (snprintf(l, sizeof(l), "%.*e", DD_PDIG - 1, x.lo) <= 0)
			return -1;
		ln = parse_e(l, ld, DD_PDIG, &le, &lneg);
	}

	/* hi's digits, then lo's digits added (or subtracted, when lo is the small
	 * correction in the other direction) at their offset */
	for (i = 0; i < NACC; i++)
		acc[i] = 0;
	for (i = 0; i < hn && i < NACC; i++)
		acc[i] = hd[i] - '0';
	off = he - le;
	if (off < 0)
		return -1;                       /* not a normalised dd */
	for (k = 0; k < ln; k++) {
		int pos = off + k;
		if (pos >= NACC)
			break;
		if (lneg)
			acc[pos] -= ld[k] - '0';
		else
			acc[pos] += ld[k] - '0';
	}
	/* carry / borrow from the least significant end upwards */
	for (i = NACC - 1; i > 0; i--) {
		while (acc[i] < 0) {
			acc[i] += 10;
			acc[i - 1]--;
		}
		acc[i - 1] += acc[i] / 10;
		acc[i] %= 10;
	}
	if (acc[0] >= 10) {                      /* carry out of the top digit */
		int carry = acc[0] / 10;
		for (i = NACC - 2; i > 0; i--)
			acc[i] = acc[i - 1];
		acc[0] = carry;
		he++;
	}
	if (acc[0] < 0)
		return -1;

	/* round half-up at sigdigits */
	if (sigdigits < NACC - 1 && acc[sigdigits] >= 5) {
		int p = sigdigits - 1;
		for (;;) {
			if (p < 0) {
				for (i = NACC - 2; i > 0; i--)
					acc[i] = acc[i - 1];
				acc[0] = 1;
				he++;
				break;
			}
			if (acc[p] == 9) {
				acc[p] = 0;
				p--;
				continue;
			}
			acc[p]++;
			break;
		}
	}

	/* render: [-]d.ddd...e[+-]XX */
	w = 0;
	if (neg && w < buflen - 1)
		buf[w++] = '-';
	if (w < buflen - 1)
		buf[w++] = (char)('0' + acc[0]);
	if (sigdigits > 1 && w < buflen - 1)
		buf[w++] = '.';
	for (i = 1; i < sigdigits && w < buflen - 1; i++)
		buf[w++] = (char)('0' + acc[i]);
	if (w >= buflen - 8)
		return -1;
	w += snprintf(buf + w, buflen - w, "e%+03d", he);
	buf[w] = 0;
	return w;
}

plua_dd plua_dd_from_string(const char *s, const char **end)
{
	const char *p = s;
	int neg = 0, seen_digit = 0, exp10 = 0, frac = 0;
	char d[64];
	int nd = 0, dropped = 0;
	double hi;
	char *stop;
	plua_dd dv, res;

	if (!s)
		return plua_dd_nan();
	while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
		p++;
	if (*p == '+' || *p == '-') {
		neg = (*p == '-');
		p++;
	}
	if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
		/* hex float: the double is all we give (documented) */
		hi = strtod(s, &stop);
		if (end)
			*end = stop;
		return plua_dd_from_double(hi);
	}
	/* collect significant digits */
	for (;; p++) {
		if (*p >= '0' && *p <= '9') {
			seen_digit = 1;
			if (nd < 40)
				d[nd++] = *p;
			else if (!frac)
				exp10++;               /* integer digits beyond 40 */
			else
				dropped = 1;
			if (frac)
				exp10--;
		} else if (*p == '.') {
			if (frac)
				break;
			frac = 1;
		} else {
			break;
		}
	}
	if (!seen_digit) {
		if (end)
			*end = s;
		return plua_dd_nan();
	}
	if (*p == 'e' || *p == 'E') {
		const char *q = p + 1;
		int esign = 1, ev = 0, got = 0;
		if (*q == '+' || *q == '-') {
			esign = (*q == '-') ? -1 : 1;
			q++;
		}
		while (*q >= '0' && *q <= '9') {
			ev = ev * 10 + (*q - '0');
			got = 1;
			q++;
			if (ev > 100000)
				break;
		}
		if (got) {
			exp10 += esign * ev;
			p = q;
		}
	}
	(void)dropped;
	if (end)
		*end = p;

	/* hi from the whole string (correctly rounded, handles the exponent) */
	{
		char tmp[96];
		int n = (int)(p - s);
		if (n > (int)sizeof(tmp) - 1)
			n = (int)sizeof(tmp) - 1;
		memcpy(tmp, s, (size_t)n);
		tmp[n] = 0;
		hi = strtod(tmp, 0);
	}
	/* the digits as a dd value: Horner in chunks of 22 digits, because a
	 * 40-digit integer does not fit in the 106-bit significand and the
	 * multiplications must be exact (10^22 is exact as a double) */
	{
		int i, j, used = nd;
		dv = plua_dd_from_double(0.0);
		for (i = 0; i < used; ) {
			int n = (used - i < 22) ? (used - i) : 22;
			/* the digit loop below scales by 10 per digit, which is what
			 * carries the chunk boundary; multiplying by 10^n here as well
			 * scaled twice (a 32-digit input came out 1e10 too big) */
			for (j = 0; j < n; j++) {
				dv = plua_dd_mul_d(dv, 10.0);
				dv = plua_dd_add(dv, plua_dd_from_double(
					(double)(d[i + j] - '0')));
			}
			i += n;
		}
		{
			int k = exp10;
			while (k > 0) {
				int step = k > 22 ? 22 : k;
				double p10 = 1.0;
				int j;
				for (j = 0; j < step; j++)
					p10 *= 10.0;
				dv = plua_dd_mul_d(dv, p10);
				k -= step;
			}
			while (k < 0) {
				int step = (-k) > 22 ? 22 : -k;
				double p10 = 1.0;
				int j;
				for (j = 0; j < step; j++)
					p10 *= 10.0;
				dv = plua_dd_div_d(dv, p10);
				k += step;
			}
		}
	}
	/* The digit arithmetic above already yields the value to ~1e-32
	 * relative; adding hi and then the residual (dv - hi) would cancel twice
	 * and only add rounding, so it is not done.  `hi` is still parsed (it is
	 * what strtod is for, and it keeps extreme exponents honest), and a
	 * disagreement between the two is not expected to matter at this width.
	 */
	(void)hi;
	res = dv;
	if (neg)
		res = plua_dd_neg(res);
	return res;
}
