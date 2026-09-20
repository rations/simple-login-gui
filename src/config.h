/* /etc/xlogin.conf -- the handful of settings the login screen can change about itself.
 *
 * THE FILE IS SHELL-SOURCED BY THE LAUNCHER (`. /etc/xlogin.conf`, run by /bin/sh as root at
 * boot, before the X server starts), which is the fact that shapes this whole file:
 *
 *   * ANYTHING WRITTEN HERE IS EXECUTED AS ROOT AT THE NEXT BOOT. A value is written
 *     single-quoted, and a value that cannot be safely single-quoted -- one containing a
 *     single quote, a newline or a control character -- is REFUSED rather than escaped.
 *     Escaping is a thing that can be got subtly wrong; refusing is not.
 *   * UNKNOWN KEYS ARE PRESERVED. XSERVER_FLAGS lives in this file and is what decides whether
 *     the X server gets `-seat seat0 -keeptty`; a writer that rewrote the file from its own
 *     idea of what belongs in it would take the machine's graphics configuration with it.
 *     Comments and blank lines are preserved too, for whoever has to fix this from tty2.
 *   * WRITES ARE ATOMIC. Temp file in the same directory, fsync, rename, then fsync the
 *     directory. A login manager that half-wrote its own config during a power cut and left
 *     the launcher sourcing a truncated line is a machine that does not boot.
 *
 * Reading is deliberately tolerant and never fails: a missing file, an unparseable line or a
 * nonsense value all give the compiled-in default. This file is read before anybody has logged
 * in, and there is no one to tell.
 */

#ifndef XLOGIN_CONFIG_H
#define XLOGIN_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

/* Long enough for any filename the backgrounds directory can hold (NAME_MAX is 255). */
#define XLOGIN_CFG_VALUE_MAX 256

typedef struct {
    /* A plain filename inside the backgrounds directory, never a path. Empty means none. */
    char background[XLOGIN_CFG_VALUE_MAX];
    /* fill | fit | center | stretch | tile */
    char bg_mode[16];
    /* The VT the Console entry switches to. 1..63 (MIN_NR_CONSOLES..MAX_NR_CONSOLES,
     * cites: linux/vt.h:10-11). Default 2, which is where the first getty respawns. */
    int console_vt;
} xlogin_config;

void config_defaults(xlogin_config *c);

/* Read the config into `c`, starting from the defaults. Never fails. */
void config_load(xlogin_config *c);

/* Rewrite one key in place, preserving every other line. Returns 0 on success.
 * Returns -1 without touching the file if `value` cannot be safely single-quoted, and -2 if
 * the file could not be written. */
int config_set(const char *key, const char *value);

/* Where the config lives. Exposed so the diagnostics can name it. */
const char *config_path(void);

#ifdef __cplusplus
}
#endif

#endif /* XLOGIN_CONFIG_H */
