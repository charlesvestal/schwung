/*
 * child_signals.h -- what a process the shim forks must NOT inherit.
 *
 * The shim forks from MoveOriginal's own threads, and those run with signals
 * BLOCKED (measured on hardware: SIGINT, SIGUSR1 and SIGTERM in SigBlk). A
 * signal mask survives both fork() and exec(), and sigaction() does not
 * unblock anything, so every child -- shadow_ui, link-subscriber, a helper
 * command, and everything THEY launch (a standalone tool via shadow_ui) --
 * started with SIGTERM blocked. shadow_ui's save-on-SIGTERM handler never ran
 * (the signal sat pending while the launcher's 3 s save window ran out and
 * the sweep SIGKILLed it, unsaved), and a standalone tool ignored SIGTERM and
 * SIGINT alike. Call this in the child between fork() and exec().
 */
#ifndef CHILD_SIGNALS_H
#define CHILD_SIGNALS_H
#include <signal.h>

static inline void child_reset_signals(void)
{
    sigset_t none;
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, NULL);
    /* An ignored disposition also survives exec; put back the defaults that
     * matter for a child we may need to stop. */
    signal(SIGTERM, SIG_DFL);
    signal(SIGINT, SIG_DFL);
    signal(SIGHUP, SIG_DFL);
    signal(SIGPIPE, SIG_DFL);
}
#endif
