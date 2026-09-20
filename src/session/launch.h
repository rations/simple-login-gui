/* Starting the user's session: the privilege drop and the exec.
 *
 * THIS IS THE MOST SECURITY-SENSITIVE CODE IN THE PROGRAM and it is a PORT, not a rewrite. The
 * runtime-directory creation, the fd close, clearenv(), the setgid -> initgroups -> setuid
 * ordering, the minimal environment, load_locale_env() and the
 * ~/.xinitrc -> system xinitrc -> window-manager list -> xterm chain all come across from the
 * GTK implementation unchanged in behaviour. It is correct, it has been run every day, and
 * load_locale_env() in particular exists because of a bug that was already found and fixed
 * once: a cleared environment leaves glibc in the C locale, whose encoding is ASCII, and every
 * UTF-8 character in the user's terminal then renders as garbage. Retyping that from memory is
 * how it comes back.
 *
 * WHY THE ORDER OF THE PRIVILEGE DROP MATTERS: setgid first, then initgroups, then setuid.
 * setuid last because it is the call that makes the others impossible; initgroups before it
 * because it needs privilege; setgid before initgroups because initgroups(user, gid) takes the
 * primary group it is to keep. Getting this wrong does not fail loudly -- it silently leaves
 * the user in root's supplementary groups.
 */

#ifndef XLOGIN_SESSION_LAUNCH_H
#define XLOGIN_SESSION_LAUNCH_H

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int ok;
    pid_t pid;          /* the session child, valid when ok */
    const char *message; /* why not, when !ok; storage owned by this module */
} launch_result;

/* Fork, drop to `user`, and exec their session. Returns as soon as the child exists: the
 * caller watches for its exit through SIGCHLD rather than waiting, because the event loop must
 * keep turning. */
launch_result launch_session(const char *user);

#ifdef __cplusplus
}
#endif

#endif /* XLOGIN_SESSION_LAUNCH_H */
