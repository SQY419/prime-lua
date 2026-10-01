/*
 * assert.h -- primeLua's own assert header (freestanding: no stderr spam,
 * the message goes into the PRIMELOG ring so the calculator can show it).
 */
#ifndef PLUA_ASSERT_H
#define PLUA_ASSERT_H

void plua_assert_fail(const char *expr, const char *file, int line);

#ifdef NDEBUG
#define assert(e)	((void)0)
#else
#define assert(e)	((e) ? (void)0 : plua_assert_fail(#e, __FILE__, __LINE__))
#endif

#endif /* PLUA_ASSERT_H */
