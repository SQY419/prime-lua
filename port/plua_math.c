/*
 * plua_math.c -- the standard math names Lua's math library calls, mapped
 * onto openlibm.
 *
 * primetcc prebuilds openlibm (BSD, ~1ulp) as rt_math.o with every symbol
 * renamed to hp_* so it cannot collide with newlib; primeLua links that same
 * object, and this file re-exports the standard names.  Using openlibm
 * rather than the small Taylor-based hp_math.h library matters here: Lua
 * scripts and the Lua test suite compare math results against the host Lua,
 * so ~1ulp accuracy is what keeps primeLua's numbers identical.
 *
 * ldexp/frexp/modf are also in rt_math.o; hp_math.c's own hp_ldexp() is
 * global but is not referenced (the openlibm ones win by name).
 */
#include <math.h>

double hp_floor(double x);
double hp_ceil(double x);
double hp_trunc(double x);
double hp_round(double x);
double hp_fabs(double x);
double hp_fmod(double x, double y);
double hp_frexp(double x, int *e);
double hp_modf(double x, double *ip);
double hp_scalbn(double x, int n);
double hp_expm1(double x);
double hp_sqrt(double x);
double hp_pow(double x, double y);
double hp_exp(double x);
double hp_log(double x);
double hp_log10(double x);
double hp_log2(double x);	/* port/openlibm-extra/e_log2.c */
double hp_sin(double x);
double hp_cos(double x);
double hp_tan(double x);
double hp_asin(double x);
double hp_acos(double x);
double hp_atan(double x);
double hp_atan2(double y, double x);
double hp_sinh(double x);
double hp_cosh(double x);
double hp_tanh(double x);

double floor(double x)            { return hp_floor(x); }
double ceil(double x)             { return hp_ceil(x); }
double trunc(double x)            { return hp_trunc(x); }
double round(double x)            { return hp_round(x); }
double fmod(double x, double y)   { return hp_fmod(x, y); }
double frexp(double x, int *e)    { return hp_frexp(x, e); }
double modf(double x, double *ip) { return hp_modf(x, ip); }
double scalbn(double x, int n)    { return hp_scalbn(x, n); }
double ldexp(double x, int n)     { return hp_scalbn(x, n); }
double expm1(double x)            { return hp_expm1(x); }

double sqrt(double x)             { return hp_sqrt(x); }
double pow(double x, double y)    { return hp_pow(x, y); }
double exp(double x)              { return hp_exp(x); }
double log(double x)              { return hp_log(x); }
double log10(double x)            { return hp_log10(x); }
/* openlibm's s_copysign.c is not part of primetcc's subset either, and
 * e_pow.c needs it.  Bit-for-bit the upstream implementation. */
double copysign(double x, double y)
{
	union { double d; unsigned long long u; } a, b;
	a.d = x;
	b.d = y;
	a.u = (a.u & 0x7fffffffffffffffULL) | (b.u & 0x8000000000000000ULL);
	return a.d;
}
double log2(double x)             { return hp_log2(x); }

double sin(double x)              { return hp_sin(x); }
double cos(double x)              { return hp_cos(x); }
double tan(double x)              { return hp_tan(x); }
double asin(double x)             { return hp_asin(x); }
double acos(double x)             { return hp_acos(x); }
double atan(double x)             { return hp_atan(x); }
double atan2(double y, double x)  { return hp_atan2(y, x); }

double sinh(double x)             { return hp_sinh(x); }
double cosh(double x)             { return hp_cosh(x); }
double tanh(double x)             { return hp_tanh(x); }
