/* Purging what a session left behind.
 *
 * When the session script exits, orphaned X clients owned by that user can still be connected
 * to the display -- a panel, a compositor, a wallpaper setter, anything that daemonised. They
 * would draw over the login screen that is about to come back, so they are killed before it
 * does.
 */

#ifndef XLOGIN_SESSION_CLEANUP_H
#define XLOGIN_SESSION_CLEANUP_H

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* SIGTERM every process whose REAL uid is `uid`, wait briefly, then SIGKILL whatever is left.
 * Refuses uid 0 outright: this process is root, and a bug that passed 0 here would kill init.
 *
 * Returns the number of processes signalled, or -1 if /proc could not be read. */
int cleanup_kill_user_processes(uid_t uid);

/* Look `user` up and purge their processes. A no-op for an unknown user or for root. */
void cleanup_after_session(const char *user);

#ifdef __cplusplus
}
#endif

#endif /* XLOGIN_SESSION_CLEANUP_H */
