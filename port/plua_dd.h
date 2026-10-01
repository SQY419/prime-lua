/*
 * plua_dd.h -- double-double arithmetic for primeLua (a NEW library: the
 * standard `math` library and Lua's own number type are untouched).
 *
 * A double-double is a pair of doubles (hi, lo) standing for the value
 * hi + lo, with hi holding the leading ~53 bits and lo the next ~53: about
 * 106 bits, i.e. 31-32 significant decimal digits, at every operation.  That
 * is the sweet spot on this machine: the calculator has no FPU, so each dd
 * operation is a handful of soft-float multiplies (10-20x a plain double op),
 * but it stays fast enough to be usable, which true arbitrary precision
 * (bignum + AGM) would not be.
 *
 * Design notes
 *   * Plain C99, no libm except sqrt() (used only for an initial estimate) and
 *     frexp/ldexp for scaling.  No long double (on the target it *is* double),
 *     no FMA assumption (ARM926 has none): products use Dekker's split.
 *   * Every operation renormalises, so the invariant |lo| <= ulp(hi)/2 holds
 *     for the values this library returns (checked by the host test).
 *   * Accuracy target, measured by tests/test_dd_host.c against mpmath at 60+
 *     digits: a few dd-ulp (1 dd-ulp = 2^-105 relative) for the transcendentals.
 *     The tested ranges and the real numbers are in port/plua_dd_NOTES.md.
 */
#ifndef PLUA_DD_H
#define PLUA_DD_H

typedef struct plua_dd {
	double hi, lo;
} plua_dd;

/* ---- construction / conversion ---- */
plua_dd plua_dd_from_double(double x);
plua_dd plua_dd_from_parts(double hi, double lo);   /* renormalises */
plua_dd plua_dd_from_long(long long n);
plua_dd plua_dd_nan(void);
plua_dd plua_dd_inf(int sign);
double  plua_dd_to_double(plua_dd x);               /* correctly rounded sum */
int     plua_dd_is_nan(plua_dd x);
int     plua_dd_is_inf(plua_dd x);
int     plua_dd_is_zero(plua_dd x);
int     plua_dd_sign(plua_dd x);                    /* -1, 0, +1 */

/* ---- arithmetic ---- */
plua_dd plua_dd_add(plua_dd a, plua_dd b);
plua_dd plua_dd_sub(plua_dd a, plua_dd b);
plua_dd plua_dd_mul(plua_dd a, plua_dd b);
plua_dd plua_dd_div(plua_dd a, plua_dd b);
plua_dd plua_dd_neg(plua_dd a);
plua_dd plua_dd_abs(plua_dd a);
plua_dd plua_dd_mul_d(plua_dd a, double b);         /* exact-ish: one split */
plua_dd plua_dd_div_d(plua_dd a, double b);
plua_dd plua_dd_scale2(plua_dd a, int k);           /* a * 2^k, exact */
plua_dd plua_dd_fmod(plua_dd a, plua_dd b);
plua_dd plua_dd_floor(plua_dd a);
plua_dd plua_dd_ceil(plua_dd a);
plua_dd plua_dd_trunc(plua_dd a);
int     plua_dd_cmp(plua_dd a, plua_dd b);          /* -1, 0, +1 (NaN: 2) */

/* ---- functions ---- */
plua_dd plua_dd_sqrt(plua_dd a);
plua_dd plua_dd_exp(plua_dd a);
plua_dd plua_dd_log(plua_dd a);                     /* natural log */
plua_dd plua_dd_log10(plua_dd a);
plua_dd plua_dd_pow(plua_dd a, plua_dd b);
plua_dd plua_dd_sin(plua_dd a);
plua_dd plua_dd_cos(plua_dd a);
plua_dd plua_dd_tan(plua_dd a);
plua_dd plua_dd_atan(plua_dd a);
plua_dd plua_dd_asin(plua_dd a);
plua_dd plua_dd_acos(plua_dd a);
plua_dd plua_dd_atan2(plua_dd y, plua_dd x);
plua_dd plua_dd_hypot(plua_dd a, plua_dd b);

/* ---- constants ---- */
plua_dd plua_dd_pi(void);
plua_dd plua_dd_e(void);
plua_dd plua_dd_ln2(void);
plua_dd plua_dd_ln10(void);

/* ---- text ----
 * to_string(): up to 33 significant digits, written into buf (the caller's
 * buffer must hold ~48 bytes).  Returns the length written, or -1 when the
 * buffer is too small.  The value is printed as the exact sum of the two
 * doubles that make it up, so at most the last digit can be off by one; use
 * plua_dd_from_string() to read it back (round-trip is exact for the digits
 * printed).
 *
 * from_string(): parses decimal or hex floats with more precision than a
 * double can hold, by taking the first 17 digits with strtod() and adding the
 * residual contribution of the rest in dd arithmetic.  *end (may be NULL)
 * receives the first unparsed character.
 */
int  plua_dd_to_string(plua_dd x, char *buf, int buflen, int sigdigits);
plua_dd plua_dd_from_string(const char *s, const char **end);

#endif /* PLUA_DD_H */
