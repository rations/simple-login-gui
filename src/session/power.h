/* Shutdown, restart and the switch to a text console.
 *
 * Plain C, no shell anywhere, and every one of these is reachable by somebody who has NOT
 * authenticated. That is deliberate and it is written down rather than glossed over: a person
 * standing at the keyboard of this machine can already hold the power button in, so refusing
 * them a Shutdown entry buys nothing and costs them a clean unmount. What it does buy is the
 * confirm step in the menu, so that a stray click cannot do it.
 *
 * HOW THE BINARY IS FOUND. From a compile-time candidate list, probed with access(X_OK) at
 * call time, and exec'd with execve and an argv array. Never $PATH, never a shell, never a
 * path from a config file. /sbin is a symlink to /usr/sbin on the machine this was developed
 * on and that is NOT a property of Devuan, so hardcoding either one is how this breaks on
 * somebody else's box.
 *
 * WHY shutdown(8) AND NOT loginctl. This process is already root, so there is nothing to ask
 * permission for, and shutdown(8) works on a seatd-only machine with no elogind -- which is
 * the configuration this program exists to serve. (cites: on the development machine
 * sysvinit-core owns /usr/sbin/shutdown; `-h -P now` powers off and `-r now` reboots, read out
 * of `shutdown --help`, not assumed from another distribution's shutdown.)
 */

#ifndef XLOGIN_SESSION_POWER_H
#define XLOGIN_SESSION_POWER_H

#ifdef __cplusplus
extern "C" {
#endif

/* Each of these returns 0 on success. On failure it returns non-zero and *msg points at a
 * short explanation owned by this module, suitable for the status line. */

int power_shutdown(const char **msg);
int power_restart(const char **msg);

/* Switch the console to `vt` (1..63). Does NOT quit the X server: X keeps the VT it was
 * started on, so Alt+F1 comes straight back to a login screen that was never torn down.
 *
 * The caller must release the keyboard grab first -- this code cannot do it, because the grab
 * belongs to the X connection and this half of the program does not have one. */
int power_switch_console(int vt, const char **msg);

/* Is there a getty configured on `vt`? Reads /etc/inittab. Switching to a VT with nothing on
 * it gives a black screen with a blinking cursor, which is indistinguishable from a crash --
 * so the answer is used to warn, not to refuse. Returns 1 yes, 0 no, -1 could not tell. */
int power_vt_has_getty(int vt);

#ifdef __cplusplus
}
#endif

#endif /* XLOGIN_SESSION_POWER_H */
