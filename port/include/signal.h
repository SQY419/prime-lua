/*
 * signal.h -- primeLua's own signal header.
 *
 * lua.c includes <signal.h> unconditionally, but the SIGINT hook it installs
 * is only compiled when LUA_USE_POSIX is defined -- and it is not, because
 * there are no processes to interrupt here.  The calculator's ESC/ON keys are
 * handled by the launcher, not by a signal.
 */
#ifndef PLUA_SIGNAL_H
#define PLUA_SIGNAL_H

typedef int sig_atomic_t;

#define SIGINT		2
#define SIG_DFL		((void (*)(int))0)
#define SIG_IGN		((void (*)(int))1)
#define SIG_ERR		((void (*)(int))-1)

void (*signal(int sig, void (*handler)(int)))(int);

#endif /* PLUA_SIGNAL_H */
