/*
 * math.h -- primeLua's own math header (shadows newlib's).
 *
 * The transcendental functions are openlibm (BSD), prebuilt as rt_math.o
 * with every symbol renamed to hp_* (see primetcc's Makefile); plua_math.c
 * maps the standard names onto them.  Accuracy is openlibm's ~1ulp, which
 * matters because Lua's math.* is expected to be exact-ish.
 */
#ifndef PLUA_MATH_H
#define PLUA_MATH_H

#define HUGE_VAL	(__builtin_huge_val())
#define INFINITY	(__builtin_huge_valf())
#define NAN		(__builtin_nanf(""))
#define M_PI		3.14159265358979323846
#define M_E		2.7182818284590452354

#define isnan(x)	__builtin_isnan(x)
#define isinf(x)	__builtin_isinf(x)
#define isfinite(x)	__builtin_isfinite(x)
#define signbit(x)	__builtin_signbit(x)
#define fabs(x)		__builtin_fabs(x)
#define fabsf(x)	__builtin_fabsf(x)

double floor(double x);
double ceil(double x);
double trunc(double x);
double round(double x);
double fmod(double x, double y);
double frexp(double x, int *exp);
double ldexp(double x, int exp);
double modf(double x, double *iptr);
double scalbn(double x, int n);
double expm1(double x);
double copysign(double x, double y);

double sqrt(double x);
double pow(double x, double y);
double exp(double x);
double log(double x);
double log10(double x);
double log2(double x);

double sin(double x);
double cos(double x);
double tan(double x);
double asin(double x);
double acos(double x);
double atan(double x);
double atan2(double y, double x);

double sinh(double x);
double cosh(double x);
double tanh(double x);

#endif /* PLUA_MATH_H */
